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

/**
 * @file
 * @brief Common response headers for A11 HTTP servers.
 *
 * CORS and cache policies apply across HTTP transports. They control browser
 * access and intermediary handling without changing response content.
 */

#ifndef A11_NET_SERVER_HEADERS_H_
#define A11_NET_SERVER_HEADERS_H_

#include <string>
#include <string_view>

#include <absl/status/status.h>

#include "a11/net/http2.h"

namespace a11::net {

/** @brief Value of the `Server` header A11's HTTP surfaces send. */
inline constexpr std::string_view kServerHeaderValue = "a11";

/**
 * @brief Cross-origin policy for an A11 HTTP surface.
 *
 * The default admits any browser origin. Set @c allow_origin to narrow it or
 * clear @c enabled to omit CORS headers. CORS governs browser access; server
 * reachability is controlled by the listener configuration.
 */
struct CorsOptions {
  /// Whether cross-origin headers are sent at all.
  bool enabled = true;
  /// `Access-Control-Allow-Origin`. `*` admits any page.
  std::string allow_origin = "*";
  /// `Access-Control-Allow-Methods`. Every method A11's routes use.
  std::string allow_methods = "GET, POST, OPTIONS";
  /// `Access-Control-Allow-Headers`. A11's own header prefix plus the ones a
  /// JSON or SSE request carries.
  std::string allow_headers = "content-type, accept, authorization, x-a11-*";
  /**
   * `Access-Control-Expose-Headers`: what a page may read off the response.
   *
   * Exposes the stream ID and outbound modes required by browser SSE clients.
   */
  std::string expose_headers = "x-a11-stream-id, x-a11-outbound";
  /// `Access-Control-Max-Age` in seconds; 0 omits it.
  int max_age_seconds = 600;

  /** @return OK if no value would inject a header break. */
  [[nodiscard]] absl::Status Validate() const;
};

/**
 * @brief How a response may be cached, which differs by what it is.
 *
 * Live streams use no-store and disable intermediary buffering. Mutable
 * documents use no-cache so clients revalidate them.
 */
enum class CachePolicy {
  /// A live stream. `no-store`, and a hint to intermediaries not to buffer.
  kStream,
  /// A document that may change at any time. `no-cache`: use it, but ask first.
  kVolatile,
  /// Say nothing, and leave caching to whatever default applies.
  kUnset,
};

/**
 * @brief The response-header policy of one A11 HTTP surface.
 *
 * Carried by each listener's options, so a deployment sets it once per port.
 */
struct ServerHeaderOptions {
  /// `Server` value; empty sends none.
  std::string server = std::string(kServerHeaderValue);
  CorsOptions cors;
  /// Send `X-Content-Type-Options: nosniff`. A11's replies state their own
  /// content type, so letting a client guess one can only ever be wrong.
  bool nosniff = true;

  /** @return OK if no value would inject a header break. */
  [[nodiscard]] absl::Status Validate() const;
};

/**
 * @brief Adds the headers every response from an A11 server carries.
 *
 * Idempotent. Existing route headers take precedence.
 *
 * @param options The surface's policy.
 * @param cache What kind of response this is.
 * @param headers Headers to add to.
 */
void ApplyServerHeaders(const ServerHeaderOptions& options, CachePolicy cache,
                        HttpHeaders* absl_nonnull headers);

/**
 * @brief The cross-origin headers for @p options, and nothing else.
 *
 * ApplyServerHeaders also adds these headers.
 */
[[nodiscard]] HttpHeaders CorsHeaders(const CorsOptions& options);

/**
 * @brief Adds the cross-origin headers for @p options to @p headers.
 *
 * `Vary: Origin` comes with them whenever the policy names a specific origin,
 * because a cache that kept one origin's response and served it to another
 * would be handing out a header the second origin must not see.
 */
void ApplyCorsHeaders(const CorsOptions& options,
                      HttpHeaders* absl_nonnull headers);

/**
 * @brief Whether @p method is a CORS preflight this policy should answer.
 */
[[nodiscard]] bool IsPreflight(const CorsOptions& options,
                               std::string_view method);

}  // namespace a11::net

#endif  // A11_NET_SERVER_HEADERS_H_
