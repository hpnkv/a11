// Copyright 2026 The A11 Authors.

#include "a11/net/in_process_wire_stream.h"

#include <atomic>
#include <memory>
#include <optional>
#include <thread>
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
  ASSERT_TRUE(first
                  ->Start(
                      [](const std::optional<data::WireMessage>&) {
                        return a11::ReadyTask();
                      },
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

// Order is per endpoint and holds however the message got there.
TEST(InProcessWireStreamTest, PreservesSendOrderThroughBackpressure) {
  constexpr int kMessages = 400;
  WireStreamOptions narrow;
  narrow.max_buffered_incoming_messages = 4;
  auto pair = InProcessWireStream::CreatePair(narrow);
  ASSERT_TRUE(pair.ok()) << pair.status();
  auto [sender, receiver] = *pair;

  thread::Mutex mu;
  thread::CondVar cv;
  std::vector<int> seen;
  ASSERT_TRUE(sender
                  ->Start(
                      [](const std::optional<data::WireMessage>&) {
                        return a11::ReadyTask();
                      },
                      [] { return a11::ReadyTask(); })
                  .Await()
                  .ok());
  ASSERT_TRUE(
      receiver
          ->Accept(
              [&](std::optional<data::WireMessage> message) {
                if (message.has_value() && !message->node_fragments.empty()) {
                  thread::MutexLock lock(&mu);
                  // Every fragment, not every message:
                  for (const data::NodeFragment& fragment :
                       message->node_fragments) {
                    seen.push_back(static_cast<int>(fragment.seq.value_or(0)));
                  }
                  cv.SignalAll();
                }
                return a11::ReadyTask();
              },
              [] { return a11::ReadyTask(); })
          .Await()
          .ok());

  for (int index = 0; index < kMessages; ++index) {
    data::WireMessage message{.node_fragments = {{
                                  .id = "node",
                                  .data = data::Chunk{.data = "payload"},
                                  .seq = static_cast<std::uint32_t>(index),
                                  .continued = true,
                              }}};
    ASSERT_TRUE(sender->Send(std::move(message)).ok()) << index;
  }

  {
    thread::MutexLock lock(&mu);
    const absl::Time limit = absl::Now() + absl::Seconds(10);
    while (seen.size() < kMessages && absl::Now() < limit) {
      cv.WaitWithDeadline(&mu, limit);
    }
  }
  thread::MutexLock lock(&mu);
  ASSERT_EQ(seen.size(), kMessages);
  for (int index = 0; index < kMessages; ++index) {
    ASSERT_EQ(seen[static_cast<size_t>(index)], index) << "at " << index;
  }
}

// Concurrent senders interleave, but each one's own messages stay in order.
TEST(InProcessWireStreamTest, PreservesEachSendersOrderUnderConcurrency) {
  constexpr int kSenders = 4;
  constexpr int kPerSender = 250;
  WireStreamOptions narrow;
  narrow.max_buffered_incoming_messages = 8;
  auto pair = InProcessWireStream::CreatePair(narrow);
  ASSERT_TRUE(pair.ok()) << pair.status();
  auto [sender, receiver] = *pair;

  thread::Mutex mu;
  thread::CondVar cv;
  std::vector<std::vector<int>> seen(kSenders);
  int total = 0;
  ASSERT_TRUE(sender
                  ->Start(
                      [](const std::optional<data::WireMessage>&) {
                        return a11::ReadyTask();
                      },
                      [] { return a11::ReadyTask(); })
                  .Await()
                  .ok());
  ASSERT_TRUE(receiver
                  ->Accept(
                      [&](std::optional<data::WireMessage> message) {
                        if (!message.has_value()) {
                          return a11::ReadyTask();
                        }
                        thread::MutexLock lock(&mu);
                        for (const data::NodeFragment& fragment :
                             message->node_fragments) {
                          const std::uint32_t tag = fragment.seq.value_or(0);
                          seen[tag >> 24U].push_back(
                              static_cast<int>(tag & 0xFFFFFFU));
                          ++total;
                        }
                        cv.SignalAll();
                        return a11::ReadyTask();
                      },
                      [] { return a11::ReadyTask(); })
                  .Await()
                  .ok());

  std::vector<std::thread> senders;
  senders.reserve(kSenders);
  for (int index = 0; index < kSenders; ++index) {
    senders.emplace_back([&, index] {
      for (int count = 0; count < kPerSender; ++count) {
        data::WireMessage message{
            .node_fragments = {{
                .id = "node",
                .data = data::Chunk{.data = "payload"},
                .seq = static_cast<std::uint32_t>((index << 24U) | count),
                .continued = true,
            }}};
        ASSERT_TRUE(sender->Send(std::move(message)).ok());
      }
    });
  }
  for (std::thread& thread : senders) {
    thread.join();
  }

  {
    thread::MutexLock lock(&mu);
    const absl::Time limit = absl::Now() + absl::Seconds(10);
    while (total < kSenders * kPerSender && absl::Now() < limit) {
      cv.WaitWithDeadline(&mu, limit);
    }
  }
  thread::MutexLock lock(&mu);
  ASSERT_EQ(total, kSenders * kPerSender);
  for (int index = 0; index < kSenders; ++index) {
    const std::vector<int>& from = seen[static_cast<size_t>(index)];
    ASSERT_EQ(from.size(), kPerSender) << index;
    for (int count = 0; count < kPerSender; ++count) {
      ASSERT_EQ(from[static_cast<size_t>(count)], count)
          << index << " at " << count;
    }
  }
}

TEST(InProcessWireStreamTest, CommunicatesAbortStatus) {
  auto pair = *InProcessWireStream::CreatePair();
  auto first = pair.first;
  auto second = pair.second;
  std::atomic<bool> done = false;
  ASSERT_TRUE(first
                  ->Start(
                      [](const std::optional<data::WireMessage>&) {
                        return a11::ReadyTask();
                      },
                      [] { return a11::ReadyTask(); })
                  .Await()
                  .ok());
  ASSERT_TRUE(second
                  ->Accept(
                      [](const std::optional<data::WireMessage>&) {
                        return a11::ReadyTask();
                      },
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
