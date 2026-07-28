// Copyright 2026 The A11 Authors.

#include "a11/net/signalling.h"

#include <memory>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/future.h"
#include "thread/boost_primitives.h"

namespace a11::net {
namespace {

TEST(SignallingTest, RoutesMessagesInOrderByIdentity) {
  auto service = SignallingService::Create();
  auto received = std::make_shared<std::vector<std::string>>();
  auto mutex = std::make_shared<thread::Mutex>();
  auto done = std::make_shared<a11::Promise<a11::Unit>>();
  auto server = service->Connect(
      "server", [received, mutex, done](SignallingMessage message) {
        thread::MutexLock lock(&*mutex);
        received->push_back(message.candidate);
        if (received->size() == 2) {
          (void)done->SetValue(a11::Unit{});
        }
        return a11::ReadyTask();
      });
  ASSERT_TRUE(server.ok()) << server.status();
  auto client = service->Connect(
      "client", [](SignallingMessage) { return a11::ReadyTask(); });
  ASSERT_TRUE(client.ok()) << client.status();

  EXPECT_TRUE(
      (*client)
          ->Send(SignallingMessage{.type = SignallingMessageType::kCandidate,
                                   .recipient = "server",
                                   .candidate = "first",
                                   .mid = "0"})
          .ok());
  EXPECT_TRUE(
      (*client)
          ->Send(SignallingMessage{.type = SignallingMessageType::kCandidate,
                                   .recipient = "server",
                                   .candidate = "second",
                                   .mid = "0"})
          .ok());
  ASSERT_TRUE(done->future().Await(absl::Now() + absl::Seconds(2)).ok());
  thread::MutexLock lock(&*mutex);
  ASSERT_EQ(received->size(), 2);
  EXPECT_EQ((*received)[0], "first");
  EXPECT_EQ((*received)[1], "second");
  EXPECT_TRUE(service->Contains("server"));
  EXPECT_EQ(service->Identities().size(), 2);
}

TEST(SignallingTest, JsonRoundTripsStructuredErrors) {
  SignallingMessage message{.type = SignallingMessageType::kError,
                            .sender = "server",
                            .recipient = "client",
                            .error = absl::PermissionDeniedError("denied")};
  auto encoded = message.ToJson();
  ASSERT_TRUE(encoded.ok()) << encoded.status();
  auto decoded = SignallingMessage::FromJson(*encoded);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(decoded->sender, "server");
  EXPECT_EQ(decoded->recipient, "client");
  EXPECT_EQ(decoded->error.code(), absl::StatusCode::kPermissionDenied);
  EXPECT_EQ(decoded->error.message(), "denied");
}

}  // namespace
}  // namespace a11::net
