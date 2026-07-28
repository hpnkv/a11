// Copyright 2026 The A11 Authors.

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

struct WebSocketClientOptions {
  Http2Options http2_options;
  HttpHeaders headers;
  ChannelFramingOptions framing;

  absl::Status Validate() const;
};

class WebSocketWireStream final : public ChannelWireStream {
 private:
  struct ConstructorToken {};

 public:
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

struct WebSocketServerOptions {
  std::string path = "/a11";
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 0;
  WireStreamOptions stream_options;
  ChannelFramingOptions framing;
  Http2Options http2_options;

  absl::Status Validate() const;
};

using OnWebSocketStream =
    std::function<a11::Task(std::shared_ptr<WebSocketWireStream>)>;

class WebSocketWireServer
    : public std::enable_shared_from_this<WebSocketWireServer> {
 public:
  static absl::StatusOr<std::shared_ptr<WebSocketWireServer>> Create(
      OnWebSocketStream on_stream, WebSocketServerOptions options = {});
  ~WebSocketWireServer();

  absl::Status Stop();
  absl::StatusOr<std::uint16_t> port() const;
  [[nodiscard]] bool running() const;
  [[nodiscard]] void* absl_nullable GetImpl() const;

 private:
  struct State;

  explicit WebSocketWireServer(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;
};

}  // namespace a11::net

#endif  // A11_NET_WEBSOCKET_WIRE_STREAM_H_
