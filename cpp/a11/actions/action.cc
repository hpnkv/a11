// Copyright 2026 The A11 Authors.

#include "a11/actions/action.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/log/log.h>
#include <absl/random/random.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "a11/actions/internal/exception_guarded_handlers.h"
#include "a11/actions/log.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/concurrency/parallel.h"
#include "a11/data/types.h"
#include "a11/net/wire_stream.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/obs/span.h"
#include "a11/obs/trace_context.h"
#include "a11/obs/tracer.h"
#include "a11/service/session.h"
#include "a11/status.h"
#include "a11/uuid.h"
#include "thread/boost_primitives.h"
#include "thread/fiber.h"
#include "thread/select.h"
#include "thread/selectables.h"

namespace a11::actions {
namespace {

/** How many times a generated id is retried against a session's live ones. */
constexpr int kActionIdAttempts = 4;

std::string NewActionId() {
  return a11::NewShortId();
}

/**
 * @brief An id for a fresh action, unused by @p session if one is given.
 *
 * The generator's 48 bits already make a collision improbable; a session knows
 * which ids are live and can make it impossible, so where that knowledge is at
 * hand it is used. It usually is not -- an action built without a session has
 * nothing to check against, and does not need one, because the id only has to
 * be unique among the ids it will meet.
 *
 * Four collisions in a row is not luck, it is a session holding an implausible
 * number of live actions, so the last resort is a full UUID: longer than anyone
 * wanted, and certainly free.
 */
std::string NewActionId(const std::shared_ptr<service::Session>& session) {
  if (session == nullptr) {
    return NewActionId();
  }
  for (int attempt = 0; attempt < kActionIdAttempts; ++attempt) {
    std::string candidate = NewActionId();
    if (!session->GetAction(candidate).ok()) {
      return candidate;
    }
  }
  return a11::NewUuid();
}

absl::Status CancelledStatus() {
  return absl::CancelledError("Action was cancelled");
}

absl::Time TimeoutDeadline(absl::Duration timeout) {
  return timeout == absl::InfiniteDuration() ? absl::InfiniteFuture()
                                             : absl::Now() + timeout;
}

void KeepFirstError(absl::Status candidate, absl::Status* first) {
  if (first->ok() && !candidate.ok()) {
    *first = std::move(candidate);
  }
}

// Records the status and canonical error code on an action span.
void RecordSpanOutcome(obs::Span& span, const absl::Status& status) {
  span.SetStatus(status);
  if (!status.ok()) {
    span.SetAttribute("error.type", absl::StatusCodeToString(status.code()));
  }
}

}  // namespace

ActionLimiter::ActionLimiter(size_t maximum)
    : maximum_(maximum), changed_(std::make_shared<thread::PermanentEvent>()) {}

ActionHandler MakeAsyncActionHandler(SyncActionHandler handler) {
  return [handler = internal::GuardSyncHandler(std::move(handler))](
             std::shared_ptr<Action> action) mutable {
    return a11::SubmitTask(
        [handler, action = std::move(action)]() mutable -> absl::Status {
          return handler(std::move(action));
        });
  };
}

absl::StatusOr<std::shared_ptr<ActionLimiter>> ActionLimiter::Create(
    size_t maximum) {
  if (maximum == 0) {
    return absl::InvalidArgumentError(
        "Action concurrency limit must be positive");
  }

  struct MakeSharedEnabler final : ActionLimiter {
    explicit MakeSharedEnabler(size_t maximum) : ActionLimiter(maximum) {}
  };

  return std::make_shared<MakeSharedEnabler>(maximum);
}

absl::Status ActionLimiter::Acquire() {
  while (true) {
    std::shared_ptr<thread::PermanentEvent> changed;
    {
      thread::MutexLock lock(&mu_);
      if (active_ < maximum_) {
        ++active_;
        return absl::OkStatus();
      }
      changed = changed_;
    }
    const int selected =
        thread::Select({thread::OnCancel(), changed->OnEvent()});
    if (selected == 0) {
      return absl::CancelledError(
          "Action was cancelled while waiting for concurrency capacity");
    }
  }
}

void ActionLimiter::Release() {
  std::shared_ptr<thread::PermanentEvent> notify;
  {
    thread::MutexLock lock(&mu_);
    if (active_ == 0) {
      return;
    }
    --active_;
    notify =
        std::exchange(changed_, std::make_shared<thread::PermanentEvent>());
  }
  notify->Notify();
}

absl::StatusOr<std::shared_ptr<Action>> Action::Create(
    ActionSchema schema, std::string action_id, ActionHandler handler,
    std::shared_ptr<nodes::NodeMap> node_map,
    std::shared_ptr<net::WireStream> stream,
    std::shared_ptr<service::Session> session,
    std::shared_ptr<ActionRegistry> registry,
    size_t max_concurrent_nested_actions) {
  ABSL_RETURN_IF_ERROR(schema.Validate());
  if (action_id.empty()) {
    action_id = NewActionId(session);
  }
  ABSL_RETURN_IF_ERROR(data::ValidateName(action_id));
  if (node_map == nullptr && session != nullptr) {
    node_map = session->GetNodeMap();
  }
  if (node_map == nullptr) {
    ABSL_ASSIGN_OR_RETURN(node_map, nodes::NodeMap::Create());
  }
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<ActionLimiter> limiter,
                        ActionLimiter::Create(max_concurrent_nested_actions));

  struct MakeSharedEnabler final : Action {
    MakeSharedEnabler(ActionSchema schema, std::string action_id,
                      ActionHandler handler,
                      std::shared_ptr<nodes::NodeMap> node_map,
                      std::shared_ptr<net::WireStream> stream,
                      const std::shared_ptr<service::Session>& session,
                      std::shared_ptr<ActionRegistry> registry,
                      std::shared_ptr<ActionLimiter> nested_limiter)
        : Action(std::move(schema), std::move(action_id), std::move(handler),
                 std::move(node_map), std::move(stream), session,
                 std::move(registry), std::move(nested_limiter)) {}
  };

  auto action = std::make_shared<MakeSharedEnabler>(
      std::move(schema), std::move(action_id), std::move(handler),
      std::move(node_map), std::move(stream), std::move(session),
      std::move(registry), std::move(limiter));
  {
    thread::MutexLock lock(&action->mu_);
    ABSL_RETURN_IF_ERROR(action->RemapDefaultPorts());
  }
  return action;
}

Action::Action(ActionSchema schema, std::string id, ActionHandler handler,
               std::shared_ptr<nodes::NodeMap> node_map,
               std::shared_ptr<net::WireStream> stream,
               const std::shared_ptr<service::Session>& session,
               std::shared_ptr<ActionRegistry> registry,
               std::shared_ptr<ActionLimiter> nested_limiter)
    : schema_(std::move(schema)),
      // Guarded at adoption; see actions/internal/exception_guarded_handlers.h.
      handler_(internal::GuardHandler(std::move(handler))),
      id_(std::move(id)),
      node_map_(std::move(node_map)),
      stream_(std::move(stream)),
      session_(session),
      registry_(std::move(registry)),
      done_promise_(std::make_shared<a11::Promise<a11::Unit>>()),
      done_future_(done_promise_->future()),
      dispatch_promise_(std::make_shared<a11::Promise<a11::Unit>>()),
      dispatch_future_(dispatch_promise_->future()),
      nested_limiter_(std::move(nested_limiter)) {
  for (const auto& [name, header] : schema_.headers) {
    if (header.default_value.has_value()) {
      headers_.emplace(absl::AsciiStrToLower(name), *header.default_value);
    }
  }
}

absl::StatusOr<std::string> Action::MakeNodeId(std::string_view action_id,
                                               std::string_view node_name) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(action_id));
  ABSL_RETURN_IF_ERROR(data::ValidateName(node_name));
  std::string result = absl::StrCat(action_id, "#", node_name);
  ABSL_RETURN_IF_ERROR(data::ValidateName(result));
  return result;
}

std::string Action::GetId() const {
  thread::MutexLock lock(&mu_);
  return id_;
}

absl::Status Action::SetId(std::string action_id) {
  absl::Status status = data::ValidateName(action_id);
  if (!status.ok()) {
    return status;
  }
  thread::MutexLock lock(&mu_);
  if (mode_ != Mode::kNone) {
    return absl::FailedPreconditionError(
        "Cannot change Action id after it has started");
  }
  const std::string previous = std::exchange(id_, std::move(action_id));
  status = RemapDefaultPorts();
  if (!status.ok()) {
    id_ = previous;
    RemapDefaultPorts().IgnoreError();
  }
  return status;
}

ActionSchema Action::GetSchema() const {
  thread::MutexLock lock(&mu_);
  return schema_;
}

absl::Status Action::SetSchema(ActionSchema schema) {
  ABSL_RETURN_IF_ERROR(schema.Validate());
  thread::MutexLock lock(&mu_);
  if (mode_ != Mode::kNone) {
    return absl::FailedPreconditionError(
        "Cannot change Action schema after it has started");
  }
  schema_ = std::move(schema);
  return RemapDefaultPorts();
}

absl::Status Action::BindHandler(ActionHandler handler) {
  if (!handler) {
    return absl::InvalidArgumentError("handler must be callable");
  }
  thread::MutexLock lock(&mu_);
  if (mode_ != Mode::kNone) {
    return absl::FailedPreconditionError(
        "Cannot change Action handler after it has started");
  }
  handler_ = internal::GuardHandler(std::move(handler));
  return absl::OkStatus();
}

ActionHandler Action::GetHandler() const {
  thread::MutexLock lock(&mu_);
  return handler_;
}

bool Action::HasHandler() const {
  thread::MutexLock lock(&mu_);
  return static_cast<bool>(handler_);
}

ActionSettings Action::GetSettings() const {
  thread::MutexLock lock(&mu_);
  return settings_;
}

absl::Status Action::SetSettings(ActionSettings settings) {
  thread::MutexLock lock(&mu_);
  settings_ = settings;
  return absl::OkStatus();
}

absl::Status Action::BindStreamsOnInputsByDefault(bool bind) {
  thread::MutexLock lock(&mu_);
  settings_.bind_streams_on_inputs_by_default = bind;
  return absl::OkStatus();
}

absl::Status Action::BindStreamsOnOutputsByDefault(bool bind) {
  thread::MutexLock lock(&mu_);
  settings_.bind_streams_on_outputs_by_default = bind;
  return absl::OkStatus();
}

absl::Status Action::ClearInputsAfterRun(bool clear) {
  thread::MutexLock lock(&mu_);
  settings_.clear_inputs_after_run = clear;
  return absl::OkStatus();
}

absl::Status Action::ClearOutputsAfterRun(bool clear) {
  thread::MutexLock lock(&mu_);
  settings_.clear_outputs_after_run = clear;
  return absl::OkStatus();
}

absl::Status Action::BindNodeMap(std::shared_ptr<nodes::NodeMap> node_map) {
  thread::MutexLock lock(&mu_);
  node_map_ = std::move(node_map);
  return absl::OkStatus();
}

std::shared_ptr<nodes::NodeMap> Action::GetNodeMap() const {
  thread::MutexLock lock(&mu_);
  return node_map_;
}

absl::Status Action::BindStream(std::shared_ptr<net::WireStream> stream) {
  std::shared_ptr<net::WireStream> previous;
  std::vector<std::shared_ptr<nodes::AsyncNode>> nodes;
  {
    thread::MutexLock lock(&mu_);
    previous = stream_;
    if (previous == stream) {
      return absl::OkStatus();
    }
    nodes.assign(stream_bound_nodes_.begin(), stream_bound_nodes_.end());
  }
  std::vector<std::shared_ptr<nodes::AsyncNode>> rebound;
  for (const auto& node : nodes) {
    absl::Status status;
    if (previous != nullptr) {
      status = node->DetachStream(previous);
    }
    if (status.ok() && stream != nullptr) {
      status = node->AttachStream(stream);
    }
    if (!status.ok()) {
      for (const auto& completed : rebound) {
        if (stream != nullptr) {
          completed->DetachStream(stream).IgnoreError();
        }
        if (previous != nullptr) {
          completed->AttachStream(previous).IgnoreError();
        }
      }
      return status;
    }
    rebound.push_back(node);
  }
  thread::MutexLock lock(&mu_);
  if (stream_ != previous) {
    return absl::AbortedError("Action stream changed concurrently");
  }
  stream_ = std::move(stream);
  return absl::OkStatus();
}

std::shared_ptr<net::WireStream> Action::GetStream() const {
  thread::MutexLock lock(&mu_);
  return stream_;
}

absl::Status Action::BindRegistry(std::shared_ptr<ActionRegistry> registry) {
  thread::MutexLock lock(&mu_);
  registry_ = std::move(registry);
  return absl::OkStatus();
}

std::shared_ptr<ActionRegistry> Action::GetRegistry() const {
  thread::MutexLock lock(&mu_);
  return registry_;
}

absl::Status Action::BindSession(
    const std::shared_ptr<service::Session>& session) {
  std::shared_ptr<service::Session> previous_tracked;
  {
    thread::MutexLock lock(&mu_);
    if (session_.lock() == session) {
      return absl::OkStatus();
    }
    previous_tracked = tracked_session_.lock();
  }
  if (previous_tracked != nullptr && session != nullptr) {
    ABSL_RETURN_IF_ERROR(session->TrackAction(shared_from_this()));
  }
  {
    thread::MutexLock lock(&mu_);
    session_ = session;
    tracked_session_ = previous_tracked != nullptr ? session : nullptr;
  }
  if (previous_tracked != nullptr) {
    previous_tracked->UntrackAction(shared_from_this());
  }
  return absl::OkStatus();
}

std::shared_ptr<service::Session> Action::GetSession() const {
  thread::MutexLock lock(&mu_);
  return session_.lock();
}

absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> Action::GetNode(
    std::string node_id) {
  std::shared_ptr<nodes::NodeMap> node_map;
  {
    thread::MutexLock lock(&mu_);
    node_map = node_map_;
  }
  if (node_map == nullptr) {
    return absl::FailedPreconditionError("Action has no NodeMap");
  }
  return node_map->Get(std::move(node_id));
}

absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> Action::GetInput(
    const std::string& name, std::optional<bool> bind_stream) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(name));
  std::string node_id;
  bool bind = false;
  {
    thread::MutexLock lock(&mu_);
    const auto found = input_ids_.find(name);
    if (found == input_ids_.end()) {
      return absl::NotFoundError(
          absl::StrCat("Action input '", name, "' is not mapped"));
    }
    node_id = found->second;
    bind = bind_stream.value_or(
        settings_.bind_streams_on_inputs_by_default.value_or(mode_ !=
                                                             Mode::kRun));
  }
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<nodes::AsyncNode> node,
                        GetNode(node_id));
  {
    thread::MutexLock lock(&mu_);
    input_nodes_.insert(node);
  }
  ABSL_RETURN_IF_ERROR(AttachStreamIfRequested(node, bind));
  return node;
}

absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> Action::GetOutput(
    const std::string& name, std::optional<bool> bind_stream) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(name));
  std::string node_id;
  bool bind = false;
  {
    thread::MutexLock lock(&mu_);
    const auto found = output_ids_.find(name);
    if (found == output_ids_.end()) {
      return absl::NotFoundError(
          absl::StrCat("Action output '", name, "' is not mapped"));
    }
    node_id = found->second;
    bind = bind_stream.value_or(
        settings_.bind_streams_on_outputs_by_default.value_or(mode_ ==
                                                              Mode::kRun));
  }
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<nodes::AsyncNode> node,
                        GetNode(node_id));
  bool finished = false;
  absl::Status final_status;
  {
    thread::MutexLock lock(&mu_);
    output_nodes_.insert(node);
    finished = outputs_finished_;
    final_status = outputs_final_status_;
  }
  ABSL_RETURN_IF_ERROR(AttachStreamIfRequested(node, bind));
  if (finished) {
    // The Action has already closed its outputs, so this one will never be
    // written. Give the caller the terminal state its port had at completion --
    // an empty closed stream, or the failure -- rather than a node that nothing
    // will ever close and any read of which would wait forever.
    ABSL_RETURN_IF_ERROR(CloseUnwrittenOutput(node, final_status));
  }
  return node;
}

absl::Status Action::CloseUnwrittenOutput(
    const std::shared_ptr<nodes::AsyncNode>& node, const absl::Status& status) {
  const absl::StatusOr<bool> writable = node->IsWritable().Await();
  if (!writable.ok()) {
    return writable.status();
  }
  if (!*writable) {
    return absl::OkStatus();
  }
  if (status.ok()) {
    return node->Close().Await().status();
  }
  return node->AbortWithStatus(status).Await().status();
}

absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> Action::GetPort(
    const std::string& name) {
  bool input = false;
  bool output = false;
  {
    thread::MutexLock lock(&mu_);
    input = input_ids_.find(std::string(name)) != input_ids_.end();
    output = output_ids_.find(std::string(name)) != output_ids_.end();
  }
  if (input && output) {
    return absl::FailedPreconditionError(
        "Action port is both an input and output; select one explicitly");
  }
  if (input) {
    return GetInput(name);
  }
  if (output) {
    return GetOutput(name);
  }
  return absl::NotFoundError("Action port is not mapped");
}

bool Action::ContainsPort(std::string_view name) const {
  thread::MutexLock lock(&mu_);
  return input_ids_.find(std::string(name)) != input_ids_.end() ||
         output_ids_.find(std::string(name)) != output_ids_.end();
}

absl::Status Action::Log(data::Chunk chunk, const LogOptions& options) {
  if (!options.mimetype.empty()) {
    return absl::InvalidArgumentError(
        "Cannot give a log mimetype for a chunk that already has one");
  }
  return WriteLog(std::move(chunk), options);
}

absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> Action::GetLogNode() {
  {
    thread::MutexLock lock(&mu_);
    log_claimed_ = true;
  }
  return GetOutput(std::string(kActionLogOutput), /*bind_stream=*/false);
}

absl::Status Action::WriteLog(data::Chunk chunk, const LogOptions& options) {
  ABSL_ASSIGN_OR_RETURN(const LogLevel level, ParseLogLevel(options.level));
  if (!chunk.metadata.has_value()) {
    chunk.metadata.emplace();
  }
  if (chunk.metadata->mimetype.empty()) {
    chunk.metadata->mimetype = std::string(data::kBytesMimetype);
  }
  // A status chunk would abort the log port instead of becoming a log value.
  if (data::IsStatusChunk(chunk)) {
    return absl::InvalidArgumentError(
        "Cannot log a status chunk; log its message instead");
  }
  chunk.metadata->timestamp = absl::Now();
  // Named options override matching entries in the metadata map.
  if (options.metadata != nullptr) {
    for (const auto& [key, value] : *options.metadata) {
      ABSL_RETURN_IF_ERROR(chunk.metadata->SetAttribute(key, value));
    }
  }
  ABSL_RETURN_IF_ERROR(chunk.metadata->SetAttribute(
      std::string(kLogLevelAttribute), std::string(LogLevelName(level))));
  ABSL_RETURN_IF_ERROR(chunk.metadata->SetAttribute(
      std::string(kLogInternalAttribute),
      std::string(options.internal ? kLogInternalTrue : kLogInternalFalse)));
  if (!options.channel.empty()) {
    ABSL_RETURN_IF_ERROR(chunk.metadata->SetAttribute(
        std::string(kLogChannelAttribute), std::string(options.channel)));
  }
  if (!options.file.empty()) {
    ABSL_RETURN_IF_ERROR(chunk.metadata->SetAttribute(
        std::string(kLogFileAttribute), std::string(options.file)));
  }
  if (options.lineno.has_value()) {
    ABSL_RETURN_IF_ERROR(chunk.metadata->SetAttribute(
        std::string(kLogLinenoAttribute), absl::StrCat(*options.lineno)));
  }

  std::string node_id;
  std::string action_name;
  std::string action_id;
  bool claimed = false;
  bool finished = false;
  bool readable = false;
  {
    thread::MutexLock lock(&mu_);
    // A log port belongs to the running handler, including for remote calls.
    if (mode_ != Mode::kRun) {
      return absl::FailedPreconditionError(
          "Only a running Action may log; a caller logs on its own action");
    }
    action_name = schema_.name;
    action_id = id_;
    claimed = log_claimed_;
    finished = outputs_finished_;
    // Avoid buffering an unclaimed local log port. Peers mirror the node and
    // therefore count as readers.
    readable = claimed || stream_ != nullptr || !session_.expired();
    const auto found = output_ids_.find(std::string(kActionLogOutput));
    if (found != output_ids_.end()) {
      node_id = found->second;
    }
  }
  if (!claimed) {
    ReportLog(LogRecordFromChunk(chunk, action_name, action_id));
  }
  if (!readable || finished || node_id.empty()) {
    return absl::OkStatus();
  }

  // Log delivery failures are reported without changing the action outcome.
  absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> node = GetNode(node_id);
  if (!node.ok()) {
    LOG(WARNING) << "Could not open the log port of action " << action_name
                 << ": " << node.status();
    return absl::OkStatus();
  }
  const absl::Status attached = AttachStreamIfRequested(*node, /*bind=*/true);
  if (!attached.ok()) {
    LOG(WARNING) << "Could not attach the log port of action " << action_name
                 << " to its peer: " << attached;
  }
  bool finished_now = false;
  absl::Status final_status;
  {
    thread::MutexLock lock(&mu_);
    output_nodes_.insert(*node);
    finished_now = outputs_finished_;
    final_status = outputs_final_status_;
  }
  // Close a log node created after FinishOutputNodes published the outcome.
  if (finished_now) {
    (void)CloseUnwrittenOutput(*node, final_status);
    return absl::OkStatus();
  }
  const absl::StatusOr<std::uint32_t> written =
      (*node)->PutChunk(std::move(chunk)).Await();
  if (!written.ok()) {
    LOG(WARNING) << "Could not write to the log port of action " << action_name
                 << ": " << written.status();
  }
  return absl::OkStatus();
}

data::ActionMessage Action::GetActionMessage() const {
  thread::MutexLock lock(&mu_);
  data::ActionMessage result{.id = id_, .name = schema_.name};
  result.headers = headers_;
  result.inputs.reserve(schema_.inputs.size());
  result.outputs.reserve(schema_.outputs.size());
  for (const auto& name : schema_.inputs | std::views::keys) {
    result.inputs.push_back(
        data::Port{.name = name, .id = input_ids_.at(name)});
  }
  for (const auto& name : schema_.outputs | std::views::keys) {
    result.outputs.push_back(
        data::Port{.name = name, .id = output_ids_.at(name)});
  }
  return result;
}

absl::Status Action::MapPortsFromMessage(const data::ActionMessage& message) {
  ABSL_RETURN_IF_ERROR(message.Validate());
  thread::MutexLock lock(&mu_);
  if (mode_ != Mode::kNone) {
    return absl::FailedPreconditionError(
        "Cannot remap Action ports after it has started");
  }
  ABSL_RETURN_IF_ERROR(
      ValidateMessagePorts(message.inputs, schema_.inputs, "input"));
  ABSL_RETURN_IF_ERROR(
      ValidateMessagePorts(message.outputs, schema_.outputs, "output"));
  for (const data::Port& port : message.inputs) {
    input_ids_[port.name] = port.id;
  }
  for (const data::Port& port : message.outputs) {
    output_ids_[port.name] = port.id;
  }
  return absl::OkStatus();
}

data::ByteMap Action::Headers() const {
  thread::MutexLock lock(&mu_);
  return headers_;
}

absl::StatusOr<std::optional<data::Bytes>> Action::GetHeader(
    std::string_view name) const {
  ABSL_RETURN_IF_ERROR(data::ValidateName(name));
  const std::string folded = absl::AsciiStrToLower(name);
  thread::MutexLock lock(&mu_);
  const auto found = headers_.find(folded);
  if (found == headers_.end()) {
    return std::nullopt;
  }
  return std::optional<data::Bytes>(found->second);
}

bool Action::HasHeader(std::string_view name) const {
  if (!data::ValidateName(name).ok()) {
    return false;
  }
  thread::MutexLock lock(&mu_);
  return headers_.find(absl::AsciiStrToLower(name)) != headers_.end();
}

absl::Status Action::SetHeader(const std::string& name, data::Bytes value) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(name));
  thread::MutexLock lock(&mu_);
  headers_.insert_or_assign(absl::AsciiStrToLower(name), std::move(value));
  return absl::OkStatus();
}

absl::Status Action::RemoveHeader(std::string_view name) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(name));
  thread::MutexLock lock(&mu_);
  headers_.erase(absl::AsciiStrToLower(name));
  return absl::OkStatus();
}

absl::Status Action::ForwardHeader(const std::shared_ptr<Action>& target,
                                   std::string_view name) const {
  if (target == nullptr) {
    return absl::InvalidArgumentError("target must not be null");
  }
  ABSL_ASSIGN_OR_RETURN(std::optional<data::Bytes> value, GetHeader(name));
  return value.has_value()
             ? target->SetHeader(std::string(name), std::move(*value))
             : absl::OkStatus();
}

absl::Status Action::ForwardHeadersWithPrefix(
    const std::shared_ptr<Action>& target, std::string_view prefix) const {
  if (target == nullptr) {
    return absl::InvalidArgumentError("target must not be null");
  }
  const std::string folded = absl::AsciiStrToLower(prefix);
  const data::ByteMap headers = Headers();
  for (const auto& [name, value] : headers) {
    if (std::string_view(name).starts_with(folded)) {
      ABSL_RETURN_IF_ERROR(target->SetHeader(name, value));
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<std::shared_ptr<Action>> Action::MakeNested(
    const ActionSchema& schema, bool propagate_io, bool forward_headers) {
  std::shared_ptr<nodes::NodeMap> node_map;
  std::shared_ptr<net::WireStream> stream;
  std::shared_ptr<service::Session> session;
  std::shared_ptr<ActionRegistry> registry;
  std::shared_ptr<ActionLimiter> limiter;
  {
    thread::MutexLock lock(&mu_);
    std::shared_ptr<service::Session> bound_session = session_.lock();
    node_map = propagate_io ? node_map_ : nullptr;
    stream = propagate_io ? stream_ : nullptr;
    session = propagate_io ? bound_session : nullptr;
    registry = registry_;
    limiter = bound_session != nullptr ? bound_session->GetActionLimiter(true)
                                       : nested_limiter_;
  }
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<Action> child,
      Action::Create(schema, {}, {}, std::move(node_map), std::move(stream),
                     std::move(session), std::move(registry)));
  {
    thread::MutexLock child_lock(&child->mu_);
    child->nested_limiter_ = std::move(limiter);
    child->parent_ = shared_from_this();
  }
  if (forward_headers) {
    ABSL_RETURN_IF_ERROR(ForwardHeadersWithPrefix(child));
  }
  {
    thread::MutexLock lock(&mu_);
    children_.insert(child);
  }
  return child;
}

absl::StatusOr<std::shared_ptr<Action>> Action::MakeNested(
    std::string_view action_name, bool propagate_io, bool forward_headers) {
  std::shared_ptr<ActionRegistry> registry = GetRegistry();
  if (registry == nullptr) {
    return absl::FailedPreconditionError(
        "Cannot resolve a nested Action without a registry");
  }
  absl::StatusOr<ActionSchema> schema = registry->GetSchema(action_name);
  if (!schema.ok()) {
    return schema.status();
  }
  absl::StatusOr<std::shared_ptr<Action>> child =
      MakeNested(*schema, propagate_io, forward_headers);
  if (!child.ok()) {
    return child.status();
  }
  absl::StatusOr<ActionHandler> handler = registry->GetHandler(action_name);
  if (handler.ok()) {
    absl::Status status = (*child)->BindHandler(std::move(*handler));
    if (!status.ok()) {
      return status;
    }
  }
  return *child;
}

absl::StatusOr<std::shared_ptr<Action>> Action::Run() {
  std::shared_ptr<ActionLimiter> limiter;
  {
    thread::MutexLock lock(&mu_);
    if (!handler_) {
      return absl::FailedPreconditionError("Action handler has not been set");
    }
  }
  ABSL_RETURN_IF_ERROR(Begin(Mode::kRun));
  if (absl::Status span_status = StartActionSpan(Mode::kRun);
      !span_status.ok()) {
    thread::MutexLock lock(&mu_);
    mode_ = Mode::kNone;
    return span_status;
  }
  const std::shared_ptr<service::Session> session = GetSession();
  bool nested = false;
  std::shared_ptr<ActionLimiter> nested_limiter;
  {
    thread::MutexLock lock(&mu_);
    nested = !parent_.expired();
    nested_limiter = nested_limiter_;
  }
  if (session != nullptr) {
    if (absl::Status status = TrackInSession(session); !status.ok()) {
      thread::MutexLock lock(&mu_);
      mode_ = Mode::kNone;
      return status;
    }
    limiter = session->GetActionLimiter(nested);
  } else if (nested) {
    limiter = std::move(nested_limiter);
  }
  std::shared_ptr<Action> self = shared_from_this();

  // With no limiter to wait on, nothing before the handler can block, so the
  // handler is started here and its completion continued with Then(). A fibre
  // whose only job is to Await() the handler's task is one push onto the worker
  // pool's queue and one condvar signal per action, and the handler itself
  // still runs wherever it chooses to: a synchronous one is submitted to a
  // fibre by MakeAsyncActionHandler, so Run() does not become the place a
  // caller's handler body executes.
  //
  // A limiter means admission has to be waited for, and input autofills write
  // to nodes before the handler sees them, so both keep the fibre.
  if (limiter == nullptr && !HasInputAutofills()) {
    return RunHandlerWithoutFiber(self);
  }

  {
    const a11::Task task =
        a11::SubmitTask([self, limiter]() mutable -> absl::Status {
          self->RunHandler(limiter);
          return absl::OkStatus();
        });

    thread::MutexLock lock(&mu_);
    task_ = task;
    if (cancel_requested_) {
      task_.Cancel().IgnoreError();
    }
  }
  return self;
}

bool Action::HasInputAutofills() const {
  thread::MutexLock lock(&mu_);
  for (const auto& port : schema_.inputs | std::views::values) {
    if (!port.autofills.empty()) {
      return true;
    }
  }
  return false;
}

absl::StatusOr<std::shared_ptr<Action>> Action::RunHandlerWithoutFiber(
    const std::shared_ptr<Action>& self) {
  ActionHandler handler;
  bool cancelled = false;
  {
    thread::MutexLock lock(&mu_);
    handler = handler_;
    cancelled = cancel_requested_;
  }
  if (cancelled) {
    StartFinish(CancelledStatus());
    return self;
  }
  if (!handler) {
    StartFinish(
        absl::FailedPreconditionError("Action handler has not been set"));
    return self;
  }

  a11::Task task = handler(self);

  {
    thread::MutexLock lock(&mu_);
    // The handler's own task is what Cancel() reaches now, which stops the work
    // itself rather than a fibre waiting on it.
    task_ = task;
    if (cancel_requested_) {
      task_.Cancel().IgnoreError();
    }
  }

  // Finishing blocks -- it drains and closes ports -- so it keeps its fibre;
  // StartFinish() schedules it.
  a11::Then(task,
            [self](const absl::StatusOr<a11::Unit>& result)
                -> absl::StatusOr<a11::Unit> {
              absl::Status status = result.status();
              if (status.code() == absl::StatusCode::kCancelled) {
                status = CancelledStatus();
              }
              self->StartFinish(std::move(status));
              return a11::Unit{};
            });
  return self;
}

a11::Future<std::shared_ptr<Action>> Action::Call(data::ByteMap wire_headers) {
  absl::StatusOr<data::ByteMap> headers =
      net::NormalizeWireHeaders(std::move(wire_headers));
  if (!headers.ok()) {
    return a11::FailedFuture<std::shared_ptr<Action>>(headers.status());
  }
  absl::Status status = Begin(Mode::kCall);
  if (!status.ok()) {
    return a11::FailedFuture<std::shared_ptr<Action>>(status);
  }
  if (const absl::Status span_status = StartActionSpan(Mode::kCall);
      !span_status.ok()) {
    thread::MutexLock lock(&mu_);
    mode_ = Mode::kNone;
    return a11::FailedFuture<std::shared_ptr<Action>>(span_status);
  }
  {
    // Propagate this call's span context to the callee through the action's
    // headers, which travel inside the ActionMessage built below.
    thread::MutexLock lock(&mu_);
    span_.InjectContext(headers_).IgnoreError();
  }
  std::shared_ptr<service::Session> session = GetSession();
  if (session != nullptr) {
    status = TrackInSession(session);
    if (!status.ok()) {
      thread::MutexLock lock(&mu_);
      mode_ = Mode::kNone;
      return a11::FailedFuture<std::shared_ptr<Action>>(status);
    }
  }
  data::WireMessage wire{.node_fragments = CollectAutofillFragments(),
                         .actions = {GetActionMessage()},
                         .headers = std::move(*headers)};
  const std::shared_ptr<net::WireStream> stream = GetStream();
  if (stream != nullptr) {
    status = stream->Send(std::move(wire));
  } else if (session != nullptr) {
    status = session->Send(std::move(wire));
  } else {
    status = absl::FailedPreconditionError(
        "Calling an Action requires an attached WireStream or Session");
  }

  if (!status.ok()) {
    UntrackFromSession();
    thread::MutexLock lock(&mu_);
    mode_ = Mode::kNone;
    return a11::FailedFuture<std::shared_ptr<Action>>(status);
  }

  return a11::ReadyFuture(shared_from_this());
}

a11::Future<absl::Status> Action::WaitForDispatch(absl::Duration timeout) {
  if (timeout < absl::ZeroDuration()) {
    return a11::FailedFuture<absl::Status>(
        absl::InvalidArgumentError("timeout must not be negative"));
  }
  a11::Task dispatched;
  {
    thread::MutexLock lock(&mu_);
    if (mode_ != Mode::kCall) {
      return a11::FailedFuture<absl::Status>(absl::FailedPreconditionError(
          "Only a called Action has a dispatch status"));
    }
    dispatched = dispatch_future_;
  }

  std::shared_ptr<Action> self = shared_from_this();

  // Inline once the dispatch has landed, as in Wait(): a peer that has already
  // accepted the call needs no fibre to report it.
  return a11::ThenAfterWaiting(
      std::move(dispatched), TimeoutDeadline(timeout),
      [self = std::move(self)](const absl::StatusOr<a11::Unit>& ready)
          -> absl::StatusOr<absl::Status> {
        absl::StatusOr<absl::Status> result;
        if (!ready.ok()) {
          result.AssignStatus(ready.status());
          return result;
        }
        thread::MutexLock lock(&self->mu_);
        if (!self->dispatch_status_.has_value()) {
          result.AssignStatus(
              absl::InternalError("Dispatch completed without a status"));
          return result;
        }
        if (!self->dispatch_status_->ok()) {
          result.AssignStatus(*self->dispatch_status_);
          return result;
        }
        return absl::StatusOr<absl::Status>(std::in_place,
                                            *self->dispatch_status_);
      });
}

a11::Future<std::shared_ptr<Action>> Action::Wait(absl::Duration timeout) {
  if (timeout < absl::ZeroDuration()) {
    return a11::FailedFuture<std::shared_ptr<Action>>(
        absl::InvalidArgumentError("timeout must not be negative"));
  }
  a11::Task done;
  {
    thread::MutexLock lock(&mu_);
    if (mode_ == Mode::kNone) {
      return a11::FailedFuture<std::shared_ptr<Action>>(
          absl::FailedPreconditionError("Action has not been run or called"));
    }
    done = done_future_;
  }
  std::shared_ptr<Action> self = shared_from_this();

  // Answered on this thread when the Action has already finished, and by a
  // fibre only when there is really something to wait for. Callers ask after
  // the fact all the time -- `await action.wait()` at the end of a handler, a
  // registry sweeping finished work -- and for those a scheduler hop plus an
  // event-loop turn would report what this frame can already see.
  return a11::ThenAfterWaiting(
      std::move(done), TimeoutDeadline(timeout),
      [self = std::move(self)](const absl::StatusOr<a11::Unit>& finished)
          -> absl::StatusOr<std::shared_ptr<Action>> {
        ABSL_RETURN_IF_ERROR(finished.status());
        ABSL_RETURN_IF_ERROR(self->GetStatus());
        return self;
      });
}

absl::Status Action::Cancel() {
  std::vector<OnActionCancelled> callbacks;
  std::vector<std::shared_ptr<Action>> children;
  Mode mode;
  a11::Task task;
  {
    thread::MutexLock lock(&mu_);
    if (completion_status_.has_value() || finishing_ || cancel_requested_) {
      return absl::OkStatus();
    }
    cancel_requested_ = true;
    callbacks = cancel_callbacks_;
    children.assign(children_.begin(), children_.end());
    mode = mode_;
    task = task_;
  }
  absl::Status first;
  for (auto& callback : callbacks) {
    KeepFirstError(callback(shared_from_this()), &first);
  }
  for (const auto& child : children) {
    KeepFirstError(child->Cancel(), &first);
  }

  if (mode == Mode::kCall) {
    KeepFirstError(SendRemoteCancel(), &first);
    absl::Status cancelled = CancelledStatus();
    CompleteCall(cancelled, false);
    std::shared_ptr<Action> self = shared_from_this();
    a11::Schedule([self, cancelled = std::move(cancelled)]() mutable {
      self->AbortLocalCallOutputs(cancelled);
    });
  } else if (mode == Mode::kRun) {
    if (task.valid()) {
      KeepFirstError(task.Cancel(), &first);
    }
    StartFinish(CancelledStatus());
  } else if (mode == Mode::kNone) {
    std::shared_ptr<a11::Promise<a11::Unit>> promise;
    {
      thread::MutexLock lock(&mu_);
      mode_ = Mode::kCancelled;
      completion_status_ = CancelledStatus();
      promise = done_promise_;
    }
    promise->SetValue(a11::Unit{}).IgnoreError();
  }
  return first;
}

absl::Status Action::SetOnCancelled(OnActionCancelled callback) {
  if (!callback) {
    return absl::InvalidArgumentError("callback must be callable");
  }
  thread::MutexLock lock(&mu_);
  cancel_callbacks_.push_back(internal::GuardOnCancelled(std::move(callback)));
  return absl::OkStatus();
}

bool Action::IsDone() const {
  thread::MutexLock lock(&mu_);
  return completion_status_.has_value();
}

bool Action::HasBeenRun() const {
  thread::MutexLock lock(&mu_);
  return mode_ == Mode::kRun;
}

bool Action::HasBeenCalled() const {
  thread::MutexLock lock(&mu_);
  return mode_ == Mode::kCall;
}

bool Action::Cancelled() const {
  thread::MutexLock lock(&mu_);
  return cancel_requested_ ||
         (completion_status_.has_value() &&
          completion_status_->code() == absl::StatusCode::kCancelled);
}

absl::Status Action::GetStatus() const {
  thread::MutexLock lock(&mu_);
  return completion_status_.value_or(absl::OkStatus());
}

std::optional<absl::Status> Action::GetDispatchStatus() const {
  thread::MutexLock lock(&mu_);
  if (!dispatch_status_.has_value()) {
    return std::nullopt;
  }
  return *dispatch_status_;
}

absl::Status Action::Begin(Mode mode) {
  thread::MutexLock lock(&mu_);
  if (cancel_requested_) {
    return CancelledStatus();
  }
  if (mode_ != Mode::kNone) {
    return absl::FailedPreconditionError("Action has already started");
  }
  mode_ = mode;
  return absl::OkStatus();
}

obs::Span Action::MakeChildSpan(std::string_view name, obs::SpanKind kind) {
  thread::MutexLock lock(&mu_);
  return obs::Tracer::StartChildSpan(name, kind, span_);
}

absl::Status Action::StartActionSpan(Mode mode) {
  std::shared_ptr<Action> parent;
  data::ByteMap headers;
  std::string name;
  {
    thread::MutexLock lock(&mu_);
    if (span_.IsRecording()) {
      return absl::OkStatus();
    }
    parent = parent_.lock();
    headers = headers_;
    name = schema_.name;
  }
  // Calls are client spans, nested runs are internal spans, and root runs are
  // server spans.
  const obs::SpanKind kind = mode == Mode::kCall ? obs::SpanKind::kClient
                             : parent != nullptr ? obs::SpanKind::kInternal
                                                 : obs::SpanKind::kServer;
  const std::string id = GetId();

  obs::Span span;
  if (parent != nullptr) {
    // Continue an in-process parent's live span without consulting headers.
    span = parent->MakeChildSpan(name, kind);
  } else {
    absl::StatusOr<std::optional<obs::TraceContext>> context =
        obs::ExtractTraceContext(headers);
    if (!context.ok()) {
      // Reserved OTel headers are present but inconsistent -> fail the action.
      return context.status();
    }
    if (!context->has_value()) {
      // No trace context -> emit no telemetry.
      return absl::OkStatus();
    }
    span = obs::Tracer::StartSpan(name, kind, &context->value());
  }

  if (span.IsRecording()) {
    span.SetAttribute("a11.action.name", name);
    span.SetAttribute("a11.action.id", id);
    span.SetAttribute("a11.action.mode",
                      (mode == Mode::kCall) ? "call" : "run");
  }
  {
    thread::MutexLock lock(&mu_);
    span_ = std::move(span);
  }
  // A nested Call is a call the parent made: record it as an event on the
  // caller's span too (requirement: action calls are registered as events).
  if (mode == Mode::kCall && parent != nullptr) {
    parent->RecordActionCallEvent(name, id);
  }
  return absl::OkStatus();
}

void Action::RecordActionCallEvent(std::string_view name, std::string_view id) {
  thread::MutexLock lock(&mu_);
  span_.AddEvent("a11.action.call", {{"a11.action.name", std::string(name)},
                                     {"a11.action.id", std::string(id)}});
}

std::string Action::TraceId() const {
  thread::MutexLock lock(&mu_);
  return span_.TraceIdHex();
}

std::string Action::SpanId() const {
  thread::MutexLock lock(&mu_);
  return span_.SpanIdHex();
}

void Action::SetSpanAttribute(std::string_view key, std::string_view value) {
  thread::MutexLock lock(&mu_);
  span_.SetAttribute(key, value);
}

void Action::SetSpanAttribute(std::string_view key, std::int64_t value) {
  thread::MutexLock lock(&mu_);
  span_.SetAttribute(key, value);
}

void Action::SetSpanAttribute(std::string_view key, bool value) {
  thread::MutexLock lock(&mu_);
  span_.SetAttribute(key, value);
}

void Action::SetSpanAttribute(std::string_view key, double value) {
  thread::MutexLock lock(&mu_);
  span_.SetAttribute(key, value);
}

void Action::SetSpanName(std::string_view name) {
  thread::MutexLock lock(&mu_);
  span_.UpdateName(name);
}

void Action::SetSpanStatus(obs::SpanStatus status,
                           std::string_view description) {
  thread::MutexLock lock(&mu_);
  span_.SetStatus(status, description);
  span_status_set_by_user_ = true;
}

void Action::EndActionSpan(const absl::Status& status) {
  obs::Span span;
  {
    thread::MutexLock lock(&mu_);
    span = std::move(span_);
  }
  if (span.IsRecording()) {
    span.SetStatus(status);
    span.End();
  }
}

absl::Status Action::RemapDefaultPorts() {
  input_ids_.clear();
  output_ids_.clear();
  for (const auto& name : schema_.inputs | std::views::keys) {
    ABSL_ASSIGN_OR_RETURN(std::string id, MakeNodeId(id_, name));
    input_ids_.emplace(name, std::move(id));
  }
  for (const auto& name : schema_.outputs | std::views::keys) {
    ABSL_ASSIGN_OR_RETURN(std::string id, MakeNodeId(id_, name));
    output_ids_.emplace(name, std::move(id));
  }
  for (const std::string_view name :
       {kActionStatusOutput, kActionDispatchStatusOutput, kActionLogOutput}) {
    ABSL_ASSIGN_OR_RETURN(std::string id, MakeNodeId(id_, name));
    output_ids_.emplace(std::string(name), std::move(id));
  }
  return absl::OkStatus();
}

absl::Status Action::AttachStreamIfRequested(
    const std::shared_ptr<nodes::AsyncNode>& node, bool bind) {
  if (!bind) {
    return absl::OkStatus();
  }
  std::shared_ptr<net::WireStream> stream;
  {
    thread::MutexLock lock(&mu_);
    stream = stream_;
  }
  if (stream == nullptr) {
    return absl::OkStatus();
  }
  absl::Status status = node->AttachStream(stream);
  if (!status.ok()) {
    return status;
  }
  thread::MutexLock lock(&mu_);
  stream_bound_nodes_.insert(node);
  return absl::OkStatus();
}

absl::Status Action::ValidateMessagePorts(
    const std::vector<data::Port>& ports,
    const absl::flat_hash_map<std::string, ActionPortSchema>& schema_ports,
    std::string_view kind) const {
  absl::flat_hash_set<std::string> seen;
  for (const data::Port& port : ports) {
    ABSL_RETURN_IF_ERROR(port.Validate());
    if (schema_ports.find(port.name) == schema_ports.end()) {
      return absl::FailedPreconditionError(
          absl::StrCat("Unknown Action ", kind, " port '", port.name, "'"));
    }
    if (!seen.insert(port.name).second) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Action ", kind, " port '", port.name, "' is duplicated"));
    }
  }
  return absl::OkStatus();
}

void Action::RunHandler(const std::shared_ptr<ActionLimiter>& limiter) {
  absl::Status status;
  bool acquired = false;
  if (limiter != nullptr) {
    status = limiter->Acquire();
    acquired = status.ok();
  }
  if (status.ok()) {
    ActionHandler handler;
    bool cancelled = false;
    {
      thread::MutexLock lock(&mu_);
      handler = handler_;
      cancelled = cancel_requested_;
    }
    if (cancelled || thread::Cancelled()) {
      status = CancelledStatus();
    } else if (!handler) {
      status = absl::FailedPreconditionError("Action handler has not been set");
    } else if (absl::Status autofill = ApplyInputAutofills(); !autofill.ok()) {
      status = std::move(autofill);
    } else {
      const a11::Task handler_task = handler(shared_from_this());
      status = handler_task.Await().status();
      if (thread::Cancelled()) {
        // Await cancellation stops this fiber, but the awaited future may be
        // backed by an asyncio Task on another thread. Propagate explicitly
        // so Python handlers run their cancellation/finally cleanup too.
        handler_task.Cancel().IgnoreError();
        status = CancelledStatus();
      }
    }
  }
  if (acquired) {
    limiter->Release();
  }
  if (status.code() == absl::StatusCode::kCancelled) {
    status = CancelledStatus();
  }
  StartFinish(std::move(status));
}

absl::Status Action::ApplyInputAutofills() {
  std::vector<
      std::pair<std::string, std::vector<std::optional<data::NodeFragment>>>>
      work;
  std::shared_ptr<nodes::NodeMap> node_map;
  {
    thread::MutexLock lock(&mu_);
    if (input_autofills_applied_) {
      return absl::OkStatus();
    }
    node_map = node_map_;
    for (const auto& [name, port] : schema_.inputs) {
      if (port.autofills.empty()) {
        continue;
      }
      const auto found = input_ids_.find(name);
      if (found == input_ids_.end()) {
        continue;
      }
      work.emplace_back(found->second, port.autofills);
    }
  }
  if (work.empty()) {
    thread::MutexLock lock(&mu_);
    input_autofills_applied_ = true;
    return absl::OkStatus();
  }
  if (node_map == nullptr) {
    return absl::FailedPreconditionError(
        "Action has no NodeMap to apply input autofills");
  }

  // First pass: every autofilled input must be empty and writable before it is
  // filled, so a peer cannot smuggle data into a receiver-autofilled input
  // ahead of (or racing) the ActionMessage that authorizes it.
  // The writability and emptiness of every autofilled input is asked at once: the
  // checks are independent, and the first pass used to pay two pool handoffs per
  // node in series before a single byte was written.
  std::vector<std::shared_ptr<nodes::AsyncNode>> nodes;
  nodes.reserve(work.size());
  std::vector<a11::Future<bool>> checks;
  checks.reserve(work.size());
  std::vector<a11::Future<size_t>> sizes;
  sizes.reserve(work.size());
  for (const auto& [node_id, autofills] : work) {
    ABSL_ASSIGN_OR_RETURN(std::shared_ptr<nodes::AsyncNode> node,
                          node_map->Get(node_id));
    checks.push_back(node->IsWritable());
    sizes.push_back(node->GetChunkStore()->Size());
    nodes.push_back(std::move(node));
  }
  const std::vector<absl::StatusOr<bool>> writable = a11::AwaitAll(checks);
  const std::vector<absl::StatusOr<size_t>> sizes_now = a11::AwaitAll(sizes);
  for (size_t index = 0; index < work.size(); ++index) {
    const std::string& node_id = work[index].first;
    ABSL_RETURN_IF_ERROR(writable[index].status());
    if (!*writable[index]) {
      return absl::FailedPreconditionError(
          absl::StrCat("Autofilled input '", node_id, "' is not writable"));
    }
    ABSL_RETURN_IF_ERROR(sizes_now[index].status());
    if (*sizes_now[index] != 0) {
      return absl::FailedPreconditionError(absl::StrCat(
          "Autofilled input '", node_id, "' already contains data"));
    }
  }
  {
    thread::MutexLock lock(&mu_);
    for (const std::shared_ptr<nodes::AsyncNode>& node : nodes) {
      input_nodes_.insert(node);
    }
  }

  // Second pass: write and close each autofilled input. One fibre per node, and
  // the writes *within* a node stay in this order because their sequence numbers
  // are assigned by the order they are put -- that is the one thing here that is
  // not independent, so it is the one thing that stays serial.
  std::vector<absl::AnyInvocable<absl::Status() &&>> fills;
  fills.reserve(work.size());
  for (size_t index = 0; index < work.size(); ++index) {
    fills.emplace_back([node = nodes[index],
                        entry = &work[index]]() -> absl::Status {
      for (const std::optional<data::NodeFragment>& autofill : entry->second) {
        // A missing fragment is a null final marker, mirroring Finalize().
        if (!autofill.has_value()) {
          ABSL_RETURN_IF_ERROR(
              node->Finalize({.wait = true, .close = false}).Await().status());
          continue;
        }
        data::NodeFragment fragment = *autofill;
        fragment.id = entry->first;
        ABSL_RETURN_IF_ERROR(
            node->PutFragment(std::move(fragment)).Await().status());
      }
      return node->Close().Await().status();
    });
  }
  ABSL_RETURN_IF_ERROR(a11::RunAllToCompletion(std::move(fills)));
  thread::MutexLock lock(&mu_);
  input_autofills_applied_ = true;
  return absl::OkStatus();
}

std::vector<data::NodeFragment> Action::CollectAutofillFragments() const {
  std::vector<data::NodeFragment> fragments;
  thread::MutexLock lock(&mu_);
  for (const auto& [name, port] : schema_.inputs) {
    if (port.autofills.empty()) {
      continue;
    }
    const auto found = input_ids_.find(name);
    if (found == input_ids_.end()) {
      continue;
    }
    const std::string& node_id = found->second;
    const size_t start = fragments.size();
    for (const std::optional<data::NodeFragment>& autofill : port.autofills) {
      data::NodeFragment fragment;
      if (autofill.has_value()) {
        fragment = *autofill;
      } else {
        // A missing fragment is a null final marker, mirroring Finalize().
        fragment.data = data::Chunk{
            .metadata = data::ChunkMetadata{
                .mimetype = std::string("application/octet-stream")}};
      }
      fragment.id = node_id;
      fragments.push_back(std::move(fragment));
    }
    // The last fragment must close the remote input node, since a called
    // Action cannot drain and close it over the wire.
    if (fragments.size() > start) {
      fragments.back().continued = false;
    }
  }
  return fragments;
}

void Action::StartFinish(absl::Status status) {
  {
    thread::MutexLock lock(&mu_);
    if (finishing_ || completion_status_.has_value()) {
      return;
    }
    finishing_ = true;
  }
  std::shared_ptr<Action> self = shared_from_this();
  a11::Schedule([self, status = std::move(status)]() mutable {
    self->FinishRun(std::move(status)).IgnoreError();
  });
}

absl::Status Action::FinishRun(absl::Status status) {
  absl::Status final_status = std::move(status);
  if (!final_status.ok()) {
    std::vector<std::shared_ptr<Action>> children;
    {
      thread::MutexLock lock(&mu_);
      children.assign(children_.begin(), children_.end());
    }
    // One fibre per child rather than one child at a time: children are separate
    // Actions with separate nodes, and each AbortInputs is itself now two rounds
    // of pool work, so a parent with several children used to serialise all of it.
    std::vector<absl::AnyInvocable<absl::Status() &&>> aborts;
    aborts.reserve(children.size());
    const bool cancelled = final_status.code() == absl::StatusCode::kCancelled;
    for (const auto& child : children) {
      aborts.emplace_back([child, final_status, cancelled]() -> absl::Status {
        child->AbortInputs(final_status).IgnoreError();
        if (cancelled) {
          child->Cancel().IgnoreError();
        }
        return absl::OkStatus();
      });
    }
    a11::RunAllToCompletion(std::move(aborts)).IgnoreError();
    if (final_status.code() == absl::StatusCode::kCancelled) {
      AbortInputs(final_status).IgnoreError();
    }
  }

  if (const absl::Status output_error = FinishOutputNodes(final_status);
      final_status.ok() && !output_error.ok()) {
    final_status = output_error;
    FinishOutputNodes(final_status).IgnoreError();
  }
  if (const absl::Status communicate_error = CommunicateStatus(final_status);
      final_status.ok() && !communicate_error.ok()) {
    final_status = communicate_error;
    FinishOutputNodes(final_status).IgnoreError();
    CommunicateStatus(final_status).IgnoreError();
  }
  ReleaseNodesAfterRun().IgnoreError();

  std::shared_ptr<a11::Promise<a11::Unit>> promise;
  std::shared_ptr<Action> parent;
  obs::Span span;
  bool user_status = false;
  {
    thread::MutexLock lock(&mu_);
    completion_status_ = final_status;
    promise = done_promise_;
    parent = parent_.lock();
    span = std::move(span_);
    user_status = span_status_set_by_user_;
  }
  if (span.IsRecording()) {
    if (!user_status) {
      RecordSpanOutcome(span, final_status);
    }
    span.End();
  }
  promise->SetValue(a11::Unit{}).IgnoreError();
  if (parent != nullptr) {
    thread::MutexLock lock(&parent->mu_);
    parent->children_.erase(shared_from_this());
  }
  UntrackFromSession();
  return absl::OkStatus();
}

absl::Status Action::FinishOutputNodes(const absl::Status& status) {
  absl::flat_hash_map<std::string, std::shared_ptr<nodes::AsyncNode>> nodes;
  absl::flat_hash_set<std::string> ids;
  std::shared_ptr<nodes::NodeMap> node_map;
  std::string log_id;
  bool remote = false;
  {
    thread::MutexLock lock(&mu_);
    node_map = node_map_;
    remote = stream_ != nullptr || !session_.expired();
    // Publishing the terminal state and snapshotting the nodes to close under
    // one hold of mu_ is what makes the lazy path safe. GetOutput() inserts
    // into output_nodes_ and reads this flag under the same lock, so a port
    // materialised concurrently with finishing is either in the snapshot below
    // and closed here, or sees the flag and closes itself. A gap between the
    // two would leave a node nothing ever closes, and a reader of it waiting
    // forever.
    outputs_finished_ = true;
    outputs_final_status_ = status;
    for (const auto& [name, id] : output_ids_) {
      if (name != kActionStatusOutput && name != kActionDispatchStatusOutput) {
        ids.insert(id);
      }
      if (name == kActionLogOutput) {
        log_id = id;
      }
    }
    for (const auto& node : output_nodes_) {
      absl::StatusOr<std::string> id = node->GetId();
      if (id.ok() && ids.find(*id) != ids.end()) {
        nodes.emplace(*id, node);
      }
    }
  }
  absl::Status first;
  if (node_map != nullptr) {
    for (const std::string& id : ids) {
      // An output the handler never touched has no node yet, and creating one
      // to immediately close it costs a node, its reader and its writer per
      // unused port -- the dominant cost of a wide schema, where a caller
      // typically reads one output of eight. With a peer attached the closure
      // is not local bookkeeping but an end-of-stream marker it is waiting
      // for, so those are still materialised; otherwise the node is left
      // uncreated and GetOutput() closes it on the way out if anybody asks
      // for it later.
      absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> node =
          remote ? node_map->Get(id) : node_map->GetIfExists(id);
      if (!node.ok()) {
        KeepFirstError(node.status(), &first);
      } else if (*node != nullptr) {
        nodes.insert_or_assign(id, *node);
      }
    }
  }
  // A graceful close only tees its end-of-stream marker to streams something
  // attached, and nothing attaches the log port unless the handler logged. A
  // caller that opted into reading logs would then wait forever on a silent
  // action, so the attach happens here, where the node exists anyway. Only the
  // log port: it is the one output a caller reads without the schema promising
  // anything will be written to it.
  if (remote && !log_id.empty()) {
    const auto found = nodes.find(log_id);
    if (found != nodes.end()) {
      KeepFirstError(AttachStreamIfRequested(found->second, /*bind=*/true),
                     &first);
    }
  }
  if (!status.ok()) {
    KeepFirstError(SendNodeAbortStatuses(ids, status), &first);
  }
  // Two rounds rather than two handoffs per node. Every output node's writability
  // is asked at once, then every close or abort the answers call for is started at
  // once -- the nodes are independent of each other, so a wide schema used to pay
  // 2N pool handoffs in series where it now pays 2. See a11/concurrency/parallel.h.
  std::vector<std::shared_ptr<nodes::AsyncNode>> ordered;
  ordered.reserve(nodes.size());
  std::vector<a11::Future<bool>> checks;
  checks.reserve(nodes.size());
  for (const auto& node : nodes | std::views::values) {
    ordered.push_back(node);
    checks.push_back(node->IsWritable());
  }
  const std::vector<absl::StatusOr<bool>> writable = a11::AwaitAll(checks);

  std::vector<a11::Task> closes;
  closes.reserve(ordered.size());
  for (size_t index = 0; index < ordered.size(); ++index) {
    if (!writable[index].ok()) {
      KeepFirstError(writable[index].status(), &first);
      continue;
    }
    const std::shared_ptr<nodes::AsyncNode>& node = ordered[index];
    const absl::Status writer_status = node->GetWriterStatus();
    if (status.ok() && *writable[index]) {
      closes.push_back(node->Close());
    } else if (!status.ok() && (*writable[index] || !writer_status.ok())) {
      // A graceful close can fail while leaving the backing store open. The
      // failure pass must retry that writer with the Action's terminal status
      // even though it is no longer writable for ordinary puts.
      closes.push_back(node->AbortWithStatus(status));
    }
  }
  for (const absl::StatusOr<a11::Unit>& closed : a11::AwaitAll(closes)) {
    KeepFirstError(closed.status(), &first);
  }
  return first;
}

absl::Status Action::CommunicateStatus(const absl::Status& status) {
  absl::StatusOr<data::Chunk> chunk = StatusToChunk(status);
  if (!chunk.ok()) {
    return chunk.status();
  }
  std::string node_id;
  std::shared_ptr<nodes::NodeMap> node_map;
  std::shared_ptr<net::WireStream> stream;
  {
    thread::MutexLock lock(&mu_);
    node_id = output_ids_.at(std::string(kActionStatusOutput));
    node_map = node_map_;
    stream = stream_;
  }
  data::NodeFragment fragment{
      .id = node_id, .data = std::move(*chunk), .seq = 0, .continued = false};
  if (node_map == nullptr) {
    return stream != nullptr ? stream->Send(data::WireMessage{
                                   .node_fragments = {std::move(fragment)}})
                             : absl::OkStatus();
  }
  absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> node =
      node_map->Get(node_id);
  if (!node.ok()) {
    return node.status();
  }
  absl::StatusOr<bool> writable = (*node)->IsWritable().Await();
  if (!writable.ok()) {
    return writable.status();
  }
  if (!*writable) {
    return absl::FailedPreconditionError(
        "Action status node was already finalized");
  }
  if (stream != nullptr) {
    absl::Status attached = (*node)->AttachStream(stream);
    if (!attached.ok()) {
      return attached;
    }
    thread::MutexLock lock(&mu_);
    stream_bound_nodes_.insert(*node);
  }
  absl::StatusOr<std::uint32_t> stored =
      (*node)->PutFragment(std::move(fragment)).Await();
  if (!stored.ok()) {
    return stored.status();
  }
  return (*node)->Close().Await().status();
}

absl::Status Action::AbortInputs(const absl::Status& status) {
  absl::flat_hash_set<std::string> ids;
  absl::flat_hash_set<std::shared_ptr<nodes::AsyncNode>> nodes;
  std::shared_ptr<nodes::NodeMap> node_map;
  {
    thread::MutexLock lock(&mu_);
    node_map = node_map_;
    for (const auto& id : input_ids_ | std::views::values) {
      ids.insert(id);
    }
    nodes = input_nodes_;
  }
  absl::Status first;
  if (node_map != nullptr) {
    for (const std::string& id : ids) {
      absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> node =
          node_map->Get(id);
      if (node.ok()) {
        nodes.insert(*node);
      } else {
        KeepFirstError(node.status(), &first);
      }
    }
  }
  KeepFirstError(SendNodeAbortStatuses(ids, status), &first);
  // Same two rounds as CloseUnwrittenOutputs, and for the same reason: the input
  // nodes do not depend on each other.
  std::vector<std::shared_ptr<nodes::AsyncNode>> ordered(nodes.begin(),
                                                         nodes.end());
  std::vector<a11::Future<bool>> checks;
  checks.reserve(ordered.size());
  for (const auto& node : ordered) {
    checks.push_back(node->IsWritable());
  }
  const std::vector<absl::StatusOr<bool>> writable = a11::AwaitAll(checks);

  std::vector<a11::Task> aborts;
  aborts.reserve(ordered.size());
  for (size_t index = 0; index < ordered.size(); ++index) {
    if (!writable[index].ok()) {
      KeepFirstError(writable[index].status(), &first);
    } else if (*writable[index]) {
      aborts.push_back(ordered[index]->AbortWithStatus(status));
    }
  }
  for (const absl::StatusOr<a11::Unit>& aborted : a11::AwaitAll(aborts)) {
    KeepFirstError(aborted.status(), &first);
  }
  return first;
}

absl::Status Action::SendNodeAbortStatuses(
    const absl::flat_hash_set<std::string>& node_ids,
    const absl::Status& status) {
  const std::shared_ptr<net::WireStream> stream = GetStream();
  if (stream == nullptr || node_ids.empty()) {
    return absl::OkStatus();
  }

  absl::StatusOr<data::Chunk> chunk = StatusToChunk(status);
  if (!chunk.ok()) {
    return chunk.status();
  }
  data::WireMessage message;
  message.node_fragments.reserve(node_ids.size());
  for (const std::string& id : node_ids) {
    message.node_fragments.push_back(data::NodeFragment{
        .id = id, .data = *chunk, .seq = 0, .continued = false});
  }

  return stream->Send(std::move(message));
}

absl::Status Action::ReleaseNodesAfterRun() {
  absl::Status first = DetachBoundStreamNodes();
  std::shared_ptr<nodes::NodeMap> node_map;
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  bool clear_inputs = false;
  bool clear_outputs = false;
  {
    thread::MutexLock lock(&mu_);
    node_map = node_map_;
    clear_inputs = settings_.clear_inputs_after_run;
    clear_outputs = settings_.clear_outputs_after_run;
    for (const auto& id : input_ids_ | std::views::values) {
      inputs.push_back(id);
    }
    for (const auto& id : output_ids_ | std::views::values) {
      outputs.push_back(id);
    }
    input_nodes_.clear();
    output_nodes_.clear();
  }
  if (node_map != nullptr && clear_inputs) {
    for (const std::string& id : inputs) {
      absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> removed =
          node_map->Discard(id);
      if (!removed.ok()) {
        KeepFirstError(removed.status(), &first);
      } else if (*removed) {
        (*removed)->CancelReader();
      }
    }
  }
  if (node_map != nullptr && clear_outputs) {
    for (const std::string& id : outputs) {
      absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> removed =
          node_map->Discard(id);
      if (!removed.ok()) {
        KeepFirstError(removed.status(), &first);
      }
    }
  }
  return first;
}

absl::Status Action::DetachBoundStreamNodes() {
  std::shared_ptr<net::WireStream> stream;
  std::vector<std::shared_ptr<nodes::AsyncNode>> nodes;
  {
    thread::MutexLock lock(&mu_);
    stream = stream_;
    nodes.assign(stream_bound_nodes_.begin(), stream_bound_nodes_.end());
    stream_bound_nodes_.clear();
  }
  if (stream == nullptr) {
    return absl::OkStatus();
  }
  absl::Status first;
  for (const auto& node : nodes) {
    KeepFirstError(node->DetachStream(stream), &first);
  }
  return first;
}

absl::Status Action::SendRemoteCancel() {
  data::ActionMessage cancel{.id = NewActionId(),
                             .name = std::string(kCancelActionName)};
  cancel.headers.emplace(std::string(kCancelActionHeader), GetId());
  data::WireMessage message{.actions = {std::move(cancel)}};
  std::shared_ptr<net::WireStream> stream = GetStream();
  if (stream != nullptr) {
    return stream->Send(std::move(message));
  }
  std::shared_ptr<service::Session> session = GetSession();
  if (session != nullptr) {
    return session->Send(std::move(message));
  }
  return absl::FailedPreconditionError(
      "Cancelling a called Action requires a WireStream or Session");
}

void Action::CompleteCall(const absl::Status& status,
                          bool remove_from_session) {
  std::shared_ptr<a11::Promise<a11::Unit>> promise;
  bool completed = false;
  obs::Span span;
  bool user_status = false;
  {
    thread::MutexLock lock(&mu_);
    if (!completion_status_.has_value()) {
      completion_status_ = status;
      promise = done_promise_;
      completed = true;
      span = std::move(span_);
      user_status = span_status_set_by_user_;
    }
  }
  if (completed) {
    if (span.IsRecording()) {
      if (!user_status) {
        RecordSpanOutcome(span, status);
      }
      span.End();
    }
    DetachBoundStreamNodes().IgnoreError();
    promise->SetValue(a11::Unit{}).IgnoreError();
  }
  if (remove_from_session) {
    UntrackFromSession();
  }
}

void Action::AbortLocalCallOutputs(const absl::Status& status) {
  absl::flat_hash_set<std::string> ids;
  std::shared_ptr<nodes::NodeMap> node_map;
  {
    thread::MutexLock lock(&mu_);
    node_map = node_map_;
    for (const auto& [name, id] : output_ids_) {
      if (name != kActionStatusOutput && name != kActionDispatchStatusOutput) {
        ids.insert(id);
      }
    }
  }
  if (node_map == nullptr) {
    return;
  }
  std::vector<std::shared_ptr<nodes::AsyncNode>> ordered;
  ordered.reserve(ids.size());
  std::vector<a11::Future<bool>> checks;
  checks.reserve(ids.size());
  for (const std::string& id : ids) {
    absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> node = node_map->Get(id);
    if (!node.ok()) {
      continue;
    }
    ordered.push_back(*node);
    checks.push_back((*node)->IsWritable());
  }
  const std::vector<absl::StatusOr<bool>> writable = a11::AwaitAll(checks);

  std::vector<a11::Task> aborts;
  aborts.reserve(ordered.size());
  for (size_t index = 0; index < ordered.size(); ++index) {
    if (writable[index].ok() && *writable[index]) {
      // A copy per node, not a move: moving the status into the first abort left
      // every node after it being aborted with a moved-from (OK) status, which
      // silently turned an abort into a graceful close for every output but one.
      aborts.push_back(ordered[index]->AbortWithStatus(status));
    }
  }
  for (const absl::StatusOr<a11::Unit>& aborted : a11::AwaitAll(aborts)) {
    aborted.status().IgnoreError();
  }
}

absl::Status Action::TrackInSession(
    const std::shared_ptr<service::Session>& session) {
  {
    thread::MutexLock lock(&mu_);
    if (tracked_session_.lock() == session) {
      return absl::OkStatus();
    }
  }
  absl::Status status = session->TrackAction(shared_from_this());
  if (!status.ok()) {
    return status;
  }
  std::shared_ptr<service::Session> previous;
  {
    thread::MutexLock lock(&mu_);
    previous = tracked_session_.lock();
    tracked_session_ = session;
  }
  if (previous != nullptr && previous != session) {
    previous->UntrackAction(shared_from_this());
  }
  return absl::OkStatus();
}

void Action::UntrackFromSession() {
  std::shared_ptr<service::Session> session;
  {
    thread::MutexLock lock(&mu_);
    session = tracked_session_.lock();
    tracked_session_.reset();
  }
  if (session != nullptr) {
    session->UntrackAction(shared_from_this());
  }
}

void Action::SetDispatchStatus(absl::Status status) {
  std::shared_ptr<a11::Promise<a11::Unit>> promise;
  {
    thread::MutexLock lock(&mu_);
    if (mode_ != Mode::kCall || dispatch_status_.has_value()) {
      return;
    }
    dispatch_status_ = std::move(status);
    promise = dispatch_promise_;
  }
  promise->SetValue(a11::Unit{}).IgnoreError();
}

void Action::SetCompletionStatus(const absl::Status& status) {
  std::shared_ptr<a11::Promise<a11::Unit>> dispatch;
  bool late_after_cancellation = false;
  bool should_complete = false;
  {
    thread::MutexLock lock(&mu_);
    if (mode_ != Mode::kCall) {
      return;
    }
    if (!dispatch_status_.has_value()) {
      // A completion that arrives with nothing yet said about the dispatch
      // answers for the dispatch too, and it must not answer OK for a failure.
      // When a peer refuses a dispatch it sends the same status to both the
      // dispatch-status and the status node in one WireMessage, and the two
      // node writes race: filling OK here lost the real refusal about half the
      // time (see the test named for it). Adopting the completion is also the
      // honest answer when a stream dies before any dispatch was acknowledged,
      // and it can never contradict a dispatch fragment that lands later --
      // that fragment carries this very status.
      dispatch_status_ = status.ok() ? absl::OkStatus() : status;
      dispatch = dispatch_promise_;
    }
    if (completion_status_.has_value()) {
      late_after_cancellation = cancel_requested_;
      if (!late_after_cancellation) {
        return;
      }
    } else {
      should_complete = true;
    }
  }
  if (dispatch != nullptr) {
    dispatch->SetValue(a11::Unit{}).IgnoreError();
  }
  if (late_after_cancellation) {
    UntrackFromSession();
  } else if (should_complete) {
    CompleteCall(status, true);
  }
}

}  // namespace a11::actions
