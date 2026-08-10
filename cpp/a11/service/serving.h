// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Handing a Service to a transport listener.
 *
 * Servers stay dumb on purpose. Every listener in a11/net takes its on-stream
 * callback at construction -- `WebSocketWireServer::Create(OnWebSocketStream,
 * options)`, `HttpSseServer::Create(host, port, OnHttpSseConnect, options)`,
 * `WebRtcWireServer::Create(identity, signalling, on_stream, config)` -- so a
 * hypothetical `Service::Attach(server)` would have to know every one of those
 * option structs, which is the coupling a11::service::Service exists to remove.
 *
 * What is needed instead is one adapter, and this is it.
 */

#ifndef A11_SERVICE_SERVING_H_
#define A11_SERVICE_SERVING_H_

#include <functional>
#include <memory>
#include <utility>

#include <absl/status/status.h>

#include "a11/concurrency/future.h"
#include "a11/service/service.h"
#include "a11/service/session.h"

namespace a11::service {

/**
 * @brief A callback for any transport's on-stream hook, serving into @p service.
 *
 * Holds a weak reference, so a server that outlives its service rejects new
 * connections instead of dereferencing a destroyed one.
 *
 * @code
 * auto server = net::WebSocketWireServer::Create(
 *     AcceptInto<net::WebSocketWireStream>(service), options);
 * @endcode
 *
 * @tparam StreamT The concrete stream type the transport hands out.
 */
template <typename StreamT>
std::function<a11::Task(std::shared_ptr<StreamT>)> AcceptInto(
    const std::shared_ptr<Service>& service,
    StreamMode mode = StreamMode::kAccept) {
  std::weak_ptr<Service> weak = service;
  return [weak = std::move(weak), mode](
             std::shared_ptr<StreamT> stream) -> a11::Task {
    std::shared_ptr<Service> resolved = weak.lock();
    if (resolved == nullptr) {
      return a11::FailedTask(absl::FailedPreconditionError(
          "The service this server was bound to is gone"));
    }
    return resolved->Serve(std::move(stream), mode);
  };
}

}  // namespace a11::service

#endif  // A11_SERVICE_SERVING_H_
