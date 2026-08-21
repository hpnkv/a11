// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The response headers every A11 HTTP server sends, in one place.
 *
 * **Why this exists.** The headers a server owes its clients were decided per
 * endpoint: CORS lived on ::a11::net::HttpSseOptions as four loose strings that
 * only the SSE routes consulted, the WebSocket server's HTTP surface sent none
 * at all, and nothing sent a `Server` header or told a cache what to do with a
 * reply. None of that is per-endpoint policy -- it is what an A11 server is --
 * so it is stated once here and applied by whichever transport holds the port.
 *
 * Everything here is a hint or a policy, not behaviour: no header changes what a
 * response *contains*. What they change is whether a browser will read it, and
 * whether an intermediary will cache, buffer or sniff it -- and the defaults
 * below are what makes a browser client work without anybody configuring one.
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
 * **Permissive by default, deliberately.** A11's browser clients are a first
 * class case -- the whole HTTP SSE transport exists because a page cannot open a
 * raw socket -- and a default that refused them would mean every demo, every
 * embedded panel and every `fetch()` began with a configuration step and a
 * confusing console error. A deployment that wants a narrower policy sets
 * @c allow_origin; one that wants none clears @c enabled.
 *
 * This is not a security boundary and must not be read as one. CORS governs what
 * *a browser* will let a page read; it says nothing to anything else. An A11
 * server that should not be reachable is one that is not listening publicly.
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
   * A11's own response headers, by default, because the SSE transport does not
   * work without them: a browser client reads its stream id and the outbound
   * modes the server offers off the connect response, and a header a page may
   * not read is one it did not receive. This defaulting to empty is why every
   * page-facing deployment had to discover and set it.
   */
  std::string expose_headers = "x-a11-stream-id, x-a11-outbound";
  /// `Access-Control-Max-Age` in seconds; 0 omits it. A preflight per request
  /// is a round trip per request, and these routes' policy does not change.
  int max_age_seconds = 600;

  /** @return OK if no value would inject a header break. */
  [[nodiscard]] absl::Status Validate() const;
};

/**
 * @brief How a response may be cached, which differs by what it is.
 *
 * A stream must never be cached or buffered; a document that changes when a
 * registry changes must be revalidated rather than kept. Stating which of the
 * two a route is, rather than spelling headers at each one, is what keeps a new
 * route from quietly getting the wrong answer.
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
 * Idempotent, and never replaces a header already set: a route that has said
 * something specific -- a content type, its own cache policy -- keeps it.
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
 * For a route that builds its headers from scratch and wants the CORS half.
 * ApplyServerHeaders adds these too.
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
