// Copyright 2026 The A11 Authors.

#include "a11/service/session.h"

#include <memory>
#include <optional>

#include <absl/status/status.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/data/types.h"
#include "a11/net/in_process_wire_stream.h"
#include "a11/nodes/async_node.h"

namespace a11::service {
namespace {

SessionOptions TestOptions() {
  SessionOptions options;
  options.no_stream_timeout = absl::InfiniteDuration();
  return options;
}

TEST(SessionTest, DispatchesFragmentsIntoItsNodeMap) {
  auto session = *Session::Create("session", {}, {}, {}, TestOptions());
  data::NodeFragment fragment{.id = "node",
                              .data = data::Chunk{.data = "payload"},
                              .seq = 0,
                              .continued = false};
  auto dispatched = session->DispatchNodeFragment(fragment).Await();
  ASSERT_TRUE(dispatched.ok()) << dispatched.status();
  EXPECT_EQ(*dispatched, 0);
  auto node = *session->GetNodeMap()->Get("node");
  auto read = node->NextChunk().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(read.ok()) << read.status();
  ASSERT_TRUE(read->has_value());
  EXPECT_EQ(read->value().data, "payload");
}

TEST(SessionTest, ReceiveAdapterReturnsStreamIdAndSessionHalfClose) {
  auto receiver = *SessionWithRecv::Create("receiver", {}, TestOptions());
  auto sender = *Session::Create("sender", {}, {}, {}, TestOptions());
  auto pair = *net::InProcessWireStream::CreatePair();
  ASSERT_TRUE(sender->AddStream(pair.first, StreamMode::kStart)->Await().ok());
  ASSERT_TRUE(
      receiver->AddStream(pair.second, StreamMode::kAccept)->Await().ok());
  data::WireMessage message{
      .node_fragments = {{.id = "n", .data = data::Chunk{.data = "x"}}}};
  ASSERT_TRUE(sender->Send(message).ok());
  auto received =
      receiver->ReceiveWithStreamId(absl::Now() + absl::Seconds(5)).Await();
  ASSERT_TRUE(received.ok()) << received.status();
  ASSERT_TRUE(received->has_value());
  EXPECT_EQ(received->value().message, message);
  EXPECT_EQ(received->value().stream_id, pair.second->GetId());

  ASSERT_TRUE(sender->HalfClose().ok());
  auto eof = receiver->Receive(absl::Now() + absl::Seconds(5)).Await();
  ASSERT_TRUE(eof.ok()) << eof.status();
  EXPECT_FALSE(eof->has_value());
}

TEST(SessionTest, AbortStatusPrecedesDoneCallback) {
  absl::Status observed;
  bool closed = false;
  OnSessionStreamDone on_done = [&observed, &closed](
                                    std::shared_ptr<net::WireStream>,
                                    std::shared_ptr<Session> session) {
    observed = session->GetStatus();
    closed = session->IsClosed();
    return a11::ReadyTask();
  };
  auto receiver = *Session::Create("receiver", {}, on_done, {}, TestOptions());
  auto sender = *Session::Create("sender", {}, {}, {}, TestOptions());
  auto pair = *net::InProcessWireStream::CreatePair();
  ASSERT_TRUE(sender->AddStream(pair.first)->Await().ok());
  ASSERT_TRUE(
      receiver->AddStream(pair.second, StreamMode::kAccept)->Await().ok());
  ASSERT_TRUE(sender->Abort(absl::DataLossError("session failed")).ok());
  ASSERT_TRUE(receiver->Done().Await(absl::Now() + absl::Seconds(5)).ok());
  EXPECT_EQ(observed.code(), absl::StatusCode::kAborted);
  EXPECT_TRUE(closed);
}

}  // namespace
}  // namespace a11::service
