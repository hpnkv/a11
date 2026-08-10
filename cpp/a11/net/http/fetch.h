// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief One-shot HTTP requests by URL, with redirects and a streaming sink.
 *
 * Http2Client already owns the hard parts -- TLS, ALPN, HTTP/2 with an HTTP/1.1
 * fallback, and backpressure -- but it is dialled by host and port and its
 * buffered Request() caps a response at Http2Options::max_response_body_size.
 * These helpers add what a caller fetching a URL needs on top: scheme-driven
 * TLS, `Location` following, and a sink so a body larger than memory (a model
 * file) never has to be one string.
 *
 * Both entry points return an a11::Future and do their blocking on a pooled
 * fiber via a11::Submit, so neither may be called from the libuv loop thread
 * and neither blocks it. Every failure is an absl::Status; a response status of
 * 400 or more becomes one through a11::StatusCodeFromHttp, with the URL and
 * response code attached as structured details.
 */

#ifndef A11_NET_HTTP_FETCH_H_
#define A11_NET_HTTP_FETCH_H_

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/net/http2.h"

namespace a11::net {

/**
 * @brief Called as a body arrives.
 *
 * @param bytes_done Bytes delivered to the sink so far.
 * @param bytes_total Total from `Content-Length`, or 0 when it is unknown.
 */
using OnFetchProgress =
    std::function<void(std::uint64_t bytes_done, std::uint64_t bytes_total)>;

/**
 * @brief Receives body chunks in order.
 *
 * Returning a non-OK status cancels the response and fails the fetch with that
 * status, which is how a sink reports that it could not write.
 */
using FetchSink = std::function<absl::Status(std::string_view chunk)>;

/** @brief What to request, and how hard to try. */
struct FetchOptions {
  std::string method = "GET";  ///< Request method.
  HttpHeaders headers;         ///< Extra request headers.
  std::string body;            ///< Request body, for methods that take one.
  /**
   * Redirects to follow before giving up. Zero returns the 3xx response as-is
   * rather than treating it as an error, which is what a caller inspecting
   * `Location` itself wants.
   */
  int max_redirects = 5;
  /// Wall-clock bound on the whole operation, redirects included.
  absl::Duration timeout = absl::Minutes(5);
  /**
   * Transport settings. `tls.enabled` is set from the URL scheme, so leave it
   * alone and use the other TLS fields for certificates and trust roots.
   * `max_response_body_size` bounds Fetch(); FetchToSink() streams and so is
   * bounded only by what the sink accepts.
   */
  Http2Options transport;
  /// Send a default `user-agent` when @c headers does not carry one.
  bool default_user_agent = true;

  /** @return OK when the options are self-consistent. */
  absl::Status Validate() const;
};

/**
 * @brief Fetches @p url and buffers the whole response.
 *
 * @param url Absolute `http`/`https` URL.
 * @param options What to request.
 * @return An awaitable resolving to the response, or a status: InvalidArgument
 *         for an unusable URL, OutOfRange when the body exceeds
 *         `transport.max_response_body_size`, the mapped status for a 4xx/5xx,
 *         or DeadlineExceeded on timeout.
 */
a11::Future<HttpResponse> Fetch(std::string url, FetchOptions options = {});

/**
 * @brief Fetches @p url and hands the body to @p sink as it arrives.
 *
 * The body is never accumulated, so this is the entry point for a response too
 * large to hold. Redirect responses are followed without ever reaching the
 * sink, so @p sink sees the body of exactly one response.
 *
 * @param url Absolute `http`/`https` URL.
 * @param sink Receives body chunks in order.
 * @param options What to request.
 * @param on_progress Optional progress callback, invoked from the fetching
 *        fiber between chunks.
 * @return An awaitable resolving to the final response's status and headers.
 */
a11::Future<HttpResponseHead> FetchToSink(std::string url, FetchSink sink,
                                          FetchOptions options = {},
                                          OnFetchProgress on_progress = {});

namespace internal {

/**
 * @brief The blocking core of FetchToSink, for a caller already on a fiber.
 *
 * FetchToSink() is this plus an a11::Submit. A caller that is itself running
 * inside a Submit -- Download(), for one -- uses this directly rather than
 * spending a second fiber to block the first one against.
 *
 * @warning Blocks. Never call it on the libuv loop thread.
 */
absl::StatusOr<HttpResponseHead> FetchBlocking(std::string url,
                                               FetchOptions options,
                                               FetchSink sink,
                                               OnFetchProgress on_progress = {});

}  // namespace internal

}  // namespace a11::net

#endif  // A11_NET_HTTP_FETCH_H_
