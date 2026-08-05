// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Shared TCP/TLS/libuv transport substrate for the HTTP connections.
 *
 * HttpTransport owns the socket, the optional OpenSSL BIO pump, and the libuv
 * lifecycle that both the HTTP/2 (nghttp2) and HTTP/1.1 connections sit on top
 * of. The protocol-specific connections derive from it and override three
 * seams: OnInboundPlaintext (bytes decrypted/received), SendProtocolPreamble
 * (emit any startup bytes once the transport is ready), and OnClose (tear down
 * protocol state). Everything below those seams -- the single libuv loop, the
 * TLS handshake, encrypt/decrypt, backpressure to the socket -- is shared.
 *
 * This header also hosts the process-global libuv executor and the RunOnUv
 * marshalling helpers, which both connection flavours and the client/server
 * factories rely on.
 */

#ifndef A11_NET_INTERNAL_HTTP_TRANSPORT_H_
#define A11_NET_INTERNAL_HTTP_TRANSPORT_H_

#include <array>
#include <atomic>
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

#include <absl/base/no_destructor.h>
#include <absl/base/nullability.h>
#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>
#include <arpa/inet.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "a11/concurrency/future.h"
#include "a11/net/http2.h"
#include "thread/boost_primitives.h"

namespace a11::net::internal {

inline absl::Status UvError(int code, std::string_view operation) {
  return absl::UnavailableError(
      absl::StrCat(operation, " failed: ", uv_strerror(code)));
}

inline absl::Status ExceptionStatus(const std::exception& error,
                                    std::string_view operation) {
  return absl::UnknownError(
      absl::StrCat(operation, " raised an exception: ", error.what()));
}

inline std::string OpenSslErrorMessage(std::string_view operation) {
  const unsigned long code = ERR_get_error();
  if (code == 0) {
    return std::string(operation);
  }
  char message[256];
  ERR_error_string_n(code, message, sizeof(message));
  return absl::StrCat(operation, ": ", message);
}

inline absl::Status TlsError(std::string_view operation) {
  return absl::UnavailableError(OpenSslErrorMessage(operation));
}

using SslContext = std::shared_ptr<SSL_CTX>;

// ALPN protocol identifiers in OpenSSL wire form (length-prefixed).
inline constexpr unsigned char kH2Alpn[] = {2, 'h', '2'};
inline constexpr unsigned char kHttp1Alpn[] = {8, 'h', 't', 't', 'p',
                                               '/', '1', '.', '1'};

/**
 * @brief The set of HTTP protocols a transport is willing to speak.
 *
 * Threaded from Http2Options into the TLS context (which ALPN identifiers to
 * advertise/select) and the cleartext path (which prior-knowledge bytes to
 * accept). At least one flavour is always enabled.
 */
struct ProtocolPolicy {
  bool enable_h2 = true;     ///< HTTP/2 over TLS (ALPN "h2").
  bool enable_h2c = true;    ///< HTTP/2 cleartext, prior-knowledge preface.
  bool enable_http1 = true;  ///< HTTP/1.1 (ALPN "http/1.1" and/or cleartext).
  /// Client-side ALPN/attempt ordering and downgrade behaviour.
  Http2Options::ProtocolPreference client_preference =
      Http2Options::ProtocolPreference::kAuto;

  static ProtocolPolicy FromOptions(const Http2Options& options) {
    return ProtocolPolicy{.enable_h2 = options.enable_h2,
                          .enable_h2c = options.enable_h2c,
                          .enable_http1 = options.enable_http1,
                          .client_preference = options.client_preference};
  }
};

// Whether the client's length-prefixed ALPN list offers @p protocol.
inline bool AlpnListOffers(const unsigned char* input, unsigned int length,
                           std::string_view protocol) {
  unsigned int offset = 0;
  while (offset < length) {
    const unsigned int entry_length = input[offset];
    if (entry_length == 0 || offset + 1 + entry_length > length) {
      return false;
    }
    const std::string_view entry(
        reinterpret_cast<const char*>(input + offset + 1), entry_length);
    if (entry == protocol) {
      return true;
    }
    offset += 1 + entry_length;
  }
  return false;
}

// Server ALPN selection callback. Prefers h2 over http/1.1 among the protocols
// both offered and enabled by the policy. The returned pointer references the
// static ALPN constants so it stays valid after the callback returns.
inline int SelectAlpn(SSL*, const unsigned char** output,
                      unsigned char* output_length, const unsigned char* input,
                      unsigned int input_length, void* argument) noexcept {
  const auto* policy = static_cast<const ProtocolPolicy*>(argument);
  const bool allow_h2 = policy == nullptr || policy->enable_h2;
  const bool allow_http1 = policy != nullptr && policy->enable_http1;
  if (allow_h2 && AlpnListOffers(input, input_length, "h2")) {
    *output = &kH2Alpn[1];  // "h2" without the length prefix.
    *output_length = 2;
    return SSL_TLSEXT_ERR_OK;
  }
  if (allow_http1 && AlpnListOffers(input, input_length, "http/1.1")) {
    *output = &kHttp1Alpn[1];  // "http/1.1" without the length prefix.
    *output_length = 8;
    return SSL_TLSEXT_ERR_OK;
  }
  return SSL_TLSEXT_ERR_ALERT_FATAL;
}

inline absl::Status LoadCertificateAndKey(SSL_CTX* context,
                                          const Http2TlsOptions& options) {
  if (options.certificate_pem_file.empty()) {
    return absl::OkStatus();
  }
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

/**
 * @brief Builds an OpenSSL context advertising the policy's ALPN protocols.
 *
 * @param options TLS certificate/verification policy.
 * @param server Whether this is a server (accept) or client (connect) context.
 * @param policy Which HTTP protocols to advertise/select over ALPN. The server
 *     ALPN callback keeps a copy alive via SSL_CTX app data.
 */
inline absl::StatusOr<SslContext> CreateTlsContext(
    const Http2TlsOptions& options, bool server,
    ProtocolPolicy policy = ProtocolPolicy{}) {
  ABSL_RETURN_IF_ERROR(options.Validate());
  if (!options.enabled) {
    return SslContext{};
  }
  if (server && options.certificate_pem_file.empty()) {
    return absl::InvalidArgumentError(
        "A TLS HTTP server requires a certificate and private key");
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
    // The ALPN callback reads the policy through its argument pointer. It must
    // outlive every connection using the context; contexts are process-scoped
    // per server, so a single owned copy is intentionally never freed.
    auto* held_policy = new ProtocolPolicy(policy);
    SSL_CTX_set_alpn_select_cb(context.get(), &SelectAlpn, held_policy);
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

inline bool IsIpAddress(std::string_view host) {
  in_addr ipv4{};
  in6_addr ipv6{};
  const std::string value(host);
  return inet_pton(AF_INET, value.c_str(), &ipv4) == 1 ||
         inet_pton(AF_INET6, value.c_str(), &ipv6) == 1;
}

// One libuv loop is shared by native HTTP clients and servers. All uvw and
// protocol mutation is serialized on this thread; fiber callbacks communicate
// through A11 Futures and never block the loop.
class UvExecutor {
 public:
  static UvExecutor& Instance() {
    // Process-global I/O schedulers intentionally live until process exit.
    static absl::NoDestructor<UvExecutor> executor;
    return *executor;
  }

  absl::Status Post(std::function<void()> work) {
    if (!work) {
      return absl::InvalidArgumentError("uv work must be callable");
    }
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
    if (result != 0) {
      return UvError(result, "uv_async_send");
    }
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
  ABSL_RETURN_IF_ERROR(UvExecutor::Instance().Post(
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
      }));
  return future.Await();
}

inline absl::Status RunStatusOnUv(std::function<absl::Status()> operation) {
  absl::StatusOr<a11::Unit> result = RunOnUv<a11::Unit>(
      [operation =
           std::move(operation)]() mutable -> absl::StatusOr<a11::Unit> {
        ABSL_RETURN_IF_ERROR(operation());
        return a11::Unit{};
      });
  return result.status();
}

/**
 * @brief TCP + optional TLS transport shared by the HTTP connections.
 *
 * Owns the libuv socket, the OpenSSL handshake/encrypt/decrypt pump, and the
 * ready/closed lifecycle. Derived protocol connections implement the three
 * pure-virtual seams. All non-const methods run on the libuv loop thread.
 */
class HttpTransport : public std::enable_shared_from_this<HttpTransport> {
 public:
  virtual ~HttpTransport() {
    if (ssl_ != nullptr) {
      SSL_free(ssl_);
    }
  }

  [[nodiscard]] a11::Task Ready() const { return ready_future_; }
  [[nodiscard]] bool connected() const { return connected_.load(); }
  [[nodiscard]] bool secure() const { return ssl_context_ != nullptr; }
  [[nodiscard]] void* absl_nonnull GetImpl() const { return tcp_.get(); }

  absl::Status Close(
      absl::Status status = absl::CancelledError("HTTP connection closed")) {
    std::shared_ptr<HttpTransport> self = shared_from_this();
    return RunStatusOnUv(
        [self = std::move(self), status = std::move(status)]() {
          self->CloseOnLoop(status);
          return absl::OkStatus();
        });
  }

 protected:
  HttpTransport(std::shared_ptr<uvw::tcp_handle> tcp, bool server,
                Http2Options options, SslContext tls_context,
                std::string tls_server_name,
                std::function<void(HttpTransport*)> on_closed)
      : tcp_(std::move(tcp)),
        server_(server),
        options_(std::move(options)),
        ssl_context_(std::move(tls_context)),
        tls_server_name_(std::move(tls_server_name)),
        on_closed_(std::move(on_closed)),
        ready_promise_(std::make_shared<a11::Promise<a11::Unit>>()),
        ready_future_(ready_promise_->future()) {}

  // --- Seams implemented by the protocol connection. ---

  /// Feeds received (already-decrypted) plaintext into the protocol codec.
  virtual absl::Status OnInboundPlaintext(const char* data, size_t size) = 0;
  /// Emits any startup bytes once the transport is ready to send (e.g. the
  /// HTTP/2 SETTINGS preface). Returning an error fails the connection.
  virtual absl::Status SendProtocolPreamble() = 0;
  /// Tears down protocol-level state during close (finish streams, etc.).
  virtual void OnClose(const absl::Status& status) = 0;
  /// The ALPN protocols this client offers, in wire form (may be empty).
  virtual std::vector<unsigned char> ClientAlpnWire() const {
    return std::vector<unsigned char>(std::begin(kH2Alpn), std::end(kH2Alpn));
  }
  /// Validates/records the ALPN protocol the TLS peer negotiated.
  virtual absl::Status OnAlpnNegotiated(std::string_view protocol) {
    if (protocol != "h2") {
      return absl::FailedPreconditionError(
          "TLS peer did not negotiate the h2 ALPN protocol");
    }
    return absl::OkStatus();
  }

  // --- Shared transport plumbing. ---

  // Wires the libuv socket callbacks and begins the TLS handshake (or the
  // cleartext preamble). Call once from the derived Initialize(), after the
  // protocol codec has been created. `prebuffered` carries any bytes already
  // read from the socket during cleartext protocol detection; they are replayed
  // into the codec after the cleartext start (never set for a TLS transport).
  absl::Status InitializeTransport(std::string prebuffered = {}) {
    std::weak_ptr<HttpTransport> weak = shared_from_this();
    tcp_->on<uvw::data_event>(
        [weak](const uvw::data_event& event, uvw::tcp_handle&) {
          if (std::shared_ptr<HttpTransport> self = weak.lock()) {
            self->OnTcpData(event.data.get(), event.length);
          }
        });
    tcp_->on<uvw::error_event>(
        [weak](const uvw::error_event& event, uvw::tcp_handle&) {
          if (std::shared_ptr<HttpTransport> self = weak.lock()) {
            self->CloseOnLoop(absl::UnavailableError(
                absl::StrCat("HTTP TCP error: ", event.what())));
          }
        });
    tcp_->on<uvw::end_event>([weak](const uvw::end_event&, uvw::tcp_handle&) {
      if (std::shared_ptr<HttpTransport> self = weak.lock()) {
        self->CloseOnLoop(
            absl::UnavailableError("HTTP peer closed the TCP connection"));
      }
    });
    tcp_->on<uvw::close_event>(
        [weak](const uvw::close_event&, uvw::tcp_handle&) {
          if (std::shared_ptr<HttpTransport> self = weak.lock()) {
            self->connected_.store(false);
          }
        });
    tcp_->no_delay(true);
    tcp_->read();
    if (ssl_context_ != nullptr) {
      return InitializeTls();
    }
    ABSL_RETURN_IF_ERROR(StartProtocol());
    if (!prebuffered.empty() && !closed_) {
      // Replay bytes consumed during cleartext protocol detection, in order,
      // before any freshly-read data_event can fire on the loop thread.
      OnTcpData(prebuffered.data(), prebuffered.size());
    }
    return absl::OkStatus();
  }

  absl::Status StartProtocol() {
    if (protocol_started_) {
      return absl::OkStatus();
    }
    absl::Status status = SendProtocolPreamble();
    if (!status.ok()) {
      PublishReady(status);
      return status;
    }
    protocol_started_ = true;
    connected_.store(true);
    PublishReady(absl::OkStatus());
    return absl::OkStatus();
  }

  void OnTcpData(const char* data, size_t size) {
    if (closed_) {
      return;
    }
    absl::Status status = ssl_ == nullptr ? OnInboundPlaintext(data, size)
                                          : ReceiveTlsData(data, size);
    if (!status.ok()) {
      CloseOnLoop(status);
    }
  }

  absl::Status WriteApplicationData(const std::uint8_t* data, size_t length) {
    if (ssl_ == nullptr) {
      return WriteTcp(reinterpret_cast<const char*>(data), length);
    }
    if (!tls_handshake_complete_) {
      return absl::FailedPreconditionError(
          "Cannot write HTTP data before the TLS handshake");
    }
    size_t offset = 0;
    while (offset < length) {
      size_t written = 0;
      ERR_clear_error();
      const int result =
          SSL_write_ex(ssl_, data + offset, length - offset, &written);
      if (result == 1) {
        if (written == 0) {
          return absl::InternalError("TLS accepted no HTTP output data");
        }
        offset += written;
        ABSL_RETURN_IF_ERROR(FlushTlsOutput());
        continue;
      }
      const int error = SSL_get_error(ssl_, result);
      if (error == SSL_ERROR_WANT_WRITE) {
        ABSL_RETURN_IF_ERROR(FlushTlsOutput());
        continue;
      }
      return TlsError("Encrypting HTTP TLS data");
    }
    return absl::OkStatus();
  }

  void CloseOnLoop(absl::Status status) {
    if (closed_) {
      return;
    }
    closed_ = true;
    connected_.store(false);
    if (status.ok()) {
      status = absl::CancelledError("HTTP connection closed");
    }
    PublishReady(status);
    OnClose(status);
    if (tcp_ != nullptr && !tcp_->closing()) {
      tcp_->close();
    }
    std::function<void(HttpTransport*)> on_closed = std::move(on_closed_);
    if (on_closed) {
      try {
        on_closed(this);
      } catch (const std::exception& error) {
        LOG(ERROR) << "HTTP close callback raised: " << error.what();
      } catch (...) {
        LOG(ERROR) << "HTTP close callback raised a non-standard exception";
      }
    }
  }

  void PublishReady(const absl::Status& status) {
    if (ready_published_) {
      return;
    }
    ready_published_ = true;
    if (status.ok()) {
      (void)ready_promise_->SetValue(a11::Unit{});
    } else {
      (void)ready_promise_->SetStatus(status);
    }
  }

  // Accessors for derived protocol connections.
  [[nodiscard]] bool server() const { return server_; }
  [[nodiscard]] const Http2Options& options() const { return options_; }
  [[nodiscard]] bool closed() const { return closed_; }

  const std::shared_ptr<uvw::tcp_handle> tcp_;
  const bool server_;
  const Http2Options options_;
  const SslContext ssl_context_;
  const std::string tls_server_name_;
  std::function<void(HttpTransport*)> on_closed_;
  SSL* ssl_ = nullptr;
  const std::shared_ptr<a11::Promise<a11::Unit>> ready_promise_;
  const a11::Task ready_future_;
  std::atomic<bool> connected_ = false;
  bool closed_ = false;
  bool ready_published_ = false;
  bool tls_handshake_complete_ = false;
  bool protocol_started_ = false;

 private:
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
      return absl::InternalError("Creating TLS memory BIOs");
    }
    BIO_set_mem_eof_return(encrypted_input, -1);
    BIO_set_mem_eof_return(encrypted_output, -1);
    // SSL owns both BIOs after this call.
    SSL_set_bio(ssl_, encrypted_input, encrypted_output);

    if (server_) {
      SSL_set_accept_state(ssl_);
    } else {
      SSL_set_connect_state(ssl_);
      const std::vector<unsigned char> alpn = ClientAlpnWire();
      if (!alpn.empty() &&
          SSL_set_alpn_protos(ssl_, alpn.data(),
                              static_cast<unsigned int>(alpn.size())) != 0) {
        return absl::InternalError(OpenSslErrorMessage("Configuring HTTP ALPN"));
      }
      if (tls_server_name_.empty()) {
        return absl::InvalidArgumentError(
            "A TLS HTTP client requires a server name");
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
        if (written != 0) {
          return UvError(written, "Writing HTTP TCP data");
        }
        offset += chunk_size;
      }
      return absl::OkStatus();
    } catch (const std::exception& error) {
      return ExceptionStatus(error, "Writing HTTP TCP data");
    } catch (...) {
      return absl::UnknownError(
          "Writing HTTP TCP data raised a non-standard exception");
    }
  }

  absl::Status FlushTlsOutput() {
    if (ssl_ == nullptr) {
      return absl::OkStatus();
    }
    BIO* output = SSL_get_wbio(ssl_);
    if (output == nullptr) {
      return absl::InternalError("The TLS connection has no output BIO");
    }
    std::array<char, 16 * 1024> buffer{};
    while (BIO_ctrl_pending(output) > 0) {
      const int length = BIO_read(output, buffer.data(), buffer.size());
      if (length <= 0) {
        return TlsError("Reading encrypted TLS output");
      }
      ABSL_RETURN_IF_ERROR(WriteTcp(buffer.data(), static_cast<size_t>(length)));
    }
    return absl::OkStatus();
  }

  absl::Status AdvanceTlsHandshake() {
    if (tls_handshake_complete_) {
      return absl::OkStatus();
    }
    ERR_clear_error();
    const int result = SSL_do_handshake(ssl_);
    const int error =
        result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl_, result);
    ABSL_RETURN_IF_ERROR(FlushTlsOutput());
    if (result == 1) {
      const unsigned char* protocol = nullptr;
      unsigned int protocol_length = 0;
      SSL_get0_alpn_selected(ssl_, &protocol, &protocol_length);
      const std::string_view negotiated(
          reinterpret_cast<const char*>(protocol), protocol_length);
      ABSL_RETURN_IF_ERROR(OnAlpnNegotiated(negotiated));
      if (!server_ && options_.tls.verify_peer) {
        const long verification = SSL_get_verify_result(ssl_);
        if (verification != X509_V_OK) {
          return absl::UnauthenticatedError(
              absl::StrCat("TLS certificate verification failed: ",
                           X509_verify_cert_error_string(verification)));
        }
      }
      tls_handshake_complete_ = true;
      return StartProtocol();
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
      if (written <= 0) {
        return TlsError("Buffering encrypted TLS input");
      }
      offset += static_cast<size_t>(written);
    }

    absl::Status status = AdvanceTlsHandshake();
    if (!status.ok() || !tls_handshake_complete_) {
      return status;
    }
    std::array<char, 16 * 1024> plaintext{};
    while (true) {
      size_t length = 0;
      ERR_clear_error();
      const int read =
          SSL_read_ex(ssl_, plaintext.data(), plaintext.size(), &length);
      if (read == 1) {
        if (length == 0) {
          continue;
        }
        ABSL_RETURN_IF_ERROR(OnInboundPlaintext(plaintext.data(), length));
        continue;
      }
      const int error = SSL_get_error(ssl_, read);
      if (error == SSL_ERROR_WANT_READ) {
        break;
      }
      if (error == SSL_ERROR_WANT_WRITE) {
        ABSL_RETURN_IF_ERROR(FlushTlsOutput());
        continue;
      }
      if (error == SSL_ERROR_ZERO_RETURN) {
        return absl::UnavailableError("TLS peer closed the connection");
      }
      return TlsError("Decrypting HTTP TLS data");
    }
    return FlushTlsOutput();
  }
};

}  // namespace a11::net::internal

#endif  // A11_NET_INTERNAL_HTTP_TRANSPORT_H_
