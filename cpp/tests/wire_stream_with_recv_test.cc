// Copyright 2026 The A11 Authors.

#include "a11/net/wire_stream_with_recv.h"

#include <memory>
#include <optional>

#include <absl/status/status.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/in_process_wire_stream.h"

namespace a11::net {
namespace {

TEST(WireStreamWithRecvTest, PullsDataAndRemoteHalfCloseExactlyOnce) {
  auto pair = *InProcessWireStream::CreatePair();
  auto receiver = *WireStreamWithRecv::Create(pair.second);
  ASSERT_TRUE(pair.first
                  ->Start(
                      [](const std::optional<data::WireMessage>&) {
                        return a11::ReadyTask();
                      },
                      [] { return a11::ReadyTask(); })
                  .Await()
                  .ok());
  ASSERT_TRUE(receiver->Accept().Await().ok());

  data::WireMessage message{.node_fragments = {{
                                .id = "node",
                                .data = data::Chunk{.data = "payload"},
                                .seq = 0,
                                .continued = false,
                            }}};
  ASSERT_TRUE(pair.first->Send(message).ok());
  ASSERT_TRUE(pair.first->HalfClose().ok());

  auto received = receiver->Receive().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(received.ok()) << received.status();
  ASSERT_TRUE(received->has_value());
  EXPECT_EQ(**received, message);
  auto eof = receiver->Receive().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(eof.ok()) << eof.status();
  EXPECT_FALSE(eof->has_value());
  EXPECT_EQ(receiver->Receive().Await().status().code(),
            absl::StatusCode::kFailedPrecondition);
}

TEST(WireStreamWithRecvTest, TimeoutIsLocalAndAbortBeatsBufferedData) {
  auto pair = *InProcessWireStream::CreatePair();
  auto receiver = *WireStreamWithRecv::Create(pair.second);
  ASSERT_TRUE(pair.first
                  ->Start(
                      [](const std::optional<data::WireMessage>&) {
                        return a11::ReadyTask();
                      },
                      [] { return a11::ReadyTask(); })
                  .Await()
                  .ok());
  ASSERT_TRUE(receiver->Accept().Await().ok());
  EXPECT_EQ(receiver->Receive(absl::Milliseconds(2)).Await().status().code(),
            absl::StatusCode::kDeadlineExceeded);
  EXPECT_TRUE(receiver->GetStatus().ok());

  ASSERT_TRUE(pair.first
                  ->Send(data::WireMessage{
                      .node_fragments = {{.id = "n",
                                          .data = data::Chunk{.data = "x"}}}})
                  .ok());
  const absl::Time buffered_limit = absl::Now() + absl::Seconds(5);
  while (absl::Now() < buffered_limit) {
    // A zero-time receive would consume the item, so use the transport's
    // activity boundary as a short grace period before the local abort.
    thread::SleepFor(absl::Milliseconds(1));
    break;
  }
  ASSERT_TRUE(receiver->Abort(absl::DataLossError("stop")).ok());
  EXPECT_EQ(receiver->Receive().Await().status().code(),
            absl::StatusCode::kAborted);
}

TEST(WireStreamWithRecvTest, CancelledReceiveDoesNotConsumeNextMessage) {
  auto pair = *InProcessWireStream::CreatePair();
  auto receiver = *WireStreamWithRecv::Create(pair.second);
  ASSERT_TRUE(pair.first
                  ->Start(
                      [](const std::optional<data::WireMessage>&) {
                        return a11::ReadyTask();
                      },
                      [] { return a11::ReadyTask(); })
                  .Await()
                  .ok());
  ASSERT_TRUE(receiver->Accept().Await().ok());

  auto cancelled = receiver->Receive();
  ASSERT_TRUE(cancelled.Cancel().ok());
  EXPECT_EQ(cancelled.Await().status().code(), absl::StatusCode::kCancelled);

  data::WireMessage message{.node_fragments = {{
                                .id = "after-cancel",
                                .data = data::Chunk{.data = "payload"},
                            }}};
  ASSERT_TRUE(pair.first->Send(message).ok());
  auto received = receiver->Receive(absl::Seconds(5)).Await();
  ASSERT_TRUE(received.ok()) << received.status();
  ASSERT_TRUE(received->has_value());
  EXPECT_EQ(**received, message);
}

}  // namespace
}  // namespace a11::net
