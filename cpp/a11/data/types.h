// Copyright 2026 The A11 Authors.

#ifndef A11_DATA_TYPES_H_
#define A11_DATA_TYPES_H_

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

namespace a11::data {

using Bytes = std::string;
using ByteMap = absl::flat_hash_map<std::string, Bytes>;

absl::Status ValidateName(std::string_view name);

struct ChunkMetadata {
  std::string mimetype;
  std::optional<absl::Time> timestamp{};
  ByteMap attributes{};

  [[nodiscard]] size_t ApproxBytes() const;
  [[nodiscard]] std::string DebugString() const;
  absl::Status Validate() const;
  absl::StatusOr<std::string> GetAttribute(std::string_view key) const;
  absl::Status SetAttribute(std::string key, std::string value);

  absl::StatusOr<Bytes> ToMsgpack() const;
  static absl::StatusOr<ChunkMetadata> FromMsgpack(std::string_view bytes);

  friend bool operator==(const ChunkMetadata&, const ChunkMetadata&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ChunkMetadata& value) {
    sink.Append(value.DebugString());
  }
};

struct Chunk {
  std::optional<ChunkMetadata> metadata{};
  std::string ref{};
  Bytes data{};

  [[nodiscard]] size_t ApproxBytes() const;
  [[nodiscard]] std::string DebugString() const;
  [[nodiscard]] std::string GetMimetype() const;
  [[nodiscard]] bool IsEmpty() const;
  [[nodiscard]] bool IsNull() const;
  absl::Status Validate() const;

  absl::StatusOr<Bytes> ToMsgpack() const;
  static absl::StatusOr<Chunk> FromMsgpack(std::string_view bytes);

  friend bool operator==(const Chunk&, const Chunk&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Chunk& value) {
    sink.Append(value.DebugString());
  }
};

struct NodeRef {
  std::string id;
  std::uint32_t offset = 0;
  // 2^32 is a valid length for a full logical node and therefore requires a
  // wider representation than offset and sequence numbers.
  std::optional<std::uint64_t> length;

  [[nodiscard]] size_t ApproxBytes() const;
  [[nodiscard]] std::string DebugString() const;
  absl::Status Validate() const;

  absl::StatusOr<Bytes> ToMsgpack() const;
  static absl::StatusOr<NodeRef> FromMsgpack(std::string_view bytes);

  friend bool operator==(const NodeRef&, const NodeRef&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const NodeRef& value) {
    sink.Append(value.DebugString());
  }
};

struct NodeFragment {
  std::string id;
  std::variant<Chunk, NodeRef> data = Chunk{};
  std::optional<std::uint32_t> seq;
  bool continued = false;

  [[nodiscard]] size_t ApproxBytes() const;
  [[nodiscard]] std::string DebugString() const;
  absl::Status Validate() const;
  absl::StatusOr<Chunk* absl_nonnull> GetChunk();
  absl::StatusOr<const Chunk* absl_nonnull> GetChunk() const;
  absl::StatusOr<NodeRef* absl_nonnull> GetNodeRef();
  absl::StatusOr<const NodeRef* absl_nonnull> GetNodeRef() const;

  absl::StatusOr<Bytes> ToMsgpack() const;
  static absl::StatusOr<NodeFragment> FromMsgpack(std::string_view bytes);

  friend bool operator==(const NodeFragment&, const NodeFragment&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const NodeFragment& value) {
    sink.Append(value.DebugString());
  }
};

struct Port {
  std::string name;
  std::string id;

  [[nodiscard]] size_t ApproxBytes() const;
  [[nodiscard]] std::string DebugString() const;
  absl::Status Validate() const;

  absl::StatusOr<Bytes> ToMsgpack() const;
  static absl::StatusOr<Port> FromMsgpack(std::string_view bytes);

  friend bool operator==(const Port&, const Port&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Port& value) {
    sink.Append(value.DebugString());
  }
};

struct ActionMessage {
  std::string id;
  std::string name;
  std::vector<Port> inputs{};
  std::vector<Port> outputs{};
  ByteMap headers{};

  [[nodiscard]] size_t ApproxBytes() const;
  [[nodiscard]] std::string DebugString() const;
  absl::Status Validate() const;

  absl::StatusOr<Bytes> ToMsgpack() const;
  static absl::StatusOr<ActionMessage> FromMsgpack(std::string_view bytes);

  friend bool operator==(const ActionMessage&, const ActionMessage&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const ActionMessage& value) {
    sink.Append(value.DebugString());
  }
};

struct WireMessage {
  static constexpr std::uint32_t kVersion = 1;

  std::vector<NodeFragment> node_fragments{};
  std::vector<ActionMessage> actions{};
  ByteMap headers{};

  [[nodiscard]] size_t ApproxBytes() const;
  [[nodiscard]] std::string DebugString() const;
  absl::Status Validate() const;

  absl::StatusOr<Bytes> ToMsgpack() const;
  static absl::StatusOr<WireMessage> FromMsgpack(std::string_view bytes);

  friend bool operator==(const WireMessage&, const WireMessage&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const WireMessage& value) {
    sink.Append(value.DebugString());
  }
};

bool IsHalfCloseMessage(const WireMessage& message);
WireMessage MakeHalfCloseMessage(ByteMap trailers = {});

// Calculated from the same encoder rather than repeated as a magic constant.
size_t EmptyWireMessageSize();

}  // namespace a11::data

#endif  // A11_DATA_TYPES_H_
