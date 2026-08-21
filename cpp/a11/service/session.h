// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief A11's connection-scoped runtime: the Session.
 *
 * A `Session` multiplexes one or more `net::WireStream` transports,
 * dispatches incoming `actions::Action` calls against an
 * `actions::ActionRegistry`, and tracks their lifetimes so the connection
 * can drain and close cleanly. It is the top-level object an agent drives
 * -- server or client -- to exchange wire messages and run actions; node
 * fragments it receives are applied to a `nodes::NodeMap`.
 *
 * This header also declares `SessionOptions` (the session's limits and
 * timeouts) and `SessionWithRecv`, a variant that buffers inbound
 * messages for pull-style `Receive()` consumption instead of callbacks.
 */

#ifndef A11_SERVICE_SESSION_H_
#define A11_SERVICE_SESSION_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/wire_stream.h"
#include "a11/nodes/node_map.h"

namespace a11::service {

/// Trailer/header used to communicate the session's structured terminal status.
inline constexpr std::string_view kSessionStatusHeader = "x-a11-session-status";
/// Hard upper bound for any one WireMessage admitted by a Session.
inline constexpr size_t kMaxSingleMessageSize = 32 * 1024 * 1024;

/**
 * @brief Limits and timeouts governing a Session's buffering, concurrency,
 * and lifetime.
 *
 * These bounds cap how much a session buffers (per stream and in
 * aggregate, by message count and by bytes), how many actions may run
 * concurrently, and when the session gives up. Defaults are tuned for
 * typical agent workloads.
 */
struct SessionOptions {
  /// Maximum number of messages buffered across all streams.
  size_t max_buffered_messages_total = 256;
  /// Maximum number of messages buffered per stream.
  size_t max_buffered_messages_per_stream = 32;
  /// Maximum number of concurrently running root (top-level) actions.
  size_t max_concurrent_root_actions = 32;
  /// Maximum number of concurrently running nested (child) actions.
  size_t max_concurrent_nested_actions = 128;
  /// Maximum size in bytes of a single wire message.
  size_t max_single_message_size = kMaxSingleMessageSize;
  /// Maximum total bytes buffered across all streams.
  size_t max_buffered_bytes_total = 32 * 1024 * 1024;
  /// Maximum bytes buffered per stream.
  size_t max_buffered_bytes_per_stream = 4 * 1024 * 1024;
  /// How long the session waits with no active stream before finishing.
  absl::Duration no_stream_timeout = absl::Seconds(30);
  /// Absolute time after which the session is aborted.
  absl::Time deadline = absl::InfiniteFuture();

  /**
   * @brief Validate the option values.
   * @return OK if the configuration is valid, otherwise an error status.
   */
  absl::Status Validate() const;
};

class Session;

/// Callback invoked for each message received on a session stream (nullopt
/// signals end-of-stream); may be a coroutine.
using OnSessionStreamMessage = std::function<a11::Task(
    std::optional<data::WireMessage>, std::shared_ptr<net::WireStream>,
    std::shared_ptr<Session>)>;
/// Callback invoked once a session stream has finished; may be a coroutine.
using OnSessionStreamDone = std::function<a11::Task(
    std::shared_ptr<net::WireStream>, std::shared_ptr<Session>)>;

/// Whether this side starts (`kStart`) or accepts (`kAccept`) a stream
/// during its startup handshake.
enum class StreamMode { kStart, kAccept };

/**
 * @brief A connection-scoped runtime that multiplexes wire streams and
 * runs actions.
 *
 * A session attaches one or more `net::WireStream` transports, pumps their
 * messages, dispatches incoming action calls against its
 * `actions::ActionRegistry`, and applies node fragments to its
 * `nodes::NodeMap`. It tracks every stream and action so the connection
 * can drain and close cleanly. Streams may deliver messages to the
 * optional message/done callbacks. Instances are heap-allocated and shared
 * via `Create`.
 */
class Session : public std::enable_shared_from_this<Session> {
 public:
  /**
   * @brief Create a session.
   * @param session_id Unique identifier; generated when empty.
   * @param on_stream_message Optional per-message callback (may be a
   *   coroutine).
   * @param on_stream_done Optional stream-finished callback (may be a
   *   coroutine).
   * @param headers Session-level headers.
   * @param options Limits and timeouts governing the session.
   * @param node_map Node registry backing this session's node state; a
   *   fresh one is created when null.
   * @param action_registry Registry resolving incoming action messages; may
   *   be null.
   * @return The new session, or an error status on failure.
   */
  static absl::StatusOr<std::shared_ptr<Session>> Create(
      std::string session_id = {},
      OnSessionStreamMessage on_stream_message = {},
      OnSessionStreamDone on_stream_done = {}, data::ByteMap headers = {},
      SessionOptions options = {},
      std::shared_ptr<nodes::NodeMap> node_map = nullptr,
      std::shared_ptr<actions::ActionRegistry> action_registry = nullptr);

  virtual ~Session() = default;

  /**
   * @brief Return the streams currently attached to the session.
   * @return A snapshot of (stream_id, stream) pairs; streams come and go
   *   asynchronously, so treat it as point-in-time.
   */
  absl::StatusOr<
      std::vector<std::pair<std::string, std::shared_ptr<net::WireStream>>>>
  Streams() const;

  /**
   * @brief Look up an attached stream by id.
   * @param stream_id The stream identifier.
   * @return The stream, or an error status if no such stream exists (it may
   *   have been removed since last observed).
   */
  absl::StatusOr<std::shared_ptr<net::WireStream>> GetStream(
      std::string_view stream_id) const;

  /**
   * @brief Return the session's unique identifier.
   * @return The id string; use it to correlate the session with logs and
   *   traces.
   */
  [[nodiscard]] std::string GetId() const;

  /**
   * @brief Return the NodeMap backing this session's node state.
   * @return The node map dispatched fragments are applied to.
   */
  [[nodiscard]] std::shared_ptr<nodes::NodeMap> GetNodeMap() const;

  /**
   * @brief Replace the NodeMap backing this session's node state.
   *
   * Active actions are rebound, but fragments already stored in the previous
   * map are not migrated. Configure this before traffic to avoid splitting a
   * live action's state between maps.
   *
   * @param node_map The new node map.
   * @return OK, or an error status on failure.
   */
  absl::Status SetNodeMap(const std::shared_ptr<nodes::NodeMap>& node_map);

  /**
   * @brief Return the registry used to resolve incoming action messages.
   * @return The action registry (may be null).
   */
  [[nodiscard]] std::shared_ptr<actions::ActionRegistry> GetActionRegistry()
      const;

  /**
   * @brief Replace the registry used to resolve incoming action messages.
   *
   * Active actions are rebound for subsequent nested-name resolution. Prefer
   * configuring this before dispatch so one operation does not observe
   * registrations from different registry versions.
   *
   * @param registry The new registry.
   * @return OK, or an error status on failure.
   */
  absl::Status SetActionRegistry(
      const std::shared_ptr<actions::ActionRegistry>& registry);

  /**
   * @brief Return the actions currently running in the session.
   * @return A point-in-time snapshot of (action_id, action) pairs of
   *   in-flight work.
   */
  [[nodiscard]] std::vector<
      std::pair<std::string, std::shared_ptr<actions::Action>>>
  Actions() const;

  /**
   * @brief Look up a running action by id.
   * @param action_id The action identifier.
   * @return The action, or an error status if none matches.
   */
  absl::StatusOr<std::shared_ptr<actions::Action>> GetAction(
      std::string_view action_id) const;

  /**
   * @brief Request cancellation of a running action.
   * @param action_id The action identifier.
   * @return OK, or an error status if the action is unknown. Cancellation
   *   is cooperative and completes asynchronously.
   */
  absl::Status CancelAction(std::string_view action_id);

  /**
   * @brief Request cancellation of every running action.
   * @return OK, or an error status on failure; each action unwinds
   *   asynchronously.
   */
  absl::Status CancelAllActions();

  /**
   * @brief Wait for all in-flight actions to finish.
   * @param timeout How long to wait before giving up.
   * @return An awaitable that resolves once all actions have finished or
   *   the timeout elapses.
   */
  a11::Task AwaitAllActions(absl::Duration timeout = absl::InfiniteDuration());

  /**
   * @brief Dispatch a node fragment into the session's NodeMap.
   * @param fragment The fragment to apply; fragments are applied in order.
   * @return An awaitable that resolves to the applied revision.
   */
  a11::Future<std::uint32_t> DispatchNodeFragment(data::NodeFragment fragment);

  /**
   * @brief Resolve an action message against the registry and run it.
   * @param message The action message to dispatch.
   * @param origin_stream Optional stream the message is attributed to.
   * @return An awaitable that resolves once the action has been handled.
   */
  a11::Task DispatchActionMessage(
      data::ActionMessage message,
      std::shared_ptr<net::WireStream> origin_stream = nullptr);

  /**
   * @brief Run an already-constructed action within the session.
   * @param action The action to dispatch programmatically.
   * @return An awaitable that resolves once the action has been handled.
   */
  a11::Task DispatchAction(const std::shared_ptr<actions::Action>& action);

  /**
   * @brief Route a wire message through the session as if it arrived on a
   * stream.
   * @param message The message to process.
   * @param origin_stream Optional stream the message is attributed to.
   * @return An awaitable that resolves once the message has been processed.
   */
  a11::Task DispatchWireMessage(
      data::WireMessage message,
      std::shared_ptr<net::WireStream> origin_stream = nullptr);

  /**
   * @brief Report whether the session has been closed.
   * @return True if it no longer accepts new streams or messages.
   */
  [[nodiscard]] bool IsClosed() const;

  /**
   * @brief Report whether the session has fully finished.
   * @return True once every stream and action has completed; prefer
   *   awaiting `Done()` over polling this.
   */
  [[nodiscard]] bool IsDone() const;

  /**
   * @brief Await the session's full completion.
   * @return An awaitable that resolves once every stream and action has
   *   finished.
   */
  [[nodiscard]] a11::Task Done() const;

  /**
   * @brief Return the session's terminal status.
   * @return Whether it completed successfully or was aborted.
   */
  [[nodiscard]] absl::Status GetStatus() const;

  /**
   * @brief Attach a wire stream and begin pumping its messages.
   * @param stream The transport to attach.
   * @param mode Whether this side starts or accepts the stream.
   * @return An awaitable that resolves once the stream's startup handshake
   *   completes, or an error status on failure.
   */
  absl::StatusOr<a11::Task> AddStream(std::shared_ptr<net::WireStream> stream,
                                      StreamMode mode = StreamMode::kStart);

  /**
   * @brief Signal that this side will send no more messages.
   * @return OK, or an error status on failure. The session drains and
   *   finishes once peers do the same; inbound messages keep processing.
   */
  absl::Status HalfClose();

  /**
   * @brief Abort the session immediately, cancelling streams and actions.
   * @param status The error to abort with.
   * @return OK, or an error status on failure.
   */
  virtual absl::Status Abort(absl::Status status);

  /**
   * @brief Enqueue a wire message for delivery.
   * @param message The message to send.
   * @param stream_id Target stream; the default stream when empty.
   * @return OK, or an error status on failure. Delivery happens
   *   asynchronously as the stream drains.
   */
  absl::Status Send(data::WireMessage message, std::string_view stream_id = {});

  /**
   * @brief Return the absolute deadline after which the session is aborted.
   * @return The current deadline.
   */
  [[nodiscard]] absl::Time deadline() const;

  /**
   * @brief Set the absolute deadline after which the session is aborted.
   * @param deadline The new deadline; the infinite future clears it.
   * @return OK, or an error status on failure.
   */
  absl::Status SetDeadline(absl::Time deadline = absl::InfiniteFuture());

 protected:
  Session() = default;
  absl::Status Initialize(
      const std::shared_ptr<Session>& self, std::string session_id,
      OnSessionStreamMessage on_stream_message,
      OnSessionStreamDone on_stream_done, data::ByteMap headers,
      SessionOptions options, std::shared_ptr<nodes::NodeMap> node_map,
      std::shared_ptr<actions::ActionRegistry> action_registry);

 private:
  struct State;
  struct StreamState;
  std::shared_ptr<State> state_;

  a11::Task HandleStreamMessage(
      const std::shared_ptr<StreamState>& stream_state,
      std::optional<data::WireMessage> message);
  void ProcessStreamMessages(const std::shared_ptr<StreamState>& stream_state);
  a11::Task HandleStreamDone(const std::shared_ptr<StreamState>& stream_state);
  void RemoveStream(const std::shared_ptr<StreamState>& stream_state);
  void FinishIfPossible();
  void NotifyStateChanged();

  absl::Status TrackAction(const std::shared_ptr<actions::Action>& action);
  void UntrackAction(const std::shared_ptr<actions::Action>& action);
  std::shared_ptr<actions::ActionLimiter> GetActionLimiter(bool nested) const;

  friend class actions::Action;
};

/// An inbound wire message paired with the id of the stream it arrived on.
struct ReceivedSessionMessage {
  data::WireMessage message;  ///< Received application message.
  std::string stream_id;      ///< Id of the transport that delivered it.
};

/**
 * @brief A Session variant that buffers inbound messages for pull-style
 * reception.
 *
 * Instead of delivering messages through callbacks, this session buffers
 * them so the caller can await them explicitly via `Receive()` /
 * `ReceiveWithStreamId()`. Suits agents that consume messages in their own
 * loop.
 */
class SessionWithRecv final : public Session {
 public:
  /**
   * @brief Create a pull-style session.
   * @param session_id Unique identifier; generated when empty.
   * @param headers Session-level headers.
   * @param options Limits and timeouts governing the session.
   * @param node_map Node registry backing the session; a fresh one is
   *   created when null.
   * @param action_registry Registry resolving incoming action messages; may
   *   be null.
   * @return The new session, or an error status on failure.
   */
  static absl::StatusOr<std::shared_ptr<SessionWithRecv>> Create(
      std::string session_id = {}, data::ByteMap headers = {},
      SessionOptions options = {},
      std::shared_ptr<nodes::NodeMap> node_map = nullptr,
      std::shared_ptr<actions::ActionRegistry> action_registry = nullptr);

  /**
   * @brief Await the next inbound message together with its stream id.
   * @param deadline Absolute time to stop waiting.
   * @return An awaitable resolving to the next message and its stream id,
   *   or nullopt when the session finishes.
   */
  a11::Future<std::optional<ReceivedSessionMessage>> ReceiveWithStreamId(
      absl::Time deadline = absl::InfiniteFuture());

  /**
   * @brief Await the next inbound wire message.
   * @param deadline Absolute time to stop waiting.
   * @return An awaitable resolving to the next message, or nullopt when the
   *   session finishes.
   */
  a11::Future<std::optional<data::WireMessage>> Receive(
      absl::Time deadline = absl::InfiniteFuture());

  /**
   * @brief Abort the session, also failing any pending receivers.
   * @param status The error to abort with.
   * @return OK, or an error status on failure.
   */
  absl::Status Abort(absl::Status status) override;

 private:
  struct ReceiveState;
  std::shared_ptr<ReceiveState> receive_state_;

  a11::Task OnMessage(std::optional<data::WireMessage> message,
                      std::shared_ptr<net::WireStream> stream);
  a11::Task OnDone(std::shared_ptr<net::WireStream> stream);
  void SignalReceiveError(absl::Status status);
};

/**
 * @brief Validate and case-normalize session headers.
 * @param headers The raw headers to normalize.
 * @return The normalized headers, or an error status if invalid.
 */
absl::StatusOr<data::ByteMap> NormalizeSessionHeaders(data::ByteMap headers);

}  // namespace a11::service

#endif  // A11_SERVICE_SESSION_H_
