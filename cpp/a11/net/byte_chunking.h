// Copyright 2026 The A11 Authors.

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

enum class BytePacketType : std::uint8_t {
  kCompleteBytes = 0x00,
  kByteChunk = 0x01,
  kLengthSuffixedByteChunk = 0x02,
};

struct BytePacket {
  BytePacketType type = BytePacketType::kCompleteBytes;
  std::string payload;
  std::uint64_t transient_id = 0;
  std::uint32_t sequence = 0;
  std::uint32_t packet_count = 0;

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

struct ByteChunkingOptions {
  size_t packet_size = 64 * 1024;
  size_t max_message_size = 32 * 1024 * 1024;
  size_t max_pending_messages = 64;
  size_t max_pending_bytes = 64 * 1024 * 1024;

  absl::Status Validate() const;
};

// Packet metadata is appended as fixed-width little-endian suffixes, matching
// Action Engine's complete/chunk/length-suffixed-first-chunk wire contract.
absl::StatusOr<std::vector<std::string>> SplitBytesIntoPackets(
    std::string_view bytes, std::uint64_t transient_id, size_t packet_size);
absl::StatusOr<BytePacket> ParseBytePacket(std::string_view packet);

// Bounded, thread-safe reassembly. Chunks may arrive out of order and messages
// with different transient IDs may be interleaved.
class ByteReassembler {
 public:
  explicit ByteReassembler(ByteChunkingOptions options);
  ~ByteReassembler();

  ByteReassembler(const ByteReassembler&) = delete;
  ByteReassembler& operator=(const ByteReassembler&) = delete;

  absl::StatusOr<std::optional<std::string>> Feed(std::string packet);
  void Clear();

  [[nodiscard]] size_t pending_message_count() const;
  [[nodiscard]] size_t pending_byte_count() const;

 private:
  struct Impl;
  static constexpr size_t kImplSize = 256;
  static constexpr size_t kImplAlignment = alignof(std::max_align_t);

  Impl* absl_nonnull GetImpl();
  const Impl* absl_nonnull GetImpl() const;

  alignas(kImplAlignment) std::byte impl_[kImplSize];
};

}  // namespace a11::net

#endif  // A11_NET_BYTE_CHUNKING_H_
