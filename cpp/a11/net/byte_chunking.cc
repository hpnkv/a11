// Copyright 2026 The A11 Authors.

#include "a11/net/byte_chunking.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/base/thread_annotations.h>
#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>

#include "thread/boost_primitives.h"

namespace a11::net {
namespace {

constexpr size_t kCompleteMetadataSize = sizeof(std::uint64_t) + 1;
constexpr size_t kChunkMetadataSize =
    sizeof(std::uint32_t) + sizeof(std::uint64_t) + 1;
constexpr size_t kFirstChunkMetadataSize =
    sizeof(std::uint32_t) + sizeof(std::uint32_t) + sizeof(std::uint64_t) + 1;
constexpr size_t kMinimumPacketSize = kFirstChunkMetadataSize + 1;

template <typename Integer>
void AppendLittleEndian(std::string* output, Integer value) {
  for (size_t index = 0; index < sizeof(Integer); ++index) {
    output->push_back(static_cast<char>(value & 0xffU));
    value >>= 8U;
  }
}

template <typename Integer>
Integer ReadLittleEndian(std::string_view input, size_t offset) {
  Integer result = 0;
  for (size_t index = 0; index < sizeof(Integer); ++index) {
    result |=
        static_cast<Integer>(static_cast<unsigned char>(input[offset + index]))
        << (index * 8U);
  }
  return result;
}

std::string SerializeComplete(std::string_view payload,
                              std::uint64_t transient_id) {
  std::string packet;
  packet.reserve(payload.size() + kCompleteMetadataSize);
  packet.append(payload);
  AppendLittleEndian(&packet, transient_id);
  packet.push_back(static_cast<char>(BytePacketType::kCompleteBytes));
  return packet;
}

std::string SerializeChunk(std::string_view payload, std::uint64_t transient_id,
                           std::uint32_t sequence,
                           std::optional<std::uint32_t> packet_count) {
  std::string packet;
  packet.reserve(payload.size() + (packet_count.has_value()
                                       ? kFirstChunkMetadataSize
                                       : kChunkMetadataSize));
  packet.append(payload);
  if (packet_count.has_value()) {
    AppendLittleEndian(&packet, *packet_count);
  }
  AppendLittleEndian(&packet, sequence);
  AppendLittleEndian(&packet, transient_id);
  packet.push_back(static_cast<char>(
      packet_count.has_value() ? BytePacketType::kLengthSuffixedByteChunk
                               : BytePacketType::kByteChunk));
  return packet;
}

}  // namespace

absl::Status ByteChunkingOptions::Validate() const {
  if (packet_size < kMinimumPacketSize) {
    return absl::InvalidArgumentError(
        absl::StrCat("byte packet_size must be at least ", kMinimumPacketSize));
  }
  if (max_message_size == 0 || max_pending_messages == 0 ||
      max_pending_bytes == 0) {
    return absl::InvalidArgumentError(
        "byte chunking limits must all be positive");
  }
  if (packet_size > max_message_size + kCompleteMetadataSize) {
    return absl::InvalidArgumentError(
        "byte packet_size must not exceed max_message_size plus metadata");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::vector<std::string>> SplitBytesIntoPackets(
    std::string_view bytes, std::uint64_t transient_id, size_t packet_size) {
  if (packet_size < kMinimumPacketSize) {
    return absl::InvalidArgumentError(
        absl::StrCat("byte packet_size must be at least ", kMinimumPacketSize));
  }
  if (bytes.size() <= packet_size - kCompleteMetadataSize) {
    std::vector<std::string> packets;
    packets.reserve(1);
    packets.push_back(SerializeComplete(bytes, transient_id));
    return packets;
  }

  const size_t first_payload_size = packet_size - kFirstChunkMetadataSize;
  const size_t later_payload_size = packet_size - kChunkMetadataSize;
  const size_t remaining = bytes.size() - first_payload_size;
  const size_t later_count =
      (remaining + later_payload_size - 1) / later_payload_size;
  if (later_count >= std::numeric_limits<std::uint32_t>::max()) {
    return absl::OutOfRangeError("byte message requires too many packets");
  }
  const auto packet_count = static_cast<std::uint32_t>(later_count + 1);

  std::vector<std::string> packets;
  packets.reserve(packet_count);
  packets.push_back(SerializeChunk(bytes.substr(0, first_payload_size),
                                   transient_id, 0, packet_count));
  size_t offset = first_payload_size;
  for (std::uint32_t sequence = 1; offset < bytes.size(); ++sequence) {
    const size_t payload_size =
        std::min(later_payload_size, bytes.size() - offset);
    packets.push_back(SerializeChunk(bytes.substr(offset, payload_size),
                                     transient_id, sequence, std::nullopt));
    offset += payload_size;
  }
  return packets;
}

namespace {

// Everything about a packet except its payload bytes, plus how many trailing
// bytes the metadata occupies. Shared by the two parse entry points so that the
// validation cannot drift between an owning and a borrowing caller.
absl::StatusOr<size_t> ParseBytePacketMetadata(std::string_view packet,
                                               BytePacket* parsed) {
  if (packet.size() < kCompleteMetadataSize) {
    return absl::InvalidArgumentError(
        "byte packet is shorter than complete-packet metadata");
  }
  const auto type =
      static_cast<BytePacketType>(static_cast<unsigned char>(packet.back()));
  if (type != BytePacketType::kCompleteBytes &&
      type != BytePacketType::kByteChunk &&
      type != BytePacketType::kLengthSuffixedByteChunk) {
    return absl::InvalidArgumentError("byte packet has an unknown type");
  }

  size_t metadata_size = kCompleteMetadataSize;
  if (type == BytePacketType::kByteChunk) {
    metadata_size = kChunkMetadataSize;
  } else if (type == BytePacketType::kLengthSuffixedByteChunk) {
    metadata_size = kFirstChunkMetadataSize;
  }
  if (packet.size() < metadata_size) {
    return absl::InvalidArgumentError(
        "byte packet is shorter than its declared metadata");
  }

  const size_t transient_offset = packet.size() - kCompleteMetadataSize;
  parsed->type = type;
  parsed->transient_id =
      ReadLittleEndian<std::uint64_t>(packet, transient_offset);
  parsed->sequence = 0;
  parsed->packet_count = 0;
  if (type != BytePacketType::kCompleteBytes) {
    const size_t sequence_offset = transient_offset - sizeof(std::uint32_t);
    parsed->sequence = ReadLittleEndian<std::uint32_t>(packet, sequence_offset);
    if (type == BytePacketType::kLengthSuffixedByteChunk) {
      const size_t count_offset = sequence_offset - sizeof(std::uint32_t);
      parsed->packet_count =
          ReadLittleEndian<std::uint32_t>(packet, count_offset);
      if (parsed->sequence != 0 || parsed->packet_count == 0) {
        return absl::InvalidArgumentError(
            "first byte chunk must have sequence zero and a positive count");
      }
    }
  }
  return metadata_size;
}

}  // namespace

absl::StatusOr<std::vector<std::string>> SplitOwnedBytesIntoPackets(
    std::string bytes, std::uint64_t transient_id, size_t packet_size) {
  if (packet_size < kMinimumPacketSize) {
    return absl::InvalidArgumentError(
        absl::StrCat("byte packet_size must be at least ", kMinimumPacketSize));
  }
  if (bytes.size() > packet_size - kCompleteMetadataSize) {
    return SplitBytesIntoPackets(bytes, transient_id, packet_size);
  }
  AppendLittleEndian(&bytes, transient_id);
  bytes.push_back(static_cast<char>(BytePacketType::kCompleteBytes));
  std::vector<std::string> packets;
  packets.reserve(1);
  packets.push_back(std::move(bytes));
  return packets;
}

absl::StatusOr<BytePacket> ParseBytePacket(std::string_view packet) {
  BytePacket result;
  ABSL_ASSIGN_OR_RETURN(const size_t metadata_size,
                        ParseBytePacketMetadata(packet, &result));
  result.payload = std::string(packet.substr(0, packet.size() - metadata_size));
  return result;
}

absl::StatusOr<BytePacket> ParseOwnedBytePacket(std::string packet) {
  BytePacket result;
  ABSL_ASSIGN_OR_RETURN(const size_t metadata_size,
                        ParseBytePacketMetadata(packet, &result));
  packet.resize(packet.size() - metadata_size);
  result.payload = std::move(packet);
  return result;
}

struct ByteReassembler::Impl {
  struct Pending {
    std::optional<std::uint32_t> packet_count;
    absl::flat_hash_map<std::uint32_t, std::string> chunks;
    size_t byte_count = 0;
  };

  explicit Impl(ByteChunkingOptions value_options) : options(value_options) {}

  mutable thread::Mutex mu;
  const ByteChunkingOptions options;
  absl::flat_hash_map<std::uint64_t, Pending> pending ABSL_GUARDED_BY(mu);
  size_t pending_bytes ABSL_GUARDED_BY(mu) = 0;
};

ByteReassembler::ByteReassembler(ByteChunkingOptions options) {
  static_assert(sizeof(Impl) <= kImplSize);
  static_assert(alignof(Impl) <= kImplAlignment);
  std::construct_at(reinterpret_cast<Impl*>(impl_), options);
}

ByteReassembler::~ByteReassembler() {
  std::destroy_at(GetImpl());
}

ByteReassembler::Impl* ByteReassembler::GetImpl() {
  return std::launder(reinterpret_cast<Impl*>(impl_));
}

const ByteReassembler::Impl* ByteReassembler::GetImpl() const {
  return std::launder(reinterpret_cast<const Impl*>(impl_));
}

absl::StatusOr<std::optional<std::string>> ByteReassembler::Feed(
    std::string serialized) {
  Impl* impl = GetImpl();
  if (serialized.size() > impl->options.packet_size) {
    return absl::OutOfRangeError("incoming byte packet exceeds packet_size");
  }
  ABSL_ASSIGN_OR_RETURN(BytePacket packet,
                        ParseOwnedBytePacket(std::move(serialized)));
  if (packet.payload.size() > impl->options.max_message_size) {
    return absl::OutOfRangeError("incoming byte message exceeds its limit");
  }

  thread::MutexLock lock(&impl->mu);
  if (packet.type == BytePacketType::kCompleteBytes) {
    if (impl->pending.find(packet.transient_id) != impl->pending.end()) {
      return absl::AlreadyExistsError(
          "complete byte packet collides with pending chunks");
    }
    return std::optional<std::string>(std::move(packet.payload));
  }

  if (packet.sequence > impl->options.max_message_size) {
    return absl::OutOfRangeError(
        "byte chunk sequence exceeds the configured message bound");
  }
  if (packet.type == BytePacketType::kLengthSuffixedByteChunk &&
      static_cast<size_t>(packet.packet_count) >
          impl->options.max_message_size + 1) {
    return absl::OutOfRangeError(
        "byte message declares an unreasonable packet count");
  }
  if (impl->pending_bytes + packet.payload.size() >
      impl->options.max_pending_bytes) {
    return absl::ResourceExhaustedError(
        "pending byte chunks exceed max_pending_bytes");
  }

  auto found = impl->pending.find(packet.transient_id);
  if (found == impl->pending.end()) {
    if (impl->pending.size() >= impl->options.max_pending_messages) {
      return absl::ResourceExhaustedError(
          "too many byte messages are pending reassembly");
    }
    found = impl->pending.try_emplace(packet.transient_id).first;
  }
  Impl::Pending& pending = found->second;

  if (packet.type == BytePacketType::kLengthSuffixedByteChunk) {
    if (pending.packet_count.has_value() &&
        *pending.packet_count != packet.packet_count) {
      return absl::InvalidArgumentError(
          "byte message has conflicting packet counts");
    }
    for (const auto& [sequence, chunk] : pending.chunks) {
      (void)chunk;
      if (sequence >= packet.packet_count) {
        impl->pending_bytes -= pending.byte_count;
        impl->pending.erase(found);
        return absl::OutOfRangeError(
            "byte chunk sequence exceeds the declared packet count");
      }
    }
    pending.packet_count = packet.packet_count;
  }
  if (pending.packet_count.has_value() &&
      packet.sequence >= *pending.packet_count) {
    return absl::OutOfRangeError(
        "byte chunk sequence exceeds the declared packet count");
  }
  if (pending.chunks.find(packet.sequence) != pending.chunks.end()) {
    return absl::AlreadyExistsError("duplicate byte chunk sequence");
  }
  if (pending.byte_count + packet.payload.size() >
      impl->options.max_message_size) {
    return absl::OutOfRangeError(
        "reassembled byte message exceeds max_message_size");
  }
  pending.byte_count += packet.payload.size();
  impl->pending_bytes += packet.payload.size();
  pending.chunks.emplace(packet.sequence, std::move(packet.payload));

  if (!pending.packet_count.has_value() ||
      pending.chunks.size() != *pending.packet_count) {
    return std::optional<std::string>();
  }
  for (std::uint32_t sequence = 0; sequence < *pending.packet_count;
       ++sequence) {
    if (pending.chunks.find(sequence) == pending.chunks.end()) {
      return std::optional<std::string>();
    }
  }

  std::string message;
  message.reserve(pending.byte_count);
  for (std::uint32_t sequence = 0; sequence < *pending.packet_count;
       ++sequence) {
    auto chunk = pending.chunks.find(sequence);
    message.append(chunk->second);
  }
  impl->pending_bytes -= pending.byte_count;
  impl->pending.erase(found);
  return std::optional<std::string>(std::move(message));
}

void ByteReassembler::Clear() {
  Impl* impl = GetImpl();
  thread::MutexLock lock(&impl->mu);
  impl->pending.clear();
  impl->pending_bytes = 0;
}

size_t ByteReassembler::pending_message_count() const {
  const Impl* impl = GetImpl();
  thread::MutexLock lock(&impl->mu);
  return impl->pending.size();
}

size_t ByteReassembler::pending_byte_count() const {
  const Impl* impl = GetImpl();
  thread::MutexLock lock(&impl->mu);
  return impl->pending_bytes;
}

}  // namespace a11::net
