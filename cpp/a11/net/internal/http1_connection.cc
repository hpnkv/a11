// Copyright 2026 The A11 Authors.

#include "a11/net/internal/http1_connection.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>

#include "a11/concurrency/executor.h"
#include "a11/net/internal/exception_guarded_callbacks.h"
#include "a11/net/internal/http1_codec.h"
#include "a11/net/internal/http_streams.h"
#include "a11/status.h"

namespace a11::net {
namespace {

using internal::BodyFraming;
using internal::ChunkedDecoder;
using internal::Http1RequestHead;
using internal::Http1ResponseHead;
using internal::HttpTransport;
using internal::RunOnUv;
using internal::RunStatusOnUv;

// Whether an HTTP/1.1 message signals connection close (Connection: close, or
// HTTP/1.0 without keep-alive).
bool WantsClose(std::string_view version, const HttpHeaders& headers) {
  const std::optional<std::string> connection =
      GetHttpHeader(headers, "connection");
  if (connection.has_value()) {
    std::string lowered = *connection;
    absl::AsciiStrToLower(&lowered);
    if (lowered.find("close") != std::string::npos) {
      return true;
    }
    if (lowered.find("keep-alive") != std::string::npos) {
      return false;
    }
  }
  return version == "HTTP/1.0";
}

}  // namespace

absl::StatusOr<std::shared_ptr<Http1Connection>> Http1Connection::Create(
    std::shared_ptr<uvw::tcp_handle> tcp, bool server,
    Http2RequestHandler handler, Http2Options options,
    internal::SslContext tls_context, std::string tls_server_name,
    std::function<void(HttpTransport*)> on_closed, std::string prebuffered) {
  if (tcp == nullptr) {
    return absl::InvalidArgumentError("TCP handle must not be null");
  }
  struct MakeSharedEnabler final : Http1Connection {
    MakeSharedEnabler(std::shared_ptr<uvw::tcp_handle> tcp, bool server,
                      Http2RequestHandler handler, Http2Options options,
                      internal::SslContext tls_context,
                      std::string tls_server_name,
                      std::function<void(HttpTransport*)> on_closed,
                      std::string prebuffered)
        : Http1Connection(std::move(tcp), server, std::move(handler),
                          std::move(options), std::move(tls_context),
                          std::move(tls_server_name), std::move(on_closed),
                          std::move(prebuffered)) {}
  };
  auto connection = std::make_shared<MakeSharedEnabler>(
      std::move(tcp), server, std::move(handler), std::move(options),
      std::move(tls_context), std::move(tls_server_name), std::move(on_closed),
      std::move(prebuffered));
  ABSL_RETURN_IF_ERROR(connection->Initialize());
  return connection;
}

Http1Connection::Http1Connection(
    std::shared_ptr<uvw::tcp_handle> tcp, bool server,
    Http2RequestHandler handler, Http2Options options,
    internal::SslContext tls_context, std::string tls_server_name,
    std::function<void(HttpTransport*)> on_closed, std::string prebuffered)
    : HttpTransport(std::move(tcp), server, std::move(options),
                    std::move(tls_context), std::move(tls_server_name),
                    std::move(on_closed)),
      // The handler is the server owner's; guarded on the way in for the same
      // reason as in Http2Connection. See
      // net/internal/exception_guarded_callbacks.h.
      handler_(internal::GuardRequestHandler(std::move(handler))),
      prebuffered_(std::move(prebuffered)) {}

std::shared_ptr<Http1Connection> Http1Connection::Self() {
  return std::static_pointer_cast<Http1Connection>(shared_from_this());
}

absl::Status Http1Connection::Initialize() {
  return InitializeTransport(std::move(prebuffered_));
}

std::vector<unsigned char> Http1Connection::ClientAlpnWire() const {
  return std::vector<unsigned char>(std::begin(internal::kHttp1Alpn),
                                    std::end(internal::kHttp1Alpn));
}

absl::Status Http1Connection::OnAlpnNegotiated(std::string_view protocol) {
  // Cleartext connections do not negotiate ALPN; over TLS require http/1.1.
  if (!protocol.empty() && protocol != "http/1.1") {
    return absl::FailedPreconditionError(
        "TLS peer did not negotiate the http/1.1 ALPN protocol");
  }
  return absl::OkStatus();
}

absl::Status Http1Connection::SendProtocolPreamble() {
  return absl::OkStatus();  // HTTP/1.1 sends no connection preface.
}

absl::Status Http1Connection::OnInboundPlaintext(const char* absl_nonnull data,
                                                 size_t size) {
  inbuf_.append(data, size);
  return server() ? ServerParse() : ClientParse();
}

// --- Server request parsing and dispatch. ---

absl::Status Http1Connection::ServerParse() {
  while (true) {
    if (state_ == ParseState::kHead) {
      const std::optional<size_t> head_end =
          internal::FindHeaderBlockEnd(inbuf_);
      if (!head_end.has_value()) {
        if (inbuf_.size() > options().max_buffered_request_bytes) {
          return absl::ResourceExhaustedError("HTTP/1.1 request head too large");
        }
        return absl::OkStatus();  // Await the rest of the head.
      }
      absl::StatusOr<Http1RequestHead> head =
          internal::ParseRequestHead(std::string_view(inbuf_).substr(
              0, *head_end));
      inbuf_.erase(0, *head_end);
      if (!head.ok()) {
        return head.status();  // Malformed request: fail the connection.
      }
      request_head_ = std::move(*head);
      keep_alive_ = !WantsClose(request_head_.version, request_head_.headers);
      stream_id_ = next_stream_id_++;
      writer_state_ = std::make_shared<Http2ResponseWriter::State>();
      response_headers_sent_ = false;
      response_finished_ = false;
      response_chunked_ = false;
      request_body_.clear();
      request_chunk_decoder_ = ChunkedDecoder();
      streaming_request_body_ = false;
      request_body_state_ = nullptr;

      // An RFC 6455 WebSocket upgrade: dispatch immediately with a request body
      // stream carrying the post-101 frame bytes, and remember the client key.
      const std::optional<std::string> upgrade =
          GetHttpHeader(request_head_.headers, "upgrade");
      const std::optional<std::string> ws_key =
          GetHttpHeader(request_head_.headers, "sec-websocket-key");
      ws_upgrade_ = upgrade.has_value() &&
                    absl::EqualsIgnoreCase(*upgrade, "websocket") &&
                    ws_key.has_value();
      if (ws_upgrade_) {
        ws_key_ = *ws_key;
        request_body_state_ =
            std::make_shared<Http2RequestBodyStream::State>(
                options().max_buffered_request_bytes);
        return DispatchRequest();
      }

      ABSL_ASSIGN_OR_RETURN(request_body_plan_,
                            internal::PlanRequestBody(request_head_.headers));
      if (request_body_plan_.framing == BodyFraming::kNone) {
        return DispatchRequest();
      }
      request_body_remaining_ = request_body_plan_.content_length;
      state_ = ParseState::kBody;

      // A body the owner asked to receive incrementally: dispatch now and decode
      // into the request body stream as bytes arrive, rather than buffering the
      // whole thing and dispatching at its end.
      if (options().stream_request_body != nullptr &&
          options().stream_request_body(request_head_.method,
                                        request_head_.target,
                                        request_head_.headers)) {
        streaming_request_body_ = true;
        request_body_state_ = std::make_shared<Http2RequestBodyStream::State>(
            options().max_buffered_request_bytes);
        ABSL_RETURN_IF_ERROR(DispatchRequest());
        state_ = ParseState::kStreamingBody;
        continue;
      }
    }

    if (state_ == ParseState::kStreamingBody) {
      if (inbuf_.empty()) {
        return absl::OkStatus();  // Await more body bytes.
      }
      std::string decoded;
      bool complete = false;
      if (request_body_plan_.framing == BodyFraming::kContentLength) {
        const size_t take = std::min(request_body_remaining_, inbuf_.size());
        decoded.assign(inbuf_, 0, take);
        inbuf_.erase(0, take);
        request_body_remaining_ -= take;
        complete = request_body_remaining_ == 0;
      } else {  // Chunked.
        ABSL_RETURN_IF_ERROR(
            request_chunk_decoder_.Feed(inbuf_, &decoded, &complete));
        inbuf_.clear();
      }
      if (!decoded.empty() && request_body_state_ != nullptr) {
        // Push bounds itself by max_buffered_request_bytes, so a handler that
        // stops reading fails the connection rather than growing without limit.
        ABSL_RETURN_IF_ERROR(request_body_state_->Push(std::move(decoded)));
      }
      if (complete) {
        if (request_body_state_ != nullptr) {
          request_body_state_->Finish(absl::OkStatus());
        }
        state_ = ParseState::kAwaitingResponse;
      }
      return absl::OkStatus();
    }

    if (state_ == ParseState::kRaw) {
      // WebSocket frame bytes flow straight to the duplex body stream.
      if (request_body_state_ != nullptr && !inbuf_.empty()) {
        ABSL_RETURN_IF_ERROR(request_body_state_->Push(std::move(inbuf_)));
        inbuf_.clear();
      } else {
        inbuf_.clear();
      }
      return absl::OkStatus();
    }

    if (state_ == ParseState::kBody) {
      if (request_body_plan_.framing == BodyFraming::kContentLength) {
        const size_t take = std::min(request_body_remaining_, inbuf_.size());
        request_body_.append(inbuf_, 0, take);
        inbuf_.erase(0, take);
        request_body_remaining_ -= take;
        if (request_body_remaining_ > 0) {
          if (request_body_.size() > options().max_request_body_size) {
            return absl::OutOfRangeError("HTTP/1.1 request body too large");
          }
          return absl::OkStatus();  // Await more body bytes.
        }
      } else {  // Chunked.
        bool complete = false;
        ABSL_RETURN_IF_ERROR(request_chunk_decoder_.Feed(
            inbuf_, &request_body_, &complete));
        inbuf_.clear();
        if (request_body_.size() > options().max_request_body_size) {
          return absl::OutOfRangeError("HTTP/1.1 request body too large");
        }
        if (!complete) {
          return absl::OkStatus();  // Await more chunks.
        }
      }
      return DispatchRequest();
    }

    // kAwaitingResponse / kRaw / kDone: nothing more to parse right now.
    return absl::OkStatus();
  }
}

absl::Status Http1Connection::DispatchRequest() {
  state_ = ParseState::kAwaitingResponse;

  struct MakeWriterEnabler final : Http2ResponseWriter {
    MakeWriterEnabler(std::weak_ptr<HttpConnection> connection,
                      std::int32_t stream_id,
                      std::shared_ptr<Http2ResponseWriter::State> state)
        : Http2ResponseWriter(std::move(connection), stream_id,
                              std::move(state)) {}
  };

  struct MakeRequestBodyEnabler final : Http2RequestBodyStream {
    explicit MakeRequestBodyEnabler(
        std::shared_ptr<Http2RequestBodyStream::State> state)
        : Http2RequestBodyStream(std::move(state)) {}
  };

  HttpRequest request;
  request.scheme = secure() ? "https" : "http";
  request.authority =
      GetHttpHeader(request_head_.headers, "host").value_or("");
  request.path = request_head_.target;
  request.headers = request_head_.headers;
  if (ws_upgrade_) {
    // Present the upgrade as an extended CONNECT so the WebSocket server route
    // (which matches CONNECT + protocol "websocket") fires unchanged.
    request.method = "CONNECT";
    request.protocol = "websocket";
    request.body_stream =
        std::make_shared<MakeRequestBodyEnabler>(request_body_state_);
  } else {
    request.method = request_head_.method;
    if (streaming_request_body_) {
      request.body_stream =
          std::make_shared<MakeRequestBodyEnabler>(request_body_state_);
    } else {
      request.body = std::move(request_body_);
      request_body_.clear();
    }
  }

  auto writer = std::make_shared<MakeWriterEnabler>(
      std::weak_ptr<HttpConnection>(Self()), stream_id_, writer_state_);
  Http2RequestHandler handler = handler_;
  a11::Schedule([handler = std::move(handler), request = std::move(request),
                 writer = std::move(writer)]() mutable {
    // The handler was guarded when the connection adopted it; see the
    // Http2Connection constructor and
    // net/internal/exception_guarded_callbacks.h.
    const absl::Status status =
        handler(std::move(request), writer).Await().status();
    if (!status.ok()) {
      (void)writer->Abort(status);
    } else if (!writer->headers_sent()) {
      (void)writer->SendResponse(204);
    }
  });
  return absl::OkStatus();
}

// --- Client response parsing. ---

absl::StatusOr<std::shared_ptr<Http2ResponseStream>>
Http1Connection::SubmitRequest(std::string method, std::string /*scheme*/,
                               std::string authority, std::string path,
                               HttpHeaders headers, std::string body) {
  std::shared_ptr<Http1Connection> self = Self();
  return RunOnUvForConnection<std::shared_ptr<Http2ResponseStream>>(
      [self = std::move(self), method = std::move(method),
       authority = std::move(authority), path = std::move(path),
       headers = std::move(headers), body = std::move(body)]() mutable
          -> absl::StatusOr<std::shared_ptr<Http2ResponseStream>> {
        if (self->closed() || !self->connected()) {
          return absl::UnavailableError("HTTP/1.1 connection is not connected");
        }
        if (self->client_request_sent_) {
          return absl::FailedPreconditionError(
              "HTTP/1.1 connections carry a single request; open another "
              "connection for concurrent requests");
        }
        absl::AsciiStrToUpper(&method);
        NormalizeHttpHeaders(&headers);
        ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(headers));

        HttpHeaders wire = std::move(headers);
        if (GetHttpHeader(wire, "host") == std::nullopt && !authority.empty()) {
          SetHttpHeader(&wire, "host", authority);
        }
        if (!body.empty()) {
          SetHttpHeader(&wire, "content-length", std::to_string(body.size()));
        }
        std::string request = internal::SerializeRequest(method, path, wire);
        request.append(body);
        ABSL_RETURN_IF_ERROR(self->WriteApplicationData(
            reinterpret_cast<const std::uint8_t*>(request.data()),
            request.size()));

        self->client_request_sent_ = true;
        self->client_request_finished_ = true;
        self->client_method_ = method;
        self->PrepareClientResponseState();

        struct MakeResponseEnabler final : Http2ResponseStream {
          explicit MakeResponseEnabler(
              std::shared_ptr<Http2ResponseStream::State> state)
              : Http2ResponseStream(std::move(state)) {}
        };
        return std::static_pointer_cast<Http2ResponseStream>(
            std::make_shared<MakeResponseEnabler>(self->response_state_));
      });
}

void Http1Connection::PrepareClientResponseState() {
  response_state_ = std::make_shared<Http2ResponseStream::State>(
      options().max_buffered_response_bytes);
  std::weak_ptr<Http1Connection> weak = Self();
  thread::MutexLock lock(&response_state_->mu);
  response_state_->cancel = [weak](absl::Status status) -> absl::Status {
    if (std::shared_ptr<Http1Connection> connection = weak.lock()) {
      return connection->Close(std::move(status));
    }
    return absl::OkStatus();
  };
  // Backpressure: a full response buffer stops the socket read rather than
  // failing the transfer. Wired here as well as on the HTTP/2 path, because
  // both share Http2ResponseStream::State and a state without this hook would
  // simply buffer without bound.
  response_state_->set_read_paused = [weak](bool paused) {
    if (std::shared_ptr<Http1Connection> connection = weak.lock()) {
      connection->SetReadPaused(paused);
    }
  };
}

absl::StatusOr<std::shared_ptr<Http2DuplexStream>>
Http1Connection::SubmitStreamingRequest(std::string method,
                                        std::string /*scheme*/,
                                        std::string authority, std::string path,
                                        HttpHeaders headers) {
  std::shared_ptr<Http1Connection> self = Self();
  return RunOnUvForConnection<std::shared_ptr<Http2DuplexStream>>(
      [self = std::move(self), method = std::move(method),
       authority = std::move(authority), path = std::move(path),
       headers = std::move(headers)]() mutable
          -> absl::StatusOr<std::shared_ptr<Http2DuplexStream>> {
        if (self->closed() || !self->connected()) {
          return absl::UnavailableError("HTTP/1.1 connection is not connected");
        }
        if (self->client_request_sent_) {
          return absl::FailedPreconditionError(
              "HTTP/1.1 connections carry a single request; open another "
              "connection for concurrent requests");
        }
        absl::AsciiStrToUpper(&method);
        NormalizeHttpHeaders(&headers);
        ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(headers));
        if (GetHttpHeader(headers, "content-length").has_value()) {
          return absl::InvalidArgumentError(
              "A streamed HTTP/1.1 request body is chunked and must not carry "
              "content-length");
        }

        HttpHeaders wire = std::move(headers);
        if (GetHttpHeader(wire, "host") == std::nullopt && !authority.empty()) {
          SetHttpHeader(&wire, "host", authority);
        }
        // Chunked is how HTTP/1.1 sends a body of unknown length: the head goes
        // out now and each Write() becomes a chunk.
        SetHttpHeader(&wire, "transfer-encoding", "chunked");
        const std::string request =
            internal::SerializeRequest(method, path, wire);
        ABSL_RETURN_IF_ERROR(self->WriteApplicationData(
            reinterpret_cast<const std::uint8_t*>(request.data()),
            request.size()));

        self->client_request_sent_ = true;
        self->client_request_chunked_ = true;
        self->client_method_ = method;
        self->PrepareClientResponseState();

        struct MakeResponseEnabler final : Http2ResponseStream {
          explicit MakeResponseEnabler(
              std::shared_ptr<Http2ResponseStream::State> state)
              : Http2ResponseStream(std::move(state)) {}
        };
        struct MakeDuplexEnabler final : Http2DuplexStream {
          MakeDuplexEnabler(std::weak_ptr<HttpConnection> connection,
                            std::shared_ptr<Http2ResponseStream> response)
              : Http2DuplexStream(std::move(connection), std::move(response)) {}
        };
        auto response =
            std::make_shared<MakeResponseEnabler>(self->response_state_);
        return std::static_pointer_cast<Http2DuplexStream>(
            std::make_shared<MakeDuplexEnabler>(
                std::weak_ptr<HttpConnection>(self), std::move(response)));
      });
}

absl::StatusOr<std::shared_ptr<Http2DuplexStream>>
Http1Connection::SubmitDuplex(std::string protocol, std::string /*scheme*/,
                              std::string authority, std::string path,
                              HttpHeaders headers) {
  if (protocol != "websocket") {
    return absl::InvalidArgumentError(
        "HTTP/1.1 only supports the WebSocket upgrade protocol");
  }
  std::shared_ptr<Http1Connection> self = Self();
  return RunOnUvForConnection<std::shared_ptr<Http2DuplexStream>>(
      [self = std::move(self), authority = std::move(authority),
       path = std::move(path), headers = std::move(headers)]() mutable
          -> absl::StatusOr<std::shared_ptr<Http2DuplexStream>> {
        if (self->closed() || !self->connected()) {
          return absl::UnavailableError("HTTP/1.1 connection is not connected");
        }
        if (self->client_request_sent_) {
          return absl::FailedPreconditionError(
              "HTTP/1.1 connections carry a single request");
        }
        NormalizeHttpHeaders(&headers);
        ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(headers));
        self->client_ws_key_ = internal::GenerateWebSocketKey();
        HttpHeaders wire = std::move(headers);
        if (GetHttpHeader(wire, "host") == std::nullopt && !authority.empty()) {
          SetHttpHeader(&wire, "host", authority);
        }
        SetHttpHeader(&wire, "upgrade", "websocket");
        SetHttpHeader(&wire, "connection", "Upgrade");
        SetHttpHeader(&wire, "sec-websocket-key", self->client_ws_key_);
        SetHttpHeader(&wire, "sec-websocket-version", "13");
        const std::string request =
            internal::SerializeRequest("GET", path, wire);
        ABSL_RETURN_IF_ERROR(self->WriteApplicationData(
            reinterpret_cast<const std::uint8_t*>(request.data()),
            request.size()));

        self->client_request_sent_ = true;
        self->client_ws_ = true;
        self->client_method_ = "GET";
        self->response_state_ = std::make_shared<Http2ResponseStream::State>(
            self->options().max_buffered_response_bytes);
        {
          std::weak_ptr<Http1Connection> weak = self;
          thread::MutexLock lock(&self->response_state_->mu);
          self->response_state_->cancel =
              [weak](absl::Status status) -> absl::Status {
            if (std::shared_ptr<Http1Connection> connection = weak.lock()) {
              return connection->Close(std::move(status));
            }
            return absl::OkStatus();
          };
          // Backpressure: a full response buffer stops the socket read rather
          // than failing the transfer. Wired here as well as on the HTTP/2
          // path, because both share Http2ResponseStream::State and a state
          // without this hook would simply buffer without bound.
          self->response_state_->set_read_paused = [weak](bool paused) {
            if (std::shared_ptr<Http1Connection> connection = weak.lock()) {
              connection->SetReadPaused(paused);
            }
          };
        }

        struct MakeResponseEnabler final : Http2ResponseStream {
          explicit MakeResponseEnabler(
              std::shared_ptr<Http2ResponseStream::State> state)
              : Http2ResponseStream(std::move(state)) {}
        };
        struct MakeDuplexEnabler final : Http2DuplexStream {
          MakeDuplexEnabler(std::weak_ptr<HttpConnection> connection,
                            std::shared_ptr<Http2ResponseStream> response)
              : Http2DuplexStream(std::move(connection), std::move(response)) {}
        };
        auto response =
            std::make_shared<MakeResponseEnabler>(self->response_state_);
        return std::static_pointer_cast<Http2DuplexStream>(
            std::make_shared<MakeDuplexEnabler>(
                std::weak_ptr<HttpConnection>(self), std::move(response)));
      });
}

absl::Status Http1Connection::ClientParse() {
  if (response_state_ == nullptr) {
    return absl::OkStatus();  // No request in flight yet.
  }

  // Client WebSocket: parse the 101 handshake, then flow raw frames through.
  if (client_ws_) {
    if (!client_head_parsed_) {
      const std::optional<size_t> head_end =
          internal::FindHeaderBlockEnd(inbuf_);
      if (!head_end.has_value()) {
        return absl::OkStatus();
      }
      absl::StatusOr<Http1ResponseHead> head = internal::ParseResponseHead(
          std::string_view(inbuf_).substr(0, *head_end));
      inbuf_.erase(0, *head_end);
      if (!head.ok()) {
        response_state_->Finish(head.status());
        return head.status();
      }
      if (head->status != 101) {
        const absl::Status status = absl::UnavailableError(absl::StrCat(
            "HTTP/1.1 WebSocket upgrade returned ", head->status));
        response_state_->Finish(status);
        return status;
      }
      const std::optional<std::string> accept =
          GetHttpHeader(head->headers, "sec-websocket-accept");
      if (!accept.has_value() ||
          *accept != internal::ComputeWebSocketAccept(client_ws_key_)) {
        const absl::Status status = absl::UnauthenticatedError(
            "HTTP/1.1 WebSocket Sec-WebSocket-Accept mismatch");
        response_state_->Finish(status);
        return status;
      }
      client_head_parsed_ = true;
      state_ = ParseState::kRaw;
      // The channel checks for a 2xx status; present the successful upgrade as
      // 200 so the shared WebSocket channel proceeds unchanged.
      response_state_->SetHeaders(HttpResponseHead{.status = 200});
    }
    if (!inbuf_.empty()) {
      ABSL_RETURN_IF_ERROR(response_state_->Push(std::move(inbuf_)));
      inbuf_.clear();
    }
    return absl::OkStatus();
  }

  if (!client_head_parsed_) {
    const std::optional<size_t> head_end = internal::FindHeaderBlockEnd(inbuf_);
    if (!head_end.has_value()) {
      return absl::OkStatus();
    }
    absl::StatusOr<Http1ResponseHead> head = internal::ParseResponseHead(
        std::string_view(inbuf_).substr(0, *head_end));
    inbuf_.erase(0, *head_end);
    if (!head.ok()) {
      response_state_->Finish(head.status());
      return head.status();
    }
    // Informational 1xx responses precede the real response; skip them.
    if (head->status >= 100 && head->status < 200) {
      return ClientParse();
    }
    client_head_parsed_ = true;
    HttpResponseHead response_head;
    response_head.status = head->status;
    response_head.headers = head->headers;
    response_state_->SetHeaders(std::move(response_head));
    ABSL_ASSIGN_OR_RETURN(
        response_body_plan_,
        internal::PlanResponseBody(client_method_, head->status,
                                   head->headers));
    response_body_remaining_ = response_body_plan_.content_length;
    response_chunk_decoder_ = ChunkedDecoder();
    if (response_body_plan_.framing == BodyFraming::kNone) {
      response_state_->Finish(absl::OkStatus());
      state_ = ParseState::kDone;
      return absl::OkStatus();
    }
  }

  if (inbuf_.empty()) {
    return absl::OkStatus();
  }
  switch (response_body_plan_.framing) {
    case BodyFraming::kContentLength: {
      const size_t take = std::min(response_body_remaining_, inbuf_.size());
      ABSL_RETURN_IF_ERROR(response_state_->Push(inbuf_.substr(0, take)));
      inbuf_.erase(0, take);
      response_body_remaining_ -= take;
      if (response_body_remaining_ == 0) {
        response_state_->Finish(absl::OkStatus());
        state_ = ParseState::kDone;
      }
      break;
    }
    case BodyFraming::kChunked: {
      std::string decoded;
      bool complete = false;
      ABSL_RETURN_IF_ERROR(
          response_chunk_decoder_.Feed(inbuf_, &decoded, &complete));
      inbuf_.clear();
      if (!decoded.empty()) {
        ABSL_RETURN_IF_ERROR(response_state_->Push(std::move(decoded)));
      }
      if (complete) {
        // Chunked is the only HTTP/1.1 framing that can carry a trailer
        // section, and it has to be set before Finish() publishes it.
        response_state_->SetTrailers(response_chunk_decoder_.trailers());
        response_state_->Finish(absl::OkStatus());
        state_ = ParseState::kDone;
      }
      break;
    }
    case BodyFraming::kUntilClose: {
      ABSL_RETURN_IF_ERROR(response_state_->Push(std::move(inbuf_)));
      inbuf_.clear();
      break;  // Completes when the connection closes (see OnClose).
    }
    case BodyFraming::kNone:
      break;
  }
  return absl::OkStatus();
}

// --- Response emission (server). ---

absl::Status Http1Connection::SendHeaders(std::int32_t stream_id, int status,
                                          HttpHeaders headers) {
  std::shared_ptr<Http1Connection> self = Self();
  return RunStatusOnUvForConnection([self = std::move(self), stream_id, status,
                                     headers = std::move(headers)]() mutable {
    return self->SendHeadersOnLoop(stream_id, status, std::move(headers));
  });
}

absl::Status Http1Connection::SendHeadersOnLoop(std::int32_t stream_id,
                                                int status,
                                                HttpHeaders headers) {
  if (server() == false) {
    return absl::FailedPreconditionError(
        "A client HTTP/1.1 connection cannot send response headers");
  }
  if (stream_id != stream_id_) {
    return absl::NotFoundError("HTTP/1.1 request stream is no longer active");
  }
  if (response_headers_sent_) {
    return absl::FailedPreconditionError(
        "HTTP/1.1 response headers have already been sent");
  }
  if (status < 100 || status > 599) {
    return absl::InvalidArgumentError("HTTP response status is invalid");
  }
  // A WebSocket upgrade: the handler's success (any status) becomes the RFC
  // 6455 101 handshake, after which the socket carries raw frames.
  if (ws_upgrade_) {
    HttpHeaders upgrade_headers{
        {"upgrade", "websocket"},
        {"connection", "Upgrade"},
        {"sec-websocket-accept", internal::ComputeWebSocketAccept(ws_key_)}};
    const std::string head =
        internal::SerializeResponse(101, upgrade_headers);
    ABSL_RETURN_IF_ERROR(WriteApplicationData(
        reinterpret_cast<const std::uint8_t*>(head.data()), head.size()));
    response_headers_sent_ = true;
    state_ = ParseState::kRaw;
    // Drain any frame bytes already buffered past the request head.
    if (request_body_state_ != nullptr && !inbuf_.empty()) {
      ABSL_RETURN_IF_ERROR(request_body_state_->Push(std::move(inbuf_)));
      inbuf_.clear();
    }
    return absl::OkStatus();
  }
  NormalizeHttpHeaders(&headers);
  ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(headers));
  // A streaming response (no Content-Length) uses chunked transfer-encoding.
  if (GetHttpHeader(headers, "content-length") == std::nullopt) {
    SetHttpHeader(&headers, "transfer-encoding", "chunked");
    response_chunked_ = true;
  }
  if (GetHttpHeader(headers, "connection") == std::nullopt) {
    SetHttpHeader(&headers, "connection",
                  keep_alive_ ? "keep-alive" : "close");
  }
  const std::string head = internal::SerializeResponse(status, headers);
  ABSL_RETURN_IF_ERROR(WriteApplicationData(
      reinterpret_cast<const std::uint8_t*>(head.data()), head.size()));
  response_headers_sent_ = true;
  return absl::OkStatus();
}

absl::Status Http1Connection::Write(std::int32_t stream_id, std::string data) {
  std::shared_ptr<Http1Connection> self = Self();
  const size_t bytes = data.size();
  return PostWrite(
      bytes,
      [self = std::move(self), stream_id, data = std::move(data)]() mutable {
        return self->WriteOnLoop(stream_id, std::move(data));
      });
}

absl::Status Http1Connection::WriteOnLoop(std::int32_t stream_id,
                                          std::string data) {
  if (data.empty()) {
    return absl::OkStatus();
  }
  if (stream_id != stream_id_) {
    return absl::NotFoundError("HTTP/1.1 request stream is no longer active");
  }
  if (!response_headers_sent_) {
    return absl::FailedPreconditionError(
        "SendHeaders must be called before writing an HTTP/1.1 response");
  }
  if (response_finished_) {
    return absl::FailedPreconditionError("HTTP/1.1 response has finished");
  }
  // In raw (WebSocket) mode the caller has already framed the bytes.
  if (state_ == ParseState::kRaw) {
    return WriteApplicationData(
        reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
  }
  const std::string wire =
      response_chunked_ ? internal::EncodeChunk(data) : data;
  return WriteApplicationData(
      reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size());
}

absl::Status Http1Connection::Finish(std::int32_t stream_id) {
  std::shared_ptr<Http1Connection> self = Self();
  return RunStatusOnUvForConnection([self = std::move(self), stream_id]() {
    return self->FinishOnLoop(stream_id);
  });
}

absl::Status Http1Connection::FinishWithTrailers(std::int32_t stream_id,
                                                 HttpHeaders trailers) {
  std::shared_ptr<Http1Connection> self = Self();
  return RunStatusOnUvForConnection([self = std::move(self), stream_id,
                                     trailers = std::move(trailers)]() mutable {
    return self->FinishOnLoop(stream_id, std::move(trailers));
  });
}

absl::Status Http1Connection::FinishOnLoop(std::int32_t stream_id,
                                           HttpHeaders trailers) {
  if (stream_id != stream_id_ || response_finished_) {
    return absl::OkStatus();
  }
  if (!response_headers_sent_) {
    return absl::FailedPreconditionError(
        "SendHeaders must be called before finishing an HTTP/1.1 response");
  }
  // Only a chunked body has a place to put a trailer section. On a
  // content-length response there is nowhere for them to go, so they are
  // dropped rather than corrupting the framing.
  if (response_chunked_) {
    NormalizeHttpHeaders(&trailers);
    ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(trailers));
    const std::string last = internal::EncodeLastChunk(trailers);
    ABSL_RETURN_IF_ERROR(WriteApplicationData(
        reinterpret_cast<const std::uint8_t*>(last.data()), last.size()));
  }
  response_finished_ = true;
  FinishResponseAndAdvance();
  return absl::OkStatus();
}

absl::Status Http1Connection::SendResponse(std::int32_t stream_id, int status,
                                           HttpHeaders headers,
                                           std::string body) {
  std::shared_ptr<Http1Connection> self = Self();
  return RunStatusOnUvForConnection([self = std::move(self), stream_id, status,
                                     headers = std::move(headers),
                                     body = std::move(
                                         body)]() mutable -> absl::Status {
    if (self->server() == false) {
      return absl::FailedPreconditionError(
          "A client HTTP/1.1 connection cannot send a response");
    }
    if (stream_id != self->stream_id_) {
      return absl::NotFoundError("HTTP/1.1 request stream is no longer active");
    }
    if (self->response_headers_sent_) {
      return absl::FailedPreconditionError(
          "HTTP/1.1 response headers have already been sent");
    }
    if (status < 100 || status > 599) {
      return absl::InvalidArgumentError("HTTP response status is invalid");
    }
    NormalizeHttpHeaders(&headers);
    ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(headers));
    SetHttpHeader(&headers, "content-length", std::to_string(body.size()));
    if (GetHttpHeader(headers, "connection") == std::nullopt) {
      SetHttpHeader(&headers, "connection",
                    self->keep_alive_ ? "keep-alive" : "close");
    }
    std::string wire = internal::SerializeResponse(status, headers);
    wire.append(body);
    ABSL_RETURN_IF_ERROR(self->WriteApplicationData(
        reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()));
    self->response_headers_sent_ = true;
    self->response_finished_ = true;
    self->FinishResponseAndAdvance();
    return absl::OkStatus();
  });
}

absl::StatusOr<std::shared_ptr<Http2ResponseWriter>>
Http1Connection::SubmitPushPromise(std::int32_t /*stream_id*/,
                                   std::string /*method*/, std::string /*path*/,
                                   HttpHeaders /*headers*/) {
  // HTTP/1.1 has one response per request and no frame to promise another with.
  return absl::UnimplementedError("HTTP/1.1 does not support server push");
}

absl::Status Http1Connection::AbortResponse(std::int32_t stream_id,
                                            absl::Status status) {
  if (status.ok()) {
    return absl::InvalidArgumentError("Response abort status must be non-OK");
  }
  std::shared_ptr<Http1Connection> self = Self();
  return RunStatusOnUvForConnection([self = std::move(self), stream_id,
                                     status = std::move(
                                         status)]() mutable -> absl::Status {
    if (stream_id != self->stream_id_) {
      return absl::OkStatus();
    }
    if (!self->response_headers_sent_) {
      HttpHeaders headers;
      headers.emplace_back("content-type", "text/plain; charset=utf-8");
      // Reuse the buffered-response path; SendResponse advances the exchange.
      return self->SendResponse(stream_id, StatusCodeToHttp(status.code()),
                                std::move(headers),
                                std::string(status.message()));
    }
    // Headers already sent: HTTP/1.1 cannot reset a stream, so close.
    self->CloseOnLoop(status);
    return absl::OkStatus();
  });
}

void Http1Connection::FinishResponseAndAdvance() {
  if (writer_state_ != nullptr) {
    writer_state_->Finish(absl::OkStatus());
  }
  // A finished WebSocket (or a non-keep-alive response) ends the connection;
  // an upgraded socket cannot be reused for another HTTP request. Nor can one
  // whose request body was streamed -- see streaming_request_body_.
  if (!keep_alive_ || ws_upgrade_ || streaming_request_body_ ||
      state_ == ParseState::kRaw) {
    CloseOnLoop(absl::OkStatus());
    return;
  }
  // Reset per-exchange state and parse any pipelined bytes already buffered.
  writer_state_ = nullptr;
  state_ = ParseState::kHead;
  const absl::Status parsed = ServerParse();
  if (!parsed.ok()) {
    CloseOnLoop(parsed);
  }
}

absl::StatusOr<bool> Http1Connection::ResponseHeadersSent(
    std::int32_t stream_id) {
  std::shared_ptr<Http1Connection> self = Self();
  return RunOnUvForConnection<bool>(
      [self = std::move(self), stream_id]() -> absl::StatusOr<bool> {
        return stream_id == self->stream_id_ && self->response_headers_sent_;
      });
}

absl::StatusOr<bool> Http1Connection::ResponseFinished(
    std::int32_t stream_id) {
  std::shared_ptr<Http1Connection> self = Self();
  return RunOnUvForConnection<bool>(
      [self = std::move(self), stream_id]() -> absl::StatusOr<bool> {
        return stream_id != self->stream_id_ || self->response_finished_;
      });
}

// Client WebSocket writes: the caller has already framed the bytes, so they go
// straight to the socket.
absl::Status Http1Connection::WriteRequest(std::int32_t /*stream_id*/,
                                           std::string data) {
  if (data.empty()) {
    return absl::OkStatus();
  }
  std::shared_ptr<Http1Connection> self = Self();
  const size_t bytes = data.size();
  return PostWrite(
      bytes,
      [self = std::move(self), data = std::move(data)]() -> absl::Status {
        if (self->closed()) {
          return absl::UnavailableError("HTTP/1.1 connection is closed");
        }
        // Two kinds of duplex request write here. A WebSocket's bytes are
        // already framed by its caller and go out untouched; a streamed request
        // body is framed as a chunk on the way.
        if (self->client_request_chunked_) {
          if (self->client_request_finished_) {
            return absl::FailedPreconditionError(
                "HTTP/1.1 request body has already finished");
          }
          const std::string wire = internal::EncodeChunk(data);
          return self->WriteApplicationData(
              reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size());
        }
        if (!self->client_ws_) {
          return absl::UnavailableError("HTTP/1.1 request is not writable");
        }
        return self->WriteApplicationData(
            reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
      });
}

absl::Status Http1Connection::FinishRequest(std::int32_t /*stream_id*/) {
  std::shared_ptr<Http1Connection> self = Self();
  return RunStatusOnUvForConnection([self = std::move(self)]() -> absl::Status {
    // A chunked request body ends with the terminating zero-length chunk, which
    // is HTTP/1.1's only request-side half-close. A WebSocket has none: its
    // close frame and the eventual TCP close end the exchange instead.
    if (!self->client_request_chunked_ || self->client_request_finished_) {
      return absl::OkStatus();
    }
    self->client_request_finished_ = true;
    if (self->closed()) {
      return absl::OkStatus();
    }
    const std::string last = internal::EncodeLastChunk();
    return self->WriteApplicationData(
        reinterpret_cast<const std::uint8_t*>(last.data()), last.size());
  });
}

void Http1Connection::OnClose(const absl::Status& status) {
  if (response_state_ != nullptr) {
    // A body delimited by connection close completes cleanly on EOF.
    const bool clean_eof =
        client_head_parsed_ &&
        response_body_plan_.framing == BodyFraming::kUntilClose;
    response_state_->Finish(clean_eof ? absl::OkStatus() : status);
  }
  if (writer_state_ != nullptr) {
    writer_state_->Finish(status);
  }
  if (request_body_state_ != nullptr) {
    // A cleanly-closed WebSocket ends its inbound frame stream without error,
    // and so does a streamed request body the peer already ended.
    const bool clean_eof = state_ == ParseState::kRaw ||
                           (streaming_request_body_ &&
                            state_ == ParseState::kAwaitingResponse);
    request_body_state_->Finish(clean_eof ? absl::OkStatus() : status);
  }
}

}  // namespace a11::net
