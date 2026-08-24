// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief WebSocket-transported signalling for remote WebRTC peers.
 *
 * When two WebRTC peers are on different machines they cannot share an
 * in-process SignallingService directly. WebSocketSignallingServer fronts a
 * SignallingService over a WebSocket, and WebSocketSignallingClient is the
 * SignallingTransport a remote peer connects with, registering under an
 * identity and relaying SignallingMessage traffic across the network. These
 * carry the SDP/ICE control handshake, not A11 WireMessage traffic.
 */

#ifndef A11_NET_WEBSOCKET_SIGNALLING_H_
#define A11_NET_WEBSOCKET_SIGNALLING_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/net/http2.h"
#include "a11/net/signalling.h"

namespace a11::net {

/**
 * @brief Connection tuning for a WebSocketSignallingClient.
 *
 * Covers the underlying HTTP/2 transport (including TLS), a connect deadline,
 * and the maximum accepted inbound signalling message size.
 */
struct WebSocketSignallingClientOptions {
  Http2Options http2_options;  ///< HTTP/2 connection and TLS policy.
  /**
   * @brief How long registering may take.
   *
   * Bounds the handshake only. Once registered, the socket stays open for as
   * long as the client keeps it; set `http2_options.deadline` if you want the
   * connection itself to expire.
   */
  absl::Time deadline = absl::InfiniteFuture();
  size_t max_message_size = 1024 * 1024;  ///< Inbound JSON message limit.
  /**
   * @brief Extra headers sent on the WebSocket handshake.
   *
   * How a client presents credentials to a signalling server that authenticates
   * -- an `Authorization` bearer, a deployment's own header. Without this the
   * only place to put one is the URL's query string, where it ends up in logs.
   */
  HttpHeaders headers;

  /** @return OK if the options are internally consistent. */
  absl::Status Validate() const;
};

/**
 * @brief A SignallingTransport that relays messages over a WebSocket.
 *
 * The client a remote WebRTC peer uses to reach a WebSocketSignallingServer:
 * it registers under an identity and carries SignallingMessage traffic to and
 * from the fronted SignallingService. Pass it to WebRtcWireStream::CreateClient
 * as the signalling transport.
 */
class WebSocketSignallingClient final
    : public SignallingTransport,
      public std::enable_shared_from_this<WebSocketSignallingClient> {
 private:
  struct State;

  struct ConstructorToken {};

 public:
  /**
   * @brief Asynchronously connects to a WebSocket signalling server.
   *
   * @param url The ws:// or wss:// signalling server URL.
   * @param identity Identity to register under.
   * @param on_message Optional callback for inbound signalling messages.
   * @param options Transport tuning and deadline.
   * @return An awaitable resolving to the connected client once registered.
   */
  static a11::Future<std::shared_ptr<WebSocketSignallingClient>> Connect(
      std::string url, std::string identity,
      OnSignallingMessage on_message = {},
      WebSocketSignallingClientOptions options = {});

  ~WebSocketSignallingClient() override;

  absl::Status Send(SignallingMessage message) override;
  absl::Status SetOnMessage(OnSignallingMessage on_message) override;
  absl::Status Close() override;
  [[nodiscard]] std::string identity() const override;
  [[nodiscard]] bool connected() const override;
  [[nodiscard]] absl::Status GetStatus() const override;
  /** @return An opaque native handle for advanced interop, or nullptr. */
  [[nodiscard]] void* absl_nullable GetImpl() const;

  explicit WebSocketSignallingClient(ConstructorToken,
                                     std::shared_ptr<State> state)
      : state_(std::move(state)) {}

 private:
  static void Pump(const std::shared_ptr<State>& state);
  static void Fail(const std::shared_ptr<State>& state, absl::Status status);

  std::shared_ptr<State> state_;
};

/**
 * @brief What a peer presented when asking to register an identity.
 *
 * Everything an admission decision could reasonably want, and nothing derived:
 * a deployment that authenticates by header, by query parameter or by address
 * reads it from here rather than from a request object this layer does not
 * expose.
 */
struct SignallingAdmission {
  std::string identity;  ///< Identity taken from the request path.
  std::string path;      ///< Full request path, query included.
  std::string query;     ///< The part after `?`, without it.
  HttpHeaders headers;   ///< Request headers, as sent.
};

/**
 * @brief Decides whether a peer may register, asynchronously.
 *
 * A failed task rejects the registration and its status becomes the HTTP
 * response; an OK task admits. Asynchronous because the answer usually lives
 * in a database, and this runs once per connection rather than per message.
 */
using OnSignallingAdmission = std::function<a11::Task(SignallingAdmission)>;

/** @brief Notified when a registered identity's connection goes away. */
using OnSignallingDeparture = std::function<void(std::string identity)>;

/**
 * @brief Inspects, rewrites or refuses each inbound message before routing.
 *
 * Deliberately synchronous. Signalling is ordered per connection and an
 * asynchronous filter would either reorder messages or need a queue per
 * connection to avoid it; the things a filter actually does -- rate limiting,
 * field checks -- are arithmetic. A non-OK status refuses that one message and
 * is reported to its sender as an `error` message; the connection stays open.
 */
using OnSignallingMessageFilter = std::function<absl::Status(
    SignallingMessage* absl_nonnull message)>;

/**
 * @brief Offered a message whose recipient is not connected to this server.
 *
 * The egress half of a federated fabric, paired with
 * SignallingService::Deliver(): return OK once the message has been handed to
 * whatever will carry it elsewhere, or a non-OK status to say it is genuinely
 * undeliverable, which the sender is told about.
 */
using OnSignallingUnroutable =
    std::function<absl::Status(const SignallingMessage& message)>;

/**
 * @brief Listen address, limits and policy hooks for a
 * WebSocketSignallingServer.
 *
 * A port of 0 requests an ephemeral port; `http2_options` carries transport
 * and TLS settings, and `max_message_size` bounds inbound signalling frames.
 * The four handlers are all optional, and a server with none of them set
 * behaves exactly as one that never had them.
 */
struct WebSocketSignallingServerOptions {
  std::string path_prefix =
      "/";  ///< Prefix before identity registration paths.
  std::string bind_address = "127.0.0.1";  ///< Local listen address.
  std::uint16_t port = 0;  ///< Listen port; zero requests an ephemeral port.
  Http2Options http2_options;             ///< Server HTTP/2 and TLS policy.
  size_t max_message_size = 1024 * 1024;  ///< Inbound JSON message limit.

  OnSignallingAdmission on_admit;         ///< Admission control, if any.
  OnSignallingDeparture on_departed;      ///< Presence bookkeeping, if any.
  OnSignallingMessageFilter on_message;   ///< Per-message policy, if any.
  OnSignallingUnroutable on_unroutable;   ///< Where else to look, if anywhere.

  /**
   * @brief Whether a new registration displaces an existing one.
   *
   * Off by default, which makes a second registration of a live identity
   * `ALREADY_EXISTS` -- the safe answer when nothing above this layer knows
   * which of the two is legitimate. A deployment whose `on_admit` *does* know
   * (because it just issued the newcomer an exclusive claim) sets this, and
   * the displaced connection is closed before the newcomer is registered.
   * Without it, a host that restarted cannot take its own identity back until
   * the socket its dead predecessor left behind is noticed.
   */
  bool replace_existing = false;

  /** @return OK if the options are internally consistent. */
  absl::Status Validate() const;
};

/**
 * @brief Fronts an in-process SignallingService over a WebSocket.
 *
 * Lets remote peers join the same signalling fabric: each connecting
 * WebSocketSignallingClient is registered with the wrapped SignallingService
 * under its identity, so messages route between local and remote peers alike.
 */
class WebSocketSignallingServer
    : public std::enable_shared_from_this<WebSocketSignallingServer> {
 public:
  /**
   * @brief Creates a WebSocket signalling server fronting @p service.
   *
   * @param service The in-process signalling service to expose.
   * @param options Listen address, port, path prefix, and limits.
   * @return The running server, or an error status.
   */
  static absl::StatusOr<std::shared_ptr<WebSocketSignallingServer>> Create(
      std::shared_ptr<SignallingService> service,
      WebSocketSignallingServerOptions options = {});

  ~WebSocketSignallingServer();

  /** Stops the server and closes all client connections. */
  absl::Status Stop();
  /**
   * @brief Closes one identity's connection, if this server holds it.
   *
   * The counterpart to admission: whatever authorised a registration can be
   * withdrawn -- a credential expires, a lease is taken by somebody else --
   * and the socket has to go with it rather than surviving until its next
   * message. `on_departed` fires as it would for a voluntary disconnect, so
   * bookkeeping has one path rather than two.
   *
   * @param identity The registered identity to disconnect.
   * @return OK, or `NOT_FOUND` when this server is not holding it.
   */
  absl::Status Disconnect(std::string_view identity);
  /** @return The port listened on, resolved even for an ephemeral 0. */
  [[nodiscard]] std::uint16_t port() const;
  /** @return Whether the server is currently running. */
  [[nodiscard]] bool running() const;
  /** @return The signalling service this server fronts. */
  [[nodiscard]] std::shared_ptr<SignallingService> service() const;
  /** @return An opaque native handle for advanced interop, or nullptr. */
  [[nodiscard]] void* absl_nullable GetImpl() const;

 private:
  struct State;

  explicit WebSocketSignallingServer(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  /// Registers an admitted peer: upgrades, connects, and starts pumping.
  /// Separate from the request handler because admission may be asynchronous,
  /// and this is what runs once it has said yes.
  static a11::Task Register(const std::shared_ptr<State>& state,
                            std::string identity, HttpRequest request,
                            std::shared_ptr<Http2ResponseWriter> response);

  static void Remove(const std::shared_ptr<State>& state,
                     const std::string& identity);

  std::shared_ptr<State> state_;
};

}  // namespace a11::net

#endif  // A11_NET_WEBSOCKET_SIGNALLING_H_
