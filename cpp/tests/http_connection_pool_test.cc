// Copyright 2026 The A11 Authors.

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/net/http/connection_pool.h"
#include "a11/net/http/url.h"
#include "a11/net/http2.h"

namespace a11::net {
namespace {

absl::Time Soon() {
  return absl::Now() + absl::Seconds(10);
}

/// A server that counts the connections opened to it, which is what a pool is
/// judged on.
class CountingServer {
 public:
  CountingServer() {
    auto server = Http2Server::Create(
        "127.0.0.1", 0,
        [this](
            const HttpRequest& request,
            const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
          ++requests_;
          const absl::Status status = response->SendResponse(
              200, {}, absl::StrCat("ok:", request.path));
          return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
        });
    server_ = server.ok() ? *server : nullptr;
  }

  ~CountingServer() {
    if (server_ != nullptr) {
      (void)server_->Stop();
    }
  }

  [[nodiscard]] bool ok() const { return server_ != nullptr; }

  [[nodiscard]] std::uint16_t port() const { return server_->port(); }

  [[nodiscard]] int requests() const { return requests_.load(); }

  [[nodiscard]] ParsedUrl origin() const {
    ParsedUrl url;
    url.scheme = "http";
    url.host = "127.0.0.1";
    url.port = server_->port();
    url.path = "/";
    return url;
  }

 private:
  std::shared_ptr<Http2Server> server_;
  std::atomic<int> requests_{0};
};

TEST(HttpConnectionPoolTest, SharesOneConnectionBetweenLiveLeases) {
  CountingServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<HttpConnectionPool> pool = HttpConnectionPool::Create();

  auto first = pool->Acquire(server.origin(), {}).Await(Soon());
  ASSERT_TRUE(first.ok()) << first.status();
  EXPECT_FALSE(first->reused());
  EXPECT_TRUE(first->multiplexed());

  // A second request while the first is still in flight joins the connection
  // rather than opening another.
  auto second = pool->Acquire(server.origin(), {}).Await(Soon());
  ASSERT_TRUE(second.ok()) << second.status();
  EXPECT_TRUE(second->reused());
  EXPECT_EQ(first->client().get(), second->client().get());
  EXPECT_EQ(pool->size(), 1u);

  // And it really is usable: both leases carry a request over the one socket.
  auto one = first->client()->Request("GET", "/one").Await(Soon());
  auto two = second->client()->Request("GET", "/two").Await(Soon());
  ASSERT_TRUE(one.ok()) << one.status();
  ASSERT_TRUE(two.ok()) << two.status();
  EXPECT_EQ(one->body, "ok:/one");
  EXPECT_EQ(two->body, "ok:/two");
}

TEST(HttpConnectionPoolTest, ClosesTheConnectionWhenTheLastLeaseGoes) {
  CountingServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<HttpConnectionPool> pool = HttpConnectionPool::Create();

  {
    auto first = pool->Acquire(server.origin(), {}).Await(Soon());
    ASSERT_TRUE(first.ok()) << first.status();
    auto second = pool->Acquire(server.origin(), {}).Await(Soon());
    ASSERT_TRUE(second.ok()) << second.status();
    EXPECT_EQ(pool->size(), 1u);
    // One lease left: the connection is still needed.
    second->Release();
    EXPECT_EQ(pool->size(), 1u);
  }
  // No leases: nothing is being done with the connection, so nothing is kept.
  // This is the whole point -- the pool is an index over live work, not a cache.
  EXPECT_EQ(pool->size(), 0u);

  // Asking again dials afresh, and says so.
  auto again = pool->Acquire(server.origin(), {}).Await(Soon());
  ASSERT_TRUE(again.ok()) << again.status();
  EXPECT_FALSE(again->reused());
}

TEST(HttpConnectionPoolTest, ConcurrentAcquiresOfAColdOriginDialOnce) {
  CountingServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<HttpConnectionPool> pool = HttpConnectionPool::Create();

  // Eight callers reaching a peer none of them has talked to yet. Sharing the
  // dial is what keeps that one socket rather than eight.
  std::vector<a11::Future<HttpConnectionLease>> pending;
  pending.reserve(8);
  for (int index = 0; index < 8; ++index) {
    pending.push_back(pool->Acquire(server.origin(), {}));
  }
  std::vector<HttpConnectionLease> leases;
  for (auto& future : pending) {
    auto lease = future.Await(Soon());
    ASSERT_TRUE(lease.ok()) << lease.status();
    leases.push_back(std::move(*lease));
  }
  EXPECT_EQ(pool->size(), 1u);
  for (const HttpConnectionLease& lease : leases) {
    EXPECT_EQ(lease.client().get(), leases.front().client().get());
  }
}

TEST(HttpConnectionPoolTest, KeepsOriginsAndTrustSettingsApart) {
  CountingServer first;
  CountingServer second;
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  const std::shared_ptr<HttpConnectionPool> pool = HttpConnectionPool::Create();

  auto to_first = pool->Acquire(first.origin(), {}).Await(Soon());
  auto to_second = pool->Acquire(second.origin(), {}).Await(Soon());
  ASSERT_TRUE(to_first.ok()) << to_first.status();
  ASSERT_TRUE(to_second.ok()) << to_second.status();
  EXPECT_NE(to_first->client().get(), to_second->client().get());

  // Same origin, different trust roots: a different peer as far as this is
  // concerned, because the second connection is not verified the same way.
  Http2Options lax;
  lax.tls.verify_peer = false;
  auto unverified = pool->Acquire(first.origin(), lax).Await(Soon());
  ASSERT_TRUE(unverified.ok()) << unverified.status();
  EXPECT_NE(to_first->client().get(), unverified->client().get());
  EXPECT_EQ(pool->size(), 3u);
}

TEST(HttpConnectionPoolTest, DoesNotPoolHttp1Connections) {
  CountingServer server;
  ASSERT_TRUE(server.ok());
  const std::shared_ptr<HttpConnectionPool> pool = HttpConnectionPool::Create();

  Http2Options http1;
  http1.client_preference = Http2Options::ProtocolPreference::kHttp11;
  auto first = pool->Acquire(server.origin(), http1).Await(Soon());
  ASSERT_TRUE(first.ok()) << first.status();
  EXPECT_FALSE(first->multiplexed());
  EXPECT_FALSE(first->reused());

  // An HTTP/1.1 connection carries one request, so there is nothing to share and
  // the pool never indexes it.
  auto second = pool->Acquire(server.origin(), http1).Await(Soon());
  ASSERT_TRUE(second.ok()) << second.status();
  EXPECT_FALSE(second->reused());
  EXPECT_NE(first->client().get(), second->client().get());
  EXPECT_EQ(pool->size(), 0u);
}

TEST(HttpConnectionPoolTest, ReportsADialFailureToEveryWaiter) {
  const std::shared_ptr<HttpConnectionPool> pool = HttpConnectionPool::Create();
  ParsedUrl nowhere;
  nowhere.scheme = "http";
  nowhere.host = "127.0.0.1";
  nowhere.port = 1;  // Nothing listens here.
  nowhere.path = "/";

  auto first = pool->Acquire(nowhere, {});
  auto second = pool->Acquire(nowhere, {});
  EXPECT_FALSE(first.Await(Soon()).ok());
  EXPECT_FALSE(second.Await(Soon()).ok());
  // A failed dial leaves nothing behind for the next caller to wait on.
  EXPECT_EQ(pool->size(), 0u);
  auto third = pool->Acquire(nowhere, {});
  EXPECT_FALSE(third.Await(Soon()).ok());
}

TEST(HttpConnectionPoolTest, RejectsAnOriginWithoutAHostOrPort) {
  const std::shared_ptr<HttpConnectionPool> pool = HttpConnectionPool::Create();
  ParsedUrl empty;
  empty.scheme = "http";
  EXPECT_TRUE(
      absl::IsInvalidArgument(pool->Acquire(empty, {}).Await().status()));
}

}  // namespace
}  // namespace a11::net
