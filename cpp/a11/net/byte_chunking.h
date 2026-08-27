// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Bounded packetisation and reassembly for binary channel transports.
 *
 * WebSocket and WebRTC channels use this shared format so a
 * large WireMessage cannot monopolise a channel or exceed transport limits.
 */

#ifndef A11_NET_BYTE_CHUNKING_H_
#define A11_NET_BYTE_CHUNKING_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

namespace a11::net {

/// Packet shapes in the A11 byte-chunking wire format.
enum class BytePacketType : std::uint8_t {
  kCompleteBytes = 0x00,
  kByteChunk = 0x01,
  kLengthSuffixedByteChunk = 0x02,
};

/// Parsed packet metadata plus the owned piece of application payload.
struct BytePacket {
  BytePacketType type = BytePacketType::kCompleteBytes;  ///< Packet shape.
  std::string payload;  ///< Bytes contributed by this packet.
  std::uint64_t transient_id = 0;
  std::uint32_t sequence = 0;  ///< Zero-based position in the message.
  std::uint32_t packet_count =
      0;  ///< Total count, when supplied by the first packet.

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const BytePacket& packet) {
    sink.Append("BytePacket{type=");
    sink.Append(packet.type == BytePacketType::kCompleteBytes ? "complete"
                : packet.type == BytePacketType::kByteChunk   ? "chunk"
                                                              : "first_chunk");
    sink.Append(", id=");
    sink.Append(packet.transient_id);
    sink.Append(", sequence=");
    sink.Append(packet.sequence);
    sink.Append(", packet_count=");
    sink.Append(packet.packet_count);
    sink.Append(", payload_size=");
    sink.Append(packet.payload.size());
    sink.Append("}");
  }
};

/** Bounds packet size and incomplete-message memory during reassembly. */
struct ByteChunkingOptions {
  size_t packet_size = 64 * 1024;              ///< Maximum encoded packet size.
  size_t max_message_size = 32 * 1024 * 1024;  ///< Reassembled message limit.
  size_t max_pending_messages = 64;  ///< Simultaneous incomplete messages.
  size_t max_pending_bytes =
      64 * 1024 * 1024;  ///< Aggregate pending payload limit.

  /// Validate that all limits can represent at least one useful packet.
  absl::Status Validate() const;
};

/// Split bytes into A11 packets with fixed little-endian suffixes.
absl::StatusOr<std::vector<std::string>> SplitBytesIntoPackets(
    std::string_view bytes, std::uint64_t transient_id, size_t packet_size);
/**
 * @brief Split bytes the caller owns, reusing the buffer when it fits a packet.
 *
 * Appends metadata to the owned buffer when the message fits one packet. The
 * multi-packet result matches SplitBytesIntoPackets.
 */
absl::StatusOr<std::vector<std::string>> SplitOwnedBytesIntoPackets(
    std::string bytes, std::uint64_t transient_id, size_t packet_size);
/// Parse and validate one packet without retaining the input view.
absl::StatusOr<BytePacket> ParseBytePacket(std::string_view packet);
/**
 * @brief Parse one packet, reusing its buffer as the payload.
 *
 * Removes the metadata suffix in place and returns the remaining owned payload.
 */
absl::StatusOr<BytePacket> ParseOwnedBytePacket(std::string packet);

/**
 * @brief Bounded, thread-safe reassembly for interleaved binary messages.
 *
 * Feed packets as a transport receives them. Packets for one message may be
 * out of order and packets for several messages may interleave; Feed() returns
 * an owned message only when all its pieces are present. Configured bounds
 * protect an agent endpoint from unbounded partial-message memory.
 */
class ByteReassembler {
 public:
  /// Construct a reassembler with already validated limits.
  explicit ByteReassembler(ByteChunkingOptions options);
  ~ByteReassembler();

  ByteReassembler(const ByteReassembler&) = delete;
  ByteReassembler& operator=(const ByteReassembler&) = delete;

  /// Admit one packet and return a complete message when this finishes one.
  absl::StatusOr<std::optional<std::string>> Feed(std::string packet);
  /// Discard every incomplete message, for example when a channel aborts.
  void Clear();

  /// Number of message ids currently awaiting more packets.
  [[nodiscard]] size_t pending_message_count() const;
  /// Aggregate payload bytes retained by incomplete messages.
  [[nodiscard]] size_t pending_byte_count() const;

 private:
  struct Impl;
  static constexpr size_t kImplSize = 256;
  static constexpr size_t kImplAlignment = alignof(std::max_align_t);

  Impl* absl_nonnull GetImpl();
  [[nodiscard]] const Impl* absl_nonnull GetImpl() const;

  alignas(kImplAlignment) std::byte impl_[kImplSize];
};

}  // namespace a11::net

#endif  // A11_NET_BYTE_CHUNKING_H_
