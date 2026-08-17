// Copyright 2026 The A11 Authors.

#include "a11/net/byte_chunking.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <gtest/gtest.h>

namespace a11::net {
namespace {

TEST(ByteChunkingTest, CompletePacketRoundTrips) {
  auto packets = SplitBytesIntoPackets("hello", 0x0102030405060708ULL, 32);
  ASSERT_TRUE(packets.ok()) << packets.status();
  ASSERT_EQ(packets->size(), 1);
  EXPECT_EQ(packets->front().size(), 14);
  EXPECT_EQ(static_cast<unsigned char>(packets->front().back()),
            static_cast<unsigned char>(BytePacketType::kCompleteBytes));

  auto parsed = ParseBytePacket(packets->front());
  ASSERT_TRUE(parsed.ok()) << parsed.status();
  EXPECT_EQ(parsed->payload, "hello");
  EXPECT_EQ(parsed->transient_id, 0x0102030405060708ULL);

  ByteChunkingOptions options{.packet_size = 32,
                              .max_message_size = 1024,
                              .max_pending_messages = 4,
                              .max_pending_bytes = 4096};
  ByteReassembler reassembler(options);
  auto complete = reassembler.Feed(std::move(packets->front()));
  ASSERT_TRUE(complete.ok()) << complete.status();
  ASSERT_TRUE(complete->has_value());
  EXPECT_EQ(**complete, "hello");
  EXPECT_EQ(reassembler.pending_message_count(), 0);
  EXPECT_EQ(reassembler.pending_byte_count(), 0);
}

// The owning entry points exist to avoid copying the payload, so what has to
// hold is that they produce exactly what the borrowing ones do -- for a message
// that fits one packet, for one that does not, and for a packet that is
// malformed.
TEST(ByteChunkingTest, OwningSplitAndParseMatchTheBorrowingOnes) {
  for (const size_t size : {size_t{0}, size_t{5}, size_t{22}, size_t{200}}) {
    const std::string message(size, 'q');
    auto borrowed = SplitBytesIntoPackets(message, 42, 32);
    auto owned = SplitOwnedBytesIntoPackets(message, 42, 32);
    ASSERT_TRUE(borrowed.ok()) << borrowed.status();
    ASSERT_TRUE(owned.ok()) << owned.status();
    EXPECT_EQ(*owned, *borrowed) << "at size " << size;

    for (const std::string& packet : *borrowed) {
      auto by_view = ParseBytePacket(packet);
      auto by_value = ParseOwnedBytePacket(packet);
      ASSERT_TRUE(by_view.ok()) << by_view.status();
      ASSERT_TRUE(by_value.ok()) << by_value.status();
      EXPECT_EQ(by_value->payload, by_view->payload);
      EXPECT_EQ(by_value->type, by_view->type);
      EXPECT_EQ(by_value->transient_id, by_view->transient_id);
      EXPECT_EQ(by_value->sequence, by_view->sequence);
      EXPECT_EQ(by_value->packet_count, by_view->packet_count);
    }
  }
  EXPECT_FALSE(ParseOwnedBytePacket("short").ok());
  EXPECT_FALSE(SplitOwnedBytesIntoPackets("x", 1, 4).ok());
}

TEST(ByteChunkingTest, ReassemblesOutOfOrderAndReleasesState) {
  const std::string message(200, 'x');
  auto packets = SplitBytesIntoPackets(message, 7, 32);
  ASSERT_TRUE(packets.ok()) << packets.status();
  ASSERT_GT(packets->size(), 2);
  std::rotate(packets->begin(), packets->begin() + 1, packets->end());

  ByteChunkingOptions options{.packet_size = 32,
                              .max_message_size = 1024,
                              .max_pending_messages = 4,
                              .max_pending_bytes = 4096};
  ByteReassembler reassembler(options);
  std::optional<std::string> result;
  for (std::string& packet : *packets) {
    auto fed = reassembler.Feed(std::move(packet));
    ASSERT_TRUE(fed.ok()) << fed.status();
    if (fed->has_value()) {
      result = std::move(**fed);
    }
  }
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, message);
  EXPECT_EQ(reassembler.pending_message_count(), 0);
  EXPECT_EQ(reassembler.pending_byte_count(), 0);
}

TEST(ByteChunkingTest, InterleavedMessagesRemainIndependent) {
  auto first = SplitBytesIntoPackets(std::string(100, 'a'), 10, 24);
  auto second = SplitBytesIntoPackets(std::string(100, 'b'), 11, 24);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  ASSERT_EQ(first->size(), second->size());

  ByteChunkingOptions options{.packet_size = 24,
                              .max_message_size = 1024,
                              .max_pending_messages = 4,
                              .max_pending_bytes = 4096};
  ByteReassembler reassembler(options);
  std::vector<std::string> completed;
  for (size_t index = 0; index < first->size(); ++index) {
    for (std::string* packet : {&(*first)[index], &(*second)[index]}) {
      auto fed = reassembler.Feed(std::move(*packet));
      ASSERT_TRUE(fed.ok()) << fed.status();
      if (fed->has_value()) {
        completed.push_back(std::move(**fed));
      }
    }
  }
  ASSERT_EQ(completed.size(), 2);
  EXPECT_EQ(completed[0], std::string(100, 'a'));
  EXPECT_EQ(completed[1], std::string(100, 'b'));
  EXPECT_EQ(reassembler.pending_message_count(), 0);
}

TEST(ByteChunkingTest, RejectsDuplicateWithoutGrowingAccounting) {
  auto packets = SplitBytesIntoPackets(std::string(100, 'z'), 99, 24);
  ASSERT_TRUE(packets.ok());
  ByteChunkingOptions options{.packet_size = 24,
                              .max_message_size = 1024,
                              .max_pending_messages = 4,
                              .max_pending_bytes = 4096};
  ByteReassembler reassembler(options);
  const std::string duplicate = packets->front();
  auto first = reassembler.Feed(std::move(packets->front()));
  ASSERT_TRUE(first.ok());
  const size_t bytes = reassembler.pending_byte_count();
  auto repeated = reassembler.Feed(duplicate);
  EXPECT_TRUE(absl::IsAlreadyExists(repeated.status()));
  EXPECT_EQ(reassembler.pending_byte_count(), bytes);
  reassembler.Clear();
  EXPECT_EQ(reassembler.pending_message_count(), 0);
  EXPECT_EQ(reassembler.pending_byte_count(), 0);
}

TEST(ByteChunkingTest, BoundsPendingMessagesAndRejectsMalformedPackets) {
  ByteChunkingOptions options{.packet_size = 24,
                              .max_message_size = 1024,
                              .max_pending_messages = 1,
                              .max_pending_bytes = 4096};
  ByteReassembler reassembler(options);
  auto first = SplitBytesIntoPackets(std::string(100, 'a'), 1, 24);
  auto second = SplitBytesIntoPackets(std::string(100, 'b'), 2, 24);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(reassembler.Feed(std::move(first->back())).ok());
  auto exhausted = reassembler.Feed(std::move(second->back()));
  EXPECT_TRUE(absl::IsResourceExhausted(exhausted.status()));

  auto malformed = ParseBytePacket("short");
  EXPECT_TRUE(absl::IsInvalidArgument(malformed.status()));
  std::string unknown(9, '\0');
  unknown.back() = static_cast<char>(0x7f);
  malformed = ParseBytePacket(unknown);
  EXPECT_TRUE(absl::IsInvalidArgument(malformed.status()));
}

}  // namespace
}  // namespace a11::net
