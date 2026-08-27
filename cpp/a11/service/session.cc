// Copyright 2026 The A11 Authors.

#include "a11/service/session.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/base/thread_annotations.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/functional/any_invocable.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/concurrency/parallel.h"
#include "a11/data/msgpack.h"
#include "a11/data/types.h"
#include "a11/net/wire_stream.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/obs/span.h"
#include "a11/obs/tracer.h"
#include "a11/service/internal/exception_guarded_callbacks.h"
#include "a11/status.h"
#include "a11/uuid.h"
#include "thread/boost_primitives.h"
#include "thread/fiber.h"
#include "thread/select.h"
#include "thread/selectables.h"

namespace a11::service {
namespace {

enum class Phase { kOpen, kClosing, kAborted };

std::string NewSessionId() {
  return NewStreamId();
}

absl::Status SessionStreamAbortStatus() {
  return absl::AbortedError("Session has aborted its streams");
}

bool IsSessionStreamAbortStatus(const absl::Status& status) {
  const absl::Status expected = SessionStreamAbortStatus();
  return status.code() == expected.code() &&
         status.message() == expected.message();
}

std::optional<std::pair<std::string, std::string>> ActionSpecialNode(
    std::string_view node_id) {
  for (const std::string_view name :
       {actions::kActionDispatchStatusOutput, actions::kActionStatusOutput}) {
    const std::string suffix = absl::StrCat("#", name);
    if (node_id.size() > suffix.size() && node_id.ends_with(suffix)) {
      return std::pair(
          std::string(node_id.substr(0, node_id.size() - suffix.size())),
          std::string(name));
    }
  }
  return std::nullopt;
}

void KeepFirstError(absl::Status candidate, absl::Status* first) {
  if (first->ok() && !candidate.ok()) {
    *first = std::move(candidate);
  }
}

}  // namespace

struct Session::StreamState {
  StreamState(std::shared_ptr<net::WireStream> value, std::string value_id,
              std::shared_ptr<actions::ActionLimiter> gate)
      : stream(std::move(value)),
        id(std::move(value_id)),
        action_gate(std::move(gate)) {}

  // StreamState instances are owned by State and accessed only while the
  // owning State::mutex is held. The external guard cannot be expressed as a
  // member thread annotation without adding a second lock per stream.
  std::shared_ptr<net::WireStream> stream;
  std::string id;
  std::shared_ptr<actions::ActionLimiter> action_gate;
  size_t outstanding_messages = 0;
  size_t outstanding_bytes = 0;
  std::deque<std::pair<data::WireMessage, size_t>> pending_messages;
  bool message_pump_running = false;
  std::function<void()> message_pump_cancel;
  bool accepting_messages = true;
  bool remote_half_closed = false;
  bool half_close_delivered = false;
  bool done_started = false;
  bool done = false;
};

struct Session::State {
  State(std::string value_id, data::ByteMap value_headers,
        SessionOptions value_options, std::shared_ptr<nodes::NodeMap> nodes,
        std::shared_ptr<actions::ActionRegistry> registry,
        std::shared_ptr<actions::ActionLimiter> root,
        std::shared_ptr<actions::ActionLimiter> nested,
        OnSessionStreamMessage message_callback,
        OnSessionStreamDone done_callback)
      : id(std::move(value_id)),
        headers(std::move(value_headers)),
        options(value_options),
        node_map(std::move(nodes)),
        action_registry(std::move(registry)),
        // Guarded on the way in, so every invocation below runs on an A11 fibre
        // without a try of its own. See
        // service/internal/exception_guarded_callbacks.h.
        on_message(internal::GuardOnStreamMessage(std::move(message_callback))),
        on_done(internal::GuardOnStreamDone(std::move(done_callback))),
        root_limiter(std::move(root)),
        nested_limiter(std::move(nested)),
        deadline(value_options.deadline),
        no_stream_since(absl::Now()),
        done_promise(std::make_shared<a11::Promise<a11::Unit>>()),
        done_future(done_promise->future()),
        changed(std::make_shared<thread::PermanentEvent>()) {}

  ~State() { changed->Notify(); }

  mutable thread::Mutex mu;
  const std::string id;
  const data::ByteMap headers;
  const SessionOptions options;
  std::shared_ptr<nodes::NodeMap> node_map ABSL_GUARDED_BY(mu);
  std::shared_ptr<actions::ActionRegistry> action_registry ABSL_GUARDED_BY(mu);
  const OnSessionStreamMessage on_message;
  const OnSessionStreamDone on_done;
  const std::shared_ptr<actions::ActionLimiter> root_limiter;
  const std::shared_ptr<actions::ActionLimiter> nested_limiter;

  Phase phase ABSL_GUARDED_BY(mu) = Phase::kOpen;
  absl::Status status ABSL_GUARDED_BY(mu);
  bool remote_closed ABSL_GUARDED_BY(mu) = false;
  bool destroyed ABSL_GUARDED_BY(mu) = false;
  absl::Time deadline ABSL_GUARDED_BY(mu);
  std::optional<absl::Time> no_stream_since ABSL_GUARDED_BY(mu);

  absl::flat_hash_map<std::string, std::shared_ptr<StreamState>> streams
      ABSL_GUARDED_BY(mu);
  absl::flat_hash_map<net::WireStream*, std::shared_ptr<StreamState>>
      stream_states ABSL_GUARDED_BY(mu);
  std::vector<net::WireStream*> stream_order ABSL_GUARDED_BY(mu);
  size_t round_robin_index ABSL_GUARDED_BY(mu) = 0;
  size_t buffered_messages ABSL_GUARDED_BY(mu) = 0;
  size_t buffered_bytes ABSL_GUARDED_BY(mu) = 0;
  absl::flat_hash_map<std::string, std::shared_ptr<actions::Action>> actions
      ABSL_GUARDED_BY(mu);

  const std::shared_ptr<a11::Promise<a11::Unit>> done_promise;
  const a11::Task done_future;
  std::shared_ptr<thread::PermanentEvent> changed ABSL_GUARDED_BY(mu);

  // Span covering the session lifetime. Each session is its own trace, pinned
  // (when possible) to the session id so external systems can correlate.
  obs::Span span ABSL_GUARDED_BY(mu);
};

absl::Status SessionOptions::Validate() const {
  if (max_buffered_messages_total == 0 || max_buffered_messages_total > 1024 ||
      max_buffered_messages_per_stream == 0 ||
      max_buffered_messages_per_stream > 1024) {
    return absl::InvalidArgumentError(
        "Session message limits must be between 1 and 1024");
  }
  if (max_concurrent_root_actions == 0 || max_concurrent_root_actions > 65536 ||
      max_concurrent_nested_actions == 0 ||
      max_concurrent_nested_actions > 65536) {
    return absl::InvalidArgumentError(
        "Session action limits must be between 1 and 65536");
  }
  if (max_single_message_size < data::EmptyWireMessageSize() ||
      max_single_message_size > kMaxSingleMessageSize) {
    return absl::InvalidArgumentError("Invalid max_single_message_size");
  }
  if (max_buffered_bytes_total < data::EmptyWireMessageSize() ||
      max_buffered_bytes_per_stream < data::EmptyWireMessageSize()) {
    return absl::InvalidArgumentError(
        "Session byte limits are smaller than an empty WireMessage");
  }
  if (no_stream_timeout < absl::ZeroDuration()) {
    return absl::InvalidArgumentError("no_stream_timeout must not be negative");
  }
  return absl::OkStatus();
}

absl::StatusOr<data::ByteMap> NormalizeSessionHeaders(data::ByteMap headers) {
  data::ByteMap result;
  for (auto& [name, value] : headers) {
    ABSL_RETURN_IF_ERROR(data::ValidateName(name));
    result.insert_or_assign(absl::AsciiStrToLower(name), std::move(value));
  }
  return result;
}

absl::StatusOr<std::shared_ptr<Session>> Session::Create(
    std::string session_id, OnSessionStreamMessage on_stream_message,
    OnSessionStreamDone on_stream_done, data::ByteMap headers,
    SessionOptions options, std::shared_ptr<nodes::NodeMap> node_map,
    std::shared_ptr<actions::ActionRegistry> action_registry) {
  struct MakeSharedEnabler final : Session {};

  std::shared_ptr<Session> session = std::make_shared<MakeSharedEnabler>();
  ABSL_RETURN_IF_ERROR(session->Initialize(
      session, std::move(session_id), std::move(on_stream_message),
      std::move(on_stream_done), std::move(headers), options,
      std::move(node_map), std::move(action_registry)));
  return session;
}

absl::Status Session::Initialize(
    const std::shared_ptr<Session>& self, std::string session_id,
    OnSessionStreamMessage on_stream_message,
    OnSessionStreamDone on_stream_done, data::ByteMap headers,
    SessionOptions options, std::shared_ptr<nodes::NodeMap> node_map,
    std::shared_ptr<actions::ActionRegistry> action_registry) {
  if (self == nullptr || self.get() != this) {
    return absl::InvalidArgumentError("Session self ownership is invalid");
  }
  ABSL_RETURN_IF_ERROR(options.Validate());
  ABSL_ASSIGN_OR_RETURN(data::ByteMap normalized,
                        NormalizeSessionHeaders(std::move(headers)));
  if (node_map == nullptr) {
    ABSL_ASSIGN_OR_RETURN(node_map, nodes::NodeMap::Create());
  }
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<actions::ActionLimiter> root,
      actions::ActionLimiter::Create(options.max_concurrent_root_actions));
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<actions::ActionLimiter> nested,
      actions::ActionLimiter::Create(options.max_concurrent_nested_actions));
  if (session_id.empty()) {
    session_id = NewSessionId();
  }
  std::weak_ptr<Session> weak_self = self;
  OnSessionStreamMessage message_callback =
      on_stream_message
          ? std::move(on_stream_message)
          : OnSessionStreamMessage(
                [weak_self](std::optional<data::WireMessage> message,
                            std::shared_ptr<net::WireStream> stream,
                            const std::shared_ptr<Session>&) {
                  std::shared_ptr<Session> locked = weak_self.lock();
                  if (locked == nullptr || !message.has_value()) {
                    return a11::ReadyTask();
                  }
                  return locked->DispatchWireMessage(std::move(*message),
                                                     std::move(stream));
                });
  OnSessionStreamDone done_callback =
      on_stream_done
          ? std::move(on_stream_done)
          : OnSessionStreamDone([](const std::shared_ptr<net::WireStream>&,
                                   const std::shared_ptr<Session>&) {
              return a11::ReadyTask();
            });
  state_ = std::make_shared<State>(
      std::move(session_id), std::move(normalized), options,
      std::move(node_map), std::move(action_registry), std::move(root),
      std::move(nested), std::move(message_callback), std::move(done_callback));

  {
    // Open the session span. Its trace id is pinned to the session id (a 32
    // hex-char value), giving each session its own trace.
    thread::MutexLock lock(&state_->mu);
    state_->span = obs::Tracer::StartRootSpan(
        "a11.session", obs::SpanKind::kServer, state_->id);
    if (state_->span.IsRecording()) {
      state_->span.SetAttribute("a11.session.id", state_->id);
    }
  }

  std::weak_ptr<State> weak_state = state_;
  a11::Schedule([weak_self, weak_state]() mutable {
    while (true) {
      std::shared_ptr<State> state = weak_state.lock();
      std::shared_ptr<Session> session = weak_self.lock();
      if (state == nullptr || session == nullptr) {
        return;
      }
      absl::Time wake = absl::InfiniteFuture();
      bool deadline_due = false;
      bool no_stream_due = false;
      std::shared_ptr<thread::PermanentEvent> changed;
      {
        thread::MutexLock lock(&state->mu);
        if (state->phase != Phase::kOpen) {
          return;
        }
        wake = state->deadline;
        if (state->no_stream_since.has_value() &&
            state->options.no_stream_timeout != absl::InfiniteDuration()) {
          wake = std::min(
              wake, *state->no_stream_since + state->options.no_stream_timeout);
        }
        deadline_due = state->deadline <= absl::Now();
        no_stream_due =
            state->stream_states.empty() &&
            state->no_stream_since.has_value() &&
            state->options.no_stream_timeout != absl::InfiniteDuration() &&
            *state->no_stream_since + state->options.no_stream_timeout <=
                absl::Now();
        changed = state->changed;
      }
      if (deadline_due) {
        session
            ->Abort(absl::DeadlineExceededError(
                "The Session deadline has been exceeded"))
            .IgnoreError();
        return;
      }
      if (no_stream_due) {
        session->HalfClose().IgnoreError();
        return;
      }
      const int selected =
          thread::SelectUntil(wake, {thread::OnCancel(), changed->OnEvent()});
      if (selected == 0) {
        return;
      }
    }
  });
  return absl::OkStatus();
}

void Session::NotifyStateChanged() {
  std::shared_ptr<thread::PermanentEvent> notify;
  {
    thread::MutexLock lock(&state_->mu);
    notify = std::exchange(state_->changed,
                           std::make_shared<thread::PermanentEvent>());
  }
  notify->Notify();
}

absl::StatusOr<
    std::vector<std::pair<std::string, std::shared_ptr<net::WireStream>>>>
Session::Streams() const {
  std::vector<std::pair<std::string, std::shared_ptr<net::WireStream>>> result;
  thread::MutexLock lock(&state_->mu);
  result.reserve(state_->streams.size());
  for (net::WireStream* stream : state_->stream_order) {
    const auto found = state_->stream_states.find(stream);
    if (found != state_->stream_states.end()) {
      result.emplace_back(found->second->id, found->second->stream);
    }
  }
  return result;
}

absl::StatusOr<std::shared_ptr<net::WireStream>> Session::GetStream(
    std::string_view stream_id) const {
  thread::MutexLock lock(&state_->mu);
  const auto found = state_->streams.find(stream_id);
  if (found == state_->streams.end()) {
    return absl::NotFoundError(
        absl::StrCat("Stream ", stream_id, " is not attached to the Session"));
  }
  return found->second->stream;
}

std::string Session::GetId() const {
  thread::MutexLock lock(&state_->mu);
  return state_->id;
}

std::shared_ptr<nodes::NodeMap> Session::GetNodeMap() const {
  thread::MutexLock lock(&state_->mu);
  return state_->node_map;
}

absl::Status Session::SetNodeMap(
    const std::shared_ptr<nodes::NodeMap>& node_map) {
  if (node_map == nullptr) {
    return absl::InvalidArgumentError("node_map must not be null");
  }
  std::vector<std::shared_ptr<actions::Action>> actions;
  {
    thread::MutexLock lock(&state_->mu);
    state_->node_map = node_map;
    for (const auto& [unused, action] : state_->actions) {
      (void)unused;
      actions.push_back(action);
    }
  }
  absl::Status first;
  for (const auto& action : actions) {
    KeepFirstError(action->BindNodeMap(node_map), &first);
  }
  return first;
}

std::shared_ptr<actions::ActionRegistry> Session::GetActionRegistry() const {
  thread::MutexLock lock(&state_->mu);
  return state_->action_registry;
}

absl::Status Session::SetActionRegistry(
    const std::shared_ptr<actions::ActionRegistry>& registry) {
  std::vector<std::shared_ptr<actions::Action>> actions;
  {
    thread::MutexLock lock(&state_->mu);
    state_->action_registry = registry;
    for (const auto& [unused, action] : state_->actions) {
      (void)unused;
      actions.push_back(action);
    }
  }
  absl::Status first;
  for (const auto& action : actions) {
    KeepFirstError(action->BindRegistry(registry), &first);
  }
  return first;
}

std::vector<std::pair<std::string, std::shared_ptr<actions::Action>>>
Session::Actions() const {
  thread::MutexLock lock(&state_->mu);
  return {state_->actions.begin(), state_->actions.end()};
}

absl::StatusOr<std::shared_ptr<actions::Action>> Session::GetAction(
    std::string_view action_id) const {
  thread::MutexLock lock(&state_->mu);
  const auto found = state_->actions.find(action_id);
  if (found == state_->actions.end()) {
    return absl::NotFoundError(
        absl::StrCat("Action '", action_id, "' is not active in the Session"));
  }
  return found->second;
}

absl::Status Session::CancelAction(std::string_view action_id) {
  absl::StatusOr<std::shared_ptr<actions::Action>> action =
      GetAction(action_id);
  return action.ok() ? (*action)->Cancel() : action.status();
}

absl::Status Session::CancelAllActions() {
  std::vector<std::shared_ptr<actions::Action>> actions;
  {
    thread::MutexLock lock(&state_->mu);
    for (const auto& [unused, action] : state_->actions) {
      (void)unused;
      actions.push_back(action);
    }
  }
  absl::Status first;
  for (const auto& action : actions) {
    KeepFirstError(action->Cancel(), &first);
  }
  return first;
}

a11::Task Session::AwaitAllActions(absl::Duration timeout) {
  if (timeout < absl::ZeroDuration()) {
    return a11::FailedTask(
        absl::InvalidArgumentError("timeout must not be negative"));
  }
  std::shared_ptr<Session> self = shared_from_this();
  return a11::SubmitTask([self = std::move(self), timeout]() -> absl::Status {
    const absl::Time deadline = timeout == absl::InfiniteDuration()
                                    ? absl::InfiniteFuture()
                                    : absl::Now() + timeout;
    absl::flat_hash_set<actions::Action*> observed;
    std::vector<absl::Status> failures;
    while (true) {
      std::vector<std::shared_ptr<actions::Action>> pending;
      {
        thread::MutexLock lock(&self->state_->mu);
        for (const auto& [unused, action] : self->state_->actions) {
          (void)unused;
          if (observed.find(action.get()) == observed.end()) {
            pending.push_back(action);
          }
        }
      }
      if (pending.empty()) {
        break;
      }
      for (const auto& action : pending) {
        observed.insert(action.get());
        const absl::Duration remaining =
            deadline == absl::InfiniteFuture()
                ? absl::InfiniteDuration()
                : std::max(absl::ZeroDuration(), deadline - absl::Now());
        if (absl::Status result = action->Wait(remaining).Await().status();
            !result.ok()) {
          if (result.code() == absl::StatusCode::kDeadlineExceeded &&
              !action->IsDone()) {
            return result;
          }
          failures.push_back(result);
        }
      }
    }
    if (failures.empty()) {
      return absl::OkStatus();
    }
    absl::StatusCode code = failures.front().code();
    for (const absl::Status& failure : failures) {
      if (failure.code() != code) {
        code = absl::StatusCode::kUnknown;
        break;
      }
    }
    nlohmann::json details = nlohmann::json::array();
    for (const absl::Status& failure : failures) {
      details.push_back({{"status", StatusToJsonOrEmptyDetails(failure)}});
    }
    return MakeStatus(
        code, absl::StrCat(failures.size(), " Actions completed with errors."),
        details);
  });
}

absl::Status Session::TrackAction(
    const std::shared_ptr<actions::Action>& action) {
  if (action == nullptr) {
    return absl::InvalidArgumentError("action must not be null");
  }
  const std::string id = action->GetId();
  thread::MutexLock lock(&state_->mu);
  if (state_->phase != Phase::kOpen || state_->remote_closed) {
    return absl::FailedPreconditionError(
        "Session is no longer accepting Actions");
  }
  const auto found = state_->actions.find(id);
  if (found != state_->actions.end() && found->second != action) {
    return absl::AlreadyExistsError(
        absl::StrCat("Action '", id, "' already exists in the Session"));
  }
  state_->actions.insert_or_assign(id, action);
  return absl::OkStatus();
}

void Session::UntrackAction(const std::shared_ptr<actions::Action>& action) {
  if (action == nullptr) {
    return;
  }
  thread::MutexLock lock(&state_->mu);
  const auto found = state_->actions.find(action->GetId());
  if (found != state_->actions.end() && found->second == action) {
    state_->actions.erase(found);
  }
}

std::shared_ptr<actions::ActionLimiter> Session::GetActionLimiter(
    bool nested) const {
  thread::MutexLock lock(&state_->mu);
  return nested ? state_->nested_limiter : state_->root_limiter;
}

a11::Future<std::uint32_t> Session::DispatchNodeFragment(
    data::NodeFragment fragment) {
  absl::Status validation = fragment.Validate();
  if (!validation.ok()) {
    return a11::FailedFuture<std::uint32_t>(validation);
  }

  // An ordinary data fragment for an ordinary node -- the overwhelming majority
  // of what a busy session dispatches -- is handled here without a fibre. So
  // the fibre this
  const auto fast_special = ActionSpecialNode(fragment.id);
  const data::Chunk* fast_chunk = std::get_if<data::Chunk>(&fragment.data);
  // Only two shapes genuinely need the fibre, and both await more than once
  // with the control flow depending on what came back: a close marker (which
  // reads the mirror's writability, then applies a close), and a status chunk.
  const bool close_marker =
      fast_chunk != nullptr && actions::IsCloseStatusChunk(*fast_chunk);
  const bool status_chunk =
      fast_chunk != nullptr && actions::IsStatusChunk(*fast_chunk);
  const bool needs_the_slow_path =
      close_marker || (status_chunk && !fast_special.has_value());
  if (!needs_the_slow_path) {
    // A status node's own preamble, all of it synchronous.
    std::shared_ptr<actions::Action> action;
    std::optional<absl::Status> protocol_status;
    bool sets_dispatch_status = false;
    if (fast_special.has_value()) {
      if (!status_chunk) {
        return a11::FailedFuture<std::uint32_t>(absl::InvalidArgumentError(
            "An Action status node requires a status Chunk"));
      }
      absl::StatusOr<absl::Status> decoded =
          actions::StatusFromChunk(*fast_chunk);
      if (!decoded.ok()) {
        return a11::FailedFuture<std::uint32_t>(decoded.status());
      }
      protocol_status = *std::move(decoded);
      absl::StatusOr<std::shared_ptr<actions::Action>> found =
          GetAction(fast_special->first);
      if (!found.ok()) {
        return a11::FailedFuture<std::uint32_t>(
            absl::NotFoundError("Received status for an unknown Action"));
      }
      action = *std::move(found);
      sets_dispatch_status =
          fast_special->second == actions::kActionDispatchStatusOutput;
    }
    std::shared_ptr<nodes::NodeMap> node_map = GetNodeMap();
    absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> node =
        node_map->Get(fragment.id);
    if (!node.ok()) {
      return a11::FailedFuture<std::uint32_t>(node.status());
    }
    const std::uint32_t fallback_seq = fragment.seq.value_or(0);
    if ((*node)->GetWriterAbortStatus().has_value()) {
      return a11::CompletedFuture<std::uint32_t>(fallback_seq);
    }
    std::shared_ptr<nodes::AsyncNode> owned = *std::move(node);
    return a11::ThenAfterWaiting(
        owned->PutFragment(std::move(fragment)), absl::InfiniteFuture(),
        [owned, fallback_seq, action = std::move(action),
         protocol_status = std::move(protocol_status),
         sets_dispatch_status](const absl::StatusOr<std::uint32_t>& stored)
            -> absl::StatusOr<std::uint32_t> {
          if (!stored.ok()) {
            // A write refused because the writer was aborted is not this
            // dispatch's failure: the peer is told through the abort, and the
            // fragment is accounted for. Same rule as the fibre path had.
            const std::optional<absl::Status> abort =
                owned->GetWriterAbortStatus();
            if (abort.has_value() && *abort == stored.status()) {
              return fallback_seq;
            }
            return stored.status();
          }
          // Published only after the write succeeded, exactly as the fibre path
          // did: a status the Action reports is a claim that the status node
          // carries it.
          if (action != nullptr) {
            if (sets_dispatch_status) {
              action->SetDispatchStatus(*protocol_status);
            } else {
              action->SetCompletionStatus(*protocol_status);
            }
          }
          return *stored;
        });
  }

  // The last two shapes: a close marker, and a status chunk on an ordinary
  // node. The second await needs no fibre either way: what follows it only maps
  // a status to a sequence number.
  if (close_marker || (status_chunk && !fast_special.has_value())) {
    absl::StatusOr<absl::Status> carried =
        actions::StatusFromChunk(*fast_chunk);
    if (!carried.ok()) {
      return a11::FailedFuture<std::uint32_t>(carried.status());
    }
    if (!close_marker && carried->ok()) {
      return a11::FailedFuture<std::uint32_t>(absl::InvalidArgumentError(
          "An ordinary node cannot be aborted with an OK status"));
    }
    const std::uint32_t seq = fragment.seq.value_or(0);
    // Created if absent, not looked up: a marker can arrive before whatever
    // would have materialised its node, and dropping it hangs the reader for
    // good. The fibre path's own note explains this at length.
    absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> node =
        GetNodeMap()->Get(fragment.id);
    if (!node.ok()) {
      return a11::FailedFuture<std::uint32_t>(node.status());
    }
    if (*node == nullptr) {
      return a11::CompletedFuture<std::uint32_t>(seq);
    }
    a11::Future<bool> writability = (*node)->IsWritable();
    if (writability.IsReady()) {
      const absl::StatusOr<bool> writable = writability.Await();
      if (!writable.ok()) {
        return a11::FailedFuture<std::uint32_t>(writable.status());
      }
      if (!*writable) {
        return a11::CompletedFuture<std::uint32_t>(seq);
      }
      a11::Task applied = (close_marker && carried->ok())
                              ? (*node)->Close()
                              : (*node)->AbortWithStatus(*carried);
      return a11::Then(applied,
                       [seq](const absl::StatusOr<a11::Unit>& done)
                           -> absl::StatusOr<std::uint32_t> {
                         if (!done.ok()) {
                           return done.status();
                         }
                         return seq;
                       });
    }
    // Writability is not settled: fall through and let a fibre wait for it.
  }

  std::shared_ptr<Session> self = shared_from_this();
  return a11::Submit<std::uint32_t>([self = std::move(self),
                                     fragment = std::move(fragment)]() mutable
                                        -> absl::StatusOr<std::uint32_t> {
    const auto special = ActionSpecialNode(fragment.id);
    const data::Chunk* chunk = std::get_if<data::Chunk>(&fragment.data);
    std::shared_ptr<actions::Action> action;
    std::optional<absl::Status> protocol_status;

    // A closure marker reports that the peer drained the node and closed its
    // write half; it carries no value, so it is applied to the local mirror
    // rather than stored.
    if (chunk != nullptr && actions::IsCloseStatusChunk(*chunk)) {
      ABSL_ASSIGN_OR_RETURN(absl::Status closed,
                            actions::StatusFromChunk(*chunk));
      const std::uint32_t seq = fragment.seq.value_or(0);
      // Create the node
      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<nodes::AsyncNode> mirror,
                            self->GetNodeMap()->Get(fragment.id));
      if (mirror == nullptr) {
        return seq;
      }
      ABSL_ASSIGN_OR_RETURN(bool writable, mirror->IsWritable().Await());
      if (!writable) {
        return seq;
      }
      const a11::Task applied =
          closed.ok() ? mirror->Close()
                      : mirror->AbortWithStatus(std::move(closed));
      ABSL_RETURN_IF_ERROR(applied.Await().status());
      return seq;
    }

    if (special.has_value()) {
      if (!chunk || !actions::IsStatusChunk(*chunk)) {
        return absl::InvalidArgumentError(
            "An Action status node requires a status Chunk");
      }
      ABSL_ASSIGN_OR_RETURN(absl::Status decoded,
                            actions::StatusFromChunk(*chunk));
      protocol_status = decoded;
      absl::StatusOr<std::shared_ptr<actions::Action>> found =
          self->GetAction(special->first);
      if (!found.ok()) {
        return absl::NotFoundError("Received status for an unknown Action");
      }
      action = std::move(*found);
    }
    std::shared_ptr<nodes::NodeMap> node_map = self->GetNodeMap();
    ABSL_ASSIGN_OR_RETURN(std::shared_ptr<nodes::AsyncNode> node,
                          node_map->Get(fragment.id));

    if (chunk && actions::IsStatusChunk(*chunk) && !special.has_value()) {
      ABSL_ASSIGN_OR_RETURN(absl::Status decoded,
                            actions::StatusFromChunk(*chunk));
      if (decoded.ok()) {
        return absl::InvalidArgumentError(
            "An ordinary node cannot be aborted with an OK status");
      }
      ABSL_ASSIGN_OR_RETURN(bool writable, node->IsWritable().Await());
      if (writable) {
        ABSL_RETURN_IF_ERROR(node->AbortWithStatus(decoded).Await().status());
      }
      return fragment.seq.value_or(0);
    }
    if (node->GetWriterAbortStatus().has_value()) {
      return fragment.seq.value_or(0);
    }
    absl::StatusOr<std::uint32_t> stored = node->PutFragment(fragment).Await();
    if (!stored.ok()) {
      const std::optional<absl::Status> abort = node->GetWriterAbortStatus();
      if (abort.has_value() && *abort == stored.status()) {
        return fragment.seq.value_or(0);
      }
      return stored.status();
    }
    if (special.has_value()) {
      if (special->second == actions::kActionDispatchStatusOutput) {
        action->SetDispatchStatus(*protocol_status);
      } else {
        action->SetCompletionStatus(*protocol_status);
      }
    }
    return *stored;
  });
}

a11::Task Session::DispatchActionMessage(
    data::ActionMessage message,
    std::shared_ptr<net::WireStream> origin_stream) {
  absl::Status validation = message.Validate();
  if (!validation.ok()) {
    return a11::FailedTask(validation);
  }
  std::shared_ptr<Session> self = shared_from_this();
  return a11::SubmitTask(
      [self = std::move(self), message = std::move(message),
       origin_stream = std::move(origin_stream)]() mutable -> absl::Status {
        if (message.name == actions::kCancelActionName) {
          std::optional<std::string> action_id;
          for (const auto& [name, value] : message.headers) {
            if (absl::AsciiStrToLower(name) == actions::kCancelActionHeader) {
              action_id = value;
              break;
            }
          }
          if (!action_id.has_value()) {
            return absl::InvalidArgumentError(
                "Cancel Action requires the __action header");
          }
          ABSL_RETURN_IF_ERROR(data::ValidateName(*action_id));
          absl::StatusOr<std::shared_ptr<actions::Action>> action =
              self->GetAction(*action_id);
          if (action.ok()) {
            return (*action)->Cancel();
          }
          if (action.status().code() == absl::StatusCode::kNotFound) {
            return absl::OkStatus();
          }
          return action.status();
        }

        absl::Status dispatch_status;
        std::shared_ptr<actions::ActionRegistry> registry;
        {
          thread::MutexLock lock(&self->state_->mu);
          if (self->state_->phase != Phase::kOpen ||
              self->state_->remote_closed) {
            dispatch_status = absl::FailedPreconditionError(
                "Session is no longer accepting Actions");
          } else if (self->state_->actions.find(message.id) !=
                     self->state_->actions.end()) {
            dispatch_status = absl::AlreadyExistsError(
                "Action already exists in the Session");
          } else {
            registry = self->state_->action_registry;
            if (registry == nullptr) {
              dispatch_status = absl::FailedPreconditionError(
                  "Session has no ActionRegistry");
            }
          }
        }
        if (dispatch_status.ok()) {
          absl::StatusOr<std::shared_ptr<actions::Action>> action =
              registry->MakeAction(message.name, message.id, self->GetNodeMap(),
                                   origin_stream, self);
          if (!action.ok()) {
            dispatch_status = action.status();
          } else {
            dispatch_status = (*action)->MapPortsFromMessage(message);
            for (const auto& [name, value] : message.headers) {
              if (!dispatch_status.ok()) {
                break;
              }
              dispatch_status = (*action)->SetHeader(name, value);
            }
            if (dispatch_status.ok()) {
              (void)(*action)->ClearInputsAfterRun();
              (void)(*action)->ClearOutputsAfterRun();
              // The receiver applies its own input autofills (which may differ
              // from the caller's) before running and before this WireMessage's
              // node fragments are dispatched.
              dispatch_status = (*action)->ApplyInputAutofills();
              if (dispatch_status.ok()) {
                absl::StatusOr<std::shared_ptr<actions::Action>> started =
                    (*action)->Run();
                dispatch_status = started.status();
              }
            }
          }
        }

        if (origin_stream != nullptr) {
          ABSL_ASSIGN_OR_RETURN(data::Chunk chunk,
                                actions::StatusToChunk(dispatch_status));
          ABSL_ASSIGN_OR_RETURN(
              std::string dispatch_id,
              actions::Action::MakeNodeId(
                  message.id, actions::kActionDispatchStatusOutput));
          data::WireMessage report;
          report.node_fragments.push_back(data::NodeFragment{
              .id = dispatch_id,
              .data = chunk,
              .seq = 0,
              .continued = false,
          });
          if (!dispatch_status.ok()) {
            ABSL_ASSIGN_OR_RETURN(
                std::string status_id,
                actions::Action::MakeNodeId(message.id,
                                            actions::kActionStatusOutput));
            report.node_fragments.push_back(data::NodeFragment{
                .id = status_id,
                .data = chunk,
                .seq = 0,
                .continued = false,
            });
          }
          ABSL_RETURN_IF_ERROR(origin_stream->Send(std::move(report)));
          // A successfully reported dispatch failure is local to the Action,
          // not a reason to abort its shared stream.
          return absl::OkStatus();
        }
        return dispatch_status;
      },
      {});
}

a11::Task Session::DispatchAction(
    const std::shared_ptr<actions::Action>& action) {
  if (action == nullptr) {
    return a11::FailedTask(
        absl::InvalidArgumentError("action must not be null"));
  }
  if (action->GetNodeMap() == nullptr) {
    action->BindNodeMap(GetNodeMap()).IgnoreError();
  }
  if (action->GetRegistry() == nullptr) {
    action->BindRegistry(GetActionRegistry()).IgnoreError();
  }
  if (absl::Status status = action->BindSession(shared_from_this());
      !status.ok()) {
    return a11::FailedTask(std::move(status));
  }
  const absl::StatusOr<std::shared_ptr<actions::Action>> started =
      action->Run();
  return started.ok() ? a11::ReadyTask() : a11::FailedTask(started.status());
}

a11::Task Session::DispatchWireMessage(
    data::WireMessage message, std::shared_ptr<net::WireStream> origin_stream) {
  if (const absl::Status validation_status = message.Validate();
      !validation_status.ok()) {
    return a11::FailedTask(validation_status);
  }

  std::shared_ptr<Session> self = shared_from_this();
  return a11::SubmitTask(
      [self = std::move(self), message = std::move(message),
       origin_stream = std::move(origin_stream)]() mutable -> absl::Status {
        struct DispatchFailure {
          std::string element_type;
          size_t element_index;
          absl::Status status;
        };
        std::vector<DispatchFailure> failures;
        // Actions are dispatched before node fragments so a receiver applies
        // its own input autofills (and closes those inputs) ahead of any
        // fragments in the same WireMessage that target them.
        for (size_t index = 0; index < message.actions.size(); ++index) {
          data::ActionMessage& action = message.actions[index];
          absl::Status status =
              self->DispatchActionMessage(std::move(action), origin_stream)
                  .Await()
                  .status();
          if (!status.ok()) {
            failures.push_back(DispatchFailure{
                .element_type = "action_message",
                .element_index = index,
                .status = std::move(status),
            });
          }
        }
        // Fragments for *different* nodes are independent, and one WireMessage
        // routinely carries many of them:
        std::vector<std::string> group_order;
        absl::flat_hash_map<std::string, std::vector<size_t>> groups;
        for (size_t index = 0; index < message.node_fragments.size(); ++index) {
          const std::string& id = message.node_fragments[index].id;
          auto [entry, fresh] = groups.try_emplace(id);
          if (fresh) {
            group_order.push_back(id);
          }
          entry->second.push_back(index);
        }

        // Fragment failures are collected separately and merged after the
        // action ones, because the aggregate's detail order is part of the
        // contract: actions first, then fragments, each in element order.
        std::vector<DispatchFailure> fragment_failures;
        thread::Mutex failure_mu;
        std::vector<absl::AnyInvocable<absl::Status() &&>> dispatches;
        dispatches.reserve(group_order.size());
        for (const std::string& id : group_order) {
          dispatches.emplace_back([self = self.get(), &message,
                                   &fragment_failures, &failure_mu,
                                   indices = &groups.at(id)]() -> absl::Status {
            for (const size_t index : *indices) {
              data::NodeFragment& fragment = message.node_fragments[index];
              const std::string fragment_id = fragment.id;
              absl::Status status =
                  self->DispatchNodeFragment(std::move(fragment))
                      .Await()
                      .status();
              if (status.ok()) {
                continue;
              }
              // A rejected write to an Action input (e.g. a caller trying to
              // populate a receiver-autofilled, now-closed input) cancels the
              // owning Action so the failure propagates back to the caller.
              const size_t separator = fragment_id.find('#');
              if (separator != std::string::npos) {
                self->CancelAction(fragment_id.substr(0, separator))
                    .IgnoreError();
              }
              thread::MutexLock lock(&failure_mu);
              fragment_failures.push_back(DispatchFailure{
                  .element_type = "node_fragment",
                  .element_index = index,
                  .status = std::move(status),
              });
              // Continue through this node's remaining fragments, matching the
              // serial path.
            }
            return absl::OkStatus();
          });
        }
        a11::RunAllToCompletion(std::move(dispatches)).IgnoreError();
        // In element order regardless of which group finished first, so the
        // same message always produces the same error whichever way it was
        // scheduled.
        std::sort(
            fragment_failures.begin(), fragment_failures.end(),
            [](const DispatchFailure& left, const DispatchFailure& right) {
              return left.element_index < right.element_index;
            });
        for (DispatchFailure& failure : fragment_failures) {
          failures.push_back(std::move(failure));
        }
        if (failures.empty()) {
          return absl::OkStatus();
        }
        absl::StatusCode code = failures.front().status.code();
        nlohmann::json details = nlohmann::json::array();
        for (const DispatchFailure& failure : failures) {
          if (failure.status.code() != code) {
            code = absl::StatusCode::kUnknown;
          }
          details.push_back(
              {{"element_type", failure.element_type},
               {"element_index", failure.element_index},
               {"status", StatusToJsonOrEmptyDetails(failure.status)}});
        }
        return MakeStatus(
            code,
            absl::StrCat("Failed to dispatch ", failures.size(), " of ",
                         message.node_fragments.size() + message.actions.size(),
                         " WireMessage elements"),
            details);
      },
      {});
}

bool Session::IsClosed() const {
  thread::MutexLock lock(&state_->mu);
  return state_->remote_closed || state_->phase != Phase::kOpen;
}

bool Session::IsDone() const {
  thread::MutexLock lock(&state_->mu);
  return state_->destroyed;
}

a11::Task Session::Done() const {
  thread::MutexLock lock(&state_->mu);
  return state_->done_future;
}

absl::Status Session::GetStatus() const {
  bool expired = false;
  {
    thread::MutexLock lock(&state_->mu);
    expired = state_->phase == Phase::kOpen && state_->deadline <= absl::Now();
  }
  if (expired) {
    (void)const_cast<Session*>(this)->Abort(
        absl::DeadlineExceededError("The Session deadline has been exceeded"));
  }
  thread::MutexLock lock(&state_->mu);
  return state_->status;
}

absl::StatusOr<a11::Task> Session::AddStream(
    std::shared_ptr<net::WireStream> stream, StreamMode mode) {
  if (stream == nullptr) {
    return absl::InvalidArgumentError("stream must not be null");
  }
  if (deadline() <= absl::Now()) {
    (void)Abort(
        absl::DeadlineExceededError("The Session deadline has been exceeded"));
  }
  std::string stream_id;
  stream_id = stream->GetId();
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<actions::ActionLimiter> gate,
                        actions::ActionLimiter::Create(1));
  auto stream_state =
      std::make_shared<StreamState>(stream, stream_id, std::move(gate));
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->phase != Phase::kOpen || state_->remote_closed) {
      return absl::FailedPreconditionError(
          "No streams can be attached after the Session ends");
    }
    if (state_->streams.find(stream_id) != state_->streams.end() ||
        state_->stream_states.find(stream.get()) !=
            state_->stream_states.end()) {
      return absl::AlreadyExistsError(
          "Stream is already attached to the Session");
    }
    state_->streams.emplace(stream_id, stream_state);
    state_->stream_states.emplace(stream.get(), stream_state);
    state_->stream_order.push_back(stream.get());
    state_->no_stream_since.reset();
  }
  NotifyStateChanged();
  std::weak_ptr<Session> weak = shared_from_this();
  net::OnMessage on_message =
      [weak, stream_state](std::optional<data::WireMessage> message) mutable {
        std::shared_ptr<Session> self = weak.lock();
        return self != nullptr
                   ? self->HandleStreamMessage(stream_state, std::move(message))
                   : a11::FailedTask(
                         absl::CancelledError("Session was destroyed"));
      };
  net::OnDone on_done = [weak, stream_state]() {
    std::shared_ptr<Session> self = weak.lock();
    return self != nullptr ? self->HandleStreamDone(stream_state)
                           : a11::ReadyTask();
  };
  // Run the transport's Start()/Accept() on the runtime's fiber pool rather
  // than the caller's thread.
  a11::Task startup = a11::SubmitTask(
      [stream = std::move(stream), mode, on_message = std::move(on_message),
       on_done = std::move(on_done)]() mutable -> absl::Status {
        a11::Task started =
            mode == StreamMode::kStart
                ? stream->Start(std::move(on_message), std::move(on_done))
                : stream->Accept(std::move(on_message), std::move(on_done));
        return started.Await().status();
      });
  auto promise = std::make_shared<a11::Promise<a11::Unit>>();
  a11::Task attached = promise->future();
  std::weak_ptr<Session> cleanup = shared_from_this();
  startup.OnReady([cleanup, stream_state,
                   promise](const absl::StatusOr<a11::Unit>& result) {
    if (!result.ok()) {
      std::shared_ptr<Session> session = cleanup.lock();
      if (session != nullptr) {
        session->RemoveStream(stream_state);
      }
    }
    (void)promise->SetResult(result);
  });
  const absl::Status cancellation = promise->SetCancellationCallback(
      [cleanup, stream_state, startup, promise]() mutable {
        (void)startup.Cancel();
        std::shared_ptr<Session> session = cleanup.lock();
        if (session != nullptr) {
          session->RemoveStream(stream_state);
        }
        (void)promise->SetStatus(
            absl::CancelledError("Session stream startup was cancelled"));
      });
  (void)cancellation;
  return attached;
}

a11::Task Session::HandleStreamMessage(
    const std::shared_ptr<StreamState>& stream_state,
    std::optional<data::WireMessage> message) {
  std::shared_ptr<Session> self = shared_from_this();
  return a11::SubmitTask([self = std::move(self), stream_state,
                          message =
                              std::move(message)]() mutable -> absl::Status {
    if (!message.has_value()) {
      while (true) {
        std::shared_ptr<thread::PermanentEvent> changed;
        {
          thread::MutexLock lock(&self->state_->mu);
          stream_state->remote_half_closed = true;
          if (!stream_state->accepting_messages ||
              self->state_->phase == Phase::kAborted ||
              stream_state->half_close_delivered) {
            return absl::OkStatus();
          }
          if (stream_state->outstanding_messages == 0) {
            break;
          }
          changed = self->state_->changed;
        }
        if (thread::Select({thread::OnCancel(), changed->OnEvent()}) == 0) {
          return absl::CancelledError("Session stream callback was cancelled");
        }
      }
      std::optional<data::ByteMap> trailers =
          stream_state->stream->GetTrailers();
      if (trailers.has_value()) {
        const data::Bytes* session_status = nullptr;
        for (const auto& [name, value] : *trailers) {
          if (absl::AsciiStrToLower(name) == kSessionStatusHeader) {
            session_status = &value;
            break;
          }
        }
        if (session_status != nullptr) {
          ABSL_ASSIGN_OR_RETURN(absl::Status decoded,
                                data::UnpackStatus(*session_status));
          if (!decoded.ok()) {
            return absl::FailedPreconditionError(
                "A peer must abort, not half-close, a failed Session");
          }
          thread::MutexLock lock(&self->state_->mu);
          self->state_->remote_closed = true;
        }
      }
      OnSessionStreamMessage callback;
      {
        thread::MutexLock lock(&self->state_->mu);
        if (!stream_state->accepting_messages ||
            self->state_->phase == Phase::kAborted) {
          return absl::OkStatus();
        }
        stream_state->half_close_delivered = true;
        callback = self->state_->on_message;
      }
      return callback(std::nullopt, stream_state->stream, self)
          .Await()
          .status();
    }

    ABSL_ASSIGN_OR_RETURN(std::string encoded, message->ToMsgpack());
    const size_t size = encoded.size();
    if (size > self->state_->options.max_single_message_size) {
      return absl::OutOfRangeError(
          "Incoming WireMessage exceeds max_single_message_size");
    }
    while (true) {
      std::shared_ptr<thread::PermanentEvent> changed;
      {
        thread::MutexLock lock(&self->state_->mu);
        if (!stream_state->accepting_messages ||
            self->state_->phase == Phase::kAborted) {
          return absl::OkStatus();
        }
        const bool counts_fit =
            self->state_->buffered_messages <
                self->state_->options.max_buffered_messages_total &&
            stream_state->outstanding_messages <
                self->state_->options.max_buffered_messages_per_stream;
        const bool total_bytes_fit =
            self->state_->buffered_bytes == 0 ||
            self->state_->buffered_bytes + size <=
                self->state_->options.max_buffered_bytes_total;
        const bool stream_bytes_fit =
            stream_state->outstanding_bytes == 0 ||
            stream_state->outstanding_bytes + size <=
                self->state_->options.max_buffered_bytes_per_stream;
        if (counts_fit && total_bytes_fit && stream_bytes_fit) {
          ++self->state_->buffered_messages;
          self->state_->buffered_bytes += size;
          ++stream_state->outstanding_messages;
          stream_state->outstanding_bytes += size;
          break;
        }
        changed = self->state_->changed;
      }
      if (thread::Select({thread::OnCancel(), changed->OnEvent()}) == 0) {
        return absl::CancelledError("Session message buffering was cancelled");
      }
    }

    bool start_pump = false;
    {
      thread::MutexLock lock(&self->state_->mu);
      stream_state->pending_messages.emplace_back(std::move(*message), size);
      if (!stream_state->message_pump_running) {
        stream_state->message_pump_running = true;
        start_pump = true;
      }
    }
    if (start_pump) {
      // The pump runs the session message callback synchronously (msgpack
      // decode, action creation, exception unwinding, and any tracing spans)
      // before Await()-ing, so it needs a full-size stack.
      std::function<void()> cancel = a11::ScheduleCancelable(
          [self, stream_state] { self->ProcessStreamMessages(stream_state); });
      thread::MutexLock lock(&self->state_->mu);
      if (stream_state->message_pump_running) {
        stream_state->message_pump_cancel = std::move(cancel);
      }
    }
    // Transport backpressure covers bounded admission. User callbacks run in
    // the per-stream pump so one slow callback does not hold the transport's
    // delivery stack or reorder later Action messages on the same stream.
    return absl::OkStatus();
  });
}

void Session::ProcessStreamMessages(
    const std::shared_ptr<StreamState>& stream_state) {
  while (true) {
    data::WireMessage message;
    size_t size = 0;
    {
      thread::MutexLock lock(&state_->mu);
      if (stream_state->pending_messages.empty()) {
        stream_state->message_pump_running = false;
        stream_state->message_pump_cancel = {};
        return;
      }
      message = std::move(stream_state->pending_messages.front().first);
      size = stream_state->pending_messages.front().second;
      stream_state->pending_messages.pop_front();
    }

    OnSessionStreamMessage callback;
    {
      thread::MutexLock lock(&state_->mu);
      callback = state_->on_message;
    }
    absl::Status callback_status;
    a11::Task callback_task =
        callback(std::optional<data::WireMessage>(std::move(message)),
                 stream_state->stream, shared_from_this());
    callback_status = callback_task.Await().status();
    if (thread::Cancelled()) {
      (void)callback_task.Cancel();
      callback_status =
          absl::CancelledError("Session message callback was cancelled");
    }

    bool session_aborted = false;
    {
      thread::MutexLock lock(&state_->mu);
      session_aborted = state_->phase == Phase::kAborted;
      --state_->buffered_messages;
      state_->buffered_bytes -= size;
      --stream_state->outstanding_messages;
      stream_state->outstanding_bytes -= size;
      if (!callback_status.ok()) {
        stream_state->accepting_messages = false;
        for (const auto& [unused_message, pending_size] :
             stream_state->pending_messages) {
          (void)unused_message;
          --state_->buffered_messages;
          state_->buffered_bytes -= pending_size;
          --stream_state->outstanding_messages;
          stream_state->outstanding_bytes -= pending_size;
        }
        stream_state->pending_messages.clear();
        stream_state->message_pump_running = false;
        stream_state->message_pump_cancel = {};
      }
    }
    NotifyStateChanged();
    if (!callback_status.ok() && !session_aborted) {
      (void)stream_state->stream->Abort(callback_status);
      return;
    }
  }
}

a11::Task Session::HandleStreamDone(
    const std::shared_ptr<StreamState>& stream_state) {
  std::shared_ptr<Session> self = shared_from_this();
  return a11::SubmitTask(
      [self = std::move(self), stream_state]() -> absl::Status {
        {
          thread::MutexLock lock(&self->state_->mu);
          if (stream_state->done || stream_state->done_started) {
            return absl::OkStatus();
          }
          stream_state->done_started = true;
        }
        const absl::Status stream_status = stream_state->stream->GetStatus();
        bool remote_session_abort = IsSessionStreamAbortStatus(stream_status);
        std::vector<std::shared_ptr<actions::Action>> actions_to_cancel;
        {
          thread::MutexLock lock(&self->state_->mu);
          if (remote_session_abort && self->state_->phase != Phase::kAborted) {
            self->state_->remote_closed = true;
            self->state_->phase = Phase::kAborted;
            self->state_->status = stream_status;
            for (const auto& [unused, action] : self->state_->actions) {
              (void)unused;
              actions_to_cancel.push_back(action);
            }
            for (const auto& [unused, state] : self->state_->stream_states) {
              (void)unused;
              state->accepting_messages = false;
            }
          }
          if (!stream_status.ok()) {
            stream_state->accepting_messages = false;
          }
        }
        for (const auto& action : actions_to_cancel) {
          (void)action->Cancel();
        }
        self->NotifyStateChanged();

        while (true) {
          std::shared_ptr<thread::PermanentEvent> changed;
          {
            thread::MutexLock lock(&self->state_->mu);
            if (stream_state->outstanding_messages == 0) {
              break;
            }
            changed = self->state_->changed;
          }
          if (thread::Select({thread::OnCancel(), changed->OnEvent()}) == 0) {
            break;
          }
        }
        OnSessionStreamDone callback;
        {
          thread::MutexLock lock(&self->state_->mu);
          callback = self->state_->on_done;
        }
        absl::Status callback_status;
        callback_status = callback(stream_state->stream, self).Await().status();
        self->RemoveStream(stream_state);
        return callback_status;
      });
}

void Session::RemoveStream(const std::shared_ptr<StreamState>& stream_state) {
  {
    thread::MutexLock lock(&state_->mu);
    if (stream_state->done) {
      return;
    }
    stream_state->done = true;
    stream_state->accepting_messages = false;
    state_->stream_states.erase(stream_state->stream.get());
    state_->stream_order.erase(
        std::remove(state_->stream_order.begin(), state_->stream_order.end(),
                    stream_state->stream.get()),
        state_->stream_order.end());
    const auto found = state_->streams.find(stream_state->id);
    if (found != state_->streams.end() && found->second == stream_state) {
      state_->streams.erase(found);
    }
    if (state_->stream_states.empty() && state_->phase == Phase::kOpen &&
        !state_->remote_closed) {
      state_->no_stream_since = absl::Now();
    }
  }
  NotifyStateChanged();
  FinishIfPossible();
}

void Session::FinishIfPossible() {
  std::shared_ptr<a11::Promise<a11::Unit>> promise;
  obs::Span span;
  absl::Status status;
  {
    thread::MutexLock lock(&state_->mu);
    if ((state_->phase == Phase::kOpen && !state_->remote_closed) ||
        !state_->stream_states.empty() || state_->destroyed) {
      return;
    }
    state_->destroyed = true;
    promise = state_->done_promise;
    span = std::move(state_->span);
    status = state_->status;
  }
  if (span.IsRecording()) {
    span.SetStatus(status);
    span.End();
  }
  (void)promise->SetValue(a11::Unit{});
  NotifyStateChanged();
}

absl::Status Session::HalfClose() {
  data::ByteMap trailers;
  std::vector<std::shared_ptr<StreamState>> streams;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->phase != Phase::kOpen) {
      return absl::OkStatus();
    }
    if (state_->deadline <= absl::Now()) {
      // Abort below without holding the state lock.
    } else {
      absl::StatusOr<std::string> packed = data::PackStatus(absl::OkStatus());
      if (!packed.ok()) {
        return packed.status();
      }
      trailers = state_->headers;
      trailers.insert_or_assign(std::string(kSessionStatusHeader),
                                std::move(*packed));
      state_->phase = Phase::kClosing;
      state_->status = absl::OkStatus();
      for (const auto& [unused, stream] : state_->stream_states) {
        (void)unused;
        if (!stream->done && !stream->done_started) {
          streams.push_back(stream);
        }
      }
    }
  }
  if (trailers.empty() && deadline() <= absl::Now()) {
    return Abort(
        absl::DeadlineExceededError("The Session deadline has been exceeded"));
  }
  NotifyStateChanged();
  absl::Status first;
  for (const auto& stream : streams) {
    KeepFirstError(stream->stream->HalfClose(trailers), &first);
  }
  FinishIfPossible();
  return first;
}

absl::Status Session::Abort(absl::Status status) {
  if (status.ok()) {
    return absl::InvalidArgumentError(
        "An aborted Session needs a non-OK status");
  }
  std::vector<std::shared_ptr<StreamState>> streams;
  std::vector<std::shared_ptr<actions::Action>> actions;
  std::vector<std::function<void()>> callback_cancellations;
  data::WireMessage terminal;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->phase != Phase::kOpen) {
      return absl::OkStatus();
    }
    if (state_->deadline <= absl::Now()) {
      status =
          absl::DeadlineExceededError("The Session deadline has been exceeded");
    }
    absl::StatusOr<std::string> session_status = data::PackStatus(status);
    if (!session_status.ok()) {
      return session_status.status();
    }
    absl::StatusOr<std::string> stream_status =
        data::PackStatus(SessionStreamAbortStatus());
    if (!stream_status.ok()) {
      return stream_status.status();
    }
    terminal.headers = state_->headers;
    terminal.headers.insert_or_assign(std::string(kSessionStatusHeader),
                                      std::move(*session_status));
    terminal.headers.insert_or_assign(std::string(net::kAbortStatusHeader),
                                      std::move(*stream_status));
    state_->phase = Phase::kAborted;
    state_->status = status;
    for (const auto& [unused, stream] : state_->stream_states) {
      (void)unused;
      stream->accepting_messages = false;
      if (stream->message_pump_cancel) {
        callback_cancellations.push_back(stream->message_pump_cancel);
      }
      if (!stream->done && !stream->done_started) {
        streams.push_back(stream);
      }
    }
    for (const auto& [unused, action] : state_->actions) {
      (void)unused;
      actions.push_back(action);
    }
  }
  NotifyStateChanged();
  for (auto& cancel : callback_cancellations) {
    cancel();
  }
  for (const auto& action : actions) {
    (void)action->Cancel();
  }
  absl::Status first;
  for (const auto& stream : streams) {
    absl::Status sent = stream->stream->Send(terminal);
    if (!sent.ok()) {
      KeepFirstError(sent, &first);
      (void)stream->stream->Abort(SessionStreamAbortStatus());
    }
  }
  FinishIfPossible();
  return first;
}

absl::Status Session::Send(data::WireMessage message,
                           std::string_view stream_id) {
  ABSL_RETURN_IF_ERROR(message.Validate());
  std::shared_ptr<net::WireStream> stream;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->phase != Phase::kOpen) {
      return absl::FailedPreconditionError(
          "Messages cannot be sent after the Session ends");
    }
    if (!stream_id.empty()) {
      const auto found = state_->streams.find(stream_id);
      if (found == state_->streams.end()) {
        return absl::NotFoundError("Session stream was not found");
      }
      stream = found->second->stream;
    } else {
      std::vector<std::shared_ptr<StreamState>> available;
      for (net::WireStream* candidate : state_->stream_order) {
        const auto found = state_->stream_states.find(candidate);
        if (found != state_->stream_states.end() && !found->second->done &&
            !found->second->done_started) {
          available.push_back(found->second);
        }
      }
      if (available.empty()) {
        return absl::NotFoundError("Session has no attached streams");
      }
      const size_t index = state_->round_robin_index % available.size();
      state_->round_robin_index = (index + 1) % available.size();
      stream = available[index]->stream;
    }
  }
  return stream->Send(std::move(message));
}

absl::Time Session::deadline() const {
  thread::MutexLock lock(&state_->mu);
  return state_->deadline;
}

absl::Status Session::SetDeadline(absl::Time deadline) {
  bool expired = false;
  {
    thread::MutexLock lock(&state_->mu);
    state_->deadline = deadline;
    expired = state_->phase == Phase::kOpen && deadline <= absl::Now();
  }
  NotifyStateChanged();
  return expired ? Abort(absl::DeadlineExceededError(
                       "The Session deadline has been exceeded"))
                 : absl::OkStatus();
}

struct SessionWithRecv::ReceiveState {
  thread::Mutex mu;
  std::deque<std::optional<ReceivedSessionMessage>> queue ABSL_GUARDED_BY(mu);
  std::optional<absl::Status> error ABSL_GUARDED_BY(mu);
  absl::flat_hash_set<net::WireStream*> ended_streams ABSL_GUARDED_BY(mu);
  bool session_half_closed ABSL_GUARDED_BY(mu) = false;
  bool eof_started ABSL_GUARDED_BY(mu) = false;
  bool eof_delivered ABSL_GUARDED_BY(mu) = false;
  std::shared_ptr<thread::PermanentEvent> changed ABSL_GUARDED_BY(mu) =
      std::make_shared<thread::PermanentEvent>();
};

absl::StatusOr<std::shared_ptr<SessionWithRecv>> SessionWithRecv::Create(
    std::string session_id, data::ByteMap headers, SessionOptions options,
    std::shared_ptr<nodes::NodeMap> node_map,
    std::shared_ptr<actions::ActionRegistry> action_registry) {
  auto session = std::make_shared<SessionWithRecv>();
  session->receive_state_ = std::make_shared<ReceiveState>();
  std::weak_ptr<SessionWithRecv> weak = session;
  OnSessionStreamMessage on_message =
      [weak](std::optional<data::WireMessage> message,
             std::shared_ptr<net::WireStream> stream,
             const std::shared_ptr<Session>&) {
        std::shared_ptr<SessionWithRecv> self = weak.lock();
        return self != nullptr
                   ? self->OnMessage(std::move(message), std::move(stream))
                   : a11::ReadyTask();
      };
  OnSessionStreamDone on_done = [weak](std::shared_ptr<net::WireStream> stream,
                                       const std::shared_ptr<Session>&) {
    std::shared_ptr<SessionWithRecv> self = weak.lock();
    return self != nullptr ? self->OnDone(std::move(stream)) : a11::ReadyTask();
  };
  ABSL_RETURN_IF_ERROR(
      session->Initialize(session, std::move(session_id), std::move(on_message),
                          std::move(on_done), std::move(headers), options,
                          std::move(node_map), std::move(action_registry)));
  return session;
}

a11::Task SessionWithRecv::OnMessage(std::optional<data::WireMessage> message,
                                     std::shared_ptr<net::WireStream> stream) {
  std::shared_ptr<SessionWithRecv> self =
      std::static_pointer_cast<SessionWithRecv>(shared_from_this());
  return a11::SubmitTask([self = std::move(self), message = std::move(message),
                          stream =
                              std::move(stream)]() mutable -> absl::Status {
    bool carries_session_close = false;
    std::vector<std::pair<std::string, std::shared_ptr<net::WireStream>>>
        attached;
    if (!message.has_value()) {
      const std::optional<data::ByteMap> trailers = stream->GetTrailers();
      carries_session_close =
          trailers.has_value() &&
          trailers->find(kSessionStatusHeader) != trailers->end();
      ABSL_ASSIGN_OR_RETURN(attached, self->Streams());
    }
    while (true) {
      std::shared_ptr<thread::PermanentEvent> changed;
      std::shared_ptr<thread::PermanentEvent> notify;
      bool queued = false;
      {
        thread::MutexLock lock(&self->receive_state_->mu);
        if (self->receive_state_->error.has_value()) {
          return absl::OkStatus();
        }
        if (!message.has_value()) {
          self->receive_state_->ended_streams.insert(stream.get());
          if (carries_session_close) {
            self->receive_state_->session_half_closed = true;
          }
          if (!self->receive_state_->session_half_closed ||
              self->receive_state_->eof_started) {
            return absl::OkStatus();
          }
          bool all_ended = true;
          for (const auto& entry : attached) {
            if (self->receive_state_->ended_streams.find(entry.second.get()) ==
                self->receive_state_->ended_streams.end()) {
              all_ended = false;
              break;
            }
          }
          if (!all_ended) {
            return absl::OkStatus();
          }
        }
        if (self->receive_state_->queue.empty()) {
          if (message.has_value()) {
            self->receive_state_->queue.emplace_back(ReceivedSessionMessage{
                .message = std::move(*message),
                .stream_id = stream->GetId(),
            });
          } else {
            self->receive_state_->queue.emplace_back(std::nullopt);
            self->receive_state_->eof_started = true;
          }
          notify = std::exchange(self->receive_state_->changed,
                                 std::make_shared<thread::PermanentEvent>());
          queued = true;
        } else {
          changed = self->receive_state_->changed;
        }
      }
      if (notify != nullptr) {
        notify->Notify();
      }
      if (queued) {
        return absl::OkStatus();
      }
      if (thread::Select({thread::OnCancel(), changed->OnEvent()}) == 0) {
        return absl::CancelledError("Session receive callback cancelled");
      }
    }
  });
}

a11::Task SessionWithRecv::OnDone(std::shared_ptr<net::WireStream> stream) {
  const absl::Status status = GetStatus();
  if (!status.ok()) {
    SignalReceiveError(status);
    return a11::ReadyTask();
  }
  {
    thread::MutexLock lock(&receive_state_->mu);
    receive_state_->ended_streams.insert(stream.get());
  }
  return OnMessage(std::nullopt, std::move(stream));
}

a11::Future<std::optional<ReceivedSessionMessage>>
SessionWithRecv::ReceiveWithStreamId(absl::Time deadline) {
  std::shared_ptr<SessionWithRecv> self =
      std::static_pointer_cast<SessionWithRecv>(shared_from_this());
  return a11::Submit<std::optional<ReceivedSessionMessage>>(
      [self = std::move(self),
       deadline]() -> absl::StatusOr<std::optional<ReceivedSessionMessage>> {
        while (true) {
          std::shared_ptr<thread::PermanentEvent> changed;
          std::shared_ptr<thread::PermanentEvent> notify;
          std::optional<ReceivedSessionMessage> result;
          bool has_result = false;
          {
            thread::MutexLock lock(&self->receive_state_->mu);
            if (self->receive_state_->error.has_value()) {
              return *self->receive_state_->error;
            }
            if (self->receive_state_->eof_delivered) {
              return absl::FailedPreconditionError(
                  "Remote Session half-close was already received");
            }
            if (!self->receive_state_->queue.empty()) {
              result = std::move(self->receive_state_->queue.front());
              self->receive_state_->queue.pop_front();
              if (!result.has_value()) {
                self->receive_state_->eof_delivered = true;
              }
              has_result = true;
              notify =
                  std::exchange(self->receive_state_->changed,
                                std::make_shared<thread::PermanentEvent>());
            } else {
              changed = self->receive_state_->changed;
            }
          }
          if (notify != nullptr) {
            notify->Notify();
          }
          if (has_result) {
            return result;
          }
          const int selected = thread::SelectUntil(
              deadline, {thread::OnCancel(), changed->OnEvent()});
          if (selected == 0) {
            return absl::CancelledError("Session receive was cancelled");
          }
          if (selected < 0) {
            return absl::DeadlineExceededError(
                "The Session receive deadline has been exceeded");
          }
        }
      });
}

a11::Future<std::optional<data::WireMessage>> SessionWithRecv::Receive(
    absl::Time deadline) {
  std::shared_ptr<SessionWithRecv> self =
      std::static_pointer_cast<SessionWithRecv>(shared_from_this());
  return a11::Submit<std::optional<data::WireMessage>>(
      [self = std::move(self),
       deadline]() -> absl::StatusOr<std::optional<data::WireMessage>> {
        a11::Future<std::optional<ReceivedSessionMessage>> pending =
            self->ReceiveWithStreamId(deadline);
        absl::StatusOr<std::optional<ReceivedSessionMessage>> received =
            pending.Await();
        if (thread::Cancelled()) {
          (void)pending.Cancel();
          return absl::CancelledError("Session receive was cancelled");
        }
        if (!received.ok()) {
          return received.status();
        }
        if (!received->has_value()) {
          return std::nullopt;
        }
        return std::optional<data::WireMessage>(
            std::move(received->value().message));
      });
}

void SessionWithRecv::SignalReceiveError(absl::Status status) {
  if (status.ok()) {
    return;
  }
  std::shared_ptr<thread::PermanentEvent> notify;
  {
    thread::MutexLock lock(&receive_state_->mu);
    if (receive_state_->error.has_value()) {
      return;
    }
    receive_state_->error = std::move(status);
    receive_state_->queue.clear();
    notify = std::exchange(receive_state_->changed,
                           std::make_shared<thread::PermanentEvent>());
  }
  notify->Notify();
}

absl::Status SessionWithRecv::Abort(absl::Status status) {
  absl::Status result = Session::Abort(std::move(status));
  const absl::Status recorded = GetStatus();
  if (!recorded.ok()) {
    SignalReceiveError(recorded);
  }
  return result;
}

}  // namespace a11::service
