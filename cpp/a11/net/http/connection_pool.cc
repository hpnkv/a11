// Copyright 2026 The A11 Authors.

#include "a11/net/http/connection_pool.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/net/http/url.h"
#include "a11/net/http2.h"
#include "thread/concurrency.h"

namespace a11::net {
namespace {

/**
 * Dials one connection, with the connection-wide deadline deliberately cleared.
 *
 * A pooled connection outlives the request that opened it, so a deadline meant
 * for that one request must not follow it: Http2Options::deadline schedules a
 * cancel on *every* stream the connection carries.
 */
a11::Future<std::shared_ptr<Http2Client>> Dial(const ParsedUrl& origin,
                                               Http2Options options) {
  options.tls.enabled = origin.secure();
  options.deadline = absl::InfiniteFuture();
  return Http2Client::Connect(origin.host, origin.port, std::move(options));
}

/**
 * Re-owns @p client so that the last holder closes it, running @p on_closed
 * afterwards.
 *
 * The handle points at the same object but owns a deleter instead of the object
 * itself; the deleter holds the only real reference and drops it. That is what
 * makes a lease the unit of lifetime -- there is no path by which a connection
 * outlives the work being done over it.
 */
std::shared_ptr<Http2Client> OwnUntilLastLease(
    std::shared_ptr<Http2Client> client, std::function<void()> on_closed) {
  Http2Client* raw = client.get();
  return std::shared_ptr<Http2Client>(
      raw, [owned = std::move(client),
            on_closed = std::move(on_closed)](Http2Client*) mutable {
        (void)owned->Close();
        owned.reset();
        if (on_closed) {
          on_closed();
        }
      });
}

}  // namespace

bool HttpConnectionLease::multiplexed() const {
  return client_ != nullptr && client_->multiplexed();
}

std::shared_ptr<HttpConnectionPool> HttpConnectionPool::Create() {
  struct Enabler final : HttpConnectionPool {};
  return std::make_shared<Enabler>();
}

const std::shared_ptr<HttpConnectionPool>& HttpConnectionPool::Shared() {
  static const std::shared_ptr<HttpConnectionPool>* const pool =
      new std::shared_ptr<HttpConnectionPool>(Create());
  return *pool;
}

std::string HttpConnectionPool::KeyFor(const ParsedUrl& origin,
                                       const Http2Options& options) {
  // The buffer bound is here because it sets the HTTP/2 receive window, so two
  // callers asking for different bounds genuinely want different connections.
  return absl::StrCat(
      origin.scheme, "://", origin.host, ":", origin.port, "|",
      options.tls.verify_peer ? "verify" : "trust-any", "|",
      options.tls.ca_certificate_pem_file, "|", options.tls.certificate_pem_file,
      "|", options.tls.key_pem_file, "|",
      static_cast<int>(options.client_preference), "|",
      options.enable_h2 ? "1" : "0", options.enable_h2c ? "1" : "0",
      options.enable_http1 ? "1" : "0", options.enable_push ? "1" : "0", "|",
      options.max_buffered_response_bytes);
}

void HttpConnectionPool::Forget(const std::string& key) {
  thread::MutexLock lock(&mu_);
  const auto found = entries_.find(key);
  // Only drop an entry whose connection really has gone: a later dial for the
  // same origin may already have replaced it.
  if (found != entries_.end() && found->second.client.expired() &&
      found->second.dialling == nullptr) {
    entries_.erase(found);
  }
}

std::shared_ptr<Http2Client> HttpConnectionPool::Adopt(
    std::string key, std::shared_ptr<Http2Client> client) {
  // The last lease closes the connection and removes the entry. That is what
  // makes the pool hold nothing idle: the leases are the lifetime, and the map
  // is only an index over them.
  std::weak_ptr<HttpConnectionPool> weak = weak_from_this();
  return OwnUntilLastLease(std::move(client), [weak, key = std::move(key)]() {
    if (const std::shared_ptr<HttpConnectionPool> pool = weak.lock()) {
      pool->Forget(key);
    }
  });
}

a11::Future<HttpConnectionLease> HttpConnectionPool::AcquireUnshared(
    const ParsedUrl& origin, Http2Options options) {
  if (origin.host.empty() || origin.port == 0) {
    return a11::FailedFuture<HttpConnectionLease>(
        absl::InvalidArgumentError("An HTTP connection needs a host and port"));
  }
  return a11::Submit<HttpConnectionLease>(
      [origin, options = std::move(options)]() mutable
          -> absl::StatusOr<HttpConnectionLease> {
        ABSL_ASSIGN_OR_RETURN(std::shared_ptr<Http2Client> client,
                              Dial(origin, std::move(options)).Await());
        return HttpConnectionLease(
            OwnUntilLastLease(std::move(client), /*on_closed=*/{}),
            /*reused=*/false);
      });
}

size_t HttpConnectionPool::size() const {
  thread::MutexLock lock(&mu_);
  size_t live = 0;
  for (const auto& [key, entry] : entries_) {
    (void)key;
    if (!entry.client.expired()) {
      ++live;
    }
  }
  return live;
}

a11::Future<HttpConnectionLease> HttpConnectionPool::Acquire(
    const ParsedUrl& origin, Http2Options options) {
  if (origin.host.empty() || origin.port == 0) {
    return a11::FailedFuture<HttpConnectionLease>(
        absl::InvalidArgumentError("An HTTP connection needs a host and port"));
  }
  // HTTP/1.1 by request: one request per connection, so there is nothing to
  // share and none of the bookkeeping below applies.
  if (options.client_preference == Http2Options::ProtocolPreference::kHttp11) {
    return AcquireUnshared(origin, std::move(options));
  }

  const std::string key = KeyFor(origin, options);
  std::shared_ptr<a11::Promise<std::shared_ptr<Http2Client>>> mine;
  a11::Future<std::shared_ptr<Http2Client>> joined;
  {
    thread::MutexLock lock(&mu_);
    Entry& entry = entries_[key];
    if (std::shared_ptr<Http2Client> live = entry.client.lock();
        live != nullptr && live->connected()) {
      // The case this exists for: a request arriving while another is in flight
      // to the same peer travels over the same connection.
      return a11::ReadyFuture(
          HttpConnectionLease(std::move(live), /*reused=*/true));
    }
    if (entry.dialling != nullptr) {
      joined = entry.dial;
    } else {
      mine = std::make_shared<a11::Promise<std::shared_ptr<Http2Client>>>();
      entry.dialling = mine;
      entry.dial = mine->future();
    }
  }

  if (mine == nullptr) {
    // Someone else is already dialling this origin; wait for their socket.
    return a11::Submit<HttpConnectionLease>(
        [self = shared_from_this(), origin, options = std::move(options),
         joined = std::move(joined)]() mutable
            -> absl::StatusOr<HttpConnectionLease> {
          ABSL_ASSIGN_OR_RETURN(std::shared_ptr<Http2Client> client,
                                joined.Await());
          if (client->multiplexed() && client->connected()) {
            return HttpConnectionLease(std::move(client), /*reused=*/true);
          }
          // It negotiated HTTP/1.1, which carries one request. Nothing to join,
          // so this caller needs a connection of its own -- and an HTTP/1.1
          // connection is never registered, so there is nothing to clean up.
          client.reset();
          return AcquireUnshared(origin, std::move(options)).Await();
        });
  }

  return a11::Submit<HttpConnectionLease>(
      [self = shared_from_this(), key, origin, options = std::move(options),
       mine = std::move(mine)]() mutable -> absl::StatusOr<HttpConnectionLease> {
        absl::StatusOr<std::shared_ptr<Http2Client>> dialled =
            Dial(origin, options).Await();
        if (!dialled.ok()) {
          {
            thread::MutexLock lock(&self->mu_);
            self->entries_.erase(key);
          }
          (void)mine->SetStatus(dialled.status());
          return dialled.status();
        }
        // Only a multiplexing connection is worth indexing; an HTTP/1.1 one is
        // this caller's alone and closes with its lease.
        const bool shareable = (*dialled)->multiplexed();
        std::shared_ptr<Http2Client> client =
            shareable ? self->Adopt(key, std::move(*dialled))
                      : OwnUntilLastLease(std::move(*dialled),
                                          /*on_closed=*/{});
        {
          thread::MutexLock lock(&self->mu_);
          if (shareable) {
            Entry& entry = self->entries_[key];
            entry.client = client;
            entry.dialling = nullptr;
            entry.dial = {};
          } else {
            self->entries_.erase(key);
          }
        }
        (void)mine->SetValue(client);
        return HttpConnectionLease(std::move(client), /*reused=*/false);
      });
}

}  // namespace a11::net
