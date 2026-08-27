// Copyright 2026 The A11 Authors.

// The striping channel behind the WebRTC transport, and in particular the frame
// layout it puts on the wire.
//
// The layout is shared with js/src/webrtc_wire_stream.ts. No generated fixture
// pins it, so this test and js/test/transports.test.mjs use the same literal;
// changing either side without the other must fail visibly.

#include "a11/net/internal/multiplexed_binary_channel.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <gtest/gtest.h>

#include "a11/net/internal/binary_channel.h"
#include "thread/boost_primitives.h"

namespace a11::net::internal {
namespace {

/// A BinaryChannel that records what was sent and lets a test inject inbound
/// frames, standing in for one rtc::DataChannel.
class FakeMember final : public BinaryChannel {
 public:
  absl::Status SetCallbacks(BinaryChannelCallbacks callbacks) override {
    callbacks_ = std::move(callbacks);
    return absl::OkStatus();
  }

  absl::Status ResetCallbacks() override {
    callbacks_ = BinaryChannelCallbacks{};
    return absl::OkStatus();
  }

  absl::Status Open() override { return absl::OkStatus(); }

  absl::Status Send(std::string bytes) override {
    if (!open_) {
      return absl::UnavailableError("fake member is closed");
    }
    thread::MutexLock lock(&mu_);
    sent_.push_back(std::move(bytes));
    return absl::OkStatus();
  }

  absl::StatusOr<size_t> BufferedAmount() const override { return 0; }

  absl::StatusOr<bool> IsOpen() const override { return open_; }

  absl::Status Close() override {
    open_ = false;
    return absl::OkStatus();
  }

  void* absl_nullable GetImpl() const override { return nullptr; }

  /// Delivers one frame as though it had arrived on this data channel.
  void Deliver(std::string framed) const {
    if (callbacks_.on_message) {
      callbacks_.on_message(std::move(framed));
    }
  }

  [[nodiscard]] std::vector<std::string> sent() const {
    thread::MutexLock lock(&mu_);
    return sent_;
  }

  void set_open(bool open) { open_ = open; }

 private:
  mutable thread::Mutex mu_;
  BinaryChannelCallbacks callbacks_;
  std::vector<std::string> sent_;
  bool open_ = true;
};

/// The 8-byte little-endian sequence a frame ends with.
std::string SequenceBytes(std::uint64_t sequence) {
  std::string suffix;
  for (int index = 0; index < 8; ++index) {
    suffix.push_back(static_cast<char>(sequence & 0xffU));
    sequence >>= 8U;
  }
  return suffix;
}

struct Received {
  thread::Mutex mu;
  std::vector<std::string> messages;
};

BinaryChannelCallbacks CollectInto(const std::shared_ptr<Received>& received) {
  return BinaryChannelCallbacks{.on_message = [received](std::string message) {
    thread::MutexLock lock(&received->mu);
    received->messages.push_back(std::move(message));
  }};
}

// The pin.
TEST(MultiplexedBinaryChannelTest, FramesThePayloadWithASequenceSuffix) {
  auto member = std::make_shared<FakeMember>();
  std::shared_ptr<MultiplexedBinaryChannel> channel =
      MultiplexedBinaryChannel::Create(
          {member}, {}, MultiplexedChannelOptions{.target_channels = 1});
  auto received = std::make_shared<Received>();
  ASSERT_TRUE(channel->SetCallbacks(CollectInto(received)).ok());
  ASSERT_TRUE(channel->Open().ok());

  ASSERT_TRUE(channel->Send("abc").ok());
  ASSERT_TRUE(channel->Send("de").ok());

  const std::vector<std::string> sent = member->sent();
  ASSERT_EQ(sent.size(), 2);
  // Sequence 0 and 1, little-endian, after the payload rather than before it.
  EXPECT_EQ(sent[0], std::string("abc\x00\x00\x00\x00\x00\x00\x00\x00", 11));
  EXPECT_EQ(sent[1], std::string("de\x01\x00\x00\x00\x00\x00\x00\x00", 10));
  EXPECT_TRUE(channel->Close().ok());
}

TEST(MultiplexedBinaryChannelTest, DeliversInSendOrderAcrossMembers) {
  auto first = std::make_shared<FakeMember>();
  auto second = std::make_shared<FakeMember>();
  std::shared_ptr<MultiplexedBinaryChannel> channel =
      MultiplexedBinaryChannel::Create(
          {first, second}, {}, MultiplexedChannelOptions{.target_channels = 2});
  auto received = std::make_shared<Received>();
  ASSERT_TRUE(channel->SetCallbacks(CollectInto(received)).ok());
  ASSERT_TRUE(channel->Open().ok());

  // Arriving out of order, which is the whole reason the sequence is there: a
  // packet striped onto a slower channel can land after a later one.
  first->Deliver("third" + SequenceBytes(2));
  first->Deliver("first" + SequenceBytes(0));
  second->Deliver("second" + SequenceBytes(1));

  thread::MutexLock lock(&received->mu);
  ASSERT_EQ(received->messages.size(), 3);
  EXPECT_EQ(received->messages[0], "first");
  EXPECT_EQ(received->messages[1], "second");
  EXPECT_EQ(received->messages[2], "third");
  EXPECT_TRUE(channel->Close().ok());
}

// An empty payload is a whole frame: eight bytes of sequence and nothing else.
TEST(MultiplexedBinaryChannelTest, CarriesAnEmptyPayload) {
  auto member = std::make_shared<FakeMember>();
  std::shared_ptr<MultiplexedBinaryChannel> channel =
      MultiplexedBinaryChannel::Create(
          {member}, {}, MultiplexedChannelOptions{.target_channels = 1});
  auto received = std::make_shared<Received>();
  ASSERT_TRUE(channel->SetCallbacks(CollectInto(received)).ok());
  ASSERT_TRUE(channel->Open().ok());

  ASSERT_TRUE(channel->Send("").ok());
  const std::vector<std::string> sent = member->sent();
  ASSERT_EQ(sent.size(), 1);
  EXPECT_EQ(sent[0], SequenceBytes(0));

  member->Deliver(SequenceBytes(0));
  thread::MutexLock lock(&received->mu);
  ASSERT_EQ(received->messages.size(), 1);
  EXPECT_TRUE(received->messages[0].empty());
  EXPECT_TRUE(channel->Close().ok());
}

// A frame shorter than the sequence itself cannot be placed in the aggregate
// order, so it fails the stream rather than being guessed at.
TEST(MultiplexedBinaryChannelTest, FailsOnAFrameWithoutItsSequence) {
  auto member = std::make_shared<FakeMember>();
  std::shared_ptr<MultiplexedBinaryChannel> channel =
      MultiplexedBinaryChannel::Create(
          {member}, {}, MultiplexedChannelOptions{.target_channels = 1});
  auto failure = std::make_shared<absl::Status>();
  ASSERT_TRUE(channel
                  ->SetCallbacks(BinaryChannelCallbacks{
                      .on_error =
                          [failure](absl::Status status) {
                            *failure = std::move(status);
                          }})
                  .ok());
  ASSERT_TRUE(channel->Open().ok());

  member->Deliver("short");
  EXPECT_EQ(failure->code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(channel->Close().ok());
}

// A member that fails mid-flight must not take its queued packets with it: a
// hole in the sequence stalls the peer's reorder buffer for good.
TEST(MultiplexedBinaryChannelTest, ReroutesPacketsFromAFailedMember) {
  auto failing = std::make_shared<FakeMember>();
  auto healthy = std::make_shared<FakeMember>();
  std::shared_ptr<MultiplexedBinaryChannel> channel =
      MultiplexedBinaryChannel::Create(
          {failing, healthy}, {},
          MultiplexedChannelOptions{.target_channels = 2});
  auto received = std::make_shared<Received>();
  ASSERT_TRUE(channel->SetCallbacks(CollectInto(received)).ok());
  ASSERT_TRUE(channel->Open().ok());

  failing->set_open(false);  // Its Send now fails, and the member is dropped.
  ASSERT_TRUE(channel->Send("payload").ok());
  ASSERT_TRUE(channel->Send("payload").ok());

  // Both packets ended up on the healthy member, with their sequence numbers
  // intact and neither lost.
  const std::vector<std::string> carried = healthy->sent();
  ASSERT_EQ(carried.size(), 2);
  EXPECT_EQ(carried[0], "payload" + SequenceBytes(0));
  EXPECT_EQ(carried[1], "payload" + SequenceBytes(1));
  EXPECT_TRUE(channel->Close().ok());
}

}  // namespace
}  // namespace a11::net::internal
