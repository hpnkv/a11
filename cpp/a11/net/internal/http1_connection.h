// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief HTTP/1.1 connection over the shared HttpTransport substrate.
 *
 * Http1Connection speaks HTTP/1.1 with chunked transfer-encoding on top of the
 * same TCP/TLS/libuv pump as Http2Connection, and implements the same
 * HttpConnection control interface so the public stream facades (and the SSE /
 * WebSocket transports built on them) work unchanged. It models a connection as
 * a single in-flight exchange (HTTP/1.1 has no multiplexing); the stream ids it
 * exposes are synthetic per-request sequence numbers.
 *
 * The server role parses requests and dispatches them to an Http2RequestHandler
 * exactly as the HTTP/2 server does; a streaming response is emitted with
 * chunked framing, a buffered one with Content-Length. The client role issues a
 * single request and exposes the response as an Http2ResponseStream. The
 * RFC 6455 WebSocket upgrade path is layered on in a later phase.
 */

#ifndef A11_NET_INTERNAL_HTTP1_CONNECTION_H_
#define A11_NET_INTERNAL_HTTP1_CONNECTION_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/net/http2.h"
#include "a11/net/internal/http1_codec.h"
#include "a11/net/internal/http_connection.h"
#include "a11/net/internal/http_transport.h"

namespace a11::net {

/**
 * @brief An HTTP/1.1 connection driving the shared stream facades.
 *
 * Created by the HTTP server accept path (when the peer negotiates HTTP/1.1)
 * and by Http2Client::Connect (when the client resolves to HTTP/1.1).
 */
class Http1Connection : public internal::HttpTransport, public HttpConnection {
 public:
  /**
   * @brief Creates and initializes an HTTP/1.1 connection on @p tcp.
   *
   * @param prebuffered Bytes already read from the socket during cleartext
   *     protocol detection; replayed into the parser before new reads.
   */
  static absl::StatusOr<std::shared_ptr<Http1Connection>> Create(
      std::shared_ptr<uvw::tcp_handle> tcp, bool server,
      Http2RequestHandler handler, Http2Options options,
      internal::SslContext tls_context = {}, std::string tls_server_name = {},
      std::function<void(HttpTransport*)> on_closed = {},
      std::string prebuffered = {});

  ~Http1Connection() override = default;

  /// Down-casts the shared HttpTransport identity to this concrete type.
  std::shared_ptr<Http1Connection> Self();

  // Client role: issue a single request, returning the response stream.
  absl::StatusOr<std::shared_ptr<Http2ResponseStream>> SubmitRequest(
      std::string method, std::string scheme, std::string authority,
      std::string path, HttpHeaders headers, std::string body) override;
  absl::StatusOr<std::shared_ptr<Http2DuplexStream>> SubmitDuplex(
      std::string protocol, std::string scheme, std::string authority,
      std::string path, HttpHeaders headers) override;

  // --- HttpConnection interface. ---
  absl::Status WriteRequest(std::int32_t stream_id, std::string data) override;
  absl::Status FinishRequest(std::int32_t stream_id) override;
  absl::Status SendHeaders(std::int32_t stream_id, int status,
                           HttpHeaders headers) override;
  absl::Status Write(std::int32_t stream_id, std::string data) override;
  absl::Status Finish(std::int32_t stream_id) override;
  absl::Status SendResponse(std::int32_t stream_id, int status,
                            HttpHeaders headers, std::string body) override;
  absl::Status AbortResponse(std::int32_t stream_id,
                             absl::Status status) override;
  absl::StatusOr<bool> ResponseHeadersSent(std::int32_t stream_id) override;
  absl::StatusOr<bool> ResponseFinished(std::int32_t stream_id) override;
  [[nodiscard]] bool secure() const override {
    return ssl_context_ != nullptr;
  }

 protected:
  // --- HttpTransport seams. ---
  absl::Status OnInboundPlaintext(const char* data, size_t size) override;
  absl::Status SendProtocolPreamble() override;
  void OnClose(const absl::Status& status) override;
  std::vector<unsigned char> ClientAlpnWire() const override;
  absl::Status OnAlpnNegotiated(std::string_view protocol) override;

 private:
  enum class ParseState { kHead, kBody, kAwaitingResponse, kRaw, kDone };

  Http1Connection(std::shared_ptr<uvw::tcp_handle> tcp, bool server,
                  Http2RequestHandler handler, Http2Options options,
                  internal::SslContext tls_context, std::string tls_server_name,
                  std::function<void(HttpTransport*)> on_closed,
                  std::string prebuffered);

  absl::Status Initialize();

  // Server parsing.
  absl::Status ServerParse();
  absl::Status DispatchRequest();
  // Client parsing.
  absl::Status ClientParse();

  // Response emission (server, on the loop thread).
  absl::Status SendHeadersOnLoop(std::int32_t stream_id, int status,
                                 HttpHeaders headers);
  absl::Status WriteOnLoop(std::int32_t stream_id, std::string data);
  absl::Status FinishOnLoop(std::int32_t stream_id);
  void FinishResponseAndAdvance();

  const Http2RequestHandler handler_;
  std::string prebuffered_;

  // Accumulated inbound bytes not yet consumed by the parser.
  std::string inbuf_;
  ParseState state_ = ParseState::kHead;

  // The current exchange's synthetic id and per-exchange state.
  std::int32_t next_stream_id_ = 1;
  std::int32_t stream_id_ = 0;

  // Server request being assembled.
  internal::Http1RequestHead request_head_;
  internal::BodyPlan request_body_plan_;
  std::string request_body_;
  internal::ChunkedDecoder request_chunk_decoder_;
  size_t request_body_remaining_ = 0;
  bool keep_alive_ = true;

  // Server response state.
  std::shared_ptr<Http2ResponseWriter::State> writer_state_;
  bool response_headers_sent_ = false;
  bool response_finished_ = false;
  bool response_chunked_ = false;

  // RFC 6455 WebSocket upgrade state. Once upgraded, the connection switches to
  // raw passthrough (kRaw): inbound bytes are pushed to the duplex body stream
  // and outbound Write bytes go straight to the socket, framed by the caller.
  bool ws_upgrade_ = false;         ///< This exchange is a WebSocket upgrade.
  std::string ws_key_;              ///< Client Sec-WebSocket-Key (server side).
  bool client_ws_ = false;          ///< Client-initiated WebSocket upgrade.
  std::string client_ws_key_;       ///< Key we sent (client side, for accept).
  std::shared_ptr<Http2RequestBodyStream::State> request_body_state_;

  // Client response state.
  std::string client_method_;
  std::shared_ptr<Http2ResponseStream::State> response_state_;
  bool client_head_parsed_ = false;
  internal::BodyPlan response_body_plan_;
  internal::ChunkedDecoder response_chunk_decoder_;
  size_t response_body_remaining_ = 0;
  bool client_request_sent_ = false;
};

}  // namespace a11::net

#endif  // A11_NET_INTERNAL_HTTP1_CONNECTION_H_
