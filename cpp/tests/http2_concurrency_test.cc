// Copyright 2026 The A11 Authors.

// Concurrent exchanges on one HTTP/2 connection. A11's own transports issue them
// -- the HTTP SSE client posts outbound messages without waiting for the one
// before it -- so "many at once on one client" is a supported usage pattern and
// not just something nghttp2 happens to allow.

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/net/http2.h"
#include "thread/boost_primitives.h"

namespace a11::net {
namespace {

TEST(Http2ConcurrencyTest, ManyConcurrentRequestsOnOneConnection) {
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [](const HttpRequest& request,
         const std::shared_ptr<Http2ResponseWriter>& response) -> a11::Task {
        absl::Status status = response->SendResponse(200, {}, request.body);
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      });
  ASSERT_TRUE(server.ok()) << server.status();
  auto client = Http2Client::Connect("127.0.0.1", (*server)->port())
                    .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();

  constexpr int kTotal = 400;
  constexpr int kSlots = 8;
  std::atomic<int> done{0};
  std::atomic<int> failed{0};
  thread::Mutex mu;
  thread::CondVar cv;
  int in_flight = 0;
  for (int index = 0; index < kTotal; ++index) {
    {
      thread::MutexLock lock(&mu);
      while (in_flight >= kSlots) {
        ASSERT_FALSE(cv.WaitWithDeadline(&mu, absl::Now() + absl::Seconds(10)))
            << "stalled after " << done.load() << " responses, " << in_flight
            << " in flight";
      }
      ++in_flight;
    }
    a11::Schedule([&, index]() {
      auto response = (*client)
                          ->Request("POST", "/echo", {}, absl::StrCat(index))
                          .Await(absl::Now() + absl::Seconds(20));
      if (!response.ok() || response->head.status != 200) {
        failed.fetch_add(1);
      } else {
        done.fetch_add(1);
      }
      thread::MutexLock lock(&mu);
      --in_flight;
      cv.SignalAll();
    });
  }
  {
    thread::MutexLock lock(&mu);
    while (in_flight > 0) {
      ASSERT_FALSE(cv.WaitWithDeadline(&mu, absl::Now() + absl::Seconds(15)))
          << "stalled with " << in_flight << " in flight, " << done.load()
          << " done, " << failed.load() << " failed";
    }
  }
  EXPECT_EQ(done.load(), kTotal);
  EXPECT_EQ(failed.load(), 0);
  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

// The HTTP SSE shape: one long-lived response stream the client keeps reading
// while it issues concurrent short requests on the same connection, and the
// server writes to that stream from the request handlers.
TEST(Http2ConcurrencyTest,
     ConcurrentRequestsAlongsideALongLivedResponseStream) {
  auto events = std::make_shared<std::shared_ptr<Http2ResponseWriter>>();
  thread::Mutex events_mu;
  auto server = Http2Server::Create(
      "127.0.0.1", 0,
      [events, &events_mu](
          const HttpRequest& request,
          std::shared_ptr<Http2ResponseWriter> response) -> a11::Task {
        if (request.path == "/connect") {
          HttpHeaders headers{{"content-type", "text/event-stream"}};
          absl::Status status = response->SendHeaders(200, headers);
          if (!status.ok()) {
            return a11::FailedTask(status);
          }
          thread::MutexLock lock(&events_mu);
          *events = std::move(response);
          return a11::ReadyTask();
        }
        std::shared_ptr<Http2ResponseWriter> writer;
        {
          thread::MutexLock lock(&events_mu);
          writer = *events;
        }
        if (writer != nullptr) {
          absl::Status written =
              writer->Write(absl::StrCat("data: ", request.body, "\n\n"));
          if (!written.ok()) {
            return a11::FailedTask(written);
          }
        }
        absl::Status status = response->SendResponse(204);
        return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
      });
  ASSERT_TRUE(server.ok()) << server.status();
  auto client = Http2Client::Connect("127.0.0.1", (*server)->port())
                    .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();

  auto sse = (*client)->RequestStream("POST", "/connect");
  ASSERT_TRUE(sse.ok()) << sse.status();
  ASSERT_TRUE((*sse)->Headers().Await(absl::Now() + absl::Seconds(5)).ok());

  // Drains the long-lived stream for the life of the test, so the connection is
  // carrying response DATA the whole time the requests are in flight.
  std::atomic<int> event_bytes{0};
  a11::Schedule([sse = *sse, &event_bytes]() {
    while (true) {
      absl::StatusOr<std::optional<std::string>> chunk =
          sse->Read().Await(absl::InfiniteFuture());
      if (!chunk.ok() || !chunk->has_value()) {
        return;
      }
      event_bytes.fetch_add(static_cast<int>((*chunk)->size()));
    }
  });

  constexpr int kTotal = 400;
  constexpr int kSlots = 8;
  std::atomic<int> done{0};
  std::atomic<int> failed{0};
  thread::Mutex mu;
  thread::CondVar cv;
  int in_flight = 0;
  for (int index = 0; index < kTotal; ++index) {
    {
      thread::MutexLock lock(&mu);
      while (in_flight >= kSlots) {
        ASSERT_FALSE(cv.WaitWithDeadline(&mu, absl::Now() + absl::Seconds(10)))
            << "stalled after " << done.load() << " responses, " << in_flight
            << " in flight, " << event_bytes.load() << " event bytes";
      }
      ++in_flight;
    }
    a11::Schedule([&, index]() {
      auto response = (*client)
                          ->Request("POST", "/message", {}, absl::StrCat(index))
                          .Await(absl::Now() + absl::Seconds(20));
      if (!response.ok() || response->head.status != 204) {
        failed.fetch_add(1);
      } else {
        done.fetch_add(1);
      }
      thread::MutexLock lock(&mu);
      --in_flight;
      cv.SignalAll();
    });
  }
  {
    thread::MutexLock lock(&mu);
    while (in_flight > 0) {
      ASSERT_FALSE(cv.WaitWithDeadline(&mu, absl::Now() + absl::Seconds(15)))
          << "stalled with " << in_flight << " in flight, " << done.load()
          << " done, " << failed.load() << " failed";
    }
  }
  EXPECT_EQ(done.load(), kTotal);
  EXPECT_EQ(failed.load(), 0);
  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

}  // namespace
}  // namespace a11::net
