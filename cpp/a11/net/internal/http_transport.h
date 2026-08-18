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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <uvw.hpp>
#include <vector>

#include <absl/base/no_destructor.h>
#include <absl/base/nullability.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
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

/**
 * The socket receive buffer requested for every HTTP connection.
 *
 * Sized as a bandwidth-delay product for a fast path -- a few gigabits at tens
 * of milliseconds -- because the kernel default is small enough to cap a single
 * connection well below the link. It doubles as the slack that absorbs data
 * already in flight when a reader pauses.
 */
constexpr std::size_t kSocketReceiveBufferSize = 4 * 1024 * 1024;

/**
 * How much plaintext to pull out of OpenSSL per SSL_read_ex.
 *
 * Reused across reads rather than allocated per call, and deliberately *not*
 * zero-initialised: a `std::array<char, N> buf{}` costs a memset of the whole
 * buffer on every TCP read for bytes that are about to be overwritten. Held on
 * the transport rather than on the stack because the libuv loop runs on a
 * std::thread, whose default stack is far smaller than the main thread's.
 */
constexpr std::size_t kTlsPlaintextBufferSize = 256 * 1024;

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
/**
 * @brief A CA bundle to trust when the caller named none, or "" if none is found.
 *
 * A11 links OpenSSL statically, so its compiled-in trust directory is the one
 * inside the deps prefix used to build it -- a path that does not exist on the
 * machine running the wheel. `SSL_CTX_set_default_verify_paths` therefore
 * succeeds while loading nothing, and the first `https://` or `wss://` request
 * fails with "unable to get local issuer certificate". Probing the platform's
 * usual bundle is what makes a client work out of the box.
 *
 * `SSL_CERT_FILE` comes first because it is OpenSSL's own override and the way a
 * container or a corporate proxy points a process at its roots.
 */
inline std::string DiscoverCaBundle() {
  if (const char* configured = std::getenv("SSL_CERT_FILE");
      configured != nullptr && *configured != '\0') {
    return configured;
  }
  static constexpr std::array<const char*, 6> kCandidates = {
      "/etc/ssl/cert.pem",                        // macOS, BSD
      "/etc/ssl/certs/ca-certificates.crt",       // Debian, Ubuntu, Alpine
      "/etc/pki/tls/certs/ca-bundle.crt",         // Fedora, RHEL
      "/etc/ssl/ca-bundle.pem",                   // openSUSE
      "/opt/homebrew/etc/openssl@3/cert.pem",     // Homebrew, Apple silicon
      "/usr/local/etc/openssl@3/cert.pem",        // Homebrew, Intel
  };
  for (const char* candidate : kCandidates) {
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error)) {
      return candidate;
    }
  }
  return "";
}

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
    int trusted = 0;
    if (!options.ca_certificate_pem_file.empty()) {
      trusted = SSL_CTX_load_verify_locations(
          context.get(), options.ca_certificate_pem_file.c_str(), nullptr);
    } else {
      // Both, and in this order. The default paths cover a system OpenSSL and
      // honour SSL_CERT_DIR; the discovered bundle covers the static build,
      // whose compiled-in path does not exist on the running machine. Loading
      // roots is additive, so trying both only widens what is trusted.
      trusted = SSL_CTX_set_default_verify_paths(context.get());
      const std::string bundle = DiscoverCaBundle();
      if (!bundle.empty()) {
        ERR_clear_error();
        if (SSL_CTX_load_verify_locations(context.get(), bundle.c_str(),
                                         nullptr) == 1) {
          trusted = 1;
        }
      }
    }
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

  /**
   * @brief Queue work for the loop thread.
   *
   * @param work  What to run on the loop.
   * @param order_key
   *   What this work must stay ordered against. Work sharing a key runs in the
   *   order it was posted; work with different keys may be interleaved by
   *   Drain(). Pass the *connection* for anything touching one socket; leave it
   *   null for work with no per-connection ordering requirement (accepting a
   *   connection, resolving a client address, stopping a server).
   *
   * **Ordering within a key is a protocol requirement, not a nicety.** A later
   * header write or Finish has to land behind the data writes already posted on
   * the same connection, or a framed protocol is corrupted -- and both the posted
   * write path (PostWrite) and the awaited path (RunOnUv) reach the same socket,
   * so both must carry the same key. Getting that wrong is not a fairness bug, it
   * is a wire bug: it fails as truncated or interleaved frames.
   */
  absl::Status Post(std::function<void()> work,
                    const void* order_key = nullptr) {
    if (!work) {
      return absl::InvalidArgumentError("uv work must be callable");
    }
    {
      thread::MutexLock lock(&mu_);
      if (!running_) {
        return absl::FailedPreconditionError("The A11 libuv loop is stopped");
      }
      work_.push_back(Item{.key = order_key, .work = std::move(work)});
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
    {
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
    }
    thread::MutexLock lock(&mu_);
    while (!loop_thread_id_.has_value()) {
      cv_.Wait(&mu_);
    }
  }

  /**
   * @brief Run everything queued, round-robin across order keys.
   *
   * Strictly FIFO draining is what let a large write delay a small one: both are
   * one queue entry, but an entry's *cost* is its transfer, so sixteen 64 KiB
   * writes queued by one connection made a 64-byte write on another wait behind
   * all sixteen. That is the mechanism behind the 5.5-7.1x small-request
   * starvation in `bench/FINDINGS.md` item 0b, which reproduced at 9-18% host
   * utilisation and so was never contention for the CPU.
   *
   * One item per key per round, in first-arrival order of the keys, so a
   * connection with a backlog yields to every other connection between each of
   * its own writes. Within a key the order is exactly the order posted, which is
   * what keeps framing intact.
   *
   * Grouping costs one pass over the batch and a small map, once per `uv_async`
   * wake-up rather than per item, and both single-item and single-key batches --
   * the overwhelmingly common cases -- skip it entirely.
   */
  void Drain() {
    std::deque<Item> batch;
    {
      thread::MutexLock lock(&mu_);
      batch.swap(work_);
    }
    if (DrainStatsEnabled()) {
      RecordDrainBatch(batch);
      // Timed per item, so "the queue is empty" can be told apart from "one item
      // holds the loop for a long time". Both readings of FINDINGS.md item 0b's
      // candidate 3 needed answering, and this is the half that answers the
      // second: measured at 2.6% loop occupancy, so there is nothing to free.
      for (Item& item : batch) {
        const absl::Time started = absl::Now();
        item.work();
        RecordItemDuration(absl::ToInt64Nanoseconds(absl::Now() - started));
      }
      return;
    }
    if (batch.size() <= 1 || !FairDraining()) {
      for (Item& item : batch) {
        item.work();
      }
      return;
    }

    std::vector<const void*> keys;
    absl::flat_hash_map<const void*, std::deque<std::function<void()>>> lanes;
    for (Item& item : batch) {
      auto [lane, fresh] = lanes.try_emplace(item.key);
      if (fresh) {
        keys.push_back(item.key);
      }
      lane->second.push_back(std::move(item.work));
    }
    if (keys.size() == 1) {
      for (std::function<void()>& work : lanes.begin()->second) {
        work();
      }
      return;
    }
    size_t remaining = batch.size();
    while (remaining > 0) {
      for (const void* key : keys) {
        std::deque<std::function<void()>>& lane = lanes.at(key);
        if (lane.empty()) {
          continue;
        }
        std::function<void()> work = std::move(lane.front());
        lane.pop_front();
        --remaining;
        work();
      }
    }
  }

  struct Item {
    /// What this work must stay ordered against; null means "only other nulls".
    const void* key = nullptr;
    std::function<void()> work;
  };

  /// A11_UV_DRAIN_STATS=1 reports the batch-size and key-count distribution at
  /// exit.
  ///
  /// This is what says whether fair draining can matter at all: if the loop is
  /// woken promptly enough that a batch is almost always one item, there is
  /// nothing to reorder and the queue is not where a delay comes from. Answering
  /// that is the difference between "fairness did not help" and "fairness had
  /// nothing to work with".
  static bool DrainStatsEnabled() {
    static const bool on = [] {
      const char* setting = std::getenv("A11_UV_DRAIN_STATS");
      return setting != nullptr && std::atoi(setting) != 0;
    }();
    return on;
  }

  /// How long one queued item held the loop thread.
  ///
  /// The loop is single-threaded, so this *is* the delay every other connection
  /// sees: an item taking 500us is 500us in which no other socket can be served.
  /// Bucketed rather than averaged, because the question is whether a *tail*
  /// exists, not what the mean is.
  static void RecordItemDuration(std::int64_t nanos) {
    struct Buckets {
      std::atomic<std::uint64_t> under_10us{0};
      std::atomic<std::uint64_t> under_100us{0};
      std::atomic<std::uint64_t> under_1ms{0};
      std::atomic<std::uint64_t> over_1ms{0};
      std::atomic<std::uint64_t> total_nanos{0};
      std::atomic<std::int64_t> worst_nanos{0};
    };
    static absl::NoDestructor<Buckets> buckets;
    static const bool registered = [] {
      std::atexit([] {
        std::fprintf(
            stderr,
            "uv item: <10us %llu, <100us %llu, <1ms %llu, >=1ms %llu, "
            "busy %.1f ms total, worst %.0f us\n",
            static_cast<unsigned long long>(buckets->under_10us.load()),
            static_cast<unsigned long long>(buckets->under_100us.load()),
            static_cast<unsigned long long>(buckets->under_1ms.load()),
            static_cast<unsigned long long>(buckets->over_1ms.load()),
            static_cast<double>(buckets->total_nanos.load()) / 1e6,
            static_cast<double>(buckets->worst_nanos.load()) / 1e3);
      });
      return true;
    }();
    (void)registered;
    // Plain literals, no digit separators: clang-format has been seen to read
    // 1'000'000 as character literals when reflowing this region.
    constexpr std::int64_t kTenMicros = 10000;
    constexpr std::int64_t kHundredMicros = 100000;
    constexpr std::int64_t kMilli = 1000000;
    buckets->total_nanos.fetch_add(static_cast<std::uint64_t>(nanos),
                                   std::memory_order_relaxed);
    if (nanos < kTenMicros) {
      buckets->under_10us.fetch_add(1, std::memory_order_relaxed);
    } else if (nanos < kHundredMicros) {
      buckets->under_100us.fetch_add(1, std::memory_order_relaxed);
    } else if (nanos < kMilli) {
      buckets->under_1ms.fetch_add(1, std::memory_order_relaxed);
    } else {
      buckets->over_1ms.fetch_add(1, std::memory_order_relaxed);
    }
    std::int64_t seen = buckets->worst_nanos.load(std::memory_order_relaxed);
    while (nanos > seen &&
           !buckets->worst_nanos.compare_exchange_weak(
               seen, nanos, std::memory_order_relaxed)) {
    }
  }

  static void RecordDrainBatch(const std::deque<Item>& batch) {
    struct Stats {
      std::atomic<std::uint64_t> drains{0};
      std::atomic<std::uint64_t> items{0};
      std::atomic<std::uint64_t> multi_item{0};
      std::atomic<std::uint64_t> multi_key{0};
      std::atomic<std::uint64_t> largest{0};
    };

    static absl::NoDestructor<Stats> stats;
    static const bool registered = [] {
      std::atexit([] {
        const std::uint64_t drains = stats->drains.load();
        std::fprintf(
            stderr,
            "uv drain: %llu drains, %llu items (%.2f/drain), "
            "%llu with >1 item (%.1f%%), %llu with >1 key (%.1f%%), "
            "largest %llu\n",
            static_cast<unsigned long long>(drains),
            static_cast<unsigned long long>(stats->items.load()),
            drains == 0 ? 0.0
                        : static_cast<double>(stats->items.load()) / drains,
            static_cast<unsigned long long>(stats->multi_item.load()),
            drains == 0
                ? 0.0
                : 100.0 * static_cast<double>(stats->multi_item.load()) /
                      drains,
            static_cast<unsigned long long>(stats->multi_key.load()),
            drains == 0
                ? 0.0
                : 100.0 * static_cast<double>(stats->multi_key.load()) / drains,
            static_cast<unsigned long long>(stats->largest.load()));
      });
      return true;
    }();
    (void)registered;
    stats->drains.fetch_add(1, std::memory_order_relaxed);
    stats->items.fetch_add(batch.size(), std::memory_order_relaxed);
    if (batch.size() > 1) {
      stats->multi_item.fetch_add(1, std::memory_order_relaxed);
      absl::flat_hash_set<const void*> keys;
      for (const Item& item : batch) {
        keys.insert(item.key);
      }
      if (keys.size() > 1) {
        stats->multi_key.fetch_add(1, std::memory_order_relaxed);
      }
    }
    std::uint64_t seen = stats->largest.load(std::memory_order_relaxed);
    while (batch.size() > seen &&
           !stats->largest.compare_exchange_weak(seen, batch.size(),
                                                 std::memory_order_relaxed)) {}
  }

  /// A11_UV_FAIR=0 restores strictly FIFO draining.
  ///
  /// Kept as the control the fairness claim is measured against, in the same
  /// spirit as the pool's dials: the starvation this fixes is a ratio between two
  /// client populations, so the only honest way to quote a number for it is to run
  /// both policies in one binary.
  static bool FairDraining() {
    static const bool fair = [] {
      const char* setting = std::getenv("A11_UV_FAIR");
      return setting == nullptr || std::atoi(setting) != 0;
    }();
    return fair;
  }

  mutable thread::Mutex mu_;
  thread::CondVar cv_;
  bool running_ ABSL_GUARDED_BY(mu_) = true;
  std::optional<std::thread::id> loop_thread_id_ ABSL_GUARDED_BY(mu_);
  std::deque<Item> work_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<uvw::loop> loop_;
  std::shared_ptr<uvw::async_handle> async_;
  std::thread thread_;
};

/**
 * @brief Run @p operation on the loop thread and wait for its result.
 *
 * @param order_key  See UvExecutor::Post. **Pass the connection whenever the
 *   operation touches one**, or its awaited work can be reordered ahead of writes
 *   posted for the same socket. `HttpTransport::RunOnUvForConnection` supplies it
 *   automatically and is what connection code should call.
 */
template <typename T>
absl::StatusOr<T> RunOnUv(std::function<absl::StatusOr<T>()> operation,
                          const void* order_key = nullptr) {
  if (UvExecutor::Instance().IsLoopThread()) {
    return operation();
  }
  auto promise = std::make_shared<a11::Promise<T>>();
  a11::Future<T> future = promise->future();
  ABSL_RETURN_IF_ERROR(UvExecutor::Instance().Post(
      [promise, operation = std::move(operation)]() mutable {
        (void)promise->SetResult(operation());
      },
      order_key));
  return future.Await();
}

/**
 * How many application bytes may be queued for the loop before a writer waits.
 *
 * Outbound writes are posted to the loop and not awaited (see
 * HttpTransport::PostWrite), so nothing but this bounds how far a fast producer
 * can run ahead of the socket. Crossing it makes the next write take the
 * awaited path, which does not return until the loop has drained everything
 * queued before it -- backpressure to the caller, in the one place where the
 * caller being a fiber makes blocking cheap.
 *
 * Sized well above a single large message so that ordinary traffic never pays
 * the crossing, and well below anything that would matter as memory.
 */
constexpr std::size_t kMaxQueuedWriteBytes = 4 * 1024 * 1024;

inline absl::Status RunStatusOnUv(std::function<absl::Status()> operation,
                                  const void* order_key = nullptr) {
  absl::StatusOr<a11::Unit> result = RunOnUv<a11::Unit>(
      [operation =
           std::move(operation)]() mutable -> absl::StatusOr<a11::Unit> {
        ABSL_RETURN_IF_ERROR(operation());
        return a11::Unit{};
      },
      order_key);
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
    return RunStatusOnUvForConnection(
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
  /**
   * @brief Stop or resume reading from the socket.
   *
   * The backpressure primitive. A reader whose buffers are full asks for a
   * pause; the kernel receive buffer then fills, the TCP window closes, and the
   * peer stops sending -- all the way back to its own sender. This is what makes
   * a large HTTP/2 window safe: the window governs how much may be *in flight*,
   * and this governs whether we are willing to take more at all.
   *
   * Without it the only bound on a fast peer is how quickly the application
   * drains, and a consumer slower than the link overflows its buffer and the
   * transfer fails -- which is exactly what happens on a loopback or
   * multi-gigabit path.
   *
   * Safe from any thread and idempotent; the work happens on the loop.
   */
  void SetReadPaused(bool paused) {
    if (read_paused_.exchange(paused) == paused) {
      return;
    }
    std::weak_ptr<HttpTransport> weak = shared_from_this();
    const auto apply = [weak, paused] {
      std::shared_ptr<HttpTransport> self = weak.lock();
      if (self == nullptr || self->closed_ || self->tcp_ == nullptr) {
        return;
      }
      if (paused) {
        self->tcp_->stop();
      } else {
        self->tcp_->read();
      }
    };
    if (UvExecutor::Instance().IsLoopThread()) {
      apply();
    } else {
      (void)UvExecutor::Instance().Post(apply, this);
    }
  }

  /**
   * @brief Runs an outbound write on the loop without waiting for it.
   *
   * The write path's equivalent of SetReadPaused: what a sender needs from the
   * loop is that the bytes go out in order, not that they have gone out before
   * it continues. Awaiting each one -- which is what RunStatusOnUv does -- puts
   * a full loop crossing (7.4us) plus a fiber park and wake in front of every
   * message, on both endpoints, and it is pure latency: the caller has nothing
   * to do with the answer, because a transport write failure reaches the
   * application through the stream's lifecycle rather than through the return
   * of the send that happened to trigger it.
   *
   * Ordering is preserved because posted and awaited work share one FIFO queue
   * on the executor, so a later awaited call (a Finish, a header write) still
   * lands behind writes posted before it.
   *
   * Three cases go down the old awaited path instead, where the semantics are
   * exactly what they were: already on the loop thread (nothing to post),
   * a connection that is not up (so the caller still gets the real error), and
   * a queue past kMaxQueuedWriteBytes (so a producer faster than the socket
   * waits rather than growing the queue).
   *
   * @param bytes  Size of the data the operation will write, for the queue
   * bound. @param write  The work to run on the loop thread.
   */
  /**
   * @brief RunOnUv keyed to this connection.
   *
   * Connection code should use this and never bare `RunOnUv`: the key is what
   * keeps an awaited operation (a Finish, a header write, a settings update)
   * behind the data writes already posted for the same socket. Forgetting it does
   * not fail loudly -- it reorders frames.
   */
  template <typename T>
  absl::StatusOr<T> RunOnUvForConnection(
      std::function<absl::StatusOr<T>()> operation) {
    return RunOnUv<T>(std::move(operation), this);
  }

  /// RunStatusOnUv keyed to this connection. See RunOnUvForConnection.
  absl::Status RunStatusOnUvForConnection(
      std::function<absl::Status()> operation) {
    return RunStatusOnUv(std::move(operation), this);
  }

  absl::Status PostWrite(std::size_t bytes,
                         std::function<absl::Status()> write) {
    if (!write) {
      return absl::InvalidArgumentError("write work must be callable");
    }
    if (UvExecutor::Instance().IsLoopThread() || !connected_.load() ||
        queued_write_bytes_.load(std::memory_order_relaxed) >=
            kMaxQueuedWriteBytes) {
      // Keyed like the posted path: this is the same socket, and a fallback write
      // that lost its key could be reordered ahead of writes already queued for
      // this connection. The three cases that come here (already on the loop
      // thread, not connected, queue past the bound) are exactly the ones where a
      // caller waits, so they must still land in order.
      return RunStatusOnUvForConnection(std::move(write));
    }
    std::shared_ptr<HttpTransport> self = shared_from_this();
    queued_write_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    const absl::Status posted = UvExecutor::Instance().Post(
        [self = std::move(self), bytes, write = std::move(write)]() mutable {
          const absl::Status status = write();
          self->queued_write_bytes_.fetch_sub(bytes,
                                              std::memory_order_relaxed);
          // Nobody is waiting for this status, so the connection carries it:
          // closing is what tells every stream on it, which is the same thing
          // that happens when the socket itself fails.
          if (!status.ok()) {
            self->CloseOnLoop(std::move(status));
          }
        },
        this);
    if (!posted.ok()) {
      queued_write_bytes_.fetch_sub(bytes, std::memory_order_relaxed);
    }
    return posted;
  }

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
    // A receive buffer sized for a fast path. The kernel's default is tens of
    // kilobytes, which caps a single connection at buffer/RTT no matter what
    // the HTTP/2 windows say, and is also the backstop that absorbs data still
    // in flight when a reader pauses. Best effort: a platform that refuses the
    // size just keeps its default.
    tcp_->recv_buffer_size(static_cast<int>(kSocketReceiveBufferSize));
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
      // A11's own: the connection registry's removal hook.
      on_closed(this);
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
  /// Whether SetReadPaused has stopped the socket read; atomic because a reader
  /// asks for it from a fiber while the loop thread acts on it.
  std::atomic<bool> read_paused_ = false;
  /// Application bytes posted to the loop by PostWrite and not yet written.
  std::atomic<std::size_t> queued_write_bytes_ = 0;
  /// Decrypted-plaintext scratch, reused across reads; only the loop thread
  /// touches it.
  std::vector<char> plaintext_;
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
    {
      // Hand it to the socket directly first.
      //
      // uv_write is an owning, queued write: it needs a buffer that outlives
      // the call, so every byte is allocated and copied before the kernel is
      // even asked whether it had room. On a socket that is not backed up --
      // which is the normal case, and always the case on loopback --
      // uv_try_write takes it straight from this buffer and the copy never
      // happens. Only the remainder of a partial write, or a socket whose send
      // buffer is full, pays for one.
      //
      // Guarded on an empty write queue, and that guard is load-bearing rather
      // than an optimisation: a try_write while an owning write is still queued
      // would put these bytes on the wire in front of bytes handed over
      // earlier, which for a framed protocol is corruption and not merely
      // reordering.
      while (offset < length && tcp_->write_queue_size() == 0) {
        const size_t remaining = length - offset;
        const unsigned int attempt = static_cast<unsigned int>(std::min(
            remaining,
            static_cast<size_t>(std::numeric_limits<unsigned int>::max())));
        const int accepted = tcp_->try_write(const_cast<char*>(data + offset),
                                             attempt);
        if (accepted > 0) {
          offset += static_cast<size_t>(accepted);
          continue;
        }
        if (accepted != 0 && accepted != UV_EAGAIN && accepted != UV_ENOSYS) {
          return UvError(accepted, "Writing HTTP TCP data");
        }
        break;  // Took nothing: fall through to the queued write.
      }
      while (offset < length) {
        const size_t remaining = std::min(
            length - offset,
            static_cast<size_t>(std::numeric_limits<unsigned int>::max()));
        auto output = std::make_unique<char[]>(remaining);
        std::memcpy(output.get(), data + offset, remaining);
        const int written = tcp_->write(std::move(output),
                                        static_cast<unsigned int>(remaining));
        if (written != 0) {
          return UvError(written, "Writing HTTP TCP data");
        }
        offset += remaining;
      }
    }
    return absl::OkStatus();
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
    // A reused member buffer: see kTlsPlaintextBufferSize. Sizing it well past
    // a TLS record's 16 KiB means one SSL_read_ex drains many records per call.
    if (plaintext_.empty()) {
      plaintext_.resize(kTlsPlaintextBufferSize);
    }
    // SSL_read_ex returns at most one TLS record -- 16 KiB -- per call, however
    // large the buffer. Dispatching each one separately hands the protocol a
    // batch of one frame every time, so nothing downstream can ever amortise:
    // one allocation, one reader wake-up and one write per 16 KiB. Filling the
    // buffer across records first, and dispatching once, is what lets the layers
    // above work in batches. It costs no latency -- these bytes have all arrived
    // already.
    size_t filled = 0;
    const auto dispatch = [this, &filled]() -> absl::Status {
      if (filled == 0) {
        return absl::OkStatus();
      }
      const size_t ready = filled;
      filled = 0;
      return OnInboundPlaintext(plaintext_.data(), ready);
    };

    while (true) {
      size_t length = 0;
      ERR_clear_error();
      const int read = SSL_read_ex(ssl_, plaintext_.data() + filled,
                                   plaintext_.size() - filled, &length);
      if (read == 1) {
        filled += length;
        if (filled == plaintext_.size()) {
          ABSL_RETURN_IF_ERROR(dispatch());
        }
        continue;
      }
      const int error = SSL_get_error(ssl_, read);
      if (error == SSL_ERROR_WANT_READ) {
        break;
      }
      if (error == SSL_ERROR_WANT_WRITE) {
        // Hand over what has been decrypted before blocking on the write side,
        // so a stalled writer cannot hold finished data hostage.
        ABSL_RETURN_IF_ERROR(dispatch());
        ABSL_RETURN_IF_ERROR(FlushTlsOutput());
        continue;
      }
      if (error == SSL_ERROR_ZERO_RETURN) {
        ABSL_RETURN_IF_ERROR(dispatch());
        return absl::UnavailableError("TLS peer closed the connection");
      }
      ABSL_RETURN_IF_ERROR(dispatch());
      return TlsError("Decrypting HTTP TLS data");
    }
    ABSL_RETURN_IF_ERROR(dispatch());
    return FlushTlsOutput();
  }
};

}  // namespace a11::net::internal

#endif  // A11_NET_INTERNAL_HTTP_TRANSPORT_H_
