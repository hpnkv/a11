// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief A11's unit of work: the Action and its supporting types.
 *
 * An a11::actions::Action is a named, schema-described operation with typed
 * input and output ports -- each an a11::nodes::AsyncNode -- and a handler
 * that runs asynchronously, streaming results into its outputs as they are
 * produced. Actions compose: a handler can create nested actions (MakeNested)
 * and run or call them. An action can run locally against in-process nodes
 * (Run / run-in-background) or be dispatched across an a11::net::WireStream to
 * a peer that executes it (Call). The various @c Bind* methods wire an action
 * up to the collaborators it needs -- a handler, node map, stream, registry
 * and session.
 */

#ifndef A11_ACTIONS_ACTION_H_
#define A11_ACTIONS_ACTION_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/base/thread_annotations.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_format.h>
#include <absl/time/time.h>

#include "a11/actions/log.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/future.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/obs/span.h"
#include "thread/boost_primitives.h"

namespace thread {
class PermanentEvent;
}  // namespace thread

namespace a11::net {
class WireStream;
}  // namespace a11::net

namespace a11::nodes {
class AsyncNode;
class NodeMap;
}  // namespace a11::nodes

namespace a11::service {
class Session;
}  // namespace a11::service

namespace a11::actions {

class Action;
class ActionRegistry;

/** @brief Default cap on concurrently running nested actions. */
inline constexpr size_t kDefaultMaxConcurrentNestedActions = 64;

/** @brief Asynchronous action handler: runs the action, returns an awaitable. */
using ActionHandler = std::function<a11::Task(std::shared_ptr<Action>)>;
/** @brief Synchronous action handler returning a completion status. */
using SyncActionHandler = std::function<absl::Status(std::shared_ptr<Action>)>;
/** @brief Callback invoked when an action is cancelled. */
using OnActionCancelled = std::function<absl::Status(std::shared_ptr<Action>)>;

/** @brief Adapts a synchronous handler into an asynchronous ActionHandler. */
ActionHandler MakeAsyncActionHandler(SyncActionHandler handler);

/**
 * @brief Cancellation-aware counting semaphore for nested-action concurrency.
 *
 * Bounds how many nested actions a root (or its descendants) may run at once;
 * Acquire blocks until a slot is free and unblocks if the owning action is
 * cancelled.
 */
class ActionLimiter {
 public:
  /** @brief Creates a limiter admitting at most @p maximum holders. */
  static absl::StatusOr<std::shared_ptr<ActionLimiter>> Create(size_t maximum);
  /** @brief Acquires a slot, blocking until one is free or cancelled. */
  absl::Status Acquire();
  /** @brief Releases a previously acquired slot. */
  void Release();

 private:
  explicit ActionLimiter(size_t maximum);

  thread::Mutex mu_;
  const size_t maximum_;
  size_t active_ ABSL_GUARDED_BY(mu_) = 0;
  std::shared_ptr<thread::PermanentEvent> changed_ ABSL_GUARDED_BY(mu_);
};

/**
 * @brief A11's unit of work: a schema-described, asynchronously run operation.
 *
 * An action pairs an a11::actions::ActionSchema with a handler and a set of
 * typed input/output ports (each an a11::nodes::AsyncNode). Running the action
 * invokes its handler, which reads from the inputs and streams into the
 * outputs. The same action can either run locally (Run) or be dispatched to a
 * peer over a bound a11::net::WireStream (Call). Actions are always held by
 * @c std::shared_ptr and compose recursively via MakeNested.
 */
class Action : public std::enable_shared_from_this<Action> {
 public:
  /**
   * @brief Creates an action.
   * @param schema Schema describing ports, headers and outputs.
   * @param action_id Instance id; generated when empty.
   * @param handler Handler run by Run/Call; may be bound later.
   * @param node_map Node map backing the action's ports.
   * @param stream Optional wire stream enabling remote Call.
   * @param session Optional owning session.
   * @param registry Optional registry used to resolve nested actions.
   * @param max_concurrent_nested_actions Cap on concurrent nested actions.
   * @return The new action, or an error when the schema is invalid.
   */
  static absl::StatusOr<std::shared_ptr<Action>> Create(
      ActionSchema schema, std::string action_id = {},
      ActionHandler handler = {},
      std::shared_ptr<nodes::NodeMap> node_map = nullptr,
      std::shared_ptr<net::WireStream> stream = nullptr,
      std::shared_ptr<service::Session> session = nullptr,
      std::shared_ptr<ActionRegistry> registry = nullptr,
      size_t max_concurrent_nested_actions =
          kDefaultMaxConcurrentNestedActions);

  /** @brief Derives the node id for port @p node_name of action @p action_id. */
  static absl::StatusOr<std::string> MakeNodeId(std::string_view action_id,
                                                std::string_view node_name);

  /** @brief Returns this action's instance id. */
  [[nodiscard]] std::string GetId() const;
  /** @brief Sets this action's instance id. */
  absl::Status SetId(std::string action_id);
  /** @brief Returns this action's schema. */
  [[nodiscard]] ActionSchema GetSchema() const;
  /** @brief Replaces this action's schema (validated). */
  absl::Status SetSchema(ActionSchema schema);
  /** @brief Binds the handler invoked when the action runs. */
  absl::Status BindHandler(ActionHandler handler);
  /** @brief Returns the currently bound handler. */
  [[nodiscard]] ActionHandler GetHandler() const;
  /** @brief Whether a handler is bound. */
  [[nodiscard]] bool HasHandler() const;

  /** @brief Returns the action's current settings. */
  [[nodiscard]] ActionSettings GetSettings() const;
  /** @brief Replaces the action's settings. */
  absl::Status SetSettings(ActionSettings settings);
  /** @brief Sets whether input port streams are bound by default. */
  absl::Status BindStreamsOnInputsByDefault(bool bind);
  /** @brief Sets whether output port streams are bound by default. */
  absl::Status BindStreamsOnOutputsByDefault(bool bind);
  /** @brief Sets whether inputs are released after each run. */
  absl::Status ClearInputsAfterRun(bool clear = true);
  /** @brief Sets whether outputs are released after each run. */
  absl::Status ClearOutputsAfterRun(bool clear = true);

  /** @brief Binds the node map that backs this action's ports. */
  absl::Status BindNodeMap(std::shared_ptr<nodes::NodeMap> node_map);
  /** @brief Returns the bound node map. */
  [[nodiscard]] std::shared_ptr<nodes::NodeMap> GetNodeMap() const;
  /** @brief Binds the wire stream used to dispatch the action remotely. */
  absl::Status BindStream(std::shared_ptr<net::WireStream> stream);
  /** @brief Returns the bound wire stream. */
  [[nodiscard]] std::shared_ptr<net::WireStream> GetStream() const;
  /** @brief Binds the registry used to resolve nested actions by name. */
  absl::Status BindRegistry(std::shared_ptr<ActionRegistry> registry);
  /** @brief Returns the bound registry. */
  [[nodiscard]] std::shared_ptr<ActionRegistry> GetRegistry() const;
  /** @brief Binds the owning session. */
  absl::Status BindSession(const std::shared_ptr<service::Session>& session);
  /** @brief Returns the owning session, if any. */
  [[nodiscard]] std::shared_ptr<service::Session> GetSession() const;

  /** @brief Returns the port node with raw id @p node_id. */
  absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> GetNode(
      std::string node_id);
  /**
   * @brief Returns the input port node named @p name.
   * @param name Input port name from the schema.
   * @param bind_stream Override for whether the node's stream is bound.
   * @return The port's node, or an error when @p name is not an input.
   */
  absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> GetInput(
      const std::string& name, std::optional<bool> bind_stream = std::nullopt);
  /**
   * @brief Returns the output port node named @p name.
   * @param name Output port name from the schema.
   * @param bind_stream Override for whether the node's stream is bound.
   * @return The port's node, or an error when @p name is not an output.
   */
  absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> GetOutput(
      const std::string& name, std::optional<bool> bind_stream = std::nullopt);
  /** @brief Returns the input or output port node named @p name. */
  absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> GetPort(
      const std::string& name);
  /** @brief Whether the schema declares a port named @p name. */
  [[nodiscard]] bool ContainsPort(std::string_view name) const;

  /**
   * @brief Logs @p value on the reserved ::kActionLogOutput port.
   *
   * The object is converted into a ::a11::data::Chunk similar to
   * ::a11::nodes::AsyncNode::Put, mapping string types (@c std::string,
   * @c std::string_view, string literals, and @c const @c char*) to
   * ::a11::data::kTextMimetype. @p options carries the
   * level, the media type, the channel, the source location and any extra
   * metadata; see a11::actions::LogOptions. The chunk's timestamp is always
   * set.
   *
   * Only a running handler may log. Logging before Run, from the calling side
   * of a Call, or after a cancellation that beat the start returns
   * @c FailedPrecondition -- the log would have nowhere to go and no reader to
   * close it. Nothing else about logging can fail the action: once the chunk is
   * built, a transport or lifecycle failure is reported through the sink rather
   * than returned, so an action never fails because it narrated itself.
   *
   * Where the log goes: always to the process's a11::actions::ActionLogSink,
   * and additionally onto the log port when something could read it -- a peer
   * is attached, or a local consumer claimed the port with GetLogNode. Nobody
   * has to drain it and nobody has to close it; the action closes it with its
   * other outputs.
   *
   * @param value Object to log.
   * @param options Level, media type, channel, location and extra metadata.
   * @return OK once the log has been reported, or an error when the action is
   *         not running or @p value cannot be serialized.
   */
  template <typename T>
  absl::Status Log(const T& value, const LogOptions& options = {});
  /**
   * @brief Logs an already-built chunk, keeping its mimetype and data.
   *
   * LogOptions::mimetype must be empty here: the chunk already says what it is,
   * and two answers to that question is a bug rather than a precedence rule.
   */
  absl::Status Log(data::Chunk chunk, const LogOptions& options = {});
  /**
   * @brief Logs a formatted string, as ::absl::StrFormat would format it.
   *
   * @code
   *   action->Logf("read %d of %d pages", done, total);
   *   action->LogfWith({.level = "warning", .channel = "fetch"},
   *                    "retrying %s", url);
   * @endcode
   *
   * The format is checked at compile time, so it must be a literal or an
   * ::absl::ParsedFormat; to log a string computed at runtime, use Log.
   */
  template <typename... Args>
  absl::Status Logf(const absl::FormatSpec<Args...>& format,
                    const Args&... args);
  /**
   * @brief Logs a formatted string with explicit options.
   *
   * A second name rather than a leading-LogOptions overload of Logf, because
   * ::absl::FormatSpec has a catch-all constructor to diagnose a non-constexpr
   * format: it makes @c Logf(options, ...) match both overloads, one better in
   * its first argument and the other in its second, which is ambiguous. Keeping
   * the format spec a direct parameter is what keeps the format checked at
   * compile time, so the name gives way rather than the checking.
   */
  template <typename... Args>
  absl::Status LogfWith(const LogOptions& options,
                        const absl::FormatSpec<Args...>& format,
                        const Args&... args);
  /**
   * @brief Returns the log port's node, claiming it for this consumer.
   *
   * Claiming suppresses the process sink for this action, so a consumer that
   * presents the logs itself does not also have them reported twice. Claim
   * before the action runs: logs written earlier have already gone to the sink.
   *
   * The node's stream is not bound: on the calling side, binding an output
   * would echo received fragments back to the peer.
   */
  absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> GetLogNode();

  /** @brief Returns the wire ::a11::data::ActionMessage describing this action. */
  [[nodiscard]] data::ActionMessage GetActionMessage() const;
  /** @brief Binds this action's ports to the nodes named in @p message. */
  absl::Status MapPortsFromMessage(const data::ActionMessage& message);

  /** @brief Returns a copy of all headers. */
  [[nodiscard]] data::ByteMap Headers() const;
  /** @brief Returns header @p name, or nullopt when absent. */
  absl::StatusOr<std::optional<data::Bytes>> GetHeader(
      std::string_view name) const;
  /** @brief Whether header @p name is set. */
  [[nodiscard]] bool HasHeader(std::string_view name) const;
  /** @brief Sets header @p name to @p value. */
  absl::Status SetHeader(const std::string& name, data::Bytes value);
  /** @brief Removes header @p name. */
  absl::Status RemoveHeader(std::string_view name);
  /** @brief Copies header @p name from this action onto @p target. */
  absl::Status ForwardHeader(const std::shared_ptr<Action>& target,
                             std::string_view name) const;
  /** @brief Copies all headers starting with @p prefix onto @p target. */
  absl::Status ForwardHeadersWithPrefix(
      const std::shared_ptr<Action>& target,
      std::string_view prefix = kActionHeaderPrefix) const;

  /**
   * @brief Creates a nested action from a schema, parented to this action.
   *
   * The child always receives a new action id and therefore new derived port
   * ids. With @p propagate_io it shares this action's NodeMap, stream, and
   * Session; it does not copy this action's port mappings. Registry and nested
   * concurrency context are inherited in either mode.
   *
   * @param schema Schema for the nested action.
   * @param propagate_io Share this action's NodeMap, stream, and Session.
   * @param forward_headers Copy this action's framework headers to the child.
   * @return The nested action.
   */
  absl::StatusOr<std::shared_ptr<Action>> MakeNested(
      const ActionSchema& schema, bool propagate_io = true,
      bool forward_headers = true);
  /**
   * @brief Creates a nested action by name from the bound registry.
   *
   * The child has its own id and port ids. With @p propagate_io it shares this
   * action's NodeMap, stream, and Session, but not its port mappings.
   *
   * @param action_name Registered action to instantiate.
   * @param propagate_io Share this action's NodeMap, stream, and Session.
   * @param forward_headers Copy this action's framework headers to the child.
   * @return The nested action, or NotFound when @p action_name is unknown.
   */
  absl::StatusOr<std::shared_ptr<Action>> MakeNested(
      std::string_view action_name, bool propagate_io = true,
      bool forward_headers = true);

  /**
   * @brief Runs the action's handler locally.
   *
   * Starts the handler and returns the action immediately; use Wait or the
   * completion status to observe the outcome. (The Python binding also exposes
   * this as @c run_in_background.)
   *
   * @return This action, or an error when it cannot be started.
   */
  absl::StatusOr<std::shared_ptr<Action>> Run();
  /**
   * @brief Dispatches the action to a peer over the bound wire stream.
   * @param wire_headers Extra headers to send with the dispatch.
   * @return An awaitable resolving to this action once dispatch is accepted.
   */
  a11::Future<std::shared_ptr<Action>> Call(data::ByteMap wire_headers = {});
  /**
   * @brief Awaits acceptance of a remote dispatch.
   * @param timeout Maximum time to wait.
   * @return An awaitable resolving to the dispatch status.
   */
  a11::Future<absl::Status> WaitForDispatch(
      absl::Duration timeout = absl::InfiniteDuration());
  /**
   * @brief Awaits completion of the action.
   * @param timeout Maximum time to wait.
   * @return An awaitable resolving to this action when it finishes (or fails
   *         with the action's error status).
   */
  a11::Future<std::shared_ptr<Action>> Wait(
      absl::Duration timeout = absl::InfiniteDuration());
  /** @brief Requests cancellation of the action (local or remote). */
  absl::Status Cancel();
  /** @brief Registers a callback invoked when the action is cancelled. */
  absl::Status SetOnCancelled(OnActionCancelled callback);

  /**
   * @brief This action's trace id as lowercase hex.
   *
   * Empty when the action is not traced (no OTel context / tracing not
   * configured). Valid once the action has started and until it finishes.
   */
  [[nodiscard]] std::string TraceId() const;
  /** @brief This action's span id as lowercase hex, or empty when untraced. */
  [[nodiscard]] std::string SpanId() const;

  /**
   * @brief Sets a string attribute on this action's span.
   *
   * No-op when the action is not traced. Intended to be called from the
   * handler while the span is active (e.g. langfuse.observation.input /
   * .output).
   */
  void SetSpanAttribute(std::string_view key, std::string_view value);
  /** @brief Sets an integer attribute on this action's span. */
  void SetSpanAttribute(std::string_view key, std::int64_t value);
  /** @brief Sets a boolean attribute on this action's span. */
  void SetSpanAttribute(std::string_view key, bool value);
  /** @brief Sets a floating-point attribute on this action's span. */
  void SetSpanAttribute(std::string_view key, double value);
  /** @brief Overrides the display name of this action's span. */
  void SetSpanName(std::string_view name);
  /**
   * @brief Sets the span status explicitly.
   *
   * Suppresses the automatic status the framework would otherwise record from
   * the action's completion status.
   */
  void SetSpanStatus(obs::SpanStatus status, std::string_view description = {});

  /** @brief Whether the action has finished (successfully or not). */
  [[nodiscard]] bool IsDone() const;
  /** @brief Whether the action has been started with Run. */
  [[nodiscard]] bool HasBeenRun() const;
  /** @brief Whether the action has been dispatched with Call. */
  [[nodiscard]] bool HasBeenCalled() const;
  /** @brief Whether cancellation has been requested/applied. */
  [[nodiscard]] bool Cancelled() const;
  /** @brief The action's completion status (OK while still running). */
  [[nodiscard]] absl::Status GetStatus() const;
  /** @brief The remote dispatch status, or nullopt when not (yet) dispatched. */
  [[nodiscard]] std::optional<absl::Status> GetDispatchStatus() const;

 private:
  enum class Mode { kNone, kRun, kCall, kCancelled };

  Action(ActionSchema schema, std::string id, ActionHandler handler,
         std::shared_ptr<nodes::NodeMap> node_map,
         std::shared_ptr<net::WireStream> stream,
         const std::shared_ptr<service::Session>& session,
         std::shared_ptr<ActionRegistry> registry,
         std::shared_ptr<ActionLimiter> nested_limiter);

  absl::Status Begin(Mode mode);
  absl::Status RemapDefaultPorts() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  absl::Status AttachStreamIfRequested(
      const std::shared_ptr<nodes::AsyncNode>& node, bool bind);
  absl::Status ValidateMessagePorts(
      const std::vector<data::Port>& ports,
      const absl::flat_hash_map<std::string, ActionPortSchema>& schema_ports,
      std::string_view kind) const;
  void RunHandler(const std::shared_ptr<ActionLimiter>& limiter);
  absl::Status ApplyInputAutofills();
  [[nodiscard]] std::vector<data::NodeFragment> CollectAutofillFragments()
      const;
  void StartFinish(absl::Status status);
  absl::Status FinishRun(absl::Status status);
  // Whether any input port declares autofills, which are written before the
  // handler runs and so must not happen on the caller's thread.
  [[nodiscard]] bool HasInputAutofills() const;
  // Starts the handler on the calling thread and continues from its task
  // without a fibre. Only valid when nothing before the handler can block.
  absl::StatusOr<std::shared_ptr<Action>> RunHandlerWithoutFiber(
      const std::shared_ptr<Action>& self);
  absl::Status FinishOutputNodes(const absl::Status& status);
  // Applies an already-finished Action's terminal state to an output node that
  // is only being materialised now, so a late reader sees the end of the stream.
  static absl::Status CloseUnwrittenOutput(
      const std::shared_ptr<nodes::AsyncNode>& node,
      const absl::Status& status);
  // Applies `options` to `chunk`, reports it to the process sink, and writes it
  // to the log port when anything could read it. The one place the log policy
  // lives; the Log/Logf templates only build the chunk.
  absl::Status WriteLog(data::Chunk chunk, const LogOptions& options);
  absl::Status CommunicateStatus(const absl::Status& status);
  absl::Status AbortInputs(const absl::Status& status);
  absl::Status SendNodeAbortStatuses(
      const absl::flat_hash_set<std::string>& node_ids,
      const absl::Status& status);
  absl::Status ReleaseNodesAfterRun();
  absl::Status DetachBoundStreamNodes();
  absl::Status SendRemoteCancel();
  void CompleteCall(const absl::Status& status, bool remove_from_session);
  void AbortLocalCallOutputs(const absl::Status& status);
  absl::Status TrackInSession(const std::shared_ptr<service::Session>& session);
  void UntrackFromSession();
  void SetDispatchStatus(absl::Status status);
  void SetCompletionStatus(const absl::Status& status);

  // Tracing hooks (a11::obs). StartActionSpan opens this action's span after a
  // successful Begin(); it fails (so the action fails) when the reserved OTel
  // headers are present but inconsistent, and is a no-op when none are present.
  // EndActionSpan closes it with the final status. MakeChildSpan lets a child
  // action open a span parented to this (parent) action's live span.
  absl::Status StartActionSpan(Mode mode);
  void EndActionSpan(const absl::Status& status);
  obs::Span MakeChildSpan(std::string_view name, obs::SpanKind kind);
  void RecordActionCallEvent(std::string_view name, std::string_view id);

  mutable thread::Mutex mu_;
  ActionSchema schema_ ABSL_GUARDED_BY(mu_);
  ActionHandler handler_ ABSL_GUARDED_BY(mu_);
  std::string id_ ABSL_GUARDED_BY(mu_);
  data::ByteMap headers_ ABSL_GUARDED_BY(mu_);
  ActionSettings settings_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<nodes::NodeMap> node_map_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<net::WireStream> stream_ ABSL_GUARDED_BY(mu_);
  std::weak_ptr<service::Session> session_ ABSL_GUARDED_BY(mu_);
  std::weak_ptr<service::Session> tracked_session_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<ActionRegistry> registry_ ABSL_GUARDED_BY(mu_);
  absl::flat_hash_map<std::string, std::string> input_ids_ ABSL_GUARDED_BY(mu_);
  absl::flat_hash_map<std::string, std::string> output_ids_
      ABSL_GUARDED_BY(mu_);
  absl::flat_hash_set<std::shared_ptr<nodes::AsyncNode>> input_nodes_
      ABSL_GUARDED_BY(mu_);
  absl::flat_hash_set<std::shared_ptr<nodes::AsyncNode>> output_nodes_
      ABSL_GUARDED_BY(mu_);
  absl::flat_hash_set<std::shared_ptr<nodes::AsyncNode>> stream_bound_nodes_
      ABSL_GUARDED_BY(mu_);
  Mode mode_ ABSL_GUARDED_BY(mu_) = Mode::kNone;
  bool input_autofills_applied_ ABSL_GUARDED_BY(mu_) = false;
  a11::Task task_ ABSL_GUARDED_BY(mu_);
  obs::Span span_ ABSL_GUARDED_BY(mu_);
  bool span_status_set_by_user_ ABSL_GUARDED_BY(mu_) = false;
  bool cancel_requested_ ABSL_GUARDED_BY(mu_) = false;
  bool finishing_ ABSL_GUARDED_BY(mu_) = false;
  std::optional<absl::Status> completion_status_ ABSL_GUARDED_BY(mu_);
  // Set once the Action has closed its output ports, with the status it closed
  // them under. Read by GetOutput() so a port materialised after that point
  // carries the same terminal state as one closed in place.
  bool outputs_finished_ ABSL_GUARDED_BY(mu_) = false;
  absl::Status outputs_final_status_ ABSL_GUARDED_BY(mu_);
  // Set once a consumer has taken the log port through GetLogNode(). It then
  // owns presentation, so WriteLog stops reporting to the process sink.
  bool log_claimed_ ABSL_GUARDED_BY(mu_) = false;
  std::optional<absl::Status> dispatch_status_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<a11::Promise<a11::Unit>> done_promise_ ABSL_GUARDED_BY(mu_);
  a11::Task done_future_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<a11::Promise<a11::Unit>> dispatch_promise_
      ABSL_GUARDED_BY(mu_);
  a11::Task dispatch_future_ ABSL_GUARDED_BY(mu_);
  std::vector<OnActionCancelled> cancel_callbacks_ ABSL_GUARDED_BY(mu_);
  std::weak_ptr<Action> parent_ ABSL_GUARDED_BY(mu_);
  absl::flat_hash_set<std::shared_ptr<Action>> children_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<ActionLimiter> nested_limiter_ ABSL_GUARDED_BY(mu_);

  friend class ActionRegistry;
  friend class service::Session;
};

namespace internal {

/**
 * @brief Whether Action::Log reads a @c T as text rather than as bytes.
 *
 * C++ does not distinguish the two -- a @c std::string is a sequence of bytes,
 * which is why ::a11::data::kBytesMimetype is what
 * ::a11::nodes::AsyncNode::Put defaults a string to. A log is the case where
 * the other reading is nearly always the intended one, so anything a
 * @c std::string_view can be made from is text here: @c std::string,
 * @c std::string_view, a literal, and @c const @c char*.
 */
template <typename T>
inline constexpr bool kLogsAsText =
    std::is_convertible_v<const T&, std::string_view>;

}  // namespace internal

template <typename T>
absl::Status Action::Log(const T& value, const LogOptions& options) {
  const data::SerializationRegistry& registry =
      data::GlobalSerializationRegistry();
  absl::StatusOr<data::Chunk> chunk;
  if constexpr (internal::kLogsAsText<T>) {
    // Normalised to std::string first: the registry has a codec for that, not
    // for a pointer or an array of char.
    const std::string text{std::string_view(value)};
    chunk = registry.ToChunk<std::string>(text, options.mimetype.empty()
                                                    ? data::kTextMimetype
                                                    : options.mimetype);
  } else {
    chunk = registry.ToChunk<T>(value, options.mimetype);
  }
  if (!chunk.ok()) {
    return chunk.status();
  }
  return WriteLog(*std::move(chunk), options);
}

template <typename... Args>
absl::Status Action::Logf(const absl::FormatSpec<Args...>& format,
                          const Args&... args) {
  return Log(absl::StrFormat(format, args...), LogOptions{});
}

template <typename... Args>
absl::Status Action::LogfWith(const LogOptions& options,
                              const absl::FormatSpec<Args...>& format,
                              const Args&... args) {
  return Log(absl::StrFormat(format, args...), options);
}

}  // namespace a11::actions

#endif  // A11_ACTIONS_ACTION_H_
