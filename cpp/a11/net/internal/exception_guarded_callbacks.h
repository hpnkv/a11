// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Adoption guards for the two callbacks every WireStream takes.
 *
 * `on_message` and `on_done` are the caller's, and a caller built with
 * exceptions may hand over one that throws -- a Python callback through
 * pybind11, or an ordinary C++ lambda that calls a throwing library. Every
 * WireStream implementation passes both through these on the way into its
 * state, so the transports themselves never have to think about it again: by
 * the time a Receiver fibre invokes `on_message`, whatever it throws has
 * already become the error status that fibre knows how to handle.
 *
 * Implemented in a11/net/boundary.cc, which is compiled with exceptions for
 * this purpose. a11/exception_guard.h explains why the wrap has to happen here
 * rather than at the call.
 */

#ifndef A11_NET_INTERNAL_EXCEPTION_GUARDED_CALLBACKS_H_
#define A11_NET_INTERNAL_EXCEPTION_GUARDED_CALLBACKS_H_

#include "a11/net/http2.h"
#include "a11/net/http_sse_wire_stream.h"
#include "a11/net/signalling.h"
#include "a11/net/websocket_wire_stream.h"
#include "a11/net/wire_stream.h"

namespace a11::net::internal {

/// Wraps `on_message` so a raised exception becomes a failed Task.
[[nodiscard]] OnMessage GuardOnMessage(OnMessage callback);
/// Wraps `on_done` so a raised exception becomes a failed Task.
[[nodiscard]] OnDone GuardOnDone(OnDone callback);
/// Wraps an HTTP request handler, whose fibre belongs to the connection.
[[nodiscard]] Http2RequestHandler GuardRequestHandler(
    Http2RequestHandler handler);
/// Wraps a WebSocket server's accept callback, invoked on a connection fibre.
[[nodiscard]] OnWebSocketStream GuardOnWebSocketStream(
    OnWebSocketStream callback);
/// Wraps a signalling endpoint's message callback.
[[nodiscard]] OnSignallingMessage GuardOnSignallingMessage(
    OnSignallingMessage callback);
/// Wraps an SSE server's accept callback.
[[nodiscard]] OnHttpSseConnect GuardOnHttpSseConnect(OnHttpSseConnect callback);

}  // namespace a11::net::internal

#endif  // A11_NET_INTERNAL_EXCEPTION_GUARDED_CALLBACKS_H_
