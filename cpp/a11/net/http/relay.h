/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/** @file Streaming HTTP relay over the shared connection pool. */
#ifndef A11_NET_HTTP_RELAY_H_
#define A11_NET_HTTP_RELAY_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/net/http2.h"

namespace a11::net {
/** Observe response headers/body/completion without altering the exchange.
 * Events are headers, body, complete, limit, or error. Body data is borrowed
 * for the call. The observer is synchronous and bounded by the relay's byte
 * limit; an observer failure disables observation while forwarding continues.
 */
using HttpRelayObserver = std::function<absl::Status(
    std::string_view event, const HttpResponseHead& head,
    std::string_view data)>;

/** Forward an exchange to an explicit URL without redirects or retries.
 *
 * Status, body chunks, and trailers are relayed unchanged. Hop-by-hop headers
 * and private x-a11-* fields are removed. The deadline belongs to this stream;
 * cancellation leaves other requests on the pooled connection running.
 *
 * @param target Host-selected absolute HTTP/HTTPS URL, including path/query.
 * @param request Incoming method, headers, and buffered or streaming body.
 * @param writer Downstream response writer, retained for the exchange.
 * @param options Transport bounds and protocol/TLS settings. URL scheme selects
 *        TLS. The connection pool ignores connection-wide deadlines.
 * @param deadline Absolute per-exchange deadline, including pool acquisition.
 * @param observer Optional synchronous response observer, adopted at entry.
 * @param max_observed_bytes Maximum body bytes delivered to the observer.
 * @return Completion task. Transport/deadline failures cancel the upstream
 *         stream and abort an active downstream response. HTTP error statuses
 *         remain ordinary responses. Request trailers require a separate
 *         upload contract and are outside this helper's request-body surface.
 */
a11::Task RelayHttp(std::string target, HttpRequest request,
                    std::shared_ptr<Http2ResponseWriter> writer,
                    Http2Options options = {},
                    absl::Time deadline = absl::InfiniteFuture(),
                    HttpRelayObserver observer = {},
                    size_t max_observed_bytes = 8 * 1024 * 1024);
}  // namespace a11::net
#endif  // A11_NET_HTTP_RELAY_H_
