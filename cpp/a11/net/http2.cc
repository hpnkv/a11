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
#include <exception>
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
#include "a11/status.h"
#include "thread/boost_primitives.h"

namespace a11::net {
namespace {

absl::Status UvError(int code, std::string_view operation) {
  return absl::UnavailableError(
      absl::StrCat(operation, " failed: ", uv_strerror(code)));
}

absl::Status Nghttp2Error(int code, std::string_view operation) {
  return absl::InternalError(
      absl::StrCat(operation, " failed: ", nghttp2_strerror(code)));
}

absl::Status ExceptionStatus(const std::exception& error,
                             std::string_view operation) {
  return absl::UnknownError(
      absl::StrCat(operation, " raised an exception: ", error.what()));
}

std::string OpenSslErrorMessage(std::string_view operation) {
  const unsigned long code = ERR_get_error();
  if (code == 0)
    return std::string(operation);
  char message[256];
  ERR_error_string_n(code, message, sizeof(message));
  return absl::StrCat(operation, ": ", message);
}

absl::Status TlsError(std::string_view operation) {
  return absl::UnavailableError(OpenSslErrorMessage(operation));
}

using SslContext = std::shared_ptr<SSL_CTX>;

constexpr unsigned char kH2Alpn[] = {2, 'h', '2'};

int SelectH2Alpn(SSL*, const unsigned char** output,
                 unsigned char* output_length, const unsigned char* input,
                 unsigned int input_length, void*) noexcept {
  unsigned char* selected = nullptr;
  unsigned char selected_length = 0;
  const int result =
      SSL_select_next_proto(&selected, &selected_length, kH2Alpn,
                            sizeof(kH2Alpn), input, input_length);
  if (result != OPENSSL_NPN_NEGOTIATED || selected_length != 2 ||
      selected == nullptr || selected[0] != 'h' || selected[1] != '2') {
    return SSL_TLSEXT_ERR_ALERT_FATAL;
  }
  *output = selected;
  *output_length = selected_length;
  return SSL_TLSEXT_ERR_OK;
}

absl::Status LoadCertificateAndKey(SSL_CTX* context,
                                   const Http2TlsOptions& options) {
  if (options.certificate_pem_file.empty())
    return absl::OkStatus();
  ERR_clear_error();
  if (SSL_CTX_use_certificate_chain_file(
          context, options.certificate_pem_file.c_str()) != 1) {
    return absl::InvalidArgumentError(
        OpenSslErrorMessage("Loading the TLS certificate"));
  }
  ERR_clear_error();
  if (SSL_CTX_use_PrivateKey_file(context, options.key_pem_file.c_str(),
                                  SSL_FILETYPE_PEM) != 1) {
    return absl::InvalidArgumentError(
        OpenSslErrorMessage("Loading the TLS private key"));
  }
  ERR_clear_error();
  if (SSL_CTX_check_private_key(context) != 1) {
    return absl::InvalidArgumentError(
        OpenSslErrorMessage("Checking the TLS private key"));
  }
  return absl::OkStatus();
}

absl::StatusOr<SslContext> CreateTlsContext(const Http2TlsOptions& options,
                                            bool server) {
  ABSL_RETURN_IF_ERROR(options.Validate());
  if (!options.enabled)
    return SslContext{};
  if (server && options.certificate_pem_file.empty()) {
    return absl::InvalidArgumentError(
        "A TLS HTTP/2 server requires a certificate and private key");
  }

  ERR_clear_error();
  SSL_CTX* raw =
      SSL_CTX_new(server ? TLS_server_method() : TLS_client_method());
  if (raw == nullptr) {
    return absl::InternalError(OpenSslErrorMessage("Creating the TLS context"));
  }
  SslContext context(raw, SSL_CTX_free);
  if (SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1) {
    return absl::InternalError(
        OpenSslErrorMessage("Setting the minimum TLS version"));
  }
  SSL_CTX_set_options(context.get(),
                      SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
  if (server) {
    SSL_CTX_set_alpn_select_cb(context.get(), &SelectH2Alpn, nullptr);
  } else if (options.verify_peer) {
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
    ERR_clear_error();
    const int trusted =
        options.ca_certificate_pem_file.empty()
            ? SSL_CTX_set_default_verify_paths(context.get())
            : SSL_CTX_load_verify_locations(
                  context.get(), options.ca_certificate_pem_file.c_str(),
                  nullptr);
    if (trusted != 1) {
      return absl::InvalidArgumentError(
          OpenSslErrorMessage("Loading TLS trust roots"));
    }
  } else {
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_NONE, nullptr);
  }
  ABSL_RETURN_IF_ERROR(LoadCertificateAndKey(context.get(), options));
  return context;
}

bool IsIpAddress(std::string_view host) {
  in_addr ipv4{};
  in6_addr ipv6{};
  const std::string value(host);
  return inet_pton(AF_INET, value.c_str(), &ipv4) == 1 ||
         inet_pton(AF_INET6, value.c_str(), &ipv6) == 1;
}

// One libuv loop is shared by native HTTP clients and servers. All uvw and
// nghttp2 mutation is serialized on this thread; fiber callbacks communicate
// through A11 Futures and never block the loop.
class UvExecutor {
 public:
  static UvExecutor& Instance() {
    // Process-global I/O schedulers intentionally live until process exit.
    static absl::NoDestructor<UvExecutor> executor;
    return *executor;
  }

  absl::Status Post(std::function<void()> work) {
    if (!work)
      return absl::InvalidArgumentError("uv work must be callable");
    {
      thread::MutexLock lock(&mu_);
      if (!running_) {
        return absl::FailedPreconditionError("The A11 libuv loop is stopped");
      }
      try {
        work_.push_back(std::move(work));
      } catch (const std::exception& error) {
        return ExceptionStatus(error, "Queueing libuv work");
      } catch (...) {
        return absl::UnknownError(
            "Queueing libuv work raised a non-standard exception");
      }
    }
    const int result = async_->send();
    if (result != 0)
      return UvError(result, "uv_async_send");
    return absl::OkStatus();
  }

  [[nodiscard]] std::shared_ptr<uvw::loop> loop() const { return loop_; }

  [[nodiscard]] bool IsLoopThread() const {
    thread::MutexLock lock(&mu_);
    return loop_thread_id_.has_value() &&
           *loop_thread_id_ == std::this_thread::get_id();
  }

 private:
  friend class absl::NoDestructor<UvExecutor>;

  UvExecutor() {
    try {
      loop_ = uvw::loop::create();
      async_ = loop_->resource<uvw::async_handle>();
      const int initialized = async_->init();
      if (initialized != 0) {
        LOG(FATAL) << "Could not initialize the A11 libuv executor: "
                   << uv_strerror(initialized);
      }
      async_->on<uvw::async_event>(
          [this](const uvw::async_event&, uvw::async_handle&) { Drain(); });
      thread_ = std::thread([this]() {
        {
          thread::MutexLock lock(&mu_);
          loop_thread_id_ = std::this_thread::get_id();
          cv_.SignalAll();
        }
        loop_->run();
        thread::MutexLock lock(&mu_);
        running_ = false;
      });
    } catch (const std::exception& error) {
      LOG(FATAL) << "Could not create the A11 libuv executor: " << error.what();
    } catch (...) {
      LOG(FATAL) << "Could not create the A11 libuv executor";
    }
    thread::MutexLock lock(&mu_);
    while (!loop_thread_id_.has_value()) {
      cv_.Wait(&mu_);
    }
  }

  void Drain() {
    std::deque<std::function<void()>> work;
    {
      thread::MutexLock lock(&mu_);
      work.swap(work_);
    }
    for (auto& item : work) {
      try {
        item();
      } catch (const std::exception& error) {
        LOG(ERROR) << "A11 libuv work raised: " << error.what();
      } catch (...) {
        LOG(ERROR) << "A11 libuv work raised a non-standard exception";
      }
    }
  }

  mutable thread::Mutex mu_;
  thread::CondVar cv_;
  bool running_ ABSL_GUARDED_BY(mu_) = true;
  std::optional<std::thread::id> loop_thread_id_ ABSL_GUARDED_BY(mu_);
  std::deque<std::function<void()>> work_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<uvw::loop> loop_;
  std::shared_ptr<uvw::async_handle> async_;
  std::thread thread_;
};

template <typename T>
absl::StatusOr<T> RunOnUv(std::function<absl::StatusOr<T>()> operation) {
  if (UvExecutor::Instance().IsLoopThread()) {
    try {
      return operation();
    } catch (const std::exception& error) {
      return ExceptionStatus(error, "libuv operation");
    } catch (...) {
      return absl::UnknownError(
          "libuv operation raised a non-standard exception");
    }
  }
  auto promise = std::make_shared<a11::Promise<T>>();
  a11::Future<T> future = promise->future();
  absl::Status queued = UvExecutor::Instance().Post(
      [promise, operation = std::move(operation)]() mutable {
        absl::StatusOr<T> result;
        try {
          result = operation();
        } catch (const std::exception& error) {
          result = ExceptionStatus(error, "libuv operation");
        } catch (...) {
          result = absl::UnknownError(
              "libuv operation raised a non-standard exception");
        }
        (void)promise->SetResult(std::move(result));
      });
  if (!queued.ok())
    return queued;
  return future.Await();
}

absl::Status RunStatusOnUv(std::function<absl::Status()> operation) {
  absl::StatusOr<a11::Unit> result = RunOnUv<a11::Unit>(
      [operation =
           std::move(operation)]() mutable -> absl::StatusOr<a11::Unit> {
        absl::Status status = operation();
        if (!status.ok())
          return status;
        return a11::Unit{};
      });
  return result.status();
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
  if (!protocol.empty())
    values.emplace_back(":protocol", protocol);
  values.emplace_back(":scheme", scheme);
  values.emplace_back(":authority", authority);
  values.emplace_back(":path", path);
  for (const auto& header : headers)
    values.push_back(header);
  return values;
}

std::vector<std::pair<std::string, std::string>> ResponseHeaders(
    int status, HttpHeaders headers) {
  std::vector<std::pair<std::string, std::string>> values;
  values.reserve(headers.size() + 1);
  values.emplace_back(":status", std::to_string(status));
  for (auto& header : headers)
    values.push_back(std::move(header));
  return values;
}

absl::Status Http2StreamError(std::uint32_t error_code,
                              std::string_view context) {
  if (error_code == NGHTTP2_NO_ERROR)
    return absl::OkStatus();
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

struct Http2RequestBodyStream::State {
  explicit State(size_t maximum_buffered_bytes)
      : max_buffered_bytes(maximum_buffered_bytes),
        done_promise(std::make_shared<a11::Promise<a11::Unit>>()),
        done_future(done_promise->future()) {}

  mutable thread::Mutex mu;
  std::int32_t stream_id ABSL_GUARDED_BY(mu) = -1;
  const size_t max_buffered_bytes;
  size_t buffered_bytes ABSL_GUARDED_BY(mu) = 0;
  std::deque<std::string> chunks ABSL_GUARDED_BY(mu);
  bool done ABSL_GUARDED_BY(mu) = false;
  absl::Status status ABSL_GUARDED_BY(mu);
  const std::shared_ptr<a11::Promise<a11::Unit>> done_promise;
  const a11::Task done_future;
  std::shared_ptr<a11::Promise<std::optional<std::string>>> pending_read
      ABSL_GUARDED_BY(mu);
  std::function<absl::Status(absl::Status)> cancel ABSL_GUARDED_BY(mu);

  absl::Status Push(std::string data) {
    std::shared_ptr<a11::Promise<std::optional<std::string>>> reader;
    {
      thread::MutexLock lock(&mu);
      if (done)
        return status.ok() ? absl::CancelledError("Request body is done")
                           : status;
      if (pending_read != nullptr) {
        reader = std::move(pending_read);
      } else {
        if (buffered_bytes + data.size() > max_buffered_bytes &&
            !chunks.empty()) {
          return absl::ResourceExhaustedError(
              "HTTP/2 request exceeded max_buffered_request_bytes");
        }
        buffered_bytes += data.size();
        chunks.push_back(std::move(data));
        return absl::OkStatus();
      }
    }
    (void)reader->SetValue(std::optional<std::string>(std::move(data)));
    return absl::OkStatus();
  }

  void Finish(absl::Status completion) {
    std::shared_ptr<a11::Promise<std::optional<std::string>>> reader;
    {
      thread::MutexLock lock(&mu);
      if (done)
        return;
      done = true;
      status = completion;
      reader = std::move(pending_read);
    }
    if (reader != nullptr) {
      if (completion.ok()) {
        (void)reader->SetValue(std::nullopt);
      } else {
        (void)reader->SetStatus(completion);
      }
    }
    if (completion.ok()) {
      (void)done_promise->SetValue(a11::Unit{});
    } else {
      (void)done_promise->SetStatus(completion);
    }
  }
};

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
    if (!status.ok())
      return a11::FailedFuture<std::optional<std::string>>(status);
    return a11::ReadyFuture(std::optional<std::string>());
  }
  return promise->future();
}

a11::Task Http2RequestBodyStream::Done() const {
  return state_->done_future;
}

absl::Status Http2RequestBodyStream::Cancel(absl::Status status) {
  if (status.ok())
    return absl::InvalidArgumentError("Cancellation status must be non-OK");
  std::function<absl::Status(absl::Status)> cancel;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->done)
      return absl::OkStatus();
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

struct Http2ResponseStream::State {
  explicit State(size_t maximum_buffered_bytes)
      : max_buffered_bytes(maximum_buffered_bytes),
        headers_promise(std::make_shared<a11::Promise<HttpResponseHead>>()),
        headers_future(headers_promise->future()),
        done_promise(std::make_shared<a11::Promise<a11::Unit>>()),
        done_future(done_promise->future()) {}

  mutable thread::Mutex mu;
  std::int32_t stream_id ABSL_GUARDED_BY(mu) = -1;
  const size_t max_buffered_bytes;
  size_t buffered_bytes ABSL_GUARDED_BY(mu) = 0;
  std::deque<std::string> chunks ABSL_GUARDED_BY(mu);
  bool headers_ready ABSL_GUARDED_BY(mu) = false;
  bool done ABSL_GUARDED_BY(mu) = false;
  absl::Status status ABSL_GUARDED_BY(mu);
  const std::shared_ptr<a11::Promise<HttpResponseHead>> headers_promise;
  const a11::Future<HttpResponseHead> headers_future;
  const std::shared_ptr<a11::Promise<a11::Unit>> done_promise;
  const a11::Task done_future;
  std::shared_ptr<a11::Promise<std::optional<std::string>>> pending_read
      ABSL_GUARDED_BY(mu);
  std::function<absl::Status(absl::Status)> cancel ABSL_GUARDED_BY(mu);

  void SetHeaders(HttpResponseHead head) {
    bool publish = false;
    {
      thread::MutexLock lock(&mu);
      if (!headers_ready && !done) {
        headers_ready = true;
        publish = true;
      }
    }
    if (publish)
      (void)headers_promise->SetValue(std::move(head));
  }

  absl::Status Push(std::string data) {
    std::shared_ptr<a11::Promise<std::optional<std::string>>> reader;
    {
      thread::MutexLock lock(&mu);
      if (done)
        return status.ok() ? absl::CancelledError("Response is done") : status;
      if (pending_read != nullptr) {
        reader = std::move(pending_read);
      } else {
        if (buffered_bytes + data.size() > max_buffered_bytes &&
            !chunks.empty()) {
          return absl::ResourceExhaustedError(
              "HTTP/2 response exceeded max_buffered_response_bytes");
        }
        buffered_bytes += data.size();
        chunks.push_back(std::move(data));
        return absl::OkStatus();
      }
    }
    (void)reader->SetValue(std::optional<std::string>(std::move(data)));
    return absl::OkStatus();
  }

  void Finish(absl::Status completion) {
    std::shared_ptr<a11::Promise<std::optional<std::string>>> reader;
    bool publish_headers_error = false;
    {
      thread::MutexLock lock(&mu);
      if (done)
        return;
      done = true;
      status = completion;
      reader = std::move(pending_read);
      if (!headers_ready) {
        headers_ready = true;
        publish_headers_error = true;
      }
    }
    if (publish_headers_error) {
      absl::Status error =
          completion.ok() ? absl::DataLossError(
                                "HTTP/2 stream ended before response headers")
                          : completion;
      (void)headers_promise->SetStatus(error);
    }
    if (reader != nullptr) {
      if (completion.ok()) {
        (void)reader->SetValue(std::nullopt);
      } else {
        (void)reader->SetStatus(completion);
      }
    }
    if (completion.ok()) {
      (void)done_promise->SetValue(a11::Unit{});
    } else {
      (void)done_promise->SetStatus(completion);
    }
  }
};

struct Http2ResponseWriter::State {
  State()
      : done_promise(std::make_shared<a11::Promise<a11::Unit>>()),
        done_future(done_promise->future()) {}

  mutable thread::Mutex mu;
  bool done ABSL_GUARDED_BY(mu) = false;
  const std::shared_ptr<a11::Promise<a11::Unit>> done_promise;
  const a11::Task done_future;

  void Finish(const absl::Status& status) {
    {
      thread::MutexLock lock(&mu);
      if (done)
        return;
      done = true;
    }
    if (status.ok()) {
      (void)done_promise->SetValue(a11::Unit{});
    } else {
      (void)done_promise->SetStatus(status);
    }
  }
};

a11::Future<HttpResponseHead> Http2ResponseStream::Headers() const {
  return state_->headers_future;
}

a11::Future<std::optional<std::string>> Http2ResponseStream::Read() {
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
              "Only one HTTP/2 response Read may be outstanding"));
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
    if (state_->done)
      return absl::OkStatus();
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

std::optional<std::string> GetHttpHeader(const HttpHeaders& headers,
                                         std::string_view name) {
  std::string normalized(name);
  absl::AsciiStrToLower(&normalized);
  const auto iterator = std::find_if(
      headers.begin(), headers.end(),
      [&](const auto& header) { return header.first == normalized; });
  if (iterator == headers.end())
    return std::nullopt;
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
  return tls.Validate();
}

class Http2Connection : public std::enable_shared_from_this<Http2Connection> {
 public:
  static absl::StatusOr<std::shared_ptr<Http2Connection>> Create(
      std::shared_ptr<uvw::tcp_handle> tcp, bool server,
      Http2RequestHandler handler, Http2Options options,
      SslContext tls_context = {}, std::string tls_server_name = {},
      std::function<void(Http2Connection*)> on_closed = {}) {
    if (tcp == nullptr)
      return absl::InvalidArgumentError("TCP handle must not be null");

    struct MakeSharedEnabler final : Http2Connection {
      MakeSharedEnabler(std::shared_ptr<uvw::tcp_handle> tcp, bool server,
                        Http2RequestHandler handler, Http2Options options,
                        SslContext tls_context, std::string tls_server_name,
                        std::function<void(Http2Connection*)> on_closed)
          : Http2Connection(std::move(tcp), server, std::move(handler), options,
                            std::move(tls_context), std::move(tls_server_name),
                            std::move(on_closed)) {}
    };

    auto connection = std::make_shared<MakeSharedEnabler>(
        std::move(tcp), server, std::move(handler), options,
        std::move(tls_context), std::move(tls_server_name),
        std::move(on_closed));
    ABSL_RETURN_IF_ERROR(connection->Initialize());
    return connection;
  }

  ~Http2Connection() {
    if (session_ != nullptr)
      nghttp2_session_del(session_);
    if (ssl_ != nullptr)
      SSL_free(ssl_);
  }

  [[nodiscard]] a11::Task Ready() const { return ready_future_; }

  absl::StatusOr<std::shared_ptr<Http2ResponseStream>> SubmitRequest(
      std::string method, std::string scheme, std::string authority,
      std::string path, HttpHeaders headers, std::string body) {
    std::shared_ptr<Http2Connection> self = shared_from_this();
    return RunOnUv<std::shared_ptr<Http2ResponseStream>>(
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
      std::string path, HttpHeaders headers) {
    std::shared_ptr<Http2Connection> self = shared_from_this();
    return RunOnUv<std::shared_ptr<Http2DuplexStream>>(
        [self = std::move(self), protocol = std::move(protocol),
         scheme = std::move(scheme), authority = std::move(authority),
         path = std::move(path), headers = std::move(headers)]() mutable
            -> absl::StatusOr<std::shared_ptr<Http2DuplexStream>> {
          return self->SubmitDuplexOnLoop(
              std::move(protocol), std::move(scheme), std::move(authority),
              std::move(path), std::move(headers));
        });
  }

  absl::Status WriteRequest(std::int32_t stream_id, std::string data) {
    std::shared_ptr<Http2Connection> self = shared_from_this();
    return RunStatusOnUv(
        [self = std::move(self), stream_id, data = std::move(data)]() mutable {
          return self->WriteRequestOnLoop(stream_id, std::move(data));
        });
  }

  absl::Status FinishRequest(std::int32_t stream_id) {
    std::shared_ptr<Http2Connection> self = shared_from_this();
    return RunStatusOnUv([self = std::move(self), stream_id]() {
      return self->FinishRequestOnLoop(stream_id);
    });
  }

  absl::Status SendHeaders(std::int32_t stream_id, int status,
                           HttpHeaders headers) {
    std::shared_ptr<Http2Connection> self = shared_from_this();
    return RunStatusOnUv([self = std::move(self), stream_id, status,
                          headers = std::move(headers)]() mutable {
      return self->SendHeadersOnLoop(stream_id, status, std::move(headers));
    });
  }

  absl::Status Write(std::int32_t stream_id, std::string data) {
    std::shared_ptr<Http2Connection> self = shared_from_this();
    return RunStatusOnUv(
        [self = std::move(self), stream_id, data = std::move(data)]() mutable {
          return self->WriteOnLoop(stream_id, std::move(data));
        });
  }

  absl::Status Finish(std::int32_t stream_id) {
    std::shared_ptr<Http2Connection> self = shared_from_this();
    return RunStatusOnUv([self = std::move(self), stream_id]() {
      return self->FinishOnLoop(stream_id);
    });
  }

  absl::Status SendResponse(std::int32_t stream_id, int status,
                            HttpHeaders headers, std::string body) {
    std::shared_ptr<Http2Connection> self = shared_from_this();
    return RunStatusOnUv([self = std::move(self), stream_id, status,
                          headers = std::move(headers),
                          body = std::move(body)]() mutable -> absl::Status {
      ABSL_RETURN_IF_ERROR(
          self->SendHeadersOnLoop(stream_id, status, std::move(headers)));
      if (!body.empty()) {
        ABSL_RETURN_IF_ERROR(self->WriteOnLoop(stream_id, std::move(body)));
      }
      return self->FinishOnLoop(stream_id);
    });
  }

  absl::Status AbortResponse(std::int32_t stream_id, absl::Status status) {
    if (status.ok()) {
      return absl::InvalidArgumentError("Response abort status must be non-OK");
    }
    std::shared_ptr<Http2Connection> self = shared_from_this();
    return RunStatusOnUv([self = std::move(self), stream_id,
                          status = std::move(status)]() mutable {
      Stream* stream = self->FindStream(stream_id);
      if (stream == nullptr)
        return absl::OkStatus();
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
    std::shared_ptr<Http2Connection> self = shared_from_this();
    return RunStatusOnUv([self = std::move(self), stream_id,
                          status = std::move(status)]() mutable {
      Stream* stream = self->FindStream(stream_id);
      if (stream == nullptr)
        return absl::OkStatus();
      if (stream->response != nullptr)
        stream->response->Finish(status);
      const int submitted =
          nghttp2_submit_rst_stream(self->session_, NGHTTP2_FLAG_NONE,
                                    stream_id, StatusToHttp2Error(status));
      if (submitted != 0 && submitted != NGHTTP2_ERR_STREAM_CLOSED) {
        return Nghttp2Error(submitted, "nghttp2_submit_rst_stream");
      }
      return self->SendSession();
    });
  }

  absl::StatusOr<bool> ResponseHeadersSent(std::int32_t stream_id) {
    std::shared_ptr<Http2Connection> self = shared_from_this();
    return RunOnUv<bool>(
        [self = std::move(self), stream_id]() -> absl::StatusOr<bool> {
          Stream* stream = self->FindStream(stream_id);
          if (stream == nullptr)
            return false;
          return stream->response_headers_sent;
        });
  }

  absl::StatusOr<bool> ResponseFinished(std::int32_t stream_id) {
    std::shared_ptr<Http2Connection> self = shared_from_this();
    return RunOnUv<bool>(
        [self = std::move(self), stream_id]() -> absl::StatusOr<bool> {
          Stream* stream = self->FindStream(stream_id);
          if (stream == nullptr)
            return true;
          return stream->outbound_finished;
        });
  }

  absl::Status Close(
      absl::Status status = absl::CancelledError("HTTP/2 connection closed")) {
    std::shared_ptr<Http2Connection> self = shared_from_this();
    return RunStatusOnUv(
        [self = std::move(self), status = std::move(status)]() {
          self->CloseOnLoop(status);
          return absl::OkStatus();
        });
  }

  [[nodiscard]] bool connected() const { return connected_.load(); }

  [[nodiscard]] bool secure() const { return ssl_context_ != nullptr; }

  [[nodiscard]] void* absl_nonnull GetImpl() const { return tcp_.get(); }

 private:
  struct Stream {
    std::int32_t id = -1;
    HttpHeaders inbound_headers;
    HttpRequest request;
    bool request_dispatched = false;
    bool request_too_large = false;
    bool duplex = false;
    bool response_headers_sent = false;
    bool response_headers_received = false;
    bool remote_end = false;
    std::deque<std::string> outbound;
    size_t outbound_offset = 0;
    bool outbound_finished = false;
    std::shared_ptr<Http2ResponseStream::State> response;
    std::shared_ptr<Http2RequestBodyStream::State> request_body;
    std::shared_ptr<Http2ResponseWriter::State> writer;
  };

  Http2Connection(std::shared_ptr<uvw::tcp_handle> tcp, bool server,
                  Http2RequestHandler handler, Http2Options options,
                  SslContext tls_context, std::string tls_server_name,
                  std::function<void(Http2Connection*)> on_closed)
      : tcp_(std::move(tcp)),
        server_(server),
        handler_(std::move(handler)),
        options_(options),
        ssl_context_(std::move(tls_context)),
        tls_server_name_(std::move(tls_server_name)),
        on_closed_(std::move(on_closed)),
        ready_promise_(std::make_shared<a11::Promise<a11::Unit>>()),
        ready_future_(ready_promise_->future()) {}

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

    std::weak_ptr<Http2Connection> weak = shared_from_this();
    tcp_->on<uvw::data_event>(
        [weak](const uvw::data_event& event, uvw::tcp_handle&) {
          if (std::shared_ptr<Http2Connection> self = weak.lock()) {
            self->OnTcpData(event.data.get(), event.length);
          }
        });
    tcp_->on<uvw::error_event>(
        [weak](const uvw::error_event& event, uvw::tcp_handle&) {
          if (std::shared_ptr<Http2Connection> self = weak.lock()) {
            self->CloseOnLoop(absl::UnavailableError(
                absl::StrCat("HTTP/2 TCP error: ", event.what())));
          }
        });
    tcp_->on<uvw::end_event>([weak](const uvw::end_event&, uvw::tcp_handle&) {
      if (std::shared_ptr<Http2Connection> self = weak.lock()) {
        self->CloseOnLoop(
            absl::UnavailableError("HTTP/2 peer closed the TCP connection"));
      }
    });
    tcp_->on<uvw::close_event>(
        [weak](const uvw::close_event&, uvw::tcp_handle&) {
          if (std::shared_ptr<Http2Connection> self = weak.lock()) {
            self->connected_.store(false);
          }
        });
    tcp_->no_delay(true);
    tcp_->read();
    const nghttp2_settings_entry settings[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 256},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 1024 * 1024},
        {NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, 1}};
    const size_t settings_count =
        server_ ? std::size(settings) : std::size(settings) - 1;
    result = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings,
                                     settings_count);
    if (result != 0) {
      return Nghttp2Error(result, "nghttp2_submit_settings");
    }
    if (ssl_context_ != nullptr)
      return InitializeTls();
    return StartHttp2();
  }

  absl::Status InitializeTls() {
    ERR_clear_error();
    ssl_ = SSL_new(ssl_context_.get());
    if (ssl_ == nullptr) {
      return absl::InternalError(
          OpenSslErrorMessage("Creating a TLS connection"));
    }
    BIO* encrypted_input = BIO_new(BIO_s_mem());
    BIO* encrypted_output = BIO_new(BIO_s_mem());
    if (encrypted_input == nullptr || encrypted_output == nullptr) {
      BIO_free(encrypted_input);
      BIO_free(encrypted_output);
      return absl::InternalError(
          OpenSslErrorMessage("Creating TLS memory BIOs"));
    }
    BIO_set_mem_eof_return(encrypted_input, -1);
    BIO_set_mem_eof_return(encrypted_output, -1);
    // SSL owns both BIOs after this call.
    SSL_set_bio(ssl_, encrypted_input, encrypted_output);

    if (server_) {
      SSL_set_accept_state(ssl_);
    } else {
      SSL_set_connect_state(ssl_);
      if (SSL_set_alpn_protos(ssl_, kH2Alpn, sizeof(kH2Alpn)) != 0) {
        return absl::InternalError(
            OpenSslErrorMessage("Configuring HTTP/2 ALPN"));
      }
      if (tls_server_name_.empty()) {
        return absl::InvalidArgumentError(
            "A TLS HTTP/2 client requires a server name");
      }
      const bool ip_address = IsIpAddress(tls_server_name_);
      if (!ip_address &&
          SSL_set_tlsext_host_name(ssl_, tls_server_name_.c_str()) != 1) {
        return absl::InvalidArgumentError(
            OpenSslErrorMessage("Configuring TLS SNI"));
      }
      if (options_.tls.verify_peer) {
        X509_VERIFY_PARAM* parameters = SSL_get0_param(ssl_);
        if (parameters == nullptr) {
          return absl::InternalError(
              "The TLS connection has no verification parameters");
        }
        X509_VERIFY_PARAM_set_hostflags(parameters,
                                        X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        const int configured =
            ip_address ? X509_VERIFY_PARAM_set1_ip_asc(parameters,
                                                       tls_server_name_.c_str())
                       : SSL_set1_host(ssl_, tls_server_name_.c_str());
        if (configured != 1) {
          return absl::InvalidArgumentError(
              OpenSslErrorMessage("Configuring TLS hostname verification"));
        }
      }
    }
    return AdvanceTlsHandshake();
  }

  void PublishReady(const absl::Status& status) {
    if (ready_published_)
      return;
    ready_published_ = true;
    if (status.ok()) {
      (void)ready_promise_->SetValue(a11::Unit{});
    } else {
      (void)ready_promise_->SetStatus(status);
    }
  }

  absl::Status StartHttp2() {
    if (http2_started_)
      return absl::OkStatus();
    absl::Status status = SendSession();
    if (!status.ok()) {
      PublishReady(status);
      return status;
    }
    http2_started_ = true;
    connected_.store(true);
    PublishReady(absl::OkStatus());
    return absl::OkStatus();
  }

  absl::Status WriteTcp(const char* data, size_t length) {
    size_t offset = 0;
    try {
      while (offset < length) {
        const size_t chunk_size = std::min(
            length - offset,
            static_cast<size_t>(std::numeric_limits<unsigned int>::max()));
        auto output = std::make_unique<char[]>(chunk_size);
        std::memcpy(output.get(), data + offset, chunk_size);
        const int written = tcp_->write(std::move(output),
                                        static_cast<unsigned int>(chunk_size));
        if (written != 0)
          return UvError(written, "Writing HTTP/2 TCP data");
        offset += chunk_size;
      }
      return absl::OkStatus();
    } catch (const std::exception& error) {
      return ExceptionStatus(error, "Writing HTTP/2 TCP data");
    } catch (...) {
      return absl::UnknownError(
          "Writing HTTP/2 TCP data raised a non-standard exception");
    }
  }

  absl::Status FlushTlsOutput() {
    if (ssl_ == nullptr)
      return absl::OkStatus();
    BIO* output = SSL_get_wbio(ssl_);
    if (output == nullptr) {
      return absl::InternalError("The TLS connection has no output BIO");
    }
    std::array<char, 16 * 1024> buffer{};
    while (BIO_ctrl_pending(output) > 0) {
      const int length = BIO_read(output, buffer.data(), buffer.size());
      if (length <= 0)
        return TlsError("Reading encrypted TLS output");
      absl::Status status =
          WriteTcp(buffer.data(), static_cast<size_t>(length));
      if (!status.ok())
        return status;
    }
    return absl::OkStatus();
  }

  absl::Status AdvanceTlsHandshake() {
    if (tls_handshake_complete_)
      return absl::OkStatus();
    ERR_clear_error();
    const int result = SSL_do_handshake(ssl_);
    const int error =
        result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl_, result);
    absl::Status flushed = FlushTlsOutput();
    if (!flushed.ok())
      return flushed;
    if (result == 1) {
      const unsigned char* protocol = nullptr;
      unsigned int protocol_length = 0;
      SSL_get0_alpn_selected(ssl_, &protocol, &protocol_length);
      if (protocol == nullptr || protocol_length != 2 || protocol[0] != 'h' ||
          protocol[1] != '2') {
        return absl::FailedPreconditionError(
            "TLS peer did not negotiate the h2 ALPN protocol");
      }
      if (!server_ && options_.tls.verify_peer) {
        const long verification = SSL_get_verify_result(ssl_);
        if (verification != X509_V_OK) {
          return absl::UnauthenticatedError(
              absl::StrCat("TLS certificate verification failed: ",
                           X509_verify_cert_error_string(verification)));
        }
      }
      tls_handshake_complete_ = true;
      return StartHttp2();
    }
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
      return absl::OkStatus();
    }
    const long verification = SSL_get_verify_result(ssl_);
    if (!server_ && options_.tls.verify_peer && verification != X509_V_OK) {
      return absl::UnauthenticatedError(
          absl::StrCat("TLS certificate verification failed: ",
                       X509_verify_cert_error_string(verification)));
    }
    return TlsError("TLS handshake failed");
  }

  absl::Status ProcessHttp2Data(const char* data, size_t size) {
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
    return SendSession();
  }

  absl::Status ReceiveTlsData(const char* data, size_t size) {
    BIO* input = SSL_get_rbio(ssl_);
    if (input == nullptr) {
      return absl::InternalError("The TLS connection has no input BIO");
    }
    size_t offset = 0;
    while (offset < size) {
      const size_t chunk_size = std::min(
          size - offset, static_cast<size_t>(std::numeric_limits<int>::max()));
      const int written =
          BIO_write(input, data + offset, static_cast<int>(chunk_size));
      if (written <= 0)
        return TlsError("Buffering encrypted TLS input");
      offset += static_cast<size_t>(written);
    }

    absl::Status status = AdvanceTlsHandshake();
    if (!status.ok() || !tls_handshake_complete_)
      return status;
    std::array<char, 16 * 1024> plaintext{};
    while (true) {
      size_t length = 0;
      ERR_clear_error();
      const int read =
          SSL_read_ex(ssl_, plaintext.data(), plaintext.size(), &length);
      if (read == 1) {
        if (length == 0)
          continue;
        status = ProcessHttp2Data(plaintext.data(), length);
        if (!status.ok())
          return status;
        continue;
      }
      const int error = SSL_get_error(ssl_, read);
      if (error == SSL_ERROR_WANT_READ)
        break;
      if (error == SSL_ERROR_WANT_WRITE) {
        status = FlushTlsOutput();
        if (!status.ok())
          return status;
        continue;
      }
      if (error == SSL_ERROR_ZERO_RETURN) {
        return absl::UnavailableError("TLS peer closed the connection");
      }
      return TlsError("Decrypting HTTP/2 TLS data");
    }
    return FlushTlsOutput();
  }

  absl::Status WriteApplicationData(const std::uint8_t* data, size_t length) {
    if (ssl_ == nullptr) {
      return WriteTcp(reinterpret_cast<const char*>(data), length);
    }
    if (!tls_handshake_complete_) {
      return absl::FailedPreconditionError(
          "Cannot write HTTP/2 data before the TLS handshake");
    }
    size_t offset = 0;
    while (offset < length) {
      size_t written = 0;
      ERR_clear_error();
      const int result =
          SSL_write_ex(ssl_, data + offset, length - offset, &written);
      if (result == 1) {
        if (written == 0) {
          return absl::InternalError("TLS accepted no HTTP/2 output data");
        }
        offset += written;
        absl::Status status = FlushTlsOutput();
        if (!status.ok())
          return status;
        continue;
      }
      const int error = SSL_get_error(ssl_, result);
      if (error == SSL_ERROR_WANT_WRITE) {
        absl::Status status = FlushTlsOutput();
        if (!status.ok())
          return status;
        continue;
      }
      return TlsError("Encrypting HTTP/2 TLS data");
    }
    return absl::OkStatus();
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
    ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(headers));
    if (body.size() > options_.max_request_body_size) {
      return absl::OutOfRangeError(
          "HTTP/2 request body exceeds max_request_body_size");
    }

    auto stream = std::make_unique<Stream>();
    stream->response = std::make_shared<Http2ResponseStream::State>(
        options_.max_buffered_response_bytes);
    if (!body.empty())
      stream->outbound.push_back(std::move(body));
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
    std::weak_ptr<Http2Connection> weak = shared_from_this();
    {
      thread::MutexLock lock(&stream->response->mu);
      stream->response->stream_id = stream_id;
      stream->response->cancel =
          [weak, stream_id](absl::Status status) -> absl::Status {
        const std::shared_ptr<Http2Connection> self = weak.lock();
        if (self == nullptr)
          return absl::OkStatus();
        return self->CancelRequest(stream_id, std::move(status));
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
    if (server_) {
      return absl::FailedPreconditionError(
          "A server HTTP/2 connection cannot submit requests");
    }
    if (closed_ || !connected_.load()) {
      return absl::UnavailableError("HTTP/2 connection is not connected");
    }
    if (protocol.empty() || scheme.empty() || authority.empty() ||
        path.empty() || path.front() != '/') {
      return absl::InvalidArgumentError(
          "HTTP/2 protocol, scheme, authority, and absolute path are required");
    }
    if (!std::all_of(protocol.begin(), protocol.end(), [](unsigned char value) {
          return value == '-' || value == '_' || value == '.' ||
                 std::isdigit(value) || (value >= 'a' && value <= 'z');
        })) {
      return absl::InvalidArgumentError(
          "HTTP/2 extended CONNECT protocol must be a lowercase token");
    }
    const std::string_view expected_scheme = secure() ? "https" : "http";
    if (scheme != expected_scheme) {
      return absl::InvalidArgumentError(absl::StrCat(
          "This HTTP/2 connection requires scheme '", expected_scheme, "'"));
    }
    ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(headers));

    auto stream = std::make_unique<Stream>();
    stream->duplex = true;
    stream->response = std::make_shared<Http2ResponseStream::State>(
        options_.max_buffered_response_bytes);
    auto values =
        RequestHeaders("CONNECT", scheme, authority, path, protocol, headers);
    auto fields = MakeNv(values);
    nghttp2_data_provider provider{};
    provider.source.ptr = stream.get();
    provider.read_callback = &DataSourceReadCallback;
    const std::int32_t stream_id =
        nghttp2_submit_request(session_, nullptr, fields.data(), fields.size(),
                               &provider, stream.get());
    if (stream_id < 0)
      return Nghttp2Error(stream_id, "nghttp2_submit_request");
    stream->id = stream_id;
    std::weak_ptr<Http2Connection> weak = shared_from_this();
    {
      thread::MutexLock lock(&stream->response->mu);
      stream->response->stream_id = stream_id;
      stream->response->cancel =
          [weak, stream_id](absl::Status status) -> absl::Status {
        std::shared_ptr<Http2Connection> self = weak.lock();
        if (self == nullptr)
          return absl::OkStatus();
        return self->CancelRequest(stream_id, std::move(status));
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
        std::make_shared<MakeDuplexEnabler>(weak_from_this(), response);
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
                    "HTTP/2 extended CONNECT exceeded its deadline"))
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
    if (data.empty())
      return absl::OkStatus();
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
    if (data.empty())
      return absl::OkStatus();
    if (server_) {
      return absl::FailedPreconditionError(
          "A server HTTP/2 connection cannot write request DATA");
    }
    Stream* stream = FindStream(stream_id);
    if (stream == nullptr)
      return absl::NotFoundError("HTTP/2 request stream is no longer active");
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
    if (resumed != 0 && resumed != NGHTTP2_ERR_INVALID_ARGUMENT)
      return Nghttp2Error(resumed, "nghttp2_session_resume_data");
    return SendSession();
  }

  absl::Status FinishOnLoop(std::int32_t stream_id) {
    Stream* stream = FindStream(stream_id);
    if (stream == nullptr)
      return absl::OkStatus();
    if (!stream->response_headers_sent) {
      return absl::FailedPreconditionError(
          "SendHeaders must be called before finishing an HTTP/2 response");
    }
    if (stream->outbound_finished)
      return absl::OkStatus();
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
    if (stream == nullptr)
      return absl::OkStatus();
    if (!stream->duplex) {
      return absl::FailedPreconditionError(
          "HTTP/2 request is not an extended CONNECT stream");
    }
    if (stream->outbound_finished)
      return absl::OkStatus();
    stream->outbound_finished = true;
    const int resumed = nghttp2_session_resume_data(session_, stream_id);
    if (resumed != 0 && resumed != NGHTTP2_ERR_INVALID_ARGUMENT)
      return Nghttp2Error(resumed, "nghttp2_session_resume_data");
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
    if (stream == nullptr)
      return false;
    const std::optional<std::string> method =
        GetHttpHeader(stream->inbound_headers, ":method");
    const std::optional<std::string> protocol =
        GetHttpHeader(stream->inbound_headers, ":protocol");
    return method == "CONNECT" && protocol.has_value() && !protocol->empty();
  }

  absl::Status BeginDuplexRequest(Stream* stream) {
    if (stream == nullptr)
      return absl::InvalidArgumentError("HTTP/2 stream must not be null");
    if (stream->request_body != nullptr)
      return absl::OkStatus();
    stream->duplex = true;
    stream->request_body = std::make_shared<Http2RequestBodyStream::State>(
        options_.max_buffered_request_bytes);
    {
      thread::MutexLock lock(&stream->request_body->mu);
      stream->request_body->stream_id = stream->id;
      std::weak_ptr<Http2Connection> weak = shared_from_this();
      const std::int32_t stream_id = stream->id;
      stream->request_body->cancel =
          [weak, stream_id](absl::Status status) -> absl::Status {
        std::shared_ptr<Http2Connection> self = weak.lock();
        if (self == nullptr)
          return absl::OkStatus();
        return self->CancelRequest(stream_id, std::move(status));
      };
    }
    return absl::OkStatus();
  }

  void OnTcpData(const char* data, size_t size) {
    if (closed_ || session_ == nullptr)
      return;
    absl::Status status = ssl_ == nullptr ? ProcessHttp2Data(data, size)
                                          : ReceiveTlsData(data, size);
    if (!status.ok())
      CloseOnLoop(status);
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
    if (result != 0)
      return Nghttp2Error(result, "nghttp2_session_send");
    return absl::OkStatus();
  }

  static ssize_t SendCallback(nghttp2_session*, const std::uint8_t* data,
                              size_t length, int, void* user_data) noexcept {
    auto* self = static_cast<Http2Connection*>(user_data);
    if (self == nullptr || self->closed_) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    try {
      absl::Status status = self->WriteApplicationData(data, length);
      if (!status.ok()) {
        self->pending_transport_error_ = std::move(status);
        return NGHTTP2_ERR_CALLBACK_FAILURE;
      }
      return static_cast<ssize_t>(length);
    } catch (const std::exception& error) {
      self->pending_transport_error_ =
          ExceptionStatus(error, "Writing HTTP/2 TCP data");
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    } catch (...) {
      self->pending_transport_error_ = absl::UnknownError(
          "Writing HTTP/2 TCP data raised a non-standard exception");
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
  }

  static ssize_t DataSourceReadCallback(nghttp2_session*, std::int32_t,
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
    }
    return static_cast<ssize_t>(copied);
  }

  static int OnBeginHeadersCallback(nghttp2_session*,
                                    const nghttp2_frame* frame,
                                    void* user_data) noexcept {
    auto* self = static_cast<Http2Connection*>(user_data);
    if (self == nullptr || frame == nullptr ||
        frame->hd.type != NGHTTP2_HEADERS) {
      return 0;
    }
    try {
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
    } catch (const std::exception& error) {
      self->pending_transport_error_ =
          ExceptionStatus(error, "Beginning HTTP/2 headers");
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    } catch (...) {
      self->pending_transport_error_ = absl::UnknownError(
          "Beginning HTTP/2 headers raised a non-standard exception");
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
  }

  static int OnHeaderCallback(nghttp2_session*, const nghttp2_frame* frame,
                              const std::uint8_t* name, size_t name_length,
                              const std::uint8_t* value, size_t value_length,
                              std::uint8_t, void* user_data) noexcept {
    auto* self = static_cast<Http2Connection*>(user_data);
    if (self == nullptr || frame == nullptr)
      return 0;
    Stream* stream = self->FindStream(frame->hd.stream_id);
    if (stream == nullptr)
      return 0;
    try {
      stream->inbound_headers.emplace_back(
          std::string(reinterpret_cast<const char*>(name), name_length),
          std::string(reinterpret_cast<const char*>(value), value_length));
      return 0;
    } catch (const std::exception& error) {
      self->pending_transport_error_ =
          ExceptionStatus(error, "Receiving HTTP/2 headers");
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    } catch (...) {
      self->pending_transport_error_ = absl::UnknownError(
          "Receiving HTTP/2 headers raised a non-standard exception");
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
  }

  static int OnFrameRecvCallback(nghttp2_session*, const nghttp2_frame* frame,
                                 void* user_data) noexcept {
    auto* self = static_cast<Http2Connection*>(user_data);
    if (self == nullptr || frame == nullptr)
      return 0;
    try {
      Stream* stream = self->FindStream(frame->hd.stream_id);
      if (stream == nullptr)
        return 0;
      if (frame->hd.type == NGHTTP2_HEADERS) {
        if (self->server_ && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
          if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
            stream->remote_end = true;
            self->DispatchRequest(stream);
          } else if (self->IsExtendedConnect(stream)) {
            absl::Status started = self->BeginDuplexRequest(stream);
            if (!started.ok()) {
              self->pending_transport_error_ = std::move(started);
              return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
            self->DispatchRequest(stream);
          }
        } else if (!self->server_ &&
                   frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
          self->CompleteResponseHeaders(stream);
          if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
            stream->remote_end = true;
            stream->response->Finish(absl::OkStatus());
          }
        }
      } else if (frame->hd.type == NGHTTP2_DATA &&
                 (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
        stream->remote_end = true;
        if (self->server_) {
          if (stream->request_body != nullptr)
            stream->request_body->Finish(absl::OkStatus());
          if (!stream->request_dispatched)
            self->DispatchRequest(stream);
        } else if (stream->response != nullptr) {
          stream->response->Finish(absl::OkStatus());
        }
      }
      return 0;
    } catch (const std::exception& error) {
      self->pending_transport_error_ =
          ExceptionStatus(error, "Receiving an HTTP/2 frame");
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    } catch (...) {
      self->pending_transport_error_ = absl::UnknownError(
          "Receiving an HTTP/2 frame raised a non-standard exception");
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
  }

  static int OnDataChunkCallback(nghttp2_session* session, std::uint8_t,
                                 std::int32_t stream_id,
                                 const std::uint8_t* data, size_t length,
                                 void* user_data) noexcept {
    auto* self = static_cast<Http2Connection*>(user_data);
    if (self == nullptr)
      return 0;
    Stream* stream = self->FindStream(stream_id);
    if (stream == nullptr)
      return 0;
    try {
      if (self->server_) {
        if (stream->request_body != nullptr) {
          absl::Status pushed = stream->request_body->Push(
              std::string(reinterpret_cast<const char*>(data), length));
          if (!pushed.ok()) {
            stream->request_body->Finish(pushed);
            const int reset =
                nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream_id,
                                          NGHTTP2_ENHANCE_YOUR_CALM);
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
        stream->request.body.append(reinterpret_cast<const char*>(data),
                                    length);
      } else if (stream->response != nullptr) {
        absl::Status pushed = stream->response->Push(
            std::string(reinterpret_cast<const char*>(data), length));
        if (!pushed.ok()) {
          stream->response->Finish(pushed);
          const int reset = nghttp2_submit_rst_stream(
              session, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_ENHANCE_YOUR_CALM);
          return reset == 0 ? 0 : NGHTTP2_ERR_CALLBACK_FAILURE;
        }
      }
      return 0;
    } catch (const std::exception& error) {
      self->pending_transport_error_ =
          ExceptionStatus(error, "Receiving HTTP/2 data");
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    } catch (...) {
      self->pending_transport_error_ = absl::UnknownError(
          "Receiving HTTP/2 data raised a non-standard exception");
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
  }

  static int OnStreamCloseCallback(nghttp2_session*, std::int32_t stream_id,
                                   std::uint32_t error_code,
                                   void* user_data) noexcept {
    auto* self = static_cast<Http2Connection*>(user_data);
    if (self == nullptr)
      return 0;
    try {
      auto iterator = self->streams_.find(stream_id);
      if (iterator == self->streams_.end())
        return 0;
      Stream& stream = *iterator->second;
      if (stream.response != nullptr) {
        absl::Status status = Http2StreamError(error_code, "HTTP/2 response");
        if (status.ok() && !stream.remote_end) {
          status = absl::UnavailableError(
              "HTTP/2 response closed without END_STREAM");
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
    } catch (...) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
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
    if (status < 200)
      return;
    HttpResponseHead head{.status = status};
    for (const auto& [name, value] : stream->inbound_headers) {
      if (name.empty() || name.front() != ':') {
        head.headers.emplace_back(name, value);
      }
    }
    stream->response_headers_received = true;
    stream->response->SetHeaders(std::move(head));
  }

  void DispatchRequest(Stream* stream) {
    if (stream == nullptr || stream->request_dispatched)
      return;

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
    if (stream->request_too_large)
      return;
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
      auto writer = std::make_shared<MakeWriterEnabler>(
          weak_from_this(), stream->id, stream->writer);
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
    auto writer = std::make_shared<MakeWriterEnabler>(
        weak_from_this(), stream->id, stream->writer);
    Http2RequestHandler handler = handler_;
    a11::Schedule([handler = std::move(handler), request = std::move(request),
                   writer = std::move(writer)]() mutable {
      absl::Status status;
      try {
        status = handler(std::move(request), writer).Await().status();
      } catch (const std::exception& error) {
        status = ExceptionStatus(error, "HTTP/2 request handler");
      } catch (...) {
        status = absl::UnknownError(
            "HTTP/2 request handler raised a non-standard exception");
      }
      if (!status.ok()) {
        (void)writer->Abort(status);
      } else if (!writer->headers_sent()) {
        (void)writer->SendResponse(204);
      }
    });
  }

  void CloseOnLoop(absl::Status status) {
    if (closed_)
      return;
    closed_ = true;
    connected_.store(false);
    if (status.ok()) {
      status = absl::CancelledError("HTTP/2 connection closed");
    }
    PublishReady(status);
    for (auto& [stream_id, stream] : streams_) {
      (void)stream_id;
      if (stream->response != nullptr)
        stream->response->Finish(status);
      if (stream->request_body != nullptr)
        stream->request_body->Finish(status);
      if (stream->writer != nullptr)
        stream->writer->Finish(status);
    }
    streams_.clear();
    if (session_ != nullptr) {
      nghttp2_session_del(session_);
      session_ = nullptr;
    }
    if (tcp_ != nullptr && !tcp_->closing())
      tcp_->close();
    std::function<void(Http2Connection*)> on_closed = std::move(on_closed_);
    if (on_closed) {
      try {
        on_closed(this);
      } catch (const std::exception& error) {
        LOG(ERROR) << "HTTP/2 close callback raised: " << error.what();
      } catch (...) {
        LOG(ERROR) << "HTTP/2 close callback raised a non-standard exception";
      }
    }
  }

  const std::shared_ptr<uvw::tcp_handle> tcp_;
  const bool server_;
  const Http2RequestHandler handler_;
  const Http2Options options_;
  const SslContext ssl_context_;
  const std::string tls_server_name_;
  std::function<void(Http2Connection*)> on_closed_;
  SSL* ssl_ = nullptr;
  nghttp2_session* session_ = nullptr;
  absl::flat_hash_map<std::int32_t, std::unique_ptr<Stream>> streams_;
  const std::shared_ptr<a11::Promise<a11::Unit>> ready_promise_;
  const a11::Task ready_future_;
  std::atomic<bool> connected_ = false;
  bool closed_ = false;
  bool ready_published_ = false;
  bool tls_handshake_complete_ = false;
  bool http2_started_ = false;
  std::optional<absl::Status> pending_transport_error_;
};

a11::Future<HttpResponseHead> Http2DuplexStream::Headers() const {
  return response_->Headers();
}

a11::Future<std::optional<std::string>> Http2DuplexStream::Read() {
  return response_->Read();
}

absl::Status Http2DuplexStream::Write(std::string data) {
  std::shared_ptr<Http2Connection> connection = connection_.lock();
  if (connection == nullptr)
    return absl::UnavailableError("HTTP/2 connection is no longer available");
  return connection->WriteRequest(stream_id(), std::move(data));
}

absl::Status Http2DuplexStream::Finish() {
  std::shared_ptr<Http2Connection> connection = connection_.lock();
  if (connection == nullptr)
    return absl::UnavailableError("HTTP/2 connection is no longer available");
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
  std::shared_ptr<Http2Connection> connection = connection_.lock();
  if (connection == nullptr) {
    return absl::UnavailableError("HTTP/2 connection is no longer available");
  }
  return connection->SendHeaders(stream_id_, status, std::move(headers));
}

absl::Status Http2ResponseWriter::Write(std::string data) {
  std::shared_ptr<Http2Connection> connection = connection_.lock();
  if (connection == nullptr) {
    return absl::UnavailableError("HTTP/2 connection is no longer available");
  }
  return connection->Write(stream_id_, std::move(data));
}

absl::Status Http2ResponseWriter::Finish() {
  std::shared_ptr<Http2Connection> connection = connection_.lock();
  if (connection == nullptr)
    return absl::OkStatus();
  return connection->Finish(stream_id_);
}

absl::Status Http2ResponseWriter::SendResponse(int status, HttpHeaders headers,
                                               std::string body) {
  std::shared_ptr<Http2Connection> connection = connection_.lock();
  if (connection == nullptr) {
    return absl::UnavailableError("HTTP/2 connection is no longer available");
  }
  return connection->SendResponse(stream_id_, status, std::move(headers),
                                  std::move(body));
}

absl::Status Http2ResponseWriter::Abort(absl::Status status) {
  std::shared_ptr<Http2Connection> connection = connection_.lock();
  if (connection == nullptr)
    return absl::OkStatus();
  return connection->AbortResponse(stream_id_, std::move(status));
}

a11::Task Http2ResponseWriter::Done() const {
  return state_ != nullptr ? state_->done_future
                           : a11::FailedTask(absl::FailedPreconditionError(
                                 "HTTP/2 writer has no state"));
}

bool Http2ResponseWriter::headers_sent() const {
  std::shared_ptr<Http2Connection> connection = connection_.lock();
  if (connection == nullptr)
    return false;
  absl::StatusOr<bool> result = connection->ResponseHeadersSent(stream_id_);
  return result.ok() && *result;
}

bool Http2ResponseWriter::finished() const {
  std::shared_ptr<Http2Connection> connection = connection_.lock();
  if (connection == nullptr)
    return true;
  absl::StatusOr<bool> result = connection->ResponseFinished(stream_id_);
  return !result.ok() || *result;
}

struct Http2Server::State {
  State(std::string address, Http2RequestHandler request_handler,
        Http2Options server_options, SslContext context)
      : bind_address(std::move(address)),
        handler(std::move(request_handler)),
        options(server_options),
        tls_context(std::move(context)) {}

  mutable thread::Mutex mu;
  const std::string bind_address;
  const Http2RequestHandler handler;
  const Http2Options options;
  const SslContext tls_context;
  std::uint16_t port ABSL_GUARDED_BY(mu) = 0;
  bool running ABSL_GUARDED_BY(mu) = false;
  std::shared_ptr<uvw::tcp_handle> listener ABSL_GUARDED_BY(mu);
  std::vector<std::shared_ptr<Http2Connection>> connections ABSL_GUARDED_BY(mu);
};

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
                        CreateTlsContext(options.tls, true));
  auto state =
      std::make_shared<State>(std::move(bind_address), std::move(handler),
                              options, std::move(tls_context));
  return RunOnUv<std::shared_ptr<Http2Server>>(
      [state, port]() -> absl::StatusOr<std::shared_ptr<Http2Server>> {
        std::shared_ptr<uvw::tcp_handle> listener;
        try {
          listener = UvExecutor::Instance().loop()->resource<uvw::tcp_handle>();
        } catch (const std::exception& error) {
          return ExceptionStatus(error, "Creating HTTP/2 listener");
        } catch (...) {
          return absl::UnknownError(
              "Creating HTTP/2 listener raised a non-standard exception");
        }
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
          if (server == nullptr)
            return;
          std::shared_ptr<uvw::tcp_handle> client;
          try {
            client = listening.parent().resource<uvw::tcp_handle>();
          } catch (const std::exception& error) {
            LOG(ERROR) << "Could not create accepted HTTP/2 socket: "
                       << error.what();
            return;
          } catch (...) {
            LOG(ERROR) << "Could not create accepted HTTP/2 socket";
            return;
          }
          const int accepted = listening.accept(*client);
          if (accepted != 0) {
            LOG(ERROR) << "Could not accept HTTP/2 connection: "
                       << uv_strerror(accepted);
            client->close();
            return;
          }
          auto remove_connection = [weak](Http2Connection* closed_connection) {
            if (std::shared_ptr<State> active = weak.lock()) {
              thread::MutexLock lock(&active->mu);
              std::erase_if(active->connections,
                            [closed_connection](const auto& connection) {
                              return connection.get() == closed_connection;
                            });
            }
          };
          absl::StatusOr<std::shared_ptr<Http2Connection>> connection =
              Http2Connection::Create(client, true, server->handler,
                                      server->options, server->tls_context, {},
                                      std::move(remove_connection));
          if (!connection.ok()) {
            LOG(ERROR) << "Could not initialize HTTP/2 connection: "
                       << connection.status();
            client->close();
            return;
          }
          thread::MutexLock lock(&server->mu);
          if (server->running) {
            server->connections.push_back(std::move(*connection));
          } else {
            (void)(*connection)->Close();
          }
        });
        int result = listener->bind(state->bind_address, port);
        if (result != 0)
          return UvError(result, "Binding HTTP/2 listener");
        result = listener->listen();
        if (result != 0)
          return UvError(result, "Listening for HTTP/2");
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
    std::vector<std::shared_ptr<Http2Connection>> connections;
    std::shared_ptr<uvw::tcp_handle> listener;
    {
      thread::MutexLock lock(&state->mu);
      if (!state->running && state->listener == nullptr)
        return absl::OkStatus();
      state->running = false;
      listener = std::move(state->listener);
      connections.swap(state->connections);
    }
    if (listener != nullptr && !listener->closing())
      listener->close();
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
  absl::StatusOr<SslContext> tls_context = CreateTlsContext(options.tls, false);
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
        try {
          resolver =
              UvExecutor::Instance().loop()->resource<uvw::get_addr_info_req>();
        } catch (const std::exception& error) {
          promise
              ->SetStatus(ExceptionStatus(error, "Creating HTTP/2 DNS request"))
              .IgnoreError();
          return;
        } catch (...) {
          promise
              ->SetStatus(absl::UnknownError(
                  "Creating HTTP/2 DNS request raised an exception"))
              .IgnoreError();
          return;
        }
        resolver->on<uvw::error_event>(
            [promise](const uvw::error_event& event, uvw::get_addr_info_req&) {
              promise
                  ->SetStatus(absl::UnavailableError(
                      absl::StrCat("HTTP/2 DNS lookup failed: ", event.what())))
                  .IgnoreError();
            });
        resolver->on<uvw::addr_info_event>(
            [promise, host, port, options, tls_context](
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
              try {
                tcp =
                    UvExecutor::Instance().loop()->resource<uvw::tcp_handle>();
              } catch (const std::exception& error) {
                promise
                    ->SetStatus(
                        ExceptionStatus(error, "Creating HTTP/2 client socket"))
                    .IgnoreError();
                return;
              } catch (...) {
                promise
                    ->SetStatus(absl::UnknownError(
                        "Creating HTTP/2 client socket raised an exception"))
                    .IgnoreError();
                return;
              }
              tcp->on<uvw::error_event>(
                  [promise](const uvw::error_event& error, uvw::tcp_handle&) {
                    promise
                        ->SetStatus(absl::UnavailableError(absl::StrCat(
                            "HTTP/2 connection failed: ", error.what())))
                        .IgnoreError();
                  });
              tcp->on<uvw::connect_event>([promise, host, port, options,
                                           tls_context](
                                              const uvw::connect_event&,
                                              uvw::tcp_handle& handle) {
                const std::shared_ptr<uvw::tcp_handle> tcp_handle =
                    std::static_pointer_cast<uvw::tcp_handle>(
                        handle.shared_from_this());
                absl::StatusOr<std::shared_ptr<Http2Connection>> connection =
                    Http2Connection::Create(tcp_handle, false, {}, options,
                                            tls_context, host);
                if (!connection.ok()) {
                  promise->SetStatus(connection.status()).IgnoreError();
                  handle.close();
                  return;
                }
                std::shared_ptr<Http2Connection> ready_connection =
                    std::move(*connection);
                ready_connection->Ready().OnReady(
                    [promise, host, port, options,
                     ready_connection](const absl::StatusOr<a11::Unit>& ready) {
                      if (!ready.ok()) {
                        promise->SetStatus(ready.status()).IgnoreError();
                        ready_connection->Close(ready.status()).IgnoreError();
                        return;
                      }
                      struct MakeSharedEnabler final : Http2Client {
                        MakeSharedEnabler(
                            std::string host, std::uint16_t port,
                            Http2Options options,
                            std::shared_ptr<Http2Connection> connection)
                            : Http2Client(std::move(host), port,
                                          std::move(options),
                                          std::move(connection)) {}
                      };
                      std::shared_ptr<Http2Client> client =
                          std::make_shared<MakeSharedEnabler>(
                              host, port, options, ready_connection);
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
       std::shared_ptr<Http2Connection> client_connection)
      : host(std::move(client_host)),
        port(client_port),
        options(client_options),
        connection(std::move(client_connection)) {}

  const std::string host;
  const std::uint16_t port;
  const Http2Options options;
  mutable thread::Mutex mu;
  std::shared_ptr<Http2Connection> connection ABSL_GUARDED_BY(mu);
};

Http2Client::Http2Client(std::string host, std::uint16_t port,
                         Http2Options options,
                         std::shared_ptr<Http2Connection> connection) {
  static_assert(sizeof(Impl) <= kImplSize);
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
  std::shared_ptr<Http2Connection> connection;
  {
    thread::MutexLock lock(&state()->mu);
    connection = state()->connection;
  }
  if (connection == nullptr)
    return absl::UnavailableError("HTTP/2 client is closed");
  if (scheme.empty())
    scheme = connection->secure() ? "https" : "http";
  std::string authority = state()->host;
  if (authority.find(':') != std::string::npos &&
      (authority.empty() || authority.front() != '[')) {
    authority = absl::StrCat("[", authority, "]");
  }
  authority = absl::StrCat(authority, ":", state()->port);
  return connection->SubmitRequest(std::move(method), std::move(scheme),
                                   std::move(authority), std::move(path),
                                   std::move(headers), std::move(body));
}

absl::StatusOr<std::shared_ptr<Http2DuplexStream>> Http2Client::ExtendedConnect(
    std::string protocol, std::string path, HttpHeaders headers,
    std::string scheme) {
  std::shared_ptr<Http2Connection> connection;
  {
    thread::MutexLock lock(&state()->mu);
    connection = state()->connection;
  }
  if (connection == nullptr)
    return absl::UnavailableError("HTTP/2 client is closed");
  if (scheme.empty())
    scheme = connection->secure() ? "https" : "http";
  std::string authority = state()->host;
  if (authority.find(':') != std::string::npos &&
      (authority.empty() || authority.front() != '[')) {
    authority = absl::StrCat("[", authority, "]");
  }
  authority = absl::StrCat(authority, ":", state()->port);
  return connection->SubmitDuplex(std::move(protocol), std::move(scheme),
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
        absl::StatusOr<HttpResponseHead> head =
            stream->Headers().Await(options.deadline);
        if (!head.ok())
          return head.status();
        HttpResponse response{.head = std::move(*head)};
        while (true) {
          absl::StatusOr<std::optional<std::string>> chunk =
              stream->Read().Await(options.deadline);
          if (!chunk.ok())
            return chunk.status();
          if (!chunk->has_value())
            break;
          if (response.body.size() + (*chunk)->size() >
              options.max_response_body_size) {
            const absl::Status status = absl::OutOfRangeError(
                "HTTP/2 response exceeds max_response_body_size");
            (void)stream->Cancel(status);
            return status;
          }
          response.body.append(**chunk);
        }
        absl::Status complete = stream->Done().Await(options.deadline).status();
        if (!complete.ok())
          return complete;
        return response;
      });
}

absl::Status Http2Client::Close() {
  std::shared_ptr<Http2Connection> connection;
  {
    thread::MutexLock lock(&state()->mu);
    connection = std::move(state()->connection);
  }
  if (connection == nullptr)
    return absl::OkStatus();
  return connection->Close();
}

std::string Http2Client::host() const {
  return state()->host;
}

std::uint16_t Http2Client::port() const {
  return state()->port;
}

bool Http2Client::connected() const {
  std::shared_ptr<Http2Connection> connection;
  {
    thread::MutexLock lock(&state()->mu);
    connection = state()->connection;
  }
  return connection != nullptr && connection->connected();
}

bool Http2Client::secure() const {
  return state()->options.tls.enabled;
}

void* absl_nullable Http2Client::GetImpl() const {
  std::shared_ptr<Http2Connection> connection;
  {
    thread::MutexLock lock(&state()->mu);
    connection = state()->connection;
  }
  return connection != nullptr ? connection->GetImpl() : nullptr;
}

}  // namespace a11::net
