// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Sharing one HTTP connection between concurrent requests to a peer.
 *
 * HTTP/2 multiplexes several exchanges over one connection. Reusing an active
 * connection avoids another TCP and TLS handshake and preserves the peer's
 * header-compression context.
 *
 * The pool retains only weak references. A connection lives while it has active
 * leases and closes when its final request finishes, avoiding idle file
 * descriptors, peer sessions, and keep-alive timers.
 *
 * HTTP/1.1 is not pooled. A11's HTTP/1.1 connection carries a single request by
 * design -- there is no pipelining -- so those leases are exclusive and the
 * connection is dropped when the request ends. Reuse happens where the protocol
 * actually multiplexes.
 */

#ifndef A11_NET_HTTP_CONNECTION_POOL_H_
#define A11_NET_HTTP_CONNECTION_POOL_H_

#include <memory>
#include <string>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/status/statusor.h>

#include "a11/concurrency/future.h"
#include "a11/net/http/url.h"
#include "a11/net/http2.h"
#include "thread/boost_primitives.h"

namespace a11::net {

class HttpConnectionPool;

/**
 * @brief A borrowed connection, held for as long as a caller needs it.
 *
 * The unit of lifetime the pool works in: the connection is closed when the
 * last
 * lease on it goes away. Hold one for the whole exchange -- from the request to
 * the last byte of the response, including its trailers -- because dropping it
 * early may close the socket mid-response.
 *
 * Copyable, and a copy is another share of the same connection rather than a
 * new
 * one, so a lease can be captured by whatever outlives the call that took it.
 */
class HttpConnectionLease {
 public:
  HttpConnectionLease() = default;

  /** @return The connection, or null for a default-constructed lease. */
  [[nodiscard]] const std::shared_ptr<Http2Client>& client() const {
    return client_;
  }

  /** @return Whether this lease joined a connection that already existed. */
  [[nodiscard]] bool reused() const { return reused_; }

  /** @return Whether the connection is HTTP/2. @see Http2Client::multiplexed */
  [[nodiscard]] bool multiplexed() const;

  /**
   * Releases this share of the connection early; the destructor does it too.
   */
  void Release() { client_.reset(); }

 private:
  friend class HttpConnectionPool;

  HttpConnectionLease(std::shared_ptr<Http2Client> client, bool reused)
      : client_(std::move(client)), reused_(reused) {}

  std::shared_ptr<Http2Client> client_;
  bool reused_ = false;
};

/**
 * @brief Hands out connections to origins, sharing them while they are in use.
 *
 * @warning `options.deadline` is ignored: a connection-wide deadline set by one
 *          request would cut short every other request sharing it. Bound an
 *          exchange by awaiting it with a deadline and cancelling its stream,
 *          not by telling the connection when to die.
 */
class HttpConnectionPool
    : public std::enable_shared_from_this<HttpConnectionPool> {
 public:
  /** @brief Creates an independent pool. */
  static std::shared_ptr<HttpConnectionPool> Create();
  /**
   * @brief The process-wide pool.
   *
   * What a caller wants by default: two unrelated parts of a program fetching
   * from the same peer at the same moment should share the connection, and
   * neither should have to have been told about the other.
   */
  static const std::shared_ptr<HttpConnectionPool>& Shared();

  /**
   * @brief Borrows a connection to @p origin, dialling one if needed.
   *
   * A live connection to the same origin with compatible @p options is joined
   * rather than replaced. A dial already in progress is joined too, so a burst
   * of requests to a cold origin opens one socket instead of several.
   *
   * @param origin Parsed URL; its scheme, host and port select the peer, and
   *        the scheme decides TLS.
   * @param options Transport settings. Their TLS and protocol fields are part
   * of
   *        the pool key, since a connection trusting different roots is a
   *        different peer. `deadline` is ignored (see the class note).
   * @return An awaitable resolving to the lease, or the dial's error.
   */
  a11::Future<HttpConnectionLease> Acquire(const ParsedUrl& origin,
                                           Http2Options options);

  /**
   * @brief Dials a connection nobody else will be given, and leases it.
   *
   * For a caller that asked not to share -- a request with client certificates
   * of its own, or one whose timing must not be affected by anyone else's
   * traffic. It is closed when the last copy of the lease goes, as a pooled one
   * is; the only difference is that no second caller is ever handed it.
   *
   * @param origin Parsed URL naming the peer; its scheme decides TLS.
   * @param options Transport settings. `deadline` is ignored, as in Acquire().
   * @return An awaitable resolving to the lease, or the dial's error.
   */
  static a11::Future<HttpConnectionLease> AcquireUnshared(
      const ParsedUrl& origin, Http2Options options);

  /** @return How many connections are currently live. For tests. */
  [[nodiscard]] size_t size() const;

 private:
  /// One origin's shared connection and the dial that may be producing it.
  struct Entry {
    /// Weak on purpose: the leases own the connection, not the pool.
    std::weak_ptr<Http2Client> client;
    /// The in-flight dial, so concurrent askers wait rather than each dialling.
    std::shared_ptr<a11::Promise<std::shared_ptr<Http2Client>>> dialling;
    a11::Future<std::shared_ptr<Http2Client>> dial;
  };

  /// Everything about a request that makes it a different peer or a different
  /// connection: origin, TLS trust, and protocol negotiation policy.
  static std::string KeyFor(const ParsedUrl& origin,
                            const Http2Options& options);

  /// Drops @p key's entry, once its last lease has closed the connection.
  void Forget(const std::string& key);

  /// Wraps a new client so the last lease closes and unregisters it.
  std::shared_ptr<Http2Client> Adopt(std::string key,
                                     std::shared_ptr<Http2Client> client);

  mutable thread::Mutex mu_;
  absl::flat_hash_map<std::string, Entry> entries_ ABSL_GUARDED_BY(mu_);
};

}  // namespace a11::net

#endif  // A11_NET_HTTP_CONNECTION_POOL_H_
