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
  absl::Time deadline = absl::InfiniteFuture();  ///< Registration deadline.
  size_t max_message_size = 1024 * 1024;  ///< Inbound JSON message limit.

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
 * @brief Listen address and limits for a WebSocketSignallingServer.
 *
 * A port of 0 requests an ephemeral port; `http2_options` carries transport
 * and TLS settings, and `max_message_size` bounds inbound signalling frames.
 */
struct WebSocketSignallingServerOptions {
  std::string path_prefix =
      "/";  ///< Prefix before identity registration paths.
  std::string bind_address = "127.0.0.1";  ///< Local listen address.
  std::uint16_t port = 0;  ///< Listen port; zero requests an ephemeral port.
  Http2Options http2_options;             ///< Server HTTP/2 and TLS policy.
  size_t max_message_size = 1024 * 1024;  ///< Inbound JSON message limit.

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

  static void Remove(const std::shared_ptr<State>& state, std::string identity);

  std::shared_ptr<State> state_;
};

}  // namespace a11::net

#endif  // A11_NET_WEBSOCKET_SIGNALLING_H_
