// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief A11's nghttp2 HTTP/2 client, server, and streaming primitives.
 *
 * These are the HTTP/2 building blocks -- Http2Client, Http2Server, and the
 * pull-oriented request/response body streams -- that underpin the WebSocket
 * and HTTP SSE WireStream transports. They are not themselves WireStreams;
 * most agents use them only indirectly. Reach for this header directly when
 * you need raw HTTP/2 requests, extended CONNECT duplex streams, or to share
 * one connection across several transports.
 */

#ifndef A11_NET_HTTP2_H_
#define A11_NET_HTTP2_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
// For sizeof(thread::Mutex) in Http2Client's inline-storage budget below.
#include "thread/boost_primitives.h"

namespace a11::net {

/// Protocol-neutral connection backing the stream facades (see
/// internal/http_connection.h). Either an HTTP/2 or an HTTP/1.1 connection.
class HttpConnection;

namespace internal {
/// Shared TCP/TLS transport substrate for the HTTP connections.
class HttpTransport;
}  // namespace internal

class Http2RequestBodyStream;

/**
 * @brief An ordered list of (name, value) HTTP/2 header fields.
 *
 * Field names are normalized to lowercase. A compact sequence preserves
 * repeated fields and wire order without allocating a tree node per field.
 */
using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

/** Lowercases every field name in @p headers in place. */
void NormalizeHttpHeaders(HttpHeaders* absl_nonnull headers);
/** @return The first value for @p name, or nullopt if absent. */
std::optional<std::string> GetHttpHeader(const HttpHeaders& headers,
                                         std::string_view name);
/** Removes every field named @p name from @p headers. */
void EraseHttpHeader(HttpHeaders* absl_nonnull headers, std::string_view name);
/** Replaces any existing @p name fields with a single (name, value) entry. */
void SetHttpHeader(HttpHeaders* absl_nonnull headers, std::string name,
                   std::string value);
/** @return OK if the header list is well-formed. */
absl::Status ValidateHttpHeaders(const HttpHeaders& headers);

/** @brief A parsed HTTP/2 request: pseudo-headers, fields, and body.
 *
 * `body_stream` is present only for a request that remains open after its
 * headers: an extended CONNECT stream, or one accepted by
 * Http2Options::stream_request_body. */
struct HttpRequest {
  std::string method;     ///< HTTP method pseudo-header.
  std::string protocol;   ///< Extended CONNECT protocol, if present.
  std::string scheme;     ///< Request scheme pseudo-header.
  std::string authority;  ///< Target authority/host.
  std::string path;       ///< Request path and query.
  HttpHeaders headers;    ///< Normal application headers.
  std::string body;       ///< Fully buffered body for an ordinary request.
  /// Pull stream for a request still open after its headers, otherwise null.
  std::shared_ptr<Http2RequestBodyStream> body_stream;
};

/** @brief Status line and headers of an HTTP/2 response, without the body. */
struct HttpResponseHead {
  int status = 0;            ///< Numeric HTTP response status.
  HttpHeaders headers = {};  ///< Response headers, preserving repetitions.
};

/** @brief A fully buffered HTTP/2 response: head plus complete body. */
struct HttpResponse {
  HttpResponseHead head;  ///< Status and headers.
  std::string body = {};  ///< Complete response body.
};

class Http2ResponseStream;

/**
 * @brief A response the server sent without being asked (HTTP/2 server push).
 *
 * The PUSH_PROMISE names a request the client never made -- that is what the
 * pseudo-header fields below are -- and @c response carries the response to it,
 * read exactly like any other. A client that does not want it calls Cancel() on
 * @c response, which resets the pushed stream and stops the transfer.
 *
 * Reachable only when Http2Options::enable_push was set on the connection.
 */
struct HttpPushedResponse {
  std::string method;     ///< Method of the promised request.
  std::string scheme;     ///< Scheme of the promised request.
  std::string authority;  ///< Authority of the promised request.
  std::string path;       ///< Path and query of the promised request.
  HttpHeaders headers;    ///< Header fields of the promised request.
  /// The pushed response: its own head, body, and trailers.
  std::shared_ptr<Http2ResponseStream> response;
};

/** @brief TLS settings for an HTTP/2 client or server (certificates, peer
 * verification). */
struct Http2TlsOptions {
  bool enabled = false;     ///< Negotiate TLS instead of clear-text HTTP/2.
  bool verify_peer = true;  ///< Validate the remote certificate and hostname.
  std::string certificate_pem_file;  ///< Local certificate chain, if needed.
  std::string key_pem_file;  ///< Private key matching the local certificate.
  std::string ca_certificate_pem_file;  ///< Optional custom trust roots.

  /// Validate certificate/key combinations before opening a socket.
  absl::Status Validate() const;
};

/** @brief Body-size limits, buffering thresholds, deadline, TLS, and HTTP
 * protocol negotiation for an HTTP client or server. */
struct Http2Options {
  /** @brief Client-side HTTP protocol preference / downgrade policy. */
  enum class ProtocolPreference {
    kAuto,   ///< Prefer HTTP/2, fall back to HTTP/1.1 (ALPN order / downgrade).
    kHttp2,  ///< Require HTTP/2 (h2 over TLS, prior-knowledge h2c cleartext).
    kHttp11,  ///< Require HTTP/1.1.
  };

  size_t max_request_body_size = 32 * 1024 * 1024;   ///< Request hard limit.
  size_t max_response_body_size = 32 * 1024 * 1024;  ///< Response hard limit.
  size_t max_buffered_request_bytes =
      4 * 1024 * 1024;  ///< Request backpressure bound.
  size_t max_buffered_response_bytes =
      4 * 1024 * 1024;  ///< Response backpressure bound.
  absl::Time deadline = absl::InfiniteFuture();  ///< Connection-wide deadline.
  Http2TlsOptions tls;  ///< TLS and certificate policy.

  // --- Protocol negotiation (server: which to accept; client: which to try). ---
  bool enable_h2 = true;     ///< Serve/accept HTTP/2 over TLS (ALPN "h2").
  bool enable_h2c = true;    ///< Serve/accept cleartext prior-knowledge HTTP/2.
  bool enable_http1 = true;  ///< Serve/accept HTTP/1.1 (ALPN and/or cleartext).
  /**
   * Client: accept HTTP/2 server pushes (SETTINGS_ENABLE_PUSH).
   *
   * Off by default, and advertised as off, so a peer cannot spend this side's
   * streams and flow-control window on responses nobody asked for. A client
   * that sets it must read Http2ResponseStream::NextPush() and either consume
   * or Cancel() each pushed response, or those streams stay open.
   */
  bool enable_push = false;
  /// Client protocol preference; also governs cleartext attempt order.
  ProtocolPreference client_preference = ProtocolPreference::kAuto;
  /// Whether a cleartext client may reconnect with the other protocol when its
  /// first attempt fails (ALPN makes TLS negotiation unambiguous already).
  bool client_allow_downgrade = true;

  /**
   * Server: decides from a request's head whether its body arrives
   * incrementally.
   *
   * A request this accepts is dispatched to the handler as soon as its headers
   * land, with HttpRequest::body_stream carrying the body as it arrives and
   * HttpRequest::body left empty; every other request is buffered whole and
   * dispatched once complete, which is what an unset predicate means. Reach for
   * it when a request body is an open-ended stream of application messages
   * rather than a document -- the HTTP SSE transport's streaming outbound
   * direction is exactly that. An extended CONNECT stream is delivered this way
   * regardless.
   *
   * Called on the connection's libuv thread, so keep it a cheap inspection of
   * the head. `max_buffered_request_bytes` bounds what the stream holds while
   * the handler is not reading, and `max_request_body_size` does not apply --
   * a stream has no total length to compare against.
   */
  std::function<bool(std::string_view method, std::string_view path,
                     const HttpHeaders& headers)>
      stream_request_body;

  /// Validate size relationships, deadline, TLS, and protocol settings.
  absl::Status Validate() const;
};

class Http2Connection;

/**
 * @brief Pull-oriented request DATA for a request that stays open.
 *
 * Present on HttpRequest::body_stream only when a request remains open after
 * its headers. Read incrementally with Read() until it yields nullopt.
 */
class Http2RequestBodyStream
    : public std::enable_shared_from_this<Http2RequestBodyStream> {
 public:
  /** @return An awaitable resolving to the next body chunk, or nullopt at end
   * of stream. */
  a11::Future<std::optional<std::string>> Read();
  /** @return An awaitable that resolves when the body stream is done. */
  a11::Task Done() const;
  /** Cancels the body stream with the given status. */
  absl::Status Cancel(absl::Status status = absl::CancelledError(
                          "HTTP/2 request body cancelled"));
  /** @return The HTTP/2 stream identifier. */
  [[nodiscard]] std::int32_t stream_id() const;

 private:
  struct State;

  explicit Http2RequestBodyStream(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;

  friend class Http2Connection;
  friend class Http1Connection;
};

/**
 * @brief A pull-oriented HTTP/2 response body stream.
 *
 * Read() returns nullopt after a clean END_STREAM. Only one outstanding Read()
 * is permitted, which provides a bounded handoff from the libuv thread to
 * fibers and ordinary threads.
 */
class Http2ResponseStream
    : public std::enable_shared_from_this<Http2ResponseStream> {
 public:
  /** @return An awaitable resolving to the response status and headers. */
  a11::Future<HttpResponseHead> Headers() const;
  /** @return An awaitable resolving to the next body chunk, or nullopt at end
   * of stream. */
  a11::Future<std::optional<std::string>> Read();
  /**
   * @brief The trailer fields that followed the body.
   *
   * Resolves once the response has ended -- with an empty list when the peer
   * sent no trailer section, so awaiting this never depends on having guessed
   * whether one was coming. A stream that failed fails here with the same
   * status rather than reporting an empty section.
   *
   * @return An awaitable resolving to the trailer fields.
   */
  a11::Future<HttpHeaders> Trailers() const;
  /**
   * @brief The next response the server pushed alongside this one.
   *
   * Resolves to nullopt once this response has ended, because HTTP/2 forbids a
   * PUSH_PROMISE after the stream it is associated with completes -- so at that
   * point no further push can arrive and a reader can stop asking. Requires
   * Http2Options::enable_push; without it the first call resolves to nullopt as
   * soon as the response ends and no push is ever delivered.
   *
   * @return An awaitable resolving to the next pushed response, or nullopt.
   */
  a11::Future<std::optional<HttpPushedResponse>> NextPush();
  /** @return An awaitable that resolves when the response is done. */
  a11::Task Done() const;
  /** Cancels the response stream with the given status. */
  absl::Status Cancel(
      absl::Status status = absl::CancelledError("HTTP/2 request cancelled"));
  /** @return The HTTP/2 stream identifier. */
  [[nodiscard]] std::int32_t stream_id() const;

 private:
  struct State;

  explicit Http2ResponseStream(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;

  friend class Http2Connection;
  friend class Http1Connection;
  friend class Http2Client;
};

/**
 * @brief Client side of an HTTP/2 extended CONNECT stream.
 *
 * Request DATA remains writable via Write()/Finish() while response DATA is
 * read independently with Read() on the same stream. This bidirectional
 * primitive is what the WebSocket transport is built on.
 */
class Http2DuplexStream
    : public std::enable_shared_from_this<Http2DuplexStream> {
 public:
  /** @return An awaitable resolving to the response status and headers. */
  a11::Future<HttpResponseHead> Headers() const;
  /** @return An awaitable resolving to the next inbound chunk, or nullopt at
   * end of stream. */
  a11::Future<std::optional<std::string>> Read();
  /** Writes a chunk of request DATA. */
  absl::Status Write(std::string data);
  /** Closes the request side, signalling END_STREAM to the peer. */
  absl::Status Finish();
  /** Aborts the whole duplex stream with the given status. */
  absl::Status Abort(absl::Status status = absl::CancelledError(
                         "HTTP/2 duplex stream cancelled"));
  /** @return An awaitable that resolves when the duplex stream is done. */
  a11::Task Done() const;

  /**
   * @brief The read half, for the parts of a response this facade does not
   *        forward.
   *
   * Headers() and Read() here are that stream's; its trailers and any pushed
   * responses are reached through this. Handing it out rather than mirroring
   * every method keeps one definition of what a response is, whether the request
   * that produced it was buffered or streamed.
   */
  [[nodiscard]] const std::shared_ptr<Http2ResponseStream>& response() const {
    return response_;
  }

  /** @return The HTTP/2 stream identifier. */
  [[nodiscard]] std::int32_t stream_id() const;

 private:
  Http2DuplexStream(std::weak_ptr<HttpConnection> connection,
                    std::shared_ptr<Http2ResponseStream> response)
      : connection_(std::move(connection)), response_(std::move(response)) {}

  std::weak_ptr<HttpConnection> connection_;
  std::shared_ptr<Http2ResponseStream> response_;

  friend class Http2Connection;
  friend class Http1Connection;
  friend class Http2Client;
};

/**
 * @brief Server-side handle for writing an HTTP/2 response to one request.
 *
 * Handed to an Http2RequestHandler. Either stream the response incrementally
 * (SendHeaders() then Write()/Finish()) or emit it in one call with
 * SendResponse().
 */
class Http2ResponseWriter
    : public std::enable_shared_from_this<Http2ResponseWriter> {
 public:
  /** Sends the response status and headers. */
  absl::Status SendHeaders(int status, HttpHeaders headers = {});
  /** Writes a chunk of response body data. */
  absl::Status Write(std::string data);
  /** Signals the end of the response body. */
  absl::Status Finish();
  /**
   * @brief Ends the response body with a trailer section.
   *
   * @param trailers Fields to send after the body; an empty list makes this
   *        Finish(). Pseudo-headers are not permitted in a trailer section.
   */
  absl::Status FinishWithTrailers(HttpHeaders trailers);
  /** Sends a complete response (status, headers, and body) in one call. */
  absl::Status SendResponse(int status, HttpHeaders headers = {},
                            std::string body = {});
  /**
   * @brief Promises a response the client did not ask for (server push).
   *
   * Sends a PUSH_PROMISE naming the request @p method and @p path, and returns
   * the writer for its response -- driven exactly like this one. Must be called
   * before this response is finished: HTTP/2 does not allow a promise on a
   * stream that has already closed.
   *
   * @param method Method of the promised request; must be safe and cacheable.
   * @param path Absolute path, including any query.
   * @param headers Header fields of the promised request.
   * @return The pushed response's writer, or an error -- FailedPrecondition
   *         when the client did not enable push, or when this response has
   *         already finished.
   */
  absl::StatusOr<std::shared_ptr<Http2ResponseWriter>> PushPromise(
      std::string method, std::string path, HttpHeaders headers = {});
  /** Aborts the response with the given status. */
  absl::Status Abort(absl::Status status);
  /** @return An awaitable that resolves when the response is done. */
  a11::Task Done() const;

  /** @return Whether the response headers have been sent. */
  [[nodiscard]] bool headers_sent() const;
  /** @return Whether the response has been finished. */
  [[nodiscard]] bool finished() const;

  /** @return The HTTP/2 stream identifier. */
  [[nodiscard]] std::int32_t stream_id() const { return stream_id_; }

 private:
  struct State;

  Http2ResponseWriter(std::weak_ptr<HttpConnection> connection,
                      std::int32_t stream_id, std::shared_ptr<State> state)
      : connection_(std::move(connection)),
        stream_id_(stream_id),
        state_(std::move(state)) {}

  std::weak_ptr<HttpConnection> connection_;
  std::int32_t stream_id_;
  std::shared_ptr<State> state_;

  friend class Http2Connection;
  friend class Http1Connection;
};

/** Callback dispatched for each inbound request, given a response writer. */
using Http2RequestHandler = std::function<a11::Task(
    HttpRequest request, std::shared_ptr<Http2ResponseWriter> response)>;

/**
 * @brief An HTTP/2 server that dispatches each request to an async handler.
 *
 * The listening endpoint underlying the WebSocket and HTTP SSE server
 * transports; also usable directly for plain HTTP/2 services.
 */
class Http2Server : public std::enable_shared_from_this<Http2Server> {
 public:
  /**
   * @brief Creates and starts an HTTP/2 server.
   *
   * @param bind_address Local address to bind to.
   * @param port TCP port to listen on; 0 selects an ephemeral port.
   * @param handler Async callback invoked for each request.
   * @param options Body limits, buffering, deadline, and TLS settings.
   * @return The running server, or an error status.
   */
  static absl::StatusOr<std::shared_ptr<Http2Server>> Create(
      std::string bind_address, std::uint16_t port, Http2RequestHandler handler,
      const Http2Options& options = {});

  ~Http2Server();

  /** Stops the server and releases its resources. */
  absl::Status Stop();
  /** @return The port listened on, resolved even for an ephemeral 0. */
  [[nodiscard]] std::uint16_t port() const;
  /** @return The address the server is bound to. */
  [[nodiscard]] std::string bind_address() const;
  /** @return Whether the server is currently running. */
  [[nodiscard]] bool running() const;
  /** @return Whether the server is using TLS. */
  [[nodiscard]] bool secure() const;
  /** @return An opaque native handle for advanced interop, or nullptr. */
  [[nodiscard]] void* absl_nullable GetImpl() const;

 private:
  struct State;

  explicit Http2Server(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;
};

/**
 * @brief An HTTP/2 client connection, reusable across many streams.
 *
 * Issues buffered requests, incremental response streams, or extended CONNECT
 * duplex streams over a single connection. Shared as the transport backing
 * HTTP SSE and WebSocket client wire streams.
 */
class Http2Client : public std::enable_shared_from_this<Http2Client> {
 public:
  /**
   * @brief Asynchronously connects to an HTTP/2 server.
   *
   * @param host Server hostname.
   * @param port Server port.
   * @param options Body limits, buffering, deadline, and TLS settings.
   * @return An awaitable that resolves to the connected client.
   */
  static a11::Future<std::shared_ptr<Http2Client>> Connect(
      std::string host, std::uint16_t port, const Http2Options& options = {});

  ~Http2Client();

  /** Opens a request and returns a pull-oriented response body stream. */
  absl::StatusOr<std::shared_ptr<Http2ResponseStream>> RequestStream(
      std::string method, std::string path, HttpHeaders headers = {},
      std::string body = {}, std::string scheme = {});
  /** Opens an extended CONNECT duplex stream for bidirectional data. */
  absl::StatusOr<std::shared_ptr<Http2DuplexStream>> ExtendedConnect(
      std::string protocol, std::string path, HttpHeaders headers = {},
      std::string scheme = {});
  /**
   * @brief Opens a request whose body is written incrementally.
   *
   * Where RequestStream() wants the whole body up front, this returns while the
   * request side is still open: Http2DuplexStream::Write() sends more of it and
   * Finish() ends it, with the response read from the same handle throughout.
   * That is what an upload of unknown or unbounded length needs -- HTTP/2 sends
   * the body as DATA frames, HTTP/1.1 as a chunked body, and neither needs a
   * `content-length`.
   *
   * @param method Request method.
   * @param path Absolute request path, including any query.
   * @param headers Request headers. Do not set `content-length`: the body's
   *        length is not known when the headers are sent.
   * @param scheme Request scheme; defaults to the connection's.
   * @return The duplex stream, or an error.
   */
  absl::StatusOr<std::shared_ptr<Http2DuplexStream>> RequestStreamingBody(
      std::string method, std::string path, HttpHeaders headers = {},
      std::string scheme = {});
  /** @return An awaitable resolving to the fully buffered response. */
  a11::Future<HttpResponse> Request(std::string method, std::string path,
                                    HttpHeaders headers = {},
                                    std::string body = {},
                                    std::string scheme = {});
  /** Closes the client connection. */
  absl::Status Close();

  /** @return The host the client is connected to. */
  [[nodiscard]] std::string host() const;
  /** @return The port the client is connected to. */
  [[nodiscard]] std::uint16_t port() const;
  /** @return Whether the client is currently connected. */
  [[nodiscard]] bool connected() const;
  /** @return Whether the connection is using TLS. */
  [[nodiscard]] bool secure() const;
  /**
   * @brief Whether this connection can carry several exchanges at once.
   *
   * True for HTTP/2 (h2 or h2c), false for HTTP/1.1 -- which A11 restricts to
   * one request per connection rather than pipelining. A caller sharing a
   * connection between requests has to know which it got.
   */
  [[nodiscard]] bool multiplexed() const;
  /** @return An opaque native handle for advanced interop, or nullptr. */
  [[nodiscard]] void* absl_nullable GetImpl() const;

 private:
  struct Impl;

  // Inline storage for Impl, whose definition lives in the .cc. The budget is
  // derived from the member types rather than written as a literal because
  // std::string is 24 bytes on libc++ and 32 on libstdc++: Impl holds four of
  // them (its own host plus the three in Http2Options::tls), so a constant
  // tuned on macOS silently overflows by 24 bytes on Linux and the
  // static_assert in the constructor becomes a hard build failure. Keep this
  // in sync with Impl's members; the trailing slack covers padding and the odd
  // small field.
  static constexpr size_t kImplSize =
      sizeof(std::string) +                               // host
      sizeof(std::uint16_t) +                             // port
      sizeof(Http2Options) +                              // options
      sizeof(thread::Mutex) +                             // mu
      sizeof(std::shared_ptr<internal::HttpTransport>) +  // connection
      32;                                                 // padding and slack
  static constexpr size_t kImplAlignment = alignof(std::max_align_t);

  Http2Client(std::string host, std::uint16_t port, const Http2Options& options,
              std::shared_ptr<internal::HttpTransport> connection);

  Impl* absl_nonnull state();
  const Impl* absl_nonnull state() const;

  alignas(kImplAlignment) std::byte impl_[kImplSize];
};

}  // namespace a11::net

#endif  // A11_NET_HTTP2_H_
