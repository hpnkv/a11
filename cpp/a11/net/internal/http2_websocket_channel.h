// Copyright 2026 The A11 Authors.

#ifndef A11_NET_INTERNAL_HTTP2_WEBSOCKET_CHANNEL_H_
#define A11_NET_INTERNAL_HTTP2_WEBSOCKET_CHANNEL_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <absl/status/statusor.h>

#include "a11/net/http2.h"
#include "a11/net/internal/binary_channel.h"

namespace a11::net::internal {

struct Http2WebSocketClientConfig {
  std::string host;
  std::uint16_t port = 0;
  std::string path = "/";
  HttpHeaders headers;
  Http2Options http2_options;
  size_t max_message_size = 32 * 1024 * 1024;
};

absl::StatusOr<std::shared_ptr<BinaryChannel>> MakeHttp2WebSocketClientChannel(
    Http2WebSocketClientConfig config);

absl::StatusOr<std::shared_ptr<BinaryChannel>> MakeHttp2WebSocketServerChannel(
    HttpRequest request, std::shared_ptr<Http2ResponseWriter> response,
    size_t max_message_size);

}  // namespace a11::net::internal

#endif  // A11_NET_INTERNAL_HTTP2_WEBSOCKET_CHANNEL_H_
