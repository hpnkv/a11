// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Dependency-free HTTP/1.1 message parsing and serialization.
 *
 * A small, I/O-free codec covering exactly what the A11 SSE and WebSocket
 * transports need over HTTP/1.1: request/status line and header parsing,
 * chunked transfer-encoding encode/decode, body framing determination, and the
 * RFC 6455 WebSocket handshake accept-key computation. Everything here operates
 * on byte buffers so it is unit-testable in isolation and shared by the client
 * and server connections.
 */

#ifndef A11_NET_INTERNAL_HTTP1_CODEC_H_
#define A11_NET_INTERNAL_HTTP1_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/net/http2.h"

namespace a11::net::internal {

/** The parsed start line and headers of an HTTP/1.1 request. */
struct Http1RequestHead {
  std::string method;   ///< Upper-case HTTP method.
  std::string target;   ///< Request-target (path and query).
  std::string version;  ///< e.g. "HTTP/1.1".
  HttpHeaders headers;  ///< Lower-cased field names, wire order preserved.
};

/** The parsed status line and headers of an HTTP/1.1 response. */
struct Http1ResponseHead {
  int status = 0;       ///< Numeric status code.
  std::string reason;   ///< Reason phrase (may be empty).
  std::string version;  ///< e.g. "HTTP/1.1".
  HttpHeaders headers;  ///< Lower-cased field names, wire order preserved.
};

/** How a message body is delimited on the wire. */
enum class BodyFraming {
  kNone,           ///< No body (e.g. GET request, 204/304 response).
  kContentLength,  ///< Exactly `content_length` bytes.
  kChunked,        ///< Transfer-Encoding: chunked.
  kUntilClose,     ///< Body runs until the connection closes (response only).
};

/** Body delimitation derived from a message's headers. */
struct BodyPlan {
  BodyFraming framing = BodyFraming::kNone;
  std::size_t content_length = 0;
};

/**
 * @return The index just past the end of the header block (the position after
 * the terminating CRLFCRLF) within @p data, or nullopt if the terminator has
 * not yet arrived. Also accepts a bare LFLF terminator for lenient input.
 */
std::optional<std::size_t> FindHeaderBlockEnd(std::string_view data);

/** Parses a request head (start line + headers, no trailing CRLFCRLF needed). */
absl::StatusOr<Http1RequestHead> ParseRequestHead(std::string_view head_block);
/** Parses a response head (status line + headers). */
absl::StatusOr<Http1ResponseHead> ParseResponseHead(
    std::string_view head_block);

/** @return How a request body is framed given its headers. */
absl::StatusOr<BodyPlan> PlanRequestBody(const HttpHeaders& headers);
/**
 * @return How a response body is framed given the request method and the
 * response status/headers (per RFC 9112: HEAD, 1xx, 204, 304 carry no body).
 */
absl::StatusOr<BodyPlan> PlanResponseBody(std::string_view request_method,
                                          int status,
                                          const HttpHeaders& headers);

/**
 * @brief Incremental decoder for Transfer-Encoding: chunked bodies.
 *
 * Feed raw bytes as they arrive; decoded payload is appended to the caller's
 * output. Sets `complete` once the terminating zero-length chunk (and its
 * trailer block) has been consumed, at which point trailers() holds whatever
 * trailer section followed.
 */
class ChunkedDecoder {
 public:
  /**
   * Consumes as much of @p data as possible.
   * @param data Newly received bytes.
   * @param out Decoded payload is appended here.
   * @param complete Set to true when the final chunk has been decoded.
   * @return An error on a malformed chunk header, otherwise OK.
   */
  absl::Status Feed(std::string_view data, std::string* out, bool* complete);

  [[nodiscard]] bool complete() const { return state_ == State::kComplete; }
  /**
   * @return The trailer section that followed the last chunk, with lower-cased
   * field names in wire order. Empty until the body is complete, and empty
   * afterwards when the peer sent no trailers.
   */
  [[nodiscard]] const HttpHeaders& trailers() const { return trailers_; }

 private:
  enum class State { kSize, kData, kDataCrlf, kTrailer, kComplete };

  State state_ = State::kSize;
  std::string pending_;          // Partial line (size line or trailer line).
  std::uint64_t remaining_ = 0;  // Bytes left in the current chunk's data.
  HttpHeaders trailers_;         // Trailer section, once the body completes.
};

/** @return @p data wrapped as a single chunked-transfer chunk. Empty input
 * yields an empty string (callers must not emit a zero-size data chunk). */
std::string EncodeChunk(std::string_view data);
/**
 * @return The terminating zero-length chunk that ends a chunked body, followed
 * by @p trailers as a trailer section when any are given.
 */
std::string EncodeLastChunk(const HttpHeaders& trailers = {});

/** Appends "name: value\r\n" for each header to @p out (names sent as given). */
void AppendHeaderBlock(const HttpHeaders& headers, std::string* out);
/** Serializes an HTTP/1.1 request head (start line + headers + CRLF). */
std::string SerializeRequest(std::string_view method, std::string_view target,
                             const HttpHeaders& headers);
/** Serializes an HTTP/1.1 response head (status line + headers + CRLF). */
std::string SerializeResponse(int status, const HttpHeaders& headers,
                              std::string_view reason = {});

/** The RFC 6455 magic GUID appended to a key before hashing. */
inline constexpr std::string_view kWebSocketGuid =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/** @return base64(SHA1(key + kWebSocketGuid)) for the Sec-WebSocket-Accept
 * response header. */
std::string ComputeWebSocketAccept(std::string_view key);
/** @return A fresh base64-encoded 16-byte Sec-WebSocket-Key for a client. */
std::string GenerateWebSocketKey();

/** @return The standard reason phrase for @p status (may be empty). */
std::string_view DefaultReasonPhrase(int status);

}  // namespace a11::net::internal

#endif  // A11_NET_INTERNAL_HTTP1_CODEC_H_
