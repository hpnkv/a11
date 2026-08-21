// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The net library's exception boundary.
 *
 * Compiled with exceptions (see the exception policy block in
 * cpp/CMakeLists.txt) so that the wrappers below can catch. Everything else in
 * a11_net -- the transports, the framing, the HTTP connections -- is compiled
 * without them and calls these instead of writing a `try` of its own. See
 * a11/exception_guard.h.
 */

#include <memory>
#include <optional>
#include <utility>

#include "a11/concurrency/internal/exception_guard_future.h"
#include "a11/data/types.h"
#include "a11/internal/exception_guard_impl.h"
#include "a11/net/http2.h"
#include "a11/net/http_sse_wire_stream.h"
#include "a11/net/internal/exception_guarded_callbacks.h"
#include "a11/net/signalling.h"
#include "a11/net/websocket_wire_stream.h"
#include "a11/net/wire_stream.h"

namespace a11::net::internal {

OnMessage GuardOnMessage(OnMessage callback) {
  return exception_guard::Wrap<a11::Task, std::optional<data::WireMessage>>(
      std::move(callback), "on_message");
}

OnDone GuardOnDone(OnDone callback) {
  return exception_guard::Wrap<a11::Task>(std::move(callback), "on_done");
}

Http2RequestHandler GuardRequestHandler(Http2RequestHandler handler) {
  return exception_guard::Wrap<a11::Task, HttpRequest,
                               std::shared_ptr<Http2ResponseWriter>>(
      std::move(handler), "HTTP request handler");
}

OnWebSocketStream GuardOnWebSocketStream(OnWebSocketStream callback) {
  return exception_guard::Wrap<a11::Task, std::shared_ptr<WebSocketWireStream>>(
      std::move(callback), "WebSocket on_stream");
}

OnSignallingMessage GuardOnSignallingMessage(OnSignallingMessage callback) {
  return exception_guard::Wrap<a11::Task, SignallingMessage>(
      std::move(callback), "Signalling on_message");
}

OnHttpSseConnect GuardOnHttpSseConnect(OnHttpSseConnect callback) {
  return exception_guard::Wrap<a11::Task,
                               std::shared_ptr<HttpSseServerWireStream>>(
      std::move(callback), "SSE on_connect");
}

}  // namespace a11::net::internal
