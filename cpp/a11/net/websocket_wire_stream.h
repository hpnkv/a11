// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief WebSocket WireStream transport built on A11's nghttp2/HTTP2 stack.
 *
 * A WebSocketWireStream carries A11 WireMessage traffic over a WebSocket, and
 * WebSocketWireServer hosts inbound connections. This is the default transport
 * for connecting agents across a network: pick it whenever both ends can reach
 * each other over TCP and you want a persistent bidirectional channel. Logical
 * messages are framed over the socket by the shared ChannelWireStream layer.
 * Delivery carries no global ordering guarantee but is synchronised on closure
 * -- a reader observes every delivered message before the stream completes.
 */

#ifndef A11_NET_WEBSOCKET_WIRE_STREAM_H_
#define A11_NET_WEBSOCKET_WIRE_STREAM_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/concurrency/future.h"
#include "a11/net/channel_wire_stream.h"
#include "a11/net/http2.h"
#include "a11/net/wire_stream.h"

namespace a11::net {

/**
 * @brief Client-side tuning for a WebSocketWireStream connection.
 *
 * Configures the underlying HTTP/2 transport (including TLS), extra HTTP
 * headers sent on the WebSocket handshake, and the channel framing that
 * governs message splitting and buffering.
 */
struct WebSocketClientOptions {
  Http2Options http2_options;     ///< HTTP/2 connection and TLS policy.
  HttpHeaders headers;            ///< Extra WebSocket handshake headers.
  ChannelFramingOptions framing;  ///< Packet splitting and reassembly bounds.

  /** @return OK if the options are internally consistent. */
  absl::Status Validate() const;
};

/**
 * @brief A WireStream that carries A11 traffic over a client or accepted
 * WebSocket.
 *
 * Client instances are created with CreateClient(); server-accepted instances
 * are produced by WebSocketWireServer. Framing and message delivery are
 * handled by the ChannelWireStream base.
 */
class WebSocketWireStream final : public ChannelWireStream {
 private:
  struct ConstructorToken {};

 public:
  /**
   * @brief Dials a WebSocket endpoint and returns a WireStream over it.
   *
   * @param url The ws:// or wss:// URL of the remote A11 endpoint.
   * @param options Transport-level WireStreamOptions (buffering, deadlines).
   * @param websocket_options Handshake headers, framing, HTTP/2 and TLS
   *     settings.
   * @return The connected stream, or an error status. Drive it asynchronously
   *     via Start()/Send() once returned.
   */
  static absl::StatusOr<std::shared_ptr<WebSocketWireStream>> CreateClient(
      std::string url, WireStreamOptions options = {},
      WebSocketClientOptions websocket_options = {});

  explicit WebSocketWireStream(ConstructorToken, std::shared_ptr<State> state)
      : ChannelWireStream(std::move(state)) {}

 private:
  static absl::StatusOr<std::shared_ptr<WebSocketWireStream>> CreateAccepted(
      HttpRequest request, std::shared_ptr<Http2ResponseWriter> response,
      WireStreamOptions options, ChannelFramingOptions framing);

  friend class WebSocketWireServer;
};

/**
 * @brief Listen address, path, and per-stream defaults for a
 * WebSocketWireServer.
 *
 * A port of 0 requests an ephemeral port. `stream_options` and `framing` are
 * applied to each accepted stream; `http2_options` carries transport and TLS
 * settings.
 */
struct WebSocketServerOptions {
  std::string path = "/a11";               ///< WebSocket endpoint path.
  std::string bind_address = "127.0.0.1";  ///< Local listen address.
  std::uint16_t port = 0;  ///< Listen port; zero requests an ephemeral port.
  WireStreamOptions stream_options;  ///< Defaults for accepted streams.
  ChannelFramingOptions framing;     ///< Framing for accepted streams.
  Http2Options http2_options;        ///< Server HTTP/2 and TLS policy.

  /** @return OK if the options are internally consistent. */
  absl::Status Validate() const;
};

/** Callback invoked with a fresh WireStream for each accepted connection. */
using OnWebSocketStream =
    std::function<a11::Task(std::shared_ptr<WebSocketWireStream>)>;

/**
 * @brief Accepts inbound WebSocket connections and hands each to a callback.
 *
 * The server-side entry point for hosting an agent over WebSockets: every
 * accepted connection invokes `on_stream` concurrently with a fresh
 * WebSocketWireStream, which the handler typically drives via Accept().
 */
class WebSocketWireServer
    : public std::enable_shared_from_this<WebSocketWireServer> {
 public:
  /**
   * @brief Starts a WebSocket server accepting A11 connections.
   *
   * @param on_stream Callback run for each accepted stream.
   * @param options Listen address, port, path, and TLS/framing settings.
   * @return The running server, or an error status.
   */
  static absl::StatusOr<std::shared_ptr<WebSocketWireServer>> Create(
      OnWebSocketStream on_stream, WebSocketServerOptions options = {});
  ~WebSocketWireServer();

  /** Stops the server and closes the listening socket. */
  absl::Status Stop();
  /** @return The actual port listened on, resolved even for an ephemeral 0. */
  absl::StatusOr<std::uint16_t> port() const;
  /** @return Whether the server is currently accepting connections. */
  [[nodiscard]] bool running() const;
  /** @return An opaque native handle for advanced interop, or nullptr. */
  [[nodiscard]] void* absl_nullable GetImpl() const;

 private:
  struct State;

  explicit WebSocketWireServer(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;
};

}  // namespace a11::net

#endif  // A11_NET_WEBSOCKET_WIRE_STREAM_H_
