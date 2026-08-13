// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Protocol-neutral control interface backing the public HTTP streams.
 *
 * The public request/response stream facades (Http2DuplexStream,
 * Http2ResponseWriter) route their write/finish/abort operations through a
 * connection. HttpConnection abstracts that connection so the same facade
 * objects work over either the HTTP/2 (nghttp2) or the HTTP/1.1 transport.
 * The `stream_id` arguments identify a logical exchange: a real nghttp2 stream
 * id for HTTP/2, or a synthetic per-request sequence number for HTTP/1.1.
 */

#ifndef A11_NET_INTERNAL_HTTP_CONNECTION_H_
#define A11_NET_INTERNAL_HTTP_CONNECTION_H_

#include <cstdint>
#include <string>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/net/http2.h"

namespace a11::net {

/**
 * @brief The control operations the public HTTP stream facades depend on.
 *
 * Implemented by Http2Connection (over nghttp2) and Http1Connection (over
 * HTTP/1.1). Callers hold a weak_ptr<HttpConnection> and route per-exchange
 * operations through it, keeping the facades free of any protocol specifics.
 */
class HttpConnection {
 public:
  virtual ~HttpConnection() = default;

  /** Writes request DATA on a duplex (extended CONNECT / upgraded) stream. */
  virtual absl::Status WriteRequest(std::int32_t stream_id,
                                    std::string data) = 0;
  /** Half-closes the request side of a duplex stream. */
  virtual absl::Status FinishRequest(std::int32_t stream_id) = 0;
  /** Sends the response status line and headers (server side). */
  virtual absl::Status SendHeaders(std::int32_t stream_id, int status,
                                   HttpHeaders headers) = 0;
  /** Writes a chunk of response body data (server side). */
  virtual absl::Status Write(std::int32_t stream_id, std::string data) = 0;
  /** Signals the end of the response body (server side). */
  virtual absl::Status Finish(std::int32_t stream_id) = 0;
  /**
   * Ends the response body with a trailer section (server side).
   *
   * Equivalent to Finish() when @p trailers is empty. Separate from Finish()
   * rather than an argument to it because the two end the stream differently on
   * the wire: a trailing HEADERS frame carries END_STREAM instead of the last
   * DATA frame.
   */
  virtual absl::Status FinishWithTrailers(std::int32_t stream_id,
                                          HttpHeaders trailers) = 0;
  /** Sends a complete response (status, headers, and body) in one call. */
  virtual absl::Status SendResponse(std::int32_t stream_id, int status,
                                    HttpHeaders headers, std::string body) = 0;
  /** Aborts the response with the given status. */
  virtual absl::Status AbortResponse(std::int32_t stream_id,
                                     absl::Status status) = 0;
  /**
   * Promises a pushed response on @p stream_id, returning its writer.
   *
   * HTTP/2 only; the HTTP/1.1 connection reports Unimplemented, since the
   * protocol has no way to send a response that was not requested.
   */
  virtual absl::StatusOr<std::shared_ptr<Http2ResponseWriter>>
  SubmitPushPromise(std::int32_t stream_id, std::string method,
                    std::string path, HttpHeaders headers) = 0;
  /** @return Whether the response headers have been sent. */
  virtual absl::StatusOr<bool> ResponseHeadersSent(std::int32_t stream_id) = 0;
  /** @return Whether the response has been finished. */
  virtual absl::StatusOr<bool> ResponseFinished(std::int32_t stream_id) = 0;
  /** @return Whether the underlying transport negotiated TLS. */
  [[nodiscard]] virtual bool secure() const = 0;

  // --- Client role. ---

  /** Issues a request and returns a pull-oriented response body stream. */
  virtual absl::StatusOr<std::shared_ptr<Http2ResponseStream>> SubmitRequest(
      std::string method, std::string scheme, std::string authority,
      std::string path, HttpHeaders headers, std::string body) = 0;
  /** Opens a duplex (extended CONNECT / WebSocket upgrade) stream. */
  virtual absl::StatusOr<std::shared_ptr<Http2DuplexStream>> SubmitDuplex(
      std::string protocol, std::string scheme, std::string authority,
      std::string path, HttpHeaders headers) = 0;
  /**
   * Issues a request whose body is written incrementally afterwards.
   *
   * The same duplex facade as SubmitDuplex, for an ordinary method rather than
   * an extended CONNECT: the request side stays writable
   * (Http2DuplexStream::Write / Finish) while the response is read. This is how
   * a body too large or too slow to hold in one string is uploaded -- HTTP/2
   * sends it as DATA frames, HTTP/1.1 as a chunked body.
   */
  virtual absl::StatusOr<std::shared_ptr<Http2DuplexStream>>
  SubmitStreamingRequest(std::string method, std::string scheme,
                         std::string authority, std::string path,
                         HttpHeaders headers) = 0;
};

}  // namespace a11::net

#endif  // A11_NET_INTERNAL_HTTP_CONNECTION_H_
