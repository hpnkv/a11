// Copyright 2026 The A11 Authors.

#include "a11/net/http2.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <uvw.hpp>
#include <vector>

#include <absl/base/no_destructor.h>
#include <absl/container/flat_hash_map.h>
#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <arpa/inet.h>
#include <nghttp2/nghttp2.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/net/internal/exception_guarded_callbacks.h"
#include "a11/net/internal/http1_connection.h"
#include "a11/net/internal/http_connection.h"
#include "a11/net/internal/http_streams.h"
#include "a11/net/internal/http_transport.h"
#include "a11/status.h"
#include "thread/boost_primitives.h"

namespace a11::net {

using internal::CreateTlsContext;
using internal::HttpTransport;
using internal::IsIpAddress;
using internal::OpenSslErrorMessage;
using internal::ProtocolPolicy;
using internal::RunOnUv;
using internal::RunStatusOnUv;
using internal::SslContext;
using internal::TlsError;
using internal::UvError;
using internal::UvExecutor;

namespace {

/**
 * The connection-level HTTP/2 receive window.
 *
 * Sized as a bandwidth-delay product with room to spare: 8 MiB covers a gigabit
 * path at 60 ms of latency, which is well past anything a local or regional
 * endpoint presents. The peer never has to wait for an acknowledgement to keep
 * sending, so a bulk transfer runs at line rate rather than at
 * window-per-round-trip.
 *
 * This is a *receive* window, so the cost is bounded by what a peer chooses to
 * send ahead; A11's own reader applies backpressure separately, through
 * Http2Options::max_buffered_response_bytes.
 */
/**
 * The connection-level HTTP/2 receive window.
 *
 * HTTP/2 defaults both the connection and each stream to 65535 bytes, and
 * SETTINGS_INITIAL_WINDOW_SIZE governs *streams only* -- the connection window
 * has to be raised on its own. A window is a bandwidth-delay product: 64 KiB
 * against a CDN edge 40 ms away is 1.6 MB/s no matter how fast the link, which
 * is what made downloads here run at a few MB/s on a multi-gigabit connection.
 *
 * Shared by every stream on the connection, so it is sized above the per-stream
 * window rather than equal to it.
 */
constexpr std::int32_t kConnectionWindowSize = 16 * 1024 * 1024;

/**
 * @brief The per-stream receive window, bounded by what we will buffer.
 *
 * A receive window is a promise to accept that much unacknowledged data, so
 * promising more than the reader is willing to hold just moves the queue into
 * memory we did not budget. Tying the two together means one knob --
 * Http2Options::max_buffered_response_bytes -- sets both how much is buffered
 * and how much may be in flight, and a caller doing a bulk transfer raises them
 * together.
 */
/**
 * How much space a response batch reserves on its first DATA frame.
 *
 * One TCP read commonly carries several 16 KiB frames, and a batch is bounded by
 * what a single read delivered, so this is sized to libuv's read buffer.
 * Reserving up front means the batch grows by appending into spare capacity
 * rather than reallocating and copying its way up.
 */
constexpr size_t kResponseBatchReserve = 64 * 1024;

inline std::int32_t StreamWindowSize(const Http2Options& options) {
  constexpr size_t kFloor = 256 * 1024;
  constexpr size_t kCeiling = 16 * 1024 * 1024;
  return static_cast<std::int32_t>(
      std::clamp(options.max_buffered_response_bytes, kFloor, kCeiling));
}

absl::Status Nghttp2Error(int code, std::string_view operation) {
  return absl::InternalError(
      absl::StrCat(operation, " failed: ", nghttp2_strerror(code)));
}

std::vector<nghttp2_nv> MakeNv(
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::vector<nghttp2_nv> result;
  result.reserve(values.size());
  for (const auto& [name, value] : values) {
    result.push_back(nghttp2_nv{
        .name = reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())),
        .value =
            reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
        .namelen = name.size(),
        .valuelen = value.size(),
        .flags = NGHTTP2_NV_FLAG_NONE});
  }
  return result;
}

std::vector<std::pair<std::string, std::string>> RequestHeaders(
    std::string_view method, std::string_view scheme,
    std::string_view authority, std::string_view path,
    std::string_view protocol, const HttpHeaders& headers) {
  std::vector<std::pair<std::string, std::string>> values;
  values.reserve(headers.size() + 4 + (protocol.empty() ? 0 : 1));
  values.emplace_back(":method", method);
  if (!protocol.empty()) {
    values.emplace_back(":protocol", protocol);
  }
  values.emplace_back(":scheme", scheme);
  values.emplace_back(":authority", authority);
  values.emplace_back(":path", path);
  for (const auto& header : headers) {
    values.push_back(header);
  }
  return values;
}

std::vector<std::pair<std::string, std::string>> ResponseHeaders(
    int status, HttpHeaders headers) {
  std::vector<std::pair<std::string, std::string>> values;
  values.reserve(headers.size() + 1);
  values.emplace_back(":status", std::to_string(status));
  for (auto& header : headers) {
    values.push_back(std::move(header));
  }
  return values;
}

absl::Status Http2StreamError(std::uint32_t error_code,
                              std::string_view context) {
  if (error_code == NGHTTP2_NO_ERROR) {
    return absl::OkStatus();
  }
  if (error_code == NGHTTP2_CANCEL) {
    return absl::CancelledError(absl::StrCat(context, " was cancelled"));
  }
  if (error_code == NGHTTP2_REFUSED_STREAM) {
    return absl::UnavailableError(absl::StrCat(context, " was refused"));
  }
  if (error_code == NGHTTP2_ENHANCE_YOUR_CALM) {
    return absl::ResourceExhaustedError(
        absl::StrCat(context, " exceeded peer limits"));
  }
  return absl::UnavailableError(
      absl::StrCat(context, " closed with HTTP/2 error ", error_code));
}

std::uint32_t StatusToHttp2Error(const absl::Status& status) {
  if (absl::IsCancelled(status) || absl::IsDeadlineExceeded(status)) {
    return NGHTTP2_CANCEL;
  }
  if (absl::IsResourceExhausted(status) || absl::IsOutOfRange(status)) {
    return NGHTTP2_ENHANCE_YOUR_CALM;
  }
  return NGHTTP2_INTERNAL_ERROR;
}

}  // namespace

a11::Future<std::optional<std::string>> Http2RequestBodyStream::Read() {
  std::string chunk;
  bool has_chunk = false;
  bool done = false;
  absl::Status status;
  std::shared_ptr<a11::Promise<std::optional<std::string>>> promise;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->pending_read != nullptr) {
      return a11::FailedFuture<std::optional<std::string>>(
          absl::FailedPreconditionError(
              "Only one HTTP/2 request-body Read may be outstanding"));
    }
    if (!state_->chunks.empty()) {
      chunk = std::move(state_->chunks.front());
      state_->chunks.pop_front();
      state_->buffered_bytes -= chunk.size();
      has_chunk = true;
    } else if (state_->done) {
      done = true;
      status = state_->status;
    } else {
      promise = std::make_shared<a11::Promise<std::optional<std::string>>>();
      state_->pending_read = promise;
    }
  }
  if (has_chunk) {
    return a11::ReadyFuture(std::optional<std::string>(std::move(chunk)));
  }
  if (done) {
    if (!status.ok()) {
      return a11::FailedFuture<std::optional<std::string>>(status);
    }
    return a11::ReadyFuture(std::optional<std::string>());
  }
  return promise->future();
}

a11::Task Http2RequestBodyStream::Done() const {
  return state_->done_future;
}

absl::Status Http2RequestBodyStream::Cancel(absl::Status status) {
  if (status.ok()) {
    return absl::InvalidArgumentError("Cancellation status must be non-OK");
  }
  std::function<absl::Status(absl::Status)> cancel;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->done) {
      return absl::OkStatus();
    }
    cancel = state_->cancel;
  }
  if (!cancel) {
    return absl::FailedPreconditionError(
        "HTTP/2 request body is not attached to a stream");
  }
  return cancel(std::move(status));
}

std::int32_t Http2RequestBodyStream::stream_id() const {
  thread::MutexLock lock(&state_->mu);
  return state_->stream_id;
}

a11::Future<HttpResponseHead> Http2ResponseStream::Headers() const {
  return state_->headers_future;
}

a11::Future<std::optional<std::string>> Http2ResponseStream::Read() {
  std::string chunk;
  bool has_chunk = false;
  bool done = false;
  absl::Status status;
  std::shared_ptr<a11::Promise<std::optional<std::string>>> promise;
  std::function<void(bool)> resume;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->pending_read != nullptr) {
      return a11::FailedFuture<std::optional<std::string>>(
          absl::FailedPreconditionError(
              "Only one HTTP/2 response Read may be outstanding"));
    }
    if (!state_->chunks.empty()) {
      chunk = std::move(state_->chunks.front());
      state_->chunks.pop_front();
      state_->buffered_bytes -= chunk.size();
      has_chunk = true;
      // Resume at the low-water mark rather than as soon as one chunk leaves,
      // so a reader keeping pace does not toggle the socket on every chunk.
      if (state_->buffered_bytes <= state_->max_buffered_bytes / 2) {
        resume = state_->set_read_paused;
      }
    } else if (state_->done) {
      done = true;
      status = state_->status;
    } else {
      promise = std::make_shared<a11::Promise<std::optional<std::string>>>();
      state_->pending_read = promise;
      // Nothing buffered and someone is waiting: definitely take more.
      resume = state_->set_read_paused;
    }
  }
  if (resume) {
    resume(false);
  }
  if (has_chunk) {
    return a11::ReadyFuture(std::optional<std::string>(std::move(chunk)));
  }
  if (done) {
    if (!status.ok()) {
      return a11::FailedFuture<std::optional<std::string>>(status);
    }
    return a11::ReadyFuture(std::optional<std::string>());
  }
  return promise->future();
}

a11::Future<HttpHeaders> Http2ResponseStream::Trailers() const {
  return state_->trailers_future;
}

a11::Future<std::optional<HttpPushedResponse>> Http2ResponseStream::NextPush() {
  std::optional<HttpPushedResponse> promised;
  bool done = false;
  absl::Status status;
  std::shared_ptr<a11::Promise<std::optional<HttpPushedResponse>>> promise;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->pending_push != nullptr) {
      return a11::FailedFuture<std::optional<HttpPushedResponse>>(
          absl::FailedPreconditionError(
              "Only one HTTP/2 NextPush may be outstanding"));
    }
    if (!state_->pushes.empty()) {
      promised = std::move(state_->pushes.front());
      state_->pushes.pop_front();
    } else if (state_->done) {
      // Queue drained and the associated response has ended: no further promise
      // can arrive on it.
      done = true;
      status = state_->status;
    } else {
      promise =
          std::make_shared<a11::Promise<std::optional<HttpPushedResponse>>>();
      state_->pending_push = promise;
    }
  }
  if (promised.has_value()) {
    return a11::ReadyFuture(std::move(promised));
  }
  if (done) {
    if (!status.ok()) {
      return a11::FailedFuture<std::optional<HttpPushedResponse>>(status);
    }
    return a11::ReadyFuture(std::optional<HttpPushedResponse>());
  }
  return promise->future();
}

a11::Task Http2ResponseStream::Done() const {
  return state_->done_future;
}

absl::Status Http2ResponseStream::Cancel(absl::Status status) {
  if (status.ok()) {
    return absl::InvalidArgumentError("Cancellation status must be non-OK");
  }
  std::function<absl::Status(absl::Status)> cancel;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->done) {
      return absl::OkStatus();
    }
    cancel = state_->cancel;
  }
  if (!cancel) {
    return absl::FailedPreconditionError(
        "HTTP/2 response is not attached to a request");
  }
  return cancel(std::move(status));
}

std::int32_t Http2ResponseStream::stream_id() const {
  thread::MutexLock lock(&state_->mu);
  return state_->stream_id;
}

void NormalizeHttpHeaders(HttpHeaders* headers) {
  for (auto& [name, value] : *headers) {
    (void)value;
    absl::AsciiStrToLower(&name);
  }
}

std::optional<std::string> GetHttpHeader(const HttpHeaders& headers,
                                         std::string_view name) {
  std::string normalized(name);
  absl::AsciiStrToLower(&normalized);
  const auto iterator = std::find_if(
      headers.begin(), headers.end(),
      [&](const auto& header) { return header.first == normalized; });
  if (iterator == headers.end()) {
    return std::nullopt;
  }
  return iterator->second;
}

void EraseHttpHeader(HttpHeaders* headers, std::string_view name) {
  std::string normalized(name);
  absl::AsciiStrToLower(&normalized);
  std::erase_if(*headers,
                [&](const auto& header) { return header.first == normalized; });
}

void SetHttpHeader(HttpHeaders* headers, std::string name, std::string value) {
  absl::AsciiStrToLower(&name);
  EraseHttpHeader(headers, name);
  headers->emplace_back(std::move(name), std::move(value));
}

absl::Status ValidateHttpHeaders(const HttpHeaders& headers) {
  for (const auto& [name, value] : headers) {
    if (name.empty() || name.front() == ':') {
      return absl::InvalidArgumentError(
          "Application HTTP headers must have non-pseudo field names");
    }
    if (!std::all_of(name.begin(), name.end(), [](unsigned char character) {
          return character == '-' || character == '_' || character == '.' ||
                 std::isdigit(character) ||
                 (character >= 'a' && character <= 'z');
        })) {
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid lowercase HTTP/2 field name: ", name));
    }
    if (value.find('\0') != std::string::npos ||
        value.find('\r') != std::string::npos ||
        value.find('\n') != std::string::npos) {
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid HTTP/2 field value for ", name));
    }
  }
  return absl::OkStatus();
}

absl::Status Http2TlsOptions::Validate() const {
  const bool has_certificate = !certificate_pem_file.empty();
  const bool has_key = !key_pem_file.empty();
  if (has_certificate != has_key) {
    return absl::InvalidArgumentError(
        "TLS certificate_pem_file and key_pem_file must be set together");
  }
  if (!enabled && (has_certificate || !ca_certificate_pem_file.empty())) {
    return absl::InvalidArgumentError(
        "TLS certificate paths require tls.enabled=true");
  }
  return absl::OkStatus();
}

absl::Status Http2Options::Validate() const {
  if (max_request_body_size == 0 || max_response_body_size == 0 ||
      max_buffered_request_bytes == 0 || max_buffered_response_bytes == 0) {
    return absl::InvalidArgumentError(
        "HTTP/2 body and buffer limits must be positive");
  }
  if (!enable_h2 && !enable_h2c && !enable_http1) {
    return absl::InvalidArgumentError(
        "At least one of enable_h2, enable_h2c, enable_http1 must be set");
  }
  if (tls.enabled && !enable_h2 && !enable_http1) {
    return absl::InvalidArgumentError(
        "A TLS endpoint must enable HTTP/2 (h2) or HTTP/1.1; h2c is cleartext");
  }
  if (!tls.enabled && !enable_h2c && !enable_http1) {
    return absl::InvalidArgumentError(
        "A cleartext endpoint must enable h2c or HTTP/1.1");
  }
  if (client_preference == ProtocolPreference::kHttp2 && !enable_h2 &&
      !enable_h2c) {
    return absl::InvalidArgumentError(
        "client_preference=kHttp2 requires enable_h2 or enable_h2c");
  }
  if (client_preference == ProtocolPreference::kHttp11 && !enable_http1) {
    return absl::InvalidArgumentError(
        "client_preference=kHttp11 requires enable_http1");
  }
  return tls.Validate();
}

class Http2Connection : public internal::HttpTransport, public HttpConnection {
 public:
  /// Down-casts the shared HttpTransport identity to this concrete type.
  std::shared_ptr<Http2Connection> Self() {
    return std::static_pointer_cast<Http2Connection>(shared_from_this());
  }

  static absl::StatusOr<std::shared_ptr<Http2Connection>> Create(
      std::shared_ptr<uvw::tcp_handle> tcp, bool server,
      Http2RequestHandler handler, Http2Options options,
      SslContext tls_context = {}, std::string tls_server_name = {},
      std::function<void(HttpTransport*)> on_closed = {},
      std::string prebuffered = {}) {
    if (tcp == nullptr) {
      return absl::InvalidArgumentError("TCP handle must not be null");
    }

    struct MakeSharedEnabler final : Http2Connection {
      MakeSharedEnabler(std::shared_ptr<uvw::tcp_handle> tcp, bool server,
                        Http2RequestHandler handler, Http2Options options,
                        SslContext tls_context, std::string tls_server_name,
                        std::function<void(HttpTransport*)> on_closed,
                        std::string prebuffered)
          : Http2Connection(std::move(tcp), server, std::move(handler), options,
                            std::move(tls_context), std::move(tls_server_name),
                            std::move(on_closed), std::move(prebuffered)) {}
    };

    auto connection = std::make_shared<MakeSharedEnabler>(
        std::move(tcp), server, std::move(handler), options,
        std::move(tls_context), std::move(tls_server_name),
        std::move(on_closed), std::move(prebuffered));
    ABSL_RETURN_IF_ERROR(connection->Initialize());
    return connection;
  }

  ~Http2Connection() override {
    if (session_ != nullptr) {
      nghttp2_session_del(session_);
    }
    // The base HttpTransport frees the SSL object.
  }

  absl::StatusOr<std::shared_ptr<Http2ResponseStream>> SubmitRequest(
      std::string method, std::string scheme, std::string authority,
      std::string path, HttpHeaders headers, std::string body) override {
    std::shared_ptr<Http2Connection> self = Self();
    return RunOnUvForConnection<std::shared_ptr<Http2ResponseStream>>(
        [self = std::move(self), method = std::move(method),
         scheme = std::move(scheme), authority = std::move(authority),
         path = std::move(path), headers = std::move(headers),
         body = std::move(body)]() mutable
            -> absl::StatusOr<std::shared_ptr<Http2ResponseStream>> {
          return self->SubmitRequestOnLoop(
              std::move(method), std::move(scheme), std::move(authority),
              std::move(path), std::move(headers), std::move(body));
        });
  }

  absl::StatusOr<std::shared_ptr<Http2DuplexStream>> SubmitDuplex(
      std::string protocol, std::string scheme, std::string authority,
      std::string path, HttpHeaders headers) override {
    std::shared_ptr<Http2Connection> self = Self();
    return RunOnUvForConnection<std::shared_ptr<Http2DuplexStream>>(
        [self = std::move(self), protocol = std::move(protocol),
         scheme = std::move(scheme), authority = std::move(authority),
         path = std::move(path), headers = std::move(headers)]() mutable
            -> absl::StatusOr<std::shared_ptr<Http2DuplexStream>> {
          return self->SubmitDuplexOnLoop(
              std::move(protocol), std::move(scheme), std::move(authority),
              std::move(path), std::move(headers));
        });
  }

  absl::StatusOr<std::shared_ptr<Http2DuplexStream>> SubmitStreamingRequest(
      std::string method, std::string scheme, std::string authority,
      std::string path, HttpHeaders headers) override {
    std::shared_ptr<Http2Connection> self = Self();
    return RunOnUvForConnection<std::shared_ptr<Http2DuplexStream>>(
        [self = std::move(self), method = std::move(method),
         scheme = std::move(scheme), authority = std::move(authority),
         path = std::move(path), headers = std::move(headers)]() mutable
            -> absl::StatusOr<std::shared_ptr<Http2DuplexStream>> {
          return self->OpenDuplexOnLoop(
              std::move(method), /*protocol=*/{}, std::move(scheme),
              std::move(authority), std::move(path), std::move(headers));
        });
  }

  absl::Status WriteRequest(std::int32_t stream_id, std::string data) override {
    std::shared_ptr<Http2Connection> self = Self();
    const size_t bytes = data.size();
    return PostWrite(bytes, [self = std::move(self), stream_id,
                             data = std::move(data)]() mutable {
      return self->WriteRequestOnLoop(stream_id, std::move(data));
    });
  }

  absl::Status FinishRequest(std::int32_t stream_id) override {
    std::shared_ptr<Http2Connection> self = Self();
    return RunStatusOnUvForConnection([self = std::move(self), stream_id]() {
      return self->FinishRequestOnLoop(stream_id);
    });
  }

  absl::Status SendHeaders(std::int32_t stream_id, int status,
                           HttpHeaders headers) override {
    std::shared_ptr<Http2Connection> self = Self();
    return RunStatusOnUvForConnection([self = std::move(self), stream_id,
                                       status,
                                       headers = std::move(headers)]() mutable {
      return self->SendHeadersOnLoop(stream_id, status, std::move(headers));
    });
  }

  absl::Status Write(std::int32_t stream_id, std::string data) override {
    std::shared_ptr<Http2Connection> self = Self();
    const size_t bytes = data.size();
    return PostWrite(bytes, [self = std::move(self), stream_id,
                             data = std::move(data)]() mutable {
      return self->WriteOnLoop(stream_id, std::move(data));
    });
  }

  absl::Status Finish(std::int32_t stream_id) override {
    std::shared_ptr<Http2Connection> self = Self();
    return RunStatusOnUvForConnection([self = std::move(self), stream_id]() {
      return self->FinishOnLoop(stream_id);
    });
  }

  absl::Status FinishWithTrailers(std::int32_t stream_id,
                                  HttpHeaders trailers) override {
    std::shared_ptr<Http2Connection> self = Self();
    return RunStatusOnUvForConnection(
        [self = std::move(self), stream_id,
         trailers = std::move(trailers)]() mutable {
          return self->FinishOnLoop(stream_id, std::move(trailers));
        });
  }

  absl::Status SendResponse(std::int32_t stream_id, int status,
                            HttpHeaders headers, std::string body) override {
    std::shared_ptr<Http2Connection> self = Self();
    return RunStatusOnUvForConnection([self = std::move(self), stream_id,
                                       status, headers = std::move(headers),
                                       body = std::move(
                                           body)]() mutable -> absl::Status {
      ABSL_RETURN_IF_ERROR(
          self->SendHeadersOnLoop(stream_id, status, std::move(headers)));
      if (!body.empty()) {
        ABSL_RETURN_IF_ERROR(self->WriteOnLoop(stream_id, std::move(body)));
      }
      return self->FinishOnLoop(stream_id);
    });
  }

  absl::StatusOr<std::shared_ptr<Http2ResponseWriter>> SubmitPushPromise(
      std::int32_t stream_id, std::string method, std::string path,
      HttpHeaders headers) override {
    std::shared_ptr<Http2Connection> self = Self();
    return RunOnUvForConnection<std::shared_ptr<Http2ResponseWriter>>(
        [self = std::move(self), stream_id, method = std::move(method),
         path = std::move(path), headers = std::move(headers)]() mutable
            -> absl::StatusOr<std::shared_ptr<Http2ResponseWriter>> {
          return self->SubmitPushPromiseOnLoop(stream_id, std::move(method),
                                               std::move(path),
                                               std::move(headers));
        });
  }

  absl::Status AbortResponse(std::int32_t stream_id,
                             absl::Status status) override {
    if (status.ok()) {
      return absl::InvalidArgumentError("Response abort status must be non-OK");
    }
    std::shared_ptr<Http2Connection> self = Self();
    return RunStatusOnUvForConnection([self = std::move(self), stream_id,
                                       status = std::move(status)]() mutable {
      Stream* stream = self->FindStream(stream_id);
      if (stream == nullptr) {
        return absl::OkStatus();
      }
      if (!stream->response_headers_sent) {
        HttpHeaders headers;
        headers.emplace_back("content-type", "text/plain; charset=utf-8");
        return self->SendResponseOnLoop(
            stream_id, StatusCodeToHttp(status.code()), std::move(headers),
            std::string(status.message()));
      }
      const int submitted =
          nghttp2_submit_rst_stream(self->session_, NGHTTP2_FLAG_NONE,
                                    stream_id, StatusToHttp2Error(status));
      if (submitted != 0) {
        return Nghttp2Error(submitted, "nghttp2_submit_rst_stream");
      }
      return self->SendSession();
    });
  }

  absl::Status CancelRequest(std::int32_t stream_id, absl::Status status) {
    if (status.ok()) {
      return absl::InvalidArgumentError("Request cancel status must be non-OK");
    }
    std::shared_ptr<Http2Connection> self = Self();
    return RunStatusOnUvForConnection([self = std::move(self), stream_id,
                                       status = std::move(status)]() mutable {
      Stream* stream = self->FindStream(stream_id);
      if (stream == nullptr) {
        return absl::OkStatus();
      }
      if (stream->response != nullptr) {
        stream->response->Finish(status);
      }
      const int submitted =
          nghttp2_submit_rst_stream(self->session_, NGHTTP2_FLAG_NONE,
                                    stream_id, StatusToHttp2Error(status));
      if (submitted != 0 && submitted != NGHTTP2_ERR_STREAM_CLOSED) {
        return Nghttp2Error(submitted, "nghttp2_submit_rst_stream");
      }
      return self->SendSession();
    });
  }

  absl::StatusOr<bool> ResponseHeadersSent(std::int32_t stream_id) override {
    std::shared_ptr<Http2Connection> self = Self();
    return RunOnUvForConnection<bool>(
        [self = std::move(self), stream_id]() -> absl::StatusOr<bool> {
          Stream* stream = self->FindStream(stream_id);
          if (stream == nullptr) {
            return false;
          }
          return stream->response_headers_sent;
        });
  }

  absl::StatusOr<bool> ResponseFinished(std::int32_t stream_id) override {
    std::shared_ptr<Http2Connection> self = Self();
    return RunOnUvForConnection<bool>(
        [self = std::move(self), stream_id]() -> absl::StatusOr<bool> {
          Stream* stream = self->FindStream(stream_id);
          if (stream == nullptr) {
            return true;
          }
          return stream->outbound_finished;
        });
  }

  [[nodiscard]] bool secure() const override { return ssl_context_ != nullptr; }

 private:
  struct Stream {
    std::int32_t id = -1;
    HttpHeaders inbound_headers;
    HttpRequest request;
    /**
     * The origin this exchange is against, kept apart from @c request because
     * DispatchRequest moves that into the handler. A push promise needs them
     * afterwards: it names a request on the same origin, and an empty
     * `:authority` is a protocol error the peer resets the stream over.
     */
    std::string origin_scheme;
    std::string origin_authority;
    bool request_dispatched = false;
    bool request_too_large = false;
    bool duplex = false;
    /// A stream this side never opened, created by a PUSH_PROMISE. Its response
    /// head arrives as a HEADERS block with no request of ours preceding it.
    bool pushed = false;
    bool response_headers_sent = false;
    bool response_headers_received = false;
    bool remote_end = false;
    std::deque<std::string> outbound;
    size_t outbound_offset = 0;
    bool outbound_finished = false;
    /**
     * A trailer section to send once the body reaches its end, and whether one
     * is still owed.
     *
     * Trailers cannot be submitted up front: they follow the last DATA frame,
     * so the data source has to withhold END_STREAM and submit them at the
     * moment it reports EOF. See DataSourceReadCallback.
     */
    HttpHeaders outbound_trailers;
    bool outbound_trailers_pending = false;
    std::shared_ptr<Http2ResponseStream::State> response;
    /**
     * Response DATA received so far in this receive batch, not yet handed to
     * the reader. See FlushResponseData: one buffer per TCP read rather than
     * one per DATA frame.
     */
    std::string pending_response_data;
    std::shared_ptr<Http2RequestBodyStream::State> request_body;
    std::shared_ptr<Http2ResponseWriter::State> writer;
  };

  Http2Connection(std::shared_ptr<uvw::tcp_handle> tcp, bool server,
                  Http2RequestHandler handler, Http2Options options,
                  SslContext tls_context, std::string tls_server_name,
                  std::function<void(HttpTransport*)> on_closed,
                  std::string prebuffered)
      : HttpTransport(std::move(tcp), server, std::move(options),
                      std::move(tls_context), std::move(tls_server_name),
                      std::move(on_closed)),
        // The handler is the server owner's, and the fibre that runs it below
        // is A11's, so it is guarded on the way in. See
        // net/internal/exception_guarded_callbacks.h.
        handler_(internal::GuardRequestHandler(std::move(handler))),
        prebuffered_(std::move(prebuffered)) {}

  // Creates the nghttp2 session and queues the SETTINGS preface, then hands the
  // socket to the shared transport (which drives TLS or the cleartext start).
  absl::Status Initialize() {
    nghttp2_session_callbacks* callbacks = nullptr;
    int result = nghttp2_session_callbacks_new(&callbacks);
    if (result != 0) {
      return Nghttp2Error(result, "nghttp2_session_callbacks_new");
    }
    nghttp2_session_callbacks_set_send_callback(callbacks, &SendCallback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(
        callbacks, &OnBeginHeadersCallback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks,
                                                     &OnHeaderCallback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
                                                         &OnFrameRecvCallback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        callbacks, &OnDataChunkCallback);
    nghttp2_session_callbacks_set_on_stream_close_callback(
        callbacks, &OnStreamCloseCallback);
    result = server_ ? nghttp2_session_server_new(&session_, callbacks, this)
                     : nghttp2_session_client_new(&session_, callbacks, this);
    nghttp2_session_callbacks_del(callbacks);
    if (result != 0) {
      return Nghttp2Error(result, server_ ? "nghttp2_session_server_new"
                                          : "nghttp2_session_client_new");
    }
    std::vector<nghttp2_settings_entry> settings{
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 256},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,
         static_cast<std::uint32_t>(StreamWindowSize(options_))}};
    if (server_) {
      // Extended CONNECT is a server's to offer. ENABLE_PUSH is deliberately
      // absent: a server may only ever send it as 0, so saying so adds nothing.
      settings.push_back({NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 1});
    } else {
      // Whether this side will accept pushed responses. Sent either way --
      // nghttp2 defaults it to 1, and a client that is not reading NextPush()
      // must say 0 or the peer may open streams nothing will ever consume.
      settings.push_back(
          {NGHTTP2_SETTINGS_ENABLE_PUSH, options_.enable_push ? 1u : 0u});
    }
    result = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE,
                                     settings.data(), settings.size());
    if (result != 0) {
      return Nghttp2Error(result, "nghttp2_submit_settings");
    }
    // SETTINGS_INITIAL_WINDOW_SIZE above governs *stream* windows only. The
    // connection-level receive window is separate and stays at HTTP/2's 65535
    // default unless it is raised explicitly, which caps the whole connection at
    // 64 KiB in flight -- one bandwidth-delay product of about 10 MB/s on a 6 ms
    // path, and far less once per-frame acknowledgement latency dominates. It is
    // the difference between a few MB/s and line rate on a fast link.
    result = nghttp2_session_set_local_window_size(
        session_, NGHTTP2_FLAG_NONE, /*stream_id=*/0, kConnectionWindowSize);
    if (result != 0) {
      return Nghttp2Error(result, "nghttp2_session_set_local_window_size");
    }
    return InitializeTransport(std::move(prebuffered_));
  }

  // --- HttpTransport seams. ---

  /**
   * @brief Hand this batch's accumulated response DATA to the reader.
   *
   * nghttp2 delivers one callback per DATA frame -- 16 KiB by default -- so a
   * bulk transfer would otherwise cost an allocation, a promise resolution, a
   * fiber wake-up and a downstream write *per frame*. Everything that arrived in
   * one TCP read is handed over together instead, which costs no latency at all:
   * the bytes were already here, and the reader was going to be woken for them
   * regardless.
   *
   * Must run before the response is finished, or a reader would see end-of-
   * stream ahead of the last data.
   */
  void FlushResponseData(Stream* stream) {
    if (stream == nullptr || stream->pending_response_data.empty() ||
        stream->response == nullptr) {
      return;
    }
    std::string batch = std::move(stream->pending_response_data);
    stream->pending_response_data.clear();
    const absl::Status pushed = stream->response->Push(std::move(batch));
    if (!pushed.ok()) {
      // The reader is gone or the stream is already over; the data has nowhere
      // to go and the stream should stop.
      stream->response->Finish(pushed);
      (void)nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, stream->id,
                                      NGHTTP2_INTERNAL_ERROR);
    }
  }

  /// Finish a response, delivering anything still accumulated first.
  void FinishResponse(Stream* stream, const absl::Status& status) {
    if (stream == nullptr || stream->response == nullptr) {
      return;
    }
    FlushResponseData(stream);
    stream->response->Finish(status);
  }

  void FlushAllResponseData() {
    for (auto& [stream_id, stream] : streams_) {
      (void)stream_id;
      FlushResponseData(stream.get());
    }
  }

  absl::Status OnInboundPlaintext(const char* data, size_t size) override {
    const ssize_t consumed = nghttp2_session_mem_recv(
        session_, reinterpret_cast<const std::uint8_t*>(data), size);
    if (consumed < 0) {
      return Nghttp2Error(static_cast<int>(consumed),
                          "nghttp2_session_mem_recv");
    }
    if (static_cast<size_t>(consumed) != size) {
      return absl::DataLossError(
          "nghttp2 did not consume the complete TCP frame");
    }
    // The batch boundary: everything this read carried is delivered as one
    // buffer per stream.
    FlushAllResponseData();
    return SendSession();
  }

  absl::Status SendProtocolPreamble() override { return SendSession(); }

  void OnClose(const absl::Status& status) override {
    for (auto& [stream_id, stream] : streams_) {
      (void)stream_id;
      if (stream->response != nullptr) {
        stream->response->Finish(status);
      }
      if (stream->request_body != nullptr) {
        stream->request_body->Finish(status);
      }
      if (stream->writer != nullptr) {
        stream->writer->Finish(status);
      }
    }
    streams_.clear();
    if (session_ != nullptr) {
      nghttp2_session_del(session_);
      session_ = nullptr;
    }
  }

  Stream* FindStream(std::int32_t stream_id) {
    const auto iterator = streams_.find(stream_id);
    return iterator == streams_.end() ? nullptr : iterator->second.get();
  }

  absl::StatusOr<std::shared_ptr<Http2ResponseStream>> SubmitRequestOnLoop(
      std::string method, std::string scheme, std::string authority,
      std::string path, HttpHeaders headers, std::string body) {
    if (server_) {
      return absl::FailedPreconditionError(
          "A server HTTP/2 connection cannot submit requests");
    }
    if (closed_ || !connected_.load()) {
      return absl::UnavailableError("HTTP/2 connection is not connected");
    }
    if (method.empty() || scheme.empty() || authority.empty() || path.empty() ||
        path.front() != '/') {
      return absl::InvalidArgumentError(
          "HTTP/2 method, scheme, authority, and absolute path are required");
    }
    const std::string_view expected_scheme = secure() ? "https" : "http";
    if (scheme != expected_scheme) {
      return absl::InvalidArgumentError(absl::StrCat(
          "This HTTP/2 connection requires scheme '", expected_scheme, "'"));
    }
    absl::AsciiStrToUpper(&method);
    NormalizeHttpHeaders(&headers);
    ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(headers));
    if (body.size() > options_.max_request_body_size) {
      return absl::OutOfRangeError(
          "HTTP/2 request body exceeds max_request_body_size");
    }

    auto stream = std::make_unique<Stream>();
    stream->response = std::make_shared<Http2ResponseStream::State>(
        options_.max_buffered_response_bytes);
    if (!body.empty()) {
      stream->outbound.push_back(std::move(body));
    }
    stream->outbound_finished = true;
    auto values = RequestHeaders(method, scheme, authority, path, {}, headers);
    auto fields = MakeNv(values);
    nghttp2_data_provider provider{};
    nghttp2_data_provider* provider_pointer = nullptr;
    if (!stream->outbound.empty()) {
      provider.source.ptr = stream.get();
      provider.read_callback = &DataSourceReadCallback;
      provider_pointer = &provider;
    }
    const std::int32_t stream_id =
        nghttp2_submit_request(session_, nullptr, fields.data(), fields.size(),
                               provider_pointer, stream.get());
    if (stream_id < 0) {
      return Nghttp2Error(stream_id, "nghttp2_submit_request");
    }
    stream->id = stream_id;
    std::weak_ptr<Http2Connection> weak = Self();
    {
      thread::MutexLock lock(&stream->response->mu);
      stream->response->stream_id = stream_id;
      stream->response->cancel =
          [weak, stream_id](absl::Status status) -> absl::Status {
        const std::shared_ptr<Http2Connection> self = weak.lock();
        if (self == nullptr) {
          return absl::OkStatus();
        }
        return self->CancelRequest(stream_id, std::move(status));
      };
      // Backpressure: a full response buffer stops the socket read, which
      // closes the TCP window back to the peer. See
      // Http2ResponseStream::State::set_read_paused.
      stream->response->set_read_paused = [weak](bool paused) {
        if (const std::shared_ptr<Http2Connection> self = weak.lock()) {
          self->SetReadPaused(paused);
        }
      };
    }

    struct MakeResponseEnabler final : Http2ResponseStream {
      explicit MakeResponseEnabler(
          std::shared_ptr<Http2ResponseStream::State> state)
          : Http2ResponseStream(std::move(state)) {}
    };

    auto response = std::make_shared<MakeResponseEnabler>(stream->response);
    streams_.emplace(stream_id, std::move(stream));
    absl::Status sent = SendSession();
    if (!sent.ok()) {
      streams_.erase(stream_id);
      return sent;
    }
    if (options_.deadline != absl::InfiniteFuture()) {
      a11::Task done = response->Done();
      std::weak_ptr<Http2ResponseStream> response_weak = response;
      const absl::Time deadline = options_.deadline;
      a11::Schedule([done = std::move(done), response_weak, deadline]() {
        if (const absl::Status completion_status =
                done.Await(deadline).status();
            absl::IsDeadlineExceeded(completion_status)) {
          if (const std::shared_ptr<Http2ResponseStream> stream =
                  response_weak.lock()) {
            stream
                ->Cancel(absl::DeadlineExceededError(
                    "HTTP/2 request exceeded its deadline"))
                .IgnoreError();
          }
        }
      });
    }
    return response;
  }

  absl::StatusOr<std::shared_ptr<Http2DuplexStream>> SubmitDuplexOnLoop(
      std::string protocol, std::string scheme, std::string authority,
      std::string path, HttpHeaders headers) {
    if (protocol.empty()) {
      return absl::InvalidArgumentError(
          "HTTP/2 extended CONNECT requires a protocol");
    }
    if (!std::all_of(protocol.begin(), protocol.end(), [](unsigned char value) {
          return value == '-' || value == '_' || value == '.' ||
                 std::isdigit(value) || (value >= 'a' && value <= 'z');
        })) {
      return absl::InvalidArgumentError(
          "HTTP/2 extended CONNECT protocol must be a lowercase token");
    }
    return OpenDuplexOnLoop("CONNECT", std::move(protocol), std::move(scheme),
                            std::move(authority), std::move(path),
                            std::move(headers));
  }

  /**
   * Opens a stream whose request side stays writable after its headers.
   *
   * Two callers want this and they differ only in their pseudo-headers: an
   * extended CONNECT (`CONNECT` plus `:protocol`), and an ordinary method
   * sending a body it does not have in hand yet. The mechanism is the same
   * either way -- HEADERS without END_STREAM, and a data provider that defers
   * until Write() feeds it -- so it is written once.
   */
  absl::StatusOr<std::shared_ptr<Http2DuplexStream>> OpenDuplexOnLoop(
      std::string method, std::string protocol, std::string scheme,
      std::string authority, std::string path, HttpHeaders headers) {
    if (server_) {
      return absl::FailedPreconditionError(
          "A server HTTP/2 connection cannot submit requests");
    }
    if (closed_ || !connected_.load()) {
      return absl::UnavailableError("HTTP/2 connection is not connected");
    }
    if (method.empty() || scheme.empty() || authority.empty() ||
        path.empty() || path.front() != '/') {
      return absl::InvalidArgumentError(
          "HTTP/2 method, scheme, authority, and absolute path are required");
    }
    const std::string_view expected_scheme = secure() ? "https" : "http";
    if (scheme != expected_scheme) {
      return absl::InvalidArgumentError(absl::StrCat(
          "This HTTP/2 connection requires scheme '", expected_scheme, "'"));
    }
    absl::AsciiStrToUpper(&method);
    NormalizeHttpHeaders(&headers);
    ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(headers));

    auto stream = std::make_unique<Stream>();
    stream->duplex = true;
    stream->response = std::make_shared<Http2ResponseStream::State>(
        options_.max_buffered_response_bytes);
    auto values =
        RequestHeaders(method, scheme, authority, path, protocol, headers);
    auto fields = MakeNv(values);
    nghttp2_data_provider provider{};
    provider.source.ptr = stream.get();
    provider.read_callback = &DataSourceReadCallback;
    const std::int32_t stream_id =
        nghttp2_submit_request(session_, nullptr, fields.data(), fields.size(),
                               &provider, stream.get());
    if (stream_id < 0) {
      return Nghttp2Error(stream_id, "nghttp2_submit_request");
    }
    stream->id = stream_id;
    std::weak_ptr<Http2Connection> weak = Self();
    {
      thread::MutexLock lock(&stream->response->mu);
      stream->response->stream_id = stream_id;
      stream->response->cancel =
          [weak, stream_id](absl::Status status) -> absl::Status {
        std::shared_ptr<Http2Connection> self = weak.lock();
        if (self == nullptr) {
          return absl::OkStatus();
        }
        return self->CancelRequest(stream_id, std::move(status));
      };
      stream->response->set_read_paused = [weak](bool paused) {
        if (const std::shared_ptr<Http2Connection> self = weak.lock()) {
          self->SetReadPaused(paused);
        }
      };
    }

    struct MakeResponseEnabler final : Http2ResponseStream {
      explicit MakeResponseEnabler(
          std::shared_ptr<Http2ResponseStream::State> state)
          : Http2ResponseStream(std::move(state)) {}
    };

    struct MakeDuplexEnabler final : Http2DuplexStream {
      MakeDuplexEnabler(std::weak_ptr<Http2Connection> connection,
                        std::shared_ptr<Http2ResponseStream> response)
          : Http2DuplexStream(std::move(connection), std::move(response)) {}
    };

    auto response = std::make_shared<MakeResponseEnabler>(stream->response);
    auto duplex =
        std::make_shared<MakeDuplexEnabler>(Self(), response);
    streams_.emplace(stream_id, std::move(stream));
    if (const absl::Status send_status = SendSession(); !send_status.ok()) {
      streams_.erase(stream_id);
      return send_status;
    }
    if (options_.deadline != absl::InfiniteFuture()) {
      a11::Task done = response->Done();
      std::weak_ptr<Http2ResponseStream> response_weak = response;
      const absl::Time deadline = options_.deadline;
      a11::Schedule([done = std::move(done), response_weak, deadline]() {
        if (const absl::Status completion_status =
                done.Await(deadline).status();
            absl::IsDeadlineExceeded(completion_status)) {
          if (const std::shared_ptr<Http2ResponseStream> active =
                  response_weak.lock()) {
            active
                ->Cancel(absl::DeadlineExceededError(
                    "HTTP/2 duplex stream exceeded its deadline"))
                .IgnoreError();
          }
        }
      });
    }
    return duplex;
  }

  absl::Status SendHeadersOnLoop(std::int32_t stream_id, int status,
                                 HttpHeaders headers) {
    if (!server_) {
      return absl::FailedPreconditionError(
          "A client HTTP/2 connection cannot send response headers");
    }
    if (status < 100 || status > 599) {
      return absl::InvalidArgumentError("HTTP response status is invalid");
    }
    NormalizeHttpHeaders(&headers);
    ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(headers));
    Stream* stream = FindStream(stream_id);
    if (stream == nullptr) {
      return absl::NotFoundError("HTTP/2 request stream is no longer active");
    }
    if (stream->response_headers_sent) {
      return absl::FailedPreconditionError(
          "HTTP/2 response headers have already been sent");
    }
    auto values = ResponseHeaders(status, std::move(headers));
    auto fields = MakeNv(values);
    nghttp2_data_provider provider{};
    provider.source.ptr = stream;
    provider.read_callback = &DataSourceReadCallback;
    const int submitted = nghttp2_submit_response(
        session_, stream_id, fields.data(), fields.size(), &provider);
    if (submitted != 0) {
      return Nghttp2Error(submitted, "nghttp2_submit_response");
    }
    stream->response_headers_sent = true;
    return SendSession();
  }

  absl::Status WriteOnLoop(std::int32_t stream_id, std::string data) {
    if (data.empty()) {
      return absl::OkStatus();
    }
    Stream* stream = FindStream(stream_id);
    if (stream == nullptr) {
      return absl::NotFoundError("HTTP/2 request stream is no longer active");
    }
    if (!stream->response_headers_sent) {
      return absl::FailedPreconditionError(
          "SendHeaders must be called before writing an HTTP/2 response");
    }
    if (stream->outbound_finished) {
      return absl::FailedPreconditionError(
          "HTTP/2 response has already finished");
    }
    stream->outbound.push_back(std::move(data));
    const int resumed = nghttp2_session_resume_data(session_, stream_id);
    if (resumed != 0 && resumed != NGHTTP2_ERR_INVALID_ARGUMENT) {
      return Nghttp2Error(resumed, "nghttp2_session_resume_data");
    }
    return SendSession();
  }

  absl::Status WriteRequestOnLoop(std::int32_t stream_id, std::string data) {
    if (data.empty()) {
      return absl::OkStatus();
    }
    if (server_) {
      return absl::FailedPreconditionError(
          "A server HTTP/2 connection cannot write request DATA");
    }
    Stream* stream = FindStream(stream_id);
    if (stream == nullptr) {
      return absl::NotFoundError("HTTP/2 request stream is no longer active");
    }
    if (!stream->duplex) {
      return absl::FailedPreconditionError(
          "HTTP/2 request is not an extended CONNECT stream");
    }
    if (stream->outbound_finished) {
      return absl::FailedPreconditionError(
          "HTTP/2 request body has already finished");
    }
    stream->outbound.push_back(std::move(data));
    const int resumed = nghttp2_session_resume_data(session_, stream_id);
    if (resumed != 0 && resumed != NGHTTP2_ERR_INVALID_ARGUMENT) {
      return Nghttp2Error(resumed, "nghttp2_session_resume_data");
    }
    return SendSession();
  }

  /**
   * Sends a PUSH_PROMISE on @p stream_id and prepares the promised stream.
   *
   * The scheme and authority are taken from the request the promise is
   * associated with rather than asked for: a push is only ever for the same
   * origin, and letting a caller name a different one would be a way to lie
   * about it.
   */
  absl::StatusOr<std::shared_ptr<Http2ResponseWriter>> SubmitPushPromiseOnLoop(
      std::int32_t stream_id, std::string method, std::string path,
      HttpHeaders headers) {
    if (!server_) {
      return absl::FailedPreconditionError(
          "Only a server HTTP/2 connection can push");
    }
    if (closed_ || !connected_.load()) {
      return absl::UnavailableError("HTTP/2 connection is not connected");
    }
    if (method.empty() || path.empty() || path.front() != '/') {
      return absl::InvalidArgumentError(
          "An HTTP/2 push promise needs a method and an absolute path");
    }
    Stream* associated = FindStream(stream_id);
    if (associated == nullptr) {
      return absl::NotFoundError("HTTP/2 request stream is no longer active");
    }
    if (associated->outbound_finished) {
      return absl::FailedPreconditionError(
          "An HTTP/2 push promise must be sent before the response it "
          "accompanies is finished");
    }
    absl::AsciiStrToUpper(&method);
    NormalizeHttpHeaders(&headers);
    ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(headers));

    const std::string scheme = associated->origin_scheme.empty()
                                   ? std::string(secure() ? "https" : "http")
                                   : associated->origin_scheme;
    if (associated->origin_authority.empty()) {
      // Every promised request needs an `:authority`, and the only honest source
      // of one is the request being answered.
      return absl::FailedPreconditionError(
          "The request being answered has no authority to push against");
    }
    auto values = RequestHeaders(method, scheme, associated->origin_authority,
                                 path, /*protocol=*/{}, headers);
    auto fields = MakeNv(values);
    const std::int32_t promised_id = nghttp2_submit_push_promise(
        session_, NGHTTP2_FLAG_NONE, stream_id, fields.data(), fields.size(),
        nullptr);
    if (promised_id == NGHTTP2_ERR_PUSH_DISABLED) {
      return absl::FailedPreconditionError(
          "The peer did not enable HTTP/2 server push");
    }
    if (promised_id < 0) {
      return Nghttp2Error(promised_id, "nghttp2_submit_push_promise");
    }

    auto stream = std::make_unique<Stream>();
    stream->id = promised_id;
    stream->writer = std::make_shared<Http2ResponseWriter::State>();
    // The promised request, so a further push from this stream has an origin.
    stream->request.method = method;
    stream->request.scheme = scheme;
    stream->request.authority = associated->origin_authority;
    stream->request.path = path;
    stream->request.headers = std::move(headers);
    stream->origin_scheme = scheme;
    stream->origin_authority = associated->origin_authority;
    stream->remote_end = true;  // A pushed stream has no inbound request body.
    std::shared_ptr<Http2ResponseWriter::State> writer_state = stream->writer;
    streams_.emplace(promised_id, std::move(stream));

    struct MakeWriterEnabler final : Http2ResponseWriter {
      MakeWriterEnabler(std::weak_ptr<Http2Connection> connection,
                        std::int32_t id,
                        std::shared_ptr<Http2ResponseWriter::State> state)
          : Http2ResponseWriter(std::move(connection), id, std::move(state)) {}
    };

    auto writer = std::make_shared<MakeWriterEnabler>(Self(), promised_id,
                                                      std::move(writer_state));
    if (const absl::Status sent = SendSession(); !sent.ok()) {
      streams_.erase(promised_id);
      return sent;
    }
    return std::static_pointer_cast<Http2ResponseWriter>(writer);
  }

  absl::Status FinishOnLoop(std::int32_t stream_id, HttpHeaders trailers = {}) {
    Stream* stream = FindStream(stream_id);
    if (stream == nullptr) {
      return absl::OkStatus();
    }
    if (!stream->response_headers_sent) {
      return absl::FailedPreconditionError(
          "SendHeaders must be called before finishing an HTTP/2 response");
    }
    if (stream->outbound_finished) {
      return absl::OkStatus();
    }
    if (!trailers.empty()) {
      NormalizeHttpHeaders(&trailers);
      ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(trailers));
      stream->outbound_trailers = std::move(trailers);
      stream->outbound_trailers_pending = true;
    }
    stream->outbound_finished = true;
    const int resumed = nghttp2_session_resume_data(session_, stream_id);
    if (resumed != 0 && resumed != NGHTTP2_ERR_INVALID_ARGUMENT) {
      return Nghttp2Error(resumed, "nghttp2_session_resume_data");
    }
    return SendSession();
  }

  absl::Status FinishRequestOnLoop(std::int32_t stream_id) {
    if (server_) {
      return absl::FailedPreconditionError(
          "A server HTTP/2 connection cannot finish request DATA");
    }
    Stream* stream = FindStream(stream_id);
    if (stream == nullptr) {
      return absl::OkStatus();
    }
    if (!stream->duplex) {
      return absl::FailedPreconditionError(
          "HTTP/2 request is not an extended CONNECT stream");
    }
    if (stream->outbound_finished) {
      return absl::OkStatus();
    }
    stream->outbound_finished = true;
    const int resumed = nghttp2_session_resume_data(session_, stream_id);
    if (resumed != 0 && resumed != NGHTTP2_ERR_INVALID_ARGUMENT) {
      return Nghttp2Error(resumed, "nghttp2_session_resume_data");
    }
    return SendSession();
  }

  absl::Status SendResponseOnLoop(std::int32_t stream_id, int status,
                                  HttpHeaders headers, std::string body) {
    ABSL_RETURN_IF_ERROR(
        SendHeadersOnLoop(stream_id, status, std::move(headers)));
    if (!body.empty()) {
      ABSL_RETURN_IF_ERROR(WriteOnLoop(stream_id, std::move(body)));
    }
    return FinishOnLoop(stream_id);
  }

  bool IsExtendedConnect(const Stream* stream) const {
    if (stream == nullptr) {
      return false;
    }
    const std::optional<std::string> method =
        GetHttpHeader(stream->inbound_headers, ":method");
    const std::optional<std::string> protocol =
        GetHttpHeader(stream->inbound_headers, ":protocol");
    return method == "CONNECT" && protocol.has_value() && !protocol->empty();
  }

  /**
   * Whether this request's body should reach the handler as it arrives.
   *
   * The predicate sees the head as the handler will: the method and path
   * pseudo-headers pulled out, and the ordinary fields on their own. Runs on the
   * loop thread inside nghttp2's frame callback, so a throwing or blocking
   * predicate is the caller's problem; the option documents that.
   */
  bool WantsStreamedRequestBody(const Stream* stream) const {
    if (stream == nullptr || options_.stream_request_body == nullptr) {
      return false;
    }
    const std::optional<std::string> method =
        GetHttpHeader(stream->inbound_headers, ":method");
    const std::optional<std::string> path =
        GetHttpHeader(stream->inbound_headers, ":path");
    if (!method.has_value() || !path.has_value()) {
      return false;  // A malformed head is rejected by DispatchRequest.
    }
    HttpHeaders fields;
    fields.reserve(stream->inbound_headers.size());
    for (const auto& [name, value] : stream->inbound_headers) {
      if (!name.empty() && name.front() != ':') {
        fields.emplace_back(name, value);
      }
    }
    return options_.stream_request_body(*method, *path, fields);
  }

  absl::Status BeginDuplexRequest(Stream* stream) {
    if (stream == nullptr) {
      return absl::InvalidArgumentError("HTTP/2 stream must not be null");
    }
    if (stream->request_body != nullptr) {
      return absl::OkStatus();
    }
    stream->duplex = true;
    stream->request_body = std::make_shared<Http2RequestBodyStream::State>(
        options_.max_buffered_request_bytes);
    {
      thread::MutexLock lock(&stream->request_body->mu);
      stream->request_body->stream_id = stream->id;
      std::weak_ptr<Http2Connection> weak = Self();
      const std::int32_t stream_id = stream->id;
      stream->request_body->cancel =
          [weak, stream_id](absl::Status status) -> absl::Status {
        std::shared_ptr<Http2Connection> self = weak.lock();
        if (self == nullptr) {
          return absl::OkStatus();
        }
        return self->CancelRequest(stream_id, std::move(status));
      };
    }
    return absl::OkStatus();
  }

  absl::Status SendSession() {
    if (closed_ || session_ == nullptr) {
      return absl::UnavailableError("HTTP/2 connection is closed");
    }
    pending_transport_error_.reset();
    const int result = nghttp2_session_send(session_);
    if (pending_transport_error_.has_value()) {
      return *std::exchange(pending_transport_error_, std::nullopt);
    }
    if (result != 0) {
      return Nghttp2Error(result, "nghttp2_session_send");
    }
    return absl::OkStatus();
  }

  static ssize_t SendCallback(nghttp2_session*, const std::uint8_t* data,
                              size_t length, int, void* user_data) noexcept {
    auto* self = static_cast<Http2Connection*>(user_data);
    if (self == nullptr || self->closed_) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    absl::Status status = self->WriteApplicationData(data, length);
    if (!status.ok()) {
      self->pending_transport_error_ = std::move(status);
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return static_cast<ssize_t>(length);
  }

  /**
   * Sends the trailer section owed on @p stream, if any, as the body ends.
   *
   * nghttp2 wants this at exactly one moment: the read callback reports EOF
   * *without* END_STREAM, and the trailing HEADERS frame closes the stream
   * instead. Doing it anywhere earlier would put the trailers ahead of the DATA
   * they trail; anywhere later and END_STREAM has already gone out.
   */
  static void SubmitPendingTrailers(nghttp2_session* session, Stream* stream,
                                    std::uint32_t* data_flags) {
    if (!stream->outbound_trailers_pending) {
      return;
    }
    stream->outbound_trailers_pending = false;
    std::vector<std::pair<std::string, std::string>> values(
        stream->outbound_trailers.begin(), stream->outbound_trailers.end());
    const std::vector<nghttp2_nv> fields = MakeNv(values);
    if (nghttp2_submit_trailer(session, stream->id, fields.data(),
                               fields.size()) != 0) {
      // The stream still has to end, and END_STREAM is the only way left to do
      // that: falling through without NO_END_STREAM loses the trailers rather
      // than hanging the response.
      return;
    }
    *data_flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
  }

  static ssize_t DataSourceReadCallback(nghttp2_session* session, std::int32_t,
                                        std::uint8_t* buffer, size_t length,
                                        std::uint32_t* data_flags,
                                        nghttp2_data_source* source,
                                        void*) noexcept {
    if (source == nullptr || source->ptr == nullptr) {
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    auto* stream = static_cast<Stream*>(source->ptr);
    if (stream->outbound.empty()) {
      if (stream->outbound_finished) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        SubmitPendingTrailers(session, stream, data_flags);
        return 0;
      }
      return NGHTTP2_ERR_DEFERRED;
    }
    std::string& front = stream->outbound.front();
    const size_t remaining = front.size() - stream->outbound_offset;
    const size_t copied = std::min(length, remaining);
    std::memcpy(buffer, front.data() + stream->outbound_offset, copied);
    stream->outbound_offset += copied;
    if (stream->outbound_offset == front.size()) {
      stream->outbound.pop_front();
      stream->outbound_offset = 0;
    }
    if (stream->outbound.empty() && stream->outbound_finished) {
      *data_flags |= NGHTTP2_DATA_FLAG_EOF;
      SubmitPendingTrailers(session, stream, data_flags);
    }
    return static_cast<ssize_t>(copied);
  }

  /**
   * The stream a header block belongs to.
   *
   * For a PUSH_PROMISE the two differ: the frame travels on the stream the
   * promise is associated with, while the fields describe the promised stream.
   * Everywhere else they are the same.
   */
  static std::int32_t HeaderTargetStream(const nghttp2_frame* frame) {
    if (frame->hd.type == NGHTTP2_PUSH_PROMISE) {
      return frame->push_promise.promised_stream_id;
    }
    return frame->hd.stream_id;
  }

  static int OnBeginHeadersCallback(nghttp2_session*,
                                    const nghttp2_frame* frame,
                                    void* user_data) noexcept {
    auto* self = static_cast<Http2Connection*>(user_data);
    if (self == nullptr || frame == nullptr) {
      return 0;
    }
    if (frame->hd.type == NGHTTP2_PUSH_PROMISE) {
      return self->BeginPushedStream(frame);
    }
    if (frame->hd.type != NGHTTP2_HEADERS) {
      return 0;
    }
    if (self->server_ && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
      if (self->FindStream(frame->hd.stream_id) == nullptr) {
        auto stream = std::make_unique<Stream>();
        stream->id = frame->hd.stream_id;
        stream->writer = std::make_shared<Http2ResponseWriter::State>();
        self->streams_.emplace(frame->hd.stream_id, std::move(stream));
      }
    }
    if (Stream* stream = self->FindStream(frame->hd.stream_id);
        stream != nullptr) {
      stream->inbound_headers.clear();
    }
    return 0;
  }

  static int OnHeaderCallback(nghttp2_session*, const nghttp2_frame* frame,
                              const std::uint8_t* name, size_t name_length,
                              const std::uint8_t* value, size_t value_length,
                              std::uint8_t, void* user_data) noexcept {
    auto* self = static_cast<Http2Connection*>(user_data);
    if (self == nullptr || frame == nullptr) {
      return 0;
    }
    Stream* stream = self->FindStream(HeaderTargetStream(frame));
    if (stream == nullptr) {
      return 0;
    }
    std::string normalized_name(reinterpret_cast<const char*>(name),
                                name_length);
    absl::AsciiStrToLower(&normalized_name);
    stream->inbound_headers.emplace_back(
        std::move(normalized_name),
        std::string(reinterpret_cast<const char*>(value), value_length));
    return 0;
  }

  static int OnFrameRecvCallback(nghttp2_session*, const nghttp2_frame* frame,
                                 void* user_data) noexcept {
    auto* self = static_cast<Http2Connection*>(user_data);
    if (self == nullptr || frame == nullptr) {
      return 0;
    }
    Stream* stream = self->FindStream(frame->hd.stream_id);
    if (stream == nullptr) {
      return 0;
    }
    if (frame->hd.type == NGHTTP2_PUSH_PROMISE) {
      // `stream` is the associated response, not the promised one: a promise
      // is delivered to whoever is reading the response it came with.
      self->CompletePushPromise(stream, frame->push_promise.promised_stream_id);
      return 0;
    }
    if (frame->hd.type == NGHTTP2_HEADERS) {
      if (self->server_ && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
          stream->remote_end = true;
          self->DispatchRequest(stream);
        } else if (self->IsExtendedConnect(stream) ||
                   self->WantsStreamedRequestBody(stream)) {
          absl::Status started = self->BeginDuplexRequest(stream);
          if (!started.ok()) {
            self->pending_transport_error_ = std::move(started);
            return NGHTTP2_ERR_CALLBACK_FAILURE;
          }
          self->DispatchRequest(stream);
        }
      } else if (!self->server_ &&
                 (frame->headers.cat == NGHTTP2_HCAT_RESPONSE ||
                  frame->headers.cat == NGHTTP2_HCAT_PUSH_RESPONSE ||
                  frame->headers.cat == NGHTTP2_HCAT_HEADERS)) {
        // Which of the two header blocks a response may carry this is comes
        // from the block itself rather than from nghttp2's category: one with
        // `:status` is a response head, and a trailer section is forbidden
        // from carrying pseudo-headers at all. HCAT_PUSH_RESPONSE is the head
        // of a pushed response, and HCAT_HEADERS covers the trailers of
        // either kind.
        if (GetHttpHeader(stream->inbound_headers, ":status").has_value()) {
          self->CompleteResponseHeaders(stream);
        } else {
          self->CollectTrailers(stream);
        }
        if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
          stream->remote_end = true;
          self->FinishResponse(stream, absl::OkStatus());
        }
      }
    } else if (frame->hd.type == NGHTTP2_DATA &&
               (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
      stream->remote_end = true;
      if (self->server_) {
        if (stream->request_body != nullptr) {
          stream->request_body->Finish(absl::OkStatus());
        }
        if (!stream->request_dispatched) {
          self->DispatchRequest(stream);
        }
      } else if (stream->response != nullptr) {
        self->FinishResponse(stream, absl::OkStatus());
      }
    }
    return 0;
  }

  static int OnDataChunkCallback(nghttp2_session* session, std::uint8_t,
                                 std::int32_t stream_id,
                                 const std::uint8_t* data, size_t length,
                                 void* user_data) noexcept {
    auto* self = static_cast<Http2Connection*>(user_data);
    if (self == nullptr) {
      return 0;
    }
    Stream* stream = self->FindStream(stream_id);
    if (stream == nullptr) {
      return 0;
    }
    if (self->server_) {
      if (stream->request_body != nullptr) {
        absl::Status pushed = stream->request_body->Push(
            std::string(reinterpret_cast<const char*>(data), length));
        if (!pushed.ok()) {
          stream->request_body->Finish(pushed);
          const int reset = nghttp2_submit_rst_stream(
              session, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_ENHANCE_YOUR_CALM);
          return reset == 0 ? 0 : NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        return 0;
      }
      if (stream->request.body.size() + length >
          self->options_.max_request_body_size) {
        stream->request_too_large = true;
        const int reset = nghttp2_submit_rst_stream(
            session, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_ENHANCE_YOUR_CALM);
        return reset == 0 ? 0 : NGHTTP2_ERR_CALLBACK_FAILURE;
      }
      stream->request.body.append(reinterpret_cast<const char*>(data), length);
    } else if (stream->response != nullptr) {
      // Accumulated, not pushed: the batch is delivered once this TCP read is
      // fully parsed. See FlushResponseData.
      if (stream->pending_response_data.empty()) {
        stream->pending_response_data.reserve(kResponseBatchReserve);
      }
      stream->pending_response_data.append(reinterpret_cast<const char*>(data),
                                           length);
    }
    return 0;
  }

  static int OnStreamCloseCallback(nghttp2_session*, std::int32_t stream_id,
                                   std::uint32_t error_code,
                                   void* user_data) noexcept {
    auto* self = static_cast<Http2Connection*>(user_data);
    if (self == nullptr) {
      return 0;
    }
    auto iterator = self->streams_.find(stream_id);
    if (iterator == self->streams_.end()) {
      return 0;
    }
    Stream& stream = *iterator->second;
    if (stream.response != nullptr) {
      absl::Status status = Http2StreamError(error_code, "HTTP/2 response");
      if (status.ok() && !stream.remote_end) {
        status =
            absl::UnavailableError("HTTP/2 response closed without END_STREAM");
      }
      stream.response->Finish(status);
    }
    if (stream.request_body != nullptr) {
      absl::Status status = Http2StreamError(error_code, "HTTP/2 request");
      if (status.ok() && !stream.remote_end) {
        status = absl::UnavailableError(
            "HTTP/2 request body closed without END_STREAM");
      }
      stream.request_body->Finish(status);
    }
    if (stream.writer != nullptr) {
      absl::Status status = Http2StreamError(error_code, "HTTP/2 request");
      if (status.ok() && (!stream.remote_end || !stream.outbound_finished)) {
        status = absl::UnavailableError(
            "HTTP/2 request closed before both sides reached END_STREAM");
      }
      stream.writer->Finish(status);
    }
    self->streams_.erase(iterator);
    return 0;
  }

  void CompleteResponseHeaders(Stream* stream) {
    if (stream == nullptr || stream->response == nullptr ||
        stream->response_headers_received) {
      return;
    }
    const std::optional<std::string> status_header =
        GetHttpHeader(stream->inbound_headers, ":status");
    int status = 0;
    if (!status_header.has_value() ||
        !absl::SimpleAtoi(*status_header, &status) || status < 100 ||
        status > 599) {
      stream->response->Finish(
          absl::DataLossError("HTTP/2 response has an invalid :status"));
      return;
    }
    if (status < 200) {
      return;
    }
    HttpResponseHead head{.status = status};
    for (const auto& [name, value] : stream->inbound_headers) {
      if (name.empty() || name.front() != ':') {
        head.headers.emplace_back(name, value);
      }
    }
    stream->response_headers_received = true;
    stream->response->SetHeaders(std::move(head));
  }

  /**
   * Starts tracking a stream the peer opened with a PUSH_PROMISE.
   *
   * The promised stream needs a response state before its header fields arrive,
   * because OnHeaderCallback routes them to it by id. Only a client can be
   * pushed to; a PUSH_PROMISE arriving at a server is a protocol error, which
   * nghttp2 has already rejected by the time this runs.
   */
  int BeginPushedStream(const nghttp2_frame* frame) noexcept {
    if (server_) {
      return 0;
    }
    const std::int32_t promised = frame->push_promise.promised_stream_id;
    if (FindStream(promised) != nullptr) {
      return 0;
    }
    auto stream = std::make_unique<Stream>();
    stream->id = promised;
    stream->pushed = true;
    stream->outbound_finished = true;  // A pushed stream has no request side.
    stream->response = std::make_shared<Http2ResponseStream::State>(
        options_.max_buffered_response_bytes);
    streams_.emplace(promised, std::move(stream));
    return 0;
  }

  /**
   * Hands a completed PUSH_PROMISE to the reader of the associated response.
   *
   * @param associated The stream the promise arrived on, whose NextPush() the
   *        descriptor is queued for.
   * @param promised_id The stream the pushed response will arrive on.
   */
  void CompletePushPromise(Stream* associated, std::int32_t promised_id) {
    Stream* promised = FindStream(promised_id);
    if (associated == nullptr || associated->response == nullptr ||
        promised == nullptr || promised->response == nullptr) {
      return;
    }

    HttpPushedResponse descriptor;
    descriptor.method =
        GetHttpHeader(promised->inbound_headers, ":method").value_or("GET");
    descriptor.scheme =
        GetHttpHeader(promised->inbound_headers, ":scheme").value_or("");
    descriptor.authority =
        GetHttpHeader(promised->inbound_headers, ":authority").value_or("");
    descriptor.path =
        GetHttpHeader(promised->inbound_headers, ":path").value_or("");
    for (const auto& [name, value] : promised->inbound_headers) {
      if (!name.empty() && name.front() != ':') {
        descriptor.headers.emplace_back(name, value);
      }
    }

    std::weak_ptr<Http2Connection> weak = Self();
    {
      thread::MutexLock lock(&promised->response->mu);
      promised->response->stream_id = promised_id;
      // Cancel() on a pushed response is how a client refuses it: the same
      // RST_STREAM path a cancelled request takes.
      promised->response->cancel =
          [weak, promised_id](absl::Status status) -> absl::Status {
        const std::shared_ptr<Http2Connection> self = weak.lock();
        if (self == nullptr) {
          return absl::OkStatus();
        }
        return self->CancelRequest(promised_id, std::move(status));
      };
      promised->response->set_read_paused = [weak](bool paused) {
        if (const std::shared_ptr<Http2Connection> self = weak.lock()) {
          self->SetReadPaused(paused);
        }
      };
    }

    struct MakeResponseEnabler final : Http2ResponseStream {
      explicit MakeResponseEnabler(
          std::shared_ptr<Http2ResponseStream::State> state)
          : Http2ResponseStream(std::move(state)) {}
    };

    descriptor.response =
        std::make_shared<MakeResponseEnabler>(promised->response);

    // The associated response may already have ended -- a promise racing its
    // own stream's END_STREAM. PushPromised() drops it in that case, so reset
    // the pushed stream rather than leaving it open for nobody.
    bool delivered = false;
    {
      thread::MutexLock lock(&associated->response->mu);
      delivered = !associated->response->done;
    }
    if (!delivered) {
      (void)nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, promised_id,
                                      NGHTTP2_CANCEL);
      return;
    }
    associated->response->PushPromised(std::move(descriptor));
  }

  /**
   * Records a trailer section on a client stream.
   *
   * Held rather than published: the fields only mean anything once the body
   * they follow has ended, and the END_STREAM that carries them does that a few
   * lines later. Pseudo-headers are dropped -- a trailer section may not carry
   * them, and a peer that sends one anyway should not have it mistaken for part
   * of the section.
   */
  void CollectTrailers(Stream* stream) {
    if (stream == nullptr || stream->response == nullptr) {
      return;
    }
    HttpHeaders trailers;
    for (const auto& [name, value] : stream->inbound_headers) {
      if (!name.empty() && name.front() != ':') {
        trailers.emplace_back(name, value);
      }
    }
    stream->response->SetTrailers(std::move(trailers));
  }

  void DispatchRequest(Stream* stream) {
    if (stream == nullptr || stream->request_dispatched) {
      return;
    }

    struct MakeWriterEnabler final : Http2ResponseWriter {
      MakeWriterEnabler(std::weak_ptr<Http2Connection> connection,
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

    stream->request_dispatched = true;
    if (stream->request_too_large) {
      return;
    }
    const std::optional<std::string> method =
        GetHttpHeader(stream->inbound_headers, ":method");
    const std::optional<std::string> protocol =
        GetHttpHeader(stream->inbound_headers, ":protocol");
    const std::optional<std::string> scheme =
        GetHttpHeader(stream->inbound_headers, ":scheme");
    const std::optional<std::string> authority =
        GetHttpHeader(stream->inbound_headers, ":authority");
    const std::optional<std::string> path =
        GetHttpHeader(stream->inbound_headers, ":path");
    if (!method.has_value() || !scheme.has_value() || !path.has_value()) {
      auto writer = std::make_shared<MakeWriterEnabler>(Self(), stream->id,
                                                        stream->writer);
      a11::Schedule([writer]() {
        (void)writer->SendResponse(400, {{"content-type", "text/plain"}},
                                   "Missing required HTTP/2 pseudo-header");
      });
      return;
    }
    stream->request.method = *method;
    stream->request.protocol = protocol.value_or("");
    stream->request.scheme = *scheme;
    stream->request.authority = authority.value_or("");
    stream->request.path = *path;
    stream->origin_scheme = *scheme;
    stream->origin_authority = authority.value_or("");
    if (stream->request_body != nullptr) {
      stream->request.body_stream =
          std::make_shared<MakeRequestBodyEnabler>(stream->request_body);
    }
    for (const auto& [name, value] : stream->inbound_headers) {
      if (name.empty() || name.front() != ':') {
        stream->request.headers.emplace_back(name, value);
      }
    }
    HttpRequest request = std::move(stream->request);
    auto writer =
        std::make_shared<MakeWriterEnabler>(Self(), stream->id, stream->writer);
    Http2RequestHandler handler = handler_;
    a11::Schedule([handler = std::move(handler), request = std::move(request),
                   writer = std::move(writer)]() mutable {
      const absl::Status status =
          handler(std::move(request), writer).Await().status();
      if (!status.ok()) {
        (void)writer->Abort(status);
      } else if (!writer->headers_sent()) {
        (void)writer->SendResponse(204);
      }
    });
  }

  const Http2RequestHandler handler_;
  std::string prebuffered_;
  nghttp2_session* session_ = nullptr;
  absl::flat_hash_map<std::int32_t, std::unique_ptr<Stream>> streams_;
  std::optional<absl::Status> pending_transport_error_;
};

a11::Future<HttpResponseHead> Http2DuplexStream::Headers() const {
  return response_->Headers();
}

a11::Future<std::optional<std::string>> Http2DuplexStream::Read() {
  return response_->Read();
}

absl::Status Http2DuplexStream::Write(std::string data) {
  std::shared_ptr<HttpConnection> connection = connection_.lock();
  if (connection == nullptr) {
    return absl::UnavailableError("HTTP/2 connection is no longer available");
  }
  return connection->WriteRequest(stream_id(), std::move(data));
}

absl::Status Http2DuplexStream::Finish() {
  std::shared_ptr<HttpConnection> connection = connection_.lock();
  if (connection == nullptr) {
    return absl::UnavailableError("HTTP/2 connection is no longer available");
  }
  return connection->FinishRequest(stream_id());
}

absl::Status Http2DuplexStream::Abort(absl::Status status) {
  return response_->Cancel(std::move(status));
}

a11::Task Http2DuplexStream::Done() const {
  return response_->Done();
}

std::int32_t Http2DuplexStream::stream_id() const {
  return response_->stream_id();
}

absl::Status Http2ResponseWriter::SendHeaders(int status, HttpHeaders headers) {
  std::shared_ptr<HttpConnection> connection = connection_.lock();
  if (connection == nullptr) {
    return absl::UnavailableError("HTTP/2 connection is no longer available");
  }
  return connection->SendHeaders(stream_id_, status, std::move(headers));
}

absl::Status Http2ResponseWriter::Write(std::string data) {
  std::shared_ptr<HttpConnection> connection = connection_.lock();
  if (connection == nullptr) {
    return absl::UnavailableError("HTTP/2 connection is no longer available");
  }
  return connection->Write(stream_id_, std::move(data));
}

absl::Status Http2ResponseWriter::Finish() {
  std::shared_ptr<HttpConnection> connection = connection_.lock();
  if (connection == nullptr) {
    return absl::OkStatus();
  }
  return connection->Finish(stream_id_);
}

absl::StatusOr<std::shared_ptr<Http2ResponseWriter>>
Http2ResponseWriter::PushPromise(std::string method, std::string path,
                                 HttpHeaders headers) {
  std::shared_ptr<HttpConnection> connection = connection_.lock();
  if (connection == nullptr) {
    return absl::UnavailableError("HTTP/2 connection is no longer available");
  }
  return connection->SubmitPushPromise(stream_id_, std::move(method),
                                       std::move(path), std::move(headers));
}

absl::Status Http2ResponseWriter::FinishWithTrailers(HttpHeaders trailers) {
  std::shared_ptr<HttpConnection> connection = connection_.lock();
  if (connection == nullptr) {
    return absl::OkStatus();
  }
  return connection->FinishWithTrailers(stream_id_, std::move(trailers));
}

absl::Status Http2ResponseWriter::SendResponse(int status, HttpHeaders headers,
                                               std::string body) {
  std::shared_ptr<HttpConnection> connection = connection_.lock();
  if (connection == nullptr) {
    return absl::UnavailableError("HTTP/2 connection is no longer available");
  }
  return connection->SendResponse(stream_id_, status, std::move(headers),
                                  std::move(body));
}

absl::Status Http2ResponseWriter::Abort(absl::Status status) {
  std::shared_ptr<HttpConnection> connection = connection_.lock();
  if (connection == nullptr) {
    return absl::OkStatus();
  }
  return connection->AbortResponse(stream_id_, std::move(status));
}

a11::Task Http2ResponseWriter::Done() const {
  return state_ != nullptr ? state_->done_future
                           : a11::FailedTask(absl::FailedPreconditionError(
                                 "HTTP/2 writer has no state"));
}

bool Http2ResponseWriter::headers_sent() const {
  std::shared_ptr<HttpConnection> connection = connection_.lock();
  if (connection == nullptr) {
    return false;
  }
  absl::StatusOr<bool> result = connection->ResponseHeadersSent(stream_id_);
  return result.ok() && *result;
}

bool Http2ResponseWriter::finished() const {
  std::shared_ptr<HttpConnection> connection = connection_.lock();
  if (connection == nullptr) {
    return true;
  }
  absl::StatusOr<bool> result = connection->ResponseFinished(stream_id_);
  return !result.ok() || *result;
}

struct Http2Server::State {
  State(std::string address, Http2RequestHandler request_handler,
        Http2Options server_options, SslContext context)
      : bind_address(std::move(address)),
        handler(std::move(request_handler)),
        options(std::move(server_options)),
        tls_context(std::move(context)) {}

  mutable thread::Mutex mu;
  const std::string bind_address;
  const Http2RequestHandler handler;
  const Http2Options options;
  const SslContext tls_context;
  std::uint16_t port ABSL_GUARDED_BY(mu) = 0;
  bool running ABSL_GUARDED_BY(mu) = false;
  std::shared_ptr<uvw::tcp_handle> listener ABSL_GUARDED_BY(mu);
  // Holds both HTTP/2 and HTTP/1.1 connections via their shared base.
  std::vector<std::shared_ptr<HttpTransport>> connections ABSL_GUARDED_BY(mu);
};

namespace {

// The HTTP/2 client connection preface. A cleartext peer that opens with these
// exact bytes is speaking prior-knowledge h2c; anything else is HTTP/1.1.
constexpr std::string_view kHttp2ClientPreface =
    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

enum class PrefaceGuess { kNeedMore, kHttp2, kHttp1 };

PrefaceGuess GuessPreface(std::string_view buffer) {
  const size_t compared = std::min(buffer.size(), kHttp2ClientPreface.size());
  if (buffer.substr(0, compared) != kHttp2ClientPreface.substr(0, compared)) {
    return PrefaceGuess::kHttp1;
  }
  return buffer.size() >= kHttp2ClientPreface.size() ? PrefaceGuess::kHttp2
                                                     : PrefaceGuess::kNeedMore;
}

// Creates a server-side connection of the requested protocol on @p client,
// replaying any bytes already read during cleartext detection.
absl::StatusOr<std::shared_ptr<HttpTransport>> CreateServerConnection(
    bool http1, const std::shared_ptr<uvw::tcp_handle>& client,
    Http2RequestHandler handler, Http2Options options, SslContext tls_context,
    std::function<void(HttpTransport*)> on_closed, std::string prebuffered) {
  if (http1) {
    return Http1Connection::Create(client, /*server=*/true, std::move(handler),
                                   std::move(options), std::move(tls_context),
                                   {}, std::move(on_closed),
                                   std::move(prebuffered));
  }
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<Http2Connection> connection,
      Http2Connection::Create(client, /*server=*/true, std::move(handler),
                              std::move(options), std::move(tls_context), {},
                              std::move(on_closed), std::move(prebuffered)));
  return std::static_pointer_cast<HttpTransport>(connection);
}

// Turns off Nagle, and says so if it cannot.
//
// Not fatal: a connection with Nagle on works, it just holds small writes back
// waiting for an ack that A11's request/response traffic has no reason to send
// yet. Worth a log rather than a failed connection.
void SetNoDelay(const std::shared_ptr<uvw::tcp_handle>& tcp,
                absl::string_view which) {
  if (tcp == nullptr) {
    return;
  }
  if (!tcp->no_delay(true)) {
    LOG(WARNING) << "Could not disable Nagle on the " << which
                 << " HTTP socket; small messages may wait for an ack";
  }
}

}  // namespace

absl::StatusOr<std::shared_ptr<Http2Server>> Http2Server::Create(
    std::string bind_address, std::uint16_t port, Http2RequestHandler handler,
    Http2Options options) {
  if (bind_address.empty()) {
    return absl::InvalidArgumentError("HTTP/2 bind address must not be empty");
  }
  if (!handler) {
    return absl::InvalidArgumentError(
        "HTTP/2 request handler must be callable");
  }
  ABSL_RETURN_IF_ERROR(options.Validate());
  ABSL_ASSIGN_OR_RETURN(SslContext tls_context,
                        CreateTlsContext(options.tls, true,
                                         ProtocolPolicy::FromOptions(options)));
  auto state =
      std::make_shared<State>(std::move(bind_address), std::move(handler),
                              options, std::move(tls_context));
  return RunOnUv<std::shared_ptr<Http2Server>>(
      [state, port]() -> absl::StatusOr<std::shared_ptr<Http2Server>> {
        std::shared_ptr<uvw::tcp_handle> listener;
        listener = UvExecutor::Instance().loop()->resource<uvw::tcp_handle>();
        std::weak_ptr<State> weak = state;
        listener->on<uvw::error_event>(
            [weak](const uvw::error_event& event, uvw::tcp_handle&) {
              if (std::shared_ptr<State> server = weak.lock()) {
                {
                  thread::MutexLock lock(&server->mu);
                  server->running = false;
                }
                LOG(ERROR) << "HTTP/2 listener error: " << event.what();
              }
            });
        listener->on<uvw::listen_event>([weak](const uvw::listen_event&,
                                               uvw::tcp_handle& listening) {
          std::shared_ptr<State> server = weak.lock();
          if (server == nullptr) {
            return;
          }
          std::shared_ptr<uvw::tcp_handle> client;
          client = listening.parent().resource<uvw::tcp_handle>();
          // A11's traffic is request and response: small frames whose sender
          // is waiting for an answer before it has anything else to send.
          // Nagle exists to coalesce a stream of such writes, and with
          // nothing following to coalesce it just holds each one back for an
          // ack, which is the one case where it costs latency and buys
          // nothing.
          SetNoDelay(client, "accepted");
          const int accepted = listening.accept(*client);
          if (accepted != 0) {
            LOG(ERROR) << "Could not accept HTTP/2 connection: "
                       << uv_strerror(accepted);
            client->close();
            return;
          }
          auto remove_connection = [weak](HttpTransport* closed_connection) {
            if (std::shared_ptr<State> active = weak.lock()) {
              thread::MutexLock lock(&active->mu);
              std::erase_if(active->connections,
                            [closed_connection](const auto& connection) {
                              return connection.get() == closed_connection;
                            });
            }
          };

          // Registers a freshly-created connection, or closes it if the server
          // has since stopped.
          auto register_connection =
              [weak](absl::StatusOr<std::shared_ptr<HttpTransport>> connection,
                     uvw::tcp_handle* socket) {
                if (!connection.ok()) {
                  LOG(ERROR) << "Could not initialize HTTP connection: "
                             << connection.status();
                  if (socket != nullptr && !socket->closing()) {
                    socket->close();
                  }
                  return;
                }
                // Decided under the lock, closed outside it. Close() reaches the
                // connection's on_closed hook -- remove_connection above, which
                // takes this very mutex -- and on the loop thread it gets there
                // synchronously, because RunStatusOnUv runs its operation inline
                // when it is already on the loop. Closing with the lock held
                // therefore re-locked it: a fibre mutex refuses that, so the
                // server aborted with "a deadlock is detected" on whichever
                // accept happened to land after the listener stopped running.
                bool registered = false;
                if (std::shared_ptr<State> active = weak.lock();
                    active != nullptr) {
                  thread::MutexLock lock(&active->mu);
                  registered = active->running;
                  if (registered) {
                    active->connections.push_back(std::move(*connection));
                  }
                }
                if (!registered) {
                  (void)(*connection)->Close();
                }
              };

          const bool tls = server->tls_context != nullptr;
          const bool allow_h2c = server->options.enable_h2c;
          const bool allow_http1 = server->options.enable_http1;

          // Over TLS the ALPN callback advertises h2 when enabled (preferred)
          // and otherwise http/1.1, so the accepted connection's protocol is
          // fixed by config: HTTP/2 if enabled, else HTTP/1.1.
          if (tls) {
            const bool tls_http1 =
                server->options.enable_http1 && !server->options.enable_h2;
            register_connection(
                CreateServerConnection(tls_http1, client, server->handler,
                                       server->options, server->tls_context,
                                       remove_connection, {}),
                client.get());
            return;
          }
          if (allow_h2c && !allow_http1) {
            register_connection(
                CreateServerConnection(/*http1=*/false, client,
                                       server->handler, server->options,
                                       server->tls_context, remove_connection,
                                       {}),
                client.get());
            return;
          }
          if (allow_http1 && !allow_h2c) {
            register_connection(
                CreateServerConnection(/*http1=*/true, client, server->handler,
                                       server->options, server->tls_context,
                                       remove_connection, {}),
                client.get());
            return;
          }

          // Both cleartext protocols enabled: sniff the first bytes.
          auto sniff_buffer = std::make_shared<std::string>();
          client->on<uvw::error_event>(
              [](const uvw::error_event&, uvw::tcp_handle& handle) {
                handle.close();
              });
          client->on<uvw::end_event>(
              [](const uvw::end_event&, uvw::tcp_handle& handle) {
                handle.close();
              });
          client->on<uvw::data_event>(
              [server, client, sniff_buffer, remove_connection,
               register_connection](const uvw::data_event& event,
                                     uvw::tcp_handle& handle) {
                sniff_buffer->append(event.data.get(), event.length);
                const PrefaceGuess guess = GuessPreface(*sniff_buffer);
                if (guess == PrefaceGuess::kNeedMore) {
                  return;  // Keep reading until the preface resolves.
                }
                handle.stop();  // The real connection re-arms reads.
                // Defer construction to the next loop tick: creating the real
                // connection re-registers this handle's callbacks, which would
                // otherwise destroy this lambda while it is still executing.
                const bool http1 = guess == PrefaceGuess::kHttp1;
                std::string prebuffered = std::move(*sniff_buffer);
                (void)UvExecutor::Instance().Post(
                    [server, client, remove_connection, register_connection,
                     http1, prebuffered = std::move(prebuffered)]() mutable {
                      register_connection(
                          CreateServerConnection(
                              http1, client, server->handler, server->options,
                              server->tls_context, remove_connection,
                              std::move(prebuffered)),
                          client.get());
                    });
              });
          client->no_delay(true);
          client->read();
        });
        int result = listener->bind(state->bind_address, port);
        if (result != 0) {
          return UvError(result, "Binding HTTP/2 listener");
        }
        result = listener->listen();
        if (result != 0) {
          return UvError(result, "Listening for HTTP/2");
        }
        const uvw::socket_address address = listener->sock();
        if (address.port > std::numeric_limits<std::uint16_t>::max()) {
          listener->close();
          return absl::InternalError(
              "HTTP/2 listener returned an invalid port");
        }
        {
          thread::MutexLock lock(&state->mu);
          state->listener = listener;
          state->port = static_cast<std::uint16_t>(address.port);
          state->running = true;
        }
        struct MakeSharedEnabler final : Http2Server {
          explicit MakeSharedEnabler(std::shared_ptr<State> state)
              : Http2Server(std::move(state)) {}
        };
        return std::shared_ptr<Http2Server>(
            std::make_shared<MakeSharedEnabler>(state));
      });
}

Http2Server::~Http2Server() {
  (void)Stop();
}

absl::Status Http2Server::Stop() {
  std::shared_ptr<State> state = state_;
  return RunStatusOnUv([state = std::move(state)]() {
    std::vector<std::shared_ptr<HttpTransport>> connections;
    std::shared_ptr<uvw::tcp_handle> listener;
    {
      thread::MutexLock lock(&state->mu);
      if (!state->running && state->listener == nullptr) {
        return absl::OkStatus();
      }
      state->running = false;
      listener = std::move(state->listener);
      connections.swap(state->connections);
    }
    if (listener != nullptr && !listener->closing()) {
      listener->close();
    }
    for (const auto& connection : connections) {
      if (connection != nullptr) {
        (void)connection->Close(absl::CancelledError("HTTP/2 server stopped"));
      }
    }
    return absl::OkStatus();
  });
}

std::uint16_t Http2Server::port() const {
  thread::MutexLock lock(&state_->mu);
  return state_->port;
}

std::string Http2Server::bind_address() const {
  return state_->bind_address;
}

bool Http2Server::running() const {
  thread::MutexLock lock(&state_->mu);
  return state_->running;
}

bool Http2Server::secure() const {
  return state_->options.tls.enabled;
}

void* absl_nullable Http2Server::GetImpl() const {
  thread::MutexLock lock(&state_->mu);
  return state_->listener.get();
}

a11::Future<std::shared_ptr<Http2Client>> Http2Client::Connect(
    std::string host, std::uint16_t port, Http2Options options) {
  if (host.empty()) {
    return a11::FailedFuture<std::shared_ptr<Http2Client>>(
        absl::InvalidArgumentError("HTTP/2 host must not be empty"));
  }
  absl::Status validation = options.Validate();
  if (!validation.ok()) {
    return a11::FailedFuture<std::shared_ptr<Http2Client>>(validation);
  }
  if (options.deadline <= absl::Now()) {
    return a11::FailedFuture<std::shared_ptr<Http2Client>>(
        absl::DeadlineExceededError("HTTP/2 connect deadline exceeded"));
  }
  absl::StatusOr<SslContext> tls_context = CreateTlsContext(
      options.tls, false, ProtocolPolicy::FromOptions(options));
  if (!tls_context.ok()) {
    return a11::FailedFuture<std::shared_ptr<Http2Client>>(
        tls_context.status());
  }
  auto promise = std::make_shared<a11::Promise<std::shared_ptr<Http2Client>>>();
  a11::Future<std::shared_ptr<Http2Client>> future = promise->future();
  absl::Status queued = UvExecutor::Instance().Post(
      [promise, host = std::move(host), port, options,
       tls_context = std::move(*tls_context)]() mutable {
        std::shared_ptr<uvw::get_addr_info_req> resolver;
        resolver =
            UvExecutor::Instance().loop()->resource<uvw::get_addr_info_req>();
        resolver->on<uvw::error_event>(
            [promise](const uvw::error_event& event, uvw::get_addr_info_req&) {
              promise
                  ->SetStatus(absl::UnavailableError(
                      absl::StrCat("HTTP/2 DNS lookup failed: ", event.what())))
                  .IgnoreError();
            });
        resolver->on<
            uvw::addr_info_event>([promise, host, port, options, tls_context](
                                      const uvw::addr_info_event& event,
                                      uvw::get_addr_info_req&) mutable {
          const addrinfo* address = event.data.get();
          while (address != nullptr && address->ai_family != AF_INET &&
                 address->ai_family != AF_INET6) {
            address = address->ai_next;
          }
          if (address == nullptr || address->ai_addr == nullptr) {
            promise
                ->SetStatus(absl::UnavailableError(
                    "HTTP/2 DNS lookup returned no TCP address"))
                .IgnoreError();
            return;
          }
          std::shared_ptr<uvw::tcp_handle> tcp;
          tcp = UvExecutor::Instance().loop()->resource<uvw::tcp_handle>();
          SetNoDelay(tcp, "client");
          tcp->on<uvw::error_event>(
              [promise](const uvw::error_event& error, uvw::tcp_handle&) {
                promise
                    ->SetStatus(absl::UnavailableError(absl::StrCat(
                        "HTTP/2 connection failed: ", error.what())))
                    .IgnoreError();
              });
          tcp->on<uvw::connect_event>([promise, host, port, options,
                                       tls_context](const uvw::connect_event&,
                                                    uvw::tcp_handle& handle) {
            const std::shared_ptr<uvw::tcp_handle> tcp_handle =
                std::static_pointer_cast<uvw::tcp_handle>(
                    handle.shared_from_this());
            // Select the client protocol and the ALPN it will offer.
            using Preference = Http2Options::ProtocolPreference;
            bool use_http1 = false;
            if (options.tls.enabled) {
              // Over TLS the connection offers a single ALPN identifier;
              // kHttp11 requests HTTP/1.1, otherwise HTTP/2.
              use_http1 = options.client_preference == Preference::kHttp11;
            } else if (options.client_preference == Preference::kHttp11) {
              use_http1 = true;
            } else if (options.client_preference == Preference::kHttp2) {
              use_http1 = false;
            } else {  // kAuto: prefer h2c unless only HTTP/1.1 is enabled.
              use_http1 = !options.enable_h2c && options.enable_http1;
            }
            absl::StatusOr<std::shared_ptr<HttpTransport>> connection;
            if (use_http1) {
              connection = Http1Connection::Create(tcp_handle, false, {},
                                                   options, tls_context, host);
            } else {
              connection = Http2Connection::Create(tcp_handle, false, {},
                                                   options, tls_context, host);
            }
            if (!connection.ok()) {
              promise->SetStatus(connection.status()).IgnoreError();
              handle.close();
              return;
            }
            std::shared_ptr<HttpTransport> ready_connection =
                std::move(*connection);
            const bool tried_http1 = use_http1;
            ready_connection->Ready().OnReady(
                [promise, host, port, options, tried_http1,
                 ready_connection](const absl::StatusOr<a11::Unit>& ready) {
                  if (!ready.ok()) {
                    ready_connection->Close(ready.status()).IgnoreError();
                    // Cleartext try-and-downgrade: if the preferred protocol
                    // could not be established (e.g. the server speaks the
                    // other one and closed the connection), reconnect once
                    // with the alternate protocol.
                    using Preference = Http2Options::ProtocolPreference;
                    if (!options.tls.enabled &&
                        options.client_allow_downgrade &&
                        options.client_preference == Preference::kAuto &&
                        options.enable_h2c && options.enable_http1) {
                      Http2Options retry = options;
                      retry.client_preference = tried_http1
                                                    ? Preference::kHttp2
                                                    : Preference::kHttp11;
                      retry.client_allow_downgrade = false;
                      Http2Client::Connect(host, port, retry)
                          .OnReady(
                              [promise](const absl::StatusOr<
                                        std::shared_ptr<Http2Client>>& out) {
                                (void)promise->SetResult(out);
                              });
                      return;
                    }
                    promise->SetStatus(ready.status()).IgnoreError();
                    return;
                  }
                  struct MakeSharedEnabler final : Http2Client {
                    MakeSharedEnabler(std::string host, std::uint16_t port,
                                      Http2Options options,
                                      std::shared_ptr<HttpTransport> connection)
                        : Http2Client(std::move(host), port, std::move(options),
                                      std::move(connection)) {}
                  };
                  std::shared_ptr<Http2Client> client =
                      std::make_shared<MakeSharedEnabler>(host, port, options,
                                                          ready_connection);
                  promise->SetValue(std::move(client)).IgnoreError();
                });
          });
          if (const int connected = tcp->connect(*address->ai_addr);
              connected != 0) {
            promise
                ->SetStatus(
                    UvError(connected, "Starting HTTP/2 TCP connection"))
                .IgnoreError();
            tcp->close();
          }
        });
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        const int resolving =
            resolver->addr_info(host, std::to_string(port), &hints);
        if (resolving != 0) {
          promise->SetStatus(UvError(resolving, "Starting HTTP/2 DNS lookup"))
              .IgnoreError();
        }
      });
  if (!queued.ok()) {
    promise->SetStatus(queued).IgnoreError();
    return future;
  }
  if (options.deadline != absl::InfiniteFuture()) {
    std::weak_ptr<a11::Promise<std::shared_ptr<Http2Client>>> weak = promise;
    a11::Schedule([future, weak, deadline = options.deadline]() {
      absl::Status status = future.Await(deadline).status();
      if (absl::IsDeadlineExceeded(status)) {
        if (const std::shared_ptr<a11::Promise<std::shared_ptr<Http2Client>>>
                pending = weak.lock();
            pending != nullptr) {
          pending
              ->SetStatus(absl::DeadlineExceededError(
                  "HTTP/2 connect deadline exceeded"))
              .IgnoreError();
        }
      }
    });
  }
  return future;
}

struct Http2Client::Impl {
  Impl(std::string client_host, std::uint16_t client_port,
       Http2Options client_options,
       std::shared_ptr<HttpTransport> client_connection)
      : host(std::move(client_host)),
        port(client_port),
        options(std::move(client_options)),
        connection(std::move(client_connection)) {}

  const std::string host;
  const std::uint16_t port;
  const Http2Options options;
  mutable thread::Mutex mu;
  std::shared_ptr<HttpTransport> connection ABSL_GUARDED_BY(mu);
};

Http2Client::Http2Client(std::string host, std::uint16_t port,
                         Http2Options options,
                         std::shared_ptr<HttpTransport> connection) {
  static_assert(sizeof(Impl) <= kImplSize,
                "Http2Client::Impl outgrew its inline storage. kImplSize in "
                "http2.h is derived from Impl's member types, so this means a "
                "member was added or changed without updating that expression "
                "-- do that rather than padding the literal slack, which would "
                "hide the same overflow on the other standard library.");
  static_assert(alignof(Impl) <= kImplAlignment);
  std::construct_at(reinterpret_cast<Impl*>(impl_), std::move(host), port,
                    options, std::move(connection));
}

Http2Client::Impl* Http2Client::state() {
  return std::launder(reinterpret_cast<Impl*>(impl_));
}

const Http2Client::Impl* Http2Client::state() const {
  return std::launder(reinterpret_cast<const Impl*>(impl_));
}

Http2Client::~Http2Client() {
  Close().IgnoreError();
  std::destroy_at(state());
}

absl::StatusOr<std::shared_ptr<Http2ResponseStream>> Http2Client::RequestStream(
    std::string method, std::string path, HttpHeaders headers, std::string body,
    std::string scheme) {
  std::shared_ptr<HttpTransport> connection;
  {
    thread::MutexLock lock(&state()->mu);
    connection = state()->connection;
  }
  if (connection == nullptr) {
    return absl::UnavailableError("HTTP client is closed");
  }
  auto* http = dynamic_cast<HttpConnection*>(connection.get());
  if (scheme.empty()) {
    scheme = connection->secure() ? "https" : "http";
  }
  std::string authority = state()->host;
  if (authority.find(':') != std::string::npos &&
      (authority.empty() || authority.front() != '[')) {
    authority = absl::StrCat("[", authority, "]");
  }
  authority = absl::StrCat(authority, ":", state()->port);
  return http->SubmitRequest(std::move(method), std::move(scheme),
                             std::move(authority), std::move(path),
                             std::move(headers), std::move(body));
}

absl::StatusOr<std::shared_ptr<Http2DuplexStream>> Http2Client::ExtendedConnect(
    std::string protocol, std::string path, HttpHeaders headers,
    std::string scheme) {
  std::shared_ptr<HttpTransport> connection;
  {
    thread::MutexLock lock(&state()->mu);
    connection = state()->connection;
  }
  if (connection == nullptr) {
    return absl::UnavailableError("HTTP client is closed");
  }
  auto* http = dynamic_cast<HttpConnection*>(connection.get());
  if (scheme.empty()) {
    scheme = connection->secure() ? "https" : "http";
  }
  std::string authority = state()->host;
  if (authority.find(':') != std::string::npos &&
      (authority.empty() || authority.front() != '[')) {
    authority = absl::StrCat("[", authority, "]");
  }
  authority = absl::StrCat(authority, ":", state()->port);
  return http->SubmitDuplex(std::move(protocol), std::move(scheme),
                            std::move(authority), std::move(path),
                            std::move(headers));
}

absl::StatusOr<std::shared_ptr<Http2DuplexStream>>
Http2Client::RequestStreamingBody(std::string method, std::string path,
                                  HttpHeaders headers, std::string scheme) {
  std::shared_ptr<HttpTransport> connection;
  {
    thread::MutexLock lock(&state()->mu);
    connection = state()->connection;
  }
  if (connection == nullptr) {
    return absl::UnavailableError("HTTP client is closed");
  }
  auto* http = dynamic_cast<HttpConnection*>(connection.get());
  if (scheme.empty()) {
    scheme = connection->secure() ? "https" : "http";
  }
  std::string authority = state()->host;
  if (authority.find(':') != std::string::npos &&
      (authority.empty() || authority.front() != '[')) {
    authority = absl::StrCat("[", authority, "]");
  }
  authority = absl::StrCat(authority, ":", state()->port);
  return http->SubmitStreamingRequest(std::move(method), std::move(scheme),
                                      std::move(authority), std::move(path),
                                      std::move(headers));
}

a11::Future<HttpResponse> Http2Client::Request(std::string method,
                                               std::string path,
                                               HttpHeaders headers,
                                               std::string body,
                                               std::string scheme) {
  absl::StatusOr<std::shared_ptr<Http2ResponseStream>> stream =
      RequestStream(std::move(method), std::move(path), std::move(headers),
                    std::move(body), std::move(scheme));
  if (!stream.ok()) {
    return a11::FailedFuture<HttpResponse>(stream.status());
  }
  std::shared_ptr<Http2Client> self = shared_from_this();
  return a11::Submit<HttpResponse>(
      [self = std::move(self),
       stream = std::move(*stream)]() mutable -> absl::StatusOr<HttpResponse> {
        const Http2Options& options = self->state()->options;
        ABSL_ASSIGN_OR_RETURN(HttpResponseHead head,
                              stream->Headers().Await(options.deadline));
        HttpResponse response{.head = std::move(head)};
        while (true) {
          ABSL_ASSIGN_OR_RETURN(std::optional<std::string> chunk,
                                stream->Read().Await(options.deadline));
          if (!chunk.has_value()) {
            break;
          }
          if (response.body.size() + chunk->size() >
              options.max_response_body_size) {
            const absl::Status status = absl::OutOfRangeError(
                "HTTP/2 response exceeds max_response_body_size");
            (void)stream->Cancel(status);
            return status;
          }
          response.body.append(*chunk);
        }
        ABSL_RETURN_IF_ERROR(stream->Done().Await(options.deadline).status());
        return response;
      });
}

absl::Status Http2Client::Close() {
  std::shared_ptr<HttpTransport> connection;
  {
    thread::MutexLock lock(&state()->mu);
    connection = std::move(state()->connection);
  }
  if (connection == nullptr) {
    return absl::OkStatus();
  }
  return connection->Close();
}

std::string Http2Client::host() const {
  return state()->host;
}

std::uint16_t Http2Client::port() const {
  return state()->port;
}

bool Http2Client::connected() const {
  std::shared_ptr<HttpTransport> connection;
  {
    thread::MutexLock lock(&state()->mu);
    connection = state()->connection;
  }
  return connection != nullptr && connection->connected();
}

bool Http2Client::secure() const {
  return state()->options.tls.enabled;
}

bool Http2Client::multiplexed() const {
  std::shared_ptr<HttpTransport> connection;
  {
    thread::MutexLock lock(&state()->mu);
    connection = state()->connection;
  }
  // Which transport was negotiated is the answer; the option was only a
  // preference, and kAuto is resolved by ALPN or by a downgrade.
  return dynamic_cast<Http2Connection*>(connection.get()) != nullptr;
}

void* absl_nullable Http2Client::GetImpl() const {
  std::shared_ptr<HttpTransport> connection;
  {
    thread::MutexLock lock(&state()->mu);
    connection = state()->connection;
  }
  return connection != nullptr ? connection->GetImpl() : nullptr;
}

}  // namespace a11::net
