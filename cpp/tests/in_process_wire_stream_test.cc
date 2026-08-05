// Copyright 2026 The A11 Authors.

#include "a11/net/in_process_wire_stream.h"

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"

namespace a11::net {
namespace {

TEST(InProcessWireStreamTest, DeliversMessagesAndOrderedHalfClose) {
  auto pair = InProcessWireStream::CreatePair();
  ASSERT_TRUE(pair.ok()) << pair.status();
  auto [first, second] = *pair;
  std::vector<std::optional<data::WireMessage>> received;
  std::atomic<bool> first_done = false;
  std::atomic<bool> second_done = false;
  ASSERT_TRUE(
      first
          ->Start(
              [](std::optional<data::WireMessage>) { return a11::ReadyTask(); },
              [&first_done]() {
                first_done = true;
                return a11::ReadyTask();
              })
          .Await()
          .ok());
  ASSERT_TRUE(second
                  ->Accept(
                      [&received](std::optional<data::WireMessage> message) {
                        received.push_back(std::move(message));
                        return a11::ReadyTask();
                      },
                      [&second_done]() {
                        second_done = true;
                        return a11::ReadyTask();
                      })
                  .Await()
                  .ok());

  data::WireMessage message{.node_fragments = {{
                                .id = "node",
                                .data = data::Chunk{.data = "payload"},
                                .seq = 0,
                                .continued = false,
                            }}};
  ASSERT_TRUE(first->Send(message).ok());
  ASSERT_TRUE(first->HalfClose({{"trailer", "value"}}).ok());
  ASSERT_TRUE(second->HalfClose().ok());
  ASSERT_TRUE(first->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(5))
                  .ok());
  ASSERT_TRUE(second->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(5))
                  .ok());

  const absl::Time limit = absl::Now() + absl::Seconds(5);
  while ((!first_done || !second_done) && absl::Now() < limit) {
    thread::SleepFor(absl::Milliseconds(1));
  }
  ASSERT_TRUE(first_done);
  ASSERT_TRUE(second_done);
  ASSERT_EQ(received.size(), 2);
  ASSERT_TRUE(received[0].has_value());
  EXPECT_FALSE(received[1].has_value());
  ASSERT_TRUE(second->GetTrailers().has_value());
  EXPECT_EQ(second->GetTrailers()->at("trailer"), "value");
}

TEST(InProcessWireStreamTest, CommunicatesAbortStatus) {
  auto pair = *InProcessWireStream::CreatePair();
  auto first = pair.first;
  auto second = pair.second;
  std::atomic<bool> done = false;
  ASSERT_TRUE(
      first
          ->Start(
              [](std::optional<data::WireMessage>) { return a11::ReadyTask(); },
              [] { return a11::ReadyTask(); })
          .Await()
          .ok());
  ASSERT_TRUE(
      second
          ->Accept(
              [](std::optional<data::WireMessage>) { return a11::ReadyTask(); },
              [&done] {
                done = true;
                return a11::ReadyTask();
              })
          .Await()
          .ok());
  ASSERT_TRUE(first->Abort(absl::DataLossError("corrupt")).ok());
  const absl::Time limit = absl::Now() + absl::Seconds(5);
  while (!done && absl::Now() < limit) {
    thread::SleepFor(absl::Milliseconds(1));
  }
  ASSERT_TRUE(done);
  EXPECT_EQ(second->GetStatus().code(), absl::StatusCode::kDataLoss);
  EXPECT_EQ(first->GetStatus().code(), absl::StatusCode::kAborted);
}

}  // namespace
}  // namespace a11::net
