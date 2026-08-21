// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief A service: what a peer can call, decoupled from where it listens.
 *
 * A `Session` is connection-scoped, and a transport server is socket-scoped.
 * Nothing sat between them, so every application wired the two together by
 * hand -- make a registry, make a session per accepted stream, attach, wait --
 * and each copy of that glue was slightly different.
 *
 * `Service` is that glue, named. It owns the action registry and the sessions
 * built from it, and exposes exactly one join point (`Serve`) shaped to be a
 * transport's on-stream callback. What follows from having it:
 *
 * * **A service with no server.** Hand it an in-process stream pair and it
 *   serves, which is what an embedded gateway is.
 * * **One service, several servers.** A WebSocket listener, an SSE listener and
 *   a WebRTC peer can all call `Serve` on the same instance; sessions from all
 *   of them share one registry and one lifecycle.
 * * **Several services, one server.** Servers produce streams and do not care
 *   who consumes them, so a path router in front of one listener can send
 *   ``/a11`` to one service and ``/admin`` to another
 *   (see a11::net::HttpRouter).
 *
 * Deliberately *not* here: sockets, ports, TLS, or anything else a transport
 * owns. `Service` never opens a connection; a11/service/serving.h has the
 * one-line adapter that hands it to a listener.
 */

#ifndef A11_SERVICE_SERVICE_H_
#define A11_SERVICE_SERVICE_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/actions/registry.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/describe_endpoint.h"
#include "a11/net/wire_stream.h"
#include "a11/service/session.h"

namespace a11::service {

/**
 * @brief Per-connection preparation, run before the session starts pumping.
 *
 * Called once per accepted stream, after its `Session` exists and *before*
 * AddStream, which is the only window in which a connection can be specialised
 * without racing its first message. Both handles are passed because the useful
 * things need both: swapping in a registry copy carrying this peer's own
 * actions, or binding a bridge that reverse-dispatches calls back over this
 * very stream.
 *
 * A non-OK Task rejects the connection: the stream is aborted with that status
 * and the session never runs.
 */
using OnServiceConnection = std::function<a11::Task(
    std::shared_ptr<Session>, std::shared_ptr<net::WireStream>)>;

/** @brief How a service treats the connections it accepts. */
struct ServiceOptions {
  /// Limits and timeouts for every session the service creates.
  SessionOptions session_options;
  /**
   * Give each connection its own copy of the registry.
   *
   * A service whose connections specialise their registry -- registering a
   * peer's announced tools, say -- needs this, or one peer's actions become
   * callable on another's session. Leave it false when the connection hook
   * makes the copy itself, so there is exactly one copy and the hook owns it.
   */
  bool copy_registry_per_connection = false;
  /// Headers stamped on every session the service creates.
  data::ByteMap session_headers;
  /// How long ~Service and Drain() wait for live sessions before abandoning them.
  absl::Duration drain_timeout = absl::Seconds(30);
  /// Forwarded to each Session; for a service that wants raw message access.
  OnSessionStreamMessage on_stream_message;
  /// Forwarded to each Session.
  OnSessionStreamDone on_stream_done;

  /** @return OK when the options are self-consistent. */
  absl::Status Validate() const;
};

/**
 * @brief A registry of actions, the sessions serving them, and their lifecycle.
 *
 * Thread-safe. Sessions are tracked from the moment they are created until they
 * finish, so a service can be drained: it stops accepting, waits for what is in
 * flight, and only then lets go.
 */
class Service : public std::enable_shared_from_this<Service> {
 public:
  /**
   * @brief Create a service.
   * @param action_registry Registry incoming calls resolve against; an empty
   *        one is created when null.
   * @param on_connection Optional per-connection hook; see OnServiceConnection.
   * @param options Session limits, registry-copy policy, drain timeout.
   * @return The service, or InvalidArgument when @p options are inconsistent.
   */
  static absl::StatusOr<std::shared_ptr<Service>> Create(
      std::shared_ptr<actions::ActionRegistry> action_registry = nullptr,
      OnServiceConnection on_connection = {}, ServiceOptions options = {});

  ~Service();

  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  // --- what the service can do ------------------------------------------

  /** @return The template registry new connections are built from. */
  [[nodiscard]] std::shared_ptr<actions::ActionRegistry> GetActionRegistry()
      const;
  /**
   * @brief Replace the registry new connections are built from.
   *
   * No stream is interrupted, and any action message arriving after this call
   * resolves against the new registry. Live sessions keep the registry they
   * have when `ServiceOptions::copy_registry_per_connection` is set -- their
   * per-peer additions must not be clobbered -- and are re-pointed when it is
   * not. This is what makes prototyping against a running service possible.
   */
  absl::Status SetActionRegistry(
      const std::shared_ptr<actions::ActionRegistry>& action_registry);
  /** @brief Replace the per-connection hook. Affects connections from now on. */
  absl::Status SetOnConnection(OnServiceConnection on_connection);

  /**
   * @brief Describes this service's actions, for `GET /actions`.
   *
   * The same describer the `__list_actions__` builtin runs, so the endpoint and
   * the action cannot answer differently -- which is the point of routing the
   * HTTP route back here rather than giving a transport a registry of its own.
   *
   * @param name One action to describe, or empty for the whole collection.
   * @param query URL query string, without the `?`, carrying the filters.
   * @return The `a11.actions/v1` document, or NotFound for an unknown name.
   */
  absl::StatusOr<std::string> Describe(std::string_view name,
                                       std::string_view query) const;

  /**
   * @brief A net::DescribeActionsHandler bound to this service.
   *
   * Holds a weak reference, so installing it on a listener does not keep the
   * service alive past its own shutdown.
   */
  [[nodiscard]] net::DescribeActionsHandler DescribeHandler();

  // --- the sole join point with a transport ------------------------------

  /**
   * @brief Serve one stream, resolving when its session is finished.
   *
   * The shape a transport's on-stream callback wants: hold the connection open
   * for as long as it lasts. Rejects with FailedPrecondition once the service
   * has stopped accepting.
   */
  a11::Task Serve(std::shared_ptr<net::WireStream> stream,
                  StreamMode mode = StreamMode::kAccept);
  /**
   * @brief Start serving one stream and return its session immediately.
   *
   * For a caller that wants the handle -- to add a second transport to the same
   * peer, or to inspect it -- and is content for the service to own completion.
   */
  absl::StatusOr<std::shared_ptr<Session>> StartStreamHandler(
      const std::shared_ptr<net::WireStream>& stream,
      StreamMode mode = StreamMode::kAccept);
  /** @brief Attach another transport to an existing session. */
  absl::Status AddStreamToSession(
      std::string_view session_id,
      const std::shared_ptr<net::WireStream>& stream,
      StreamMode mode = StreamMode::kAccept);

  // --- who is connected -------------------------------------------------

  /** @return The ids of the sessions currently being served. */
  [[nodiscard]] std::vector<std::string> SessionIds() const;
  /** @return The session with this id, or NotFound. */
  absl::StatusOr<std::shared_ptr<Session>> GetSession(
      std::string_view session_id) const;
  /** @return The session serving this stream, or NotFound. */
  absl::StatusOr<std::shared_ptr<Session>> GetSessionForStream(
      std::string_view stream_id) const;
  /** @return How many sessions are currently being served. */
  [[nodiscard]] size_t SessionCount() const;

  // --- lifecycle --------------------------------------------------------

  /** @return Whether new connections are still admitted. */
  [[nodiscard]] bool accepting() const;
  /**
   * @brief Refuse new connections, leaving live ones alone.
   *
   * The first half of a graceful shutdown: stop accepting, then Drain.
   */
  absl::Status StopAccepting();
  /**
   * @brief Wait for live sessions to finish.
   * @param timeout How long to wait; sessions still running after it are left
   *        alone (use Abort to end them).
   * @return A Task resolving when no sessions remain, or DeadlineExceeded.
   */
  a11::Task Drain(absl::Duration timeout = absl::InfiniteDuration());
  /** @brief Stop accepting and abort every live session with @p status. */
  absl::Status Abort(const absl::Status& status);
  /** @return A Task resolving once the service is closed and empty. */
  [[nodiscard]] a11::Task Done() const;

 private:
  // Public only so this translation unit's helpers can name it; the definition
  // lives in the .cc and is not part of the API.
 public:
  struct State;

 private:
  explicit Service(std::shared_ptr<State> state) : state_(std::move(state)) {}

  // A shared_ptr rather than inline pImpl storage: every in-flight connection
  // task captures the state and unregisters itself from it when its session
  // finishes, so the state has to be able to outlive this handle. Do not
  // "optimize" this into a fixed-size buffer like Http2Client::kImplSize.
  std::shared_ptr<State> state_;
};

}  // namespace a11::service

#endif  // A11_SERVICE_SERVICE_H_
