// Copyright 2026 The A11 Authors.

#include "a11/data/types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/data/msgpack.h"

namespace a11::data {
namespace {

constexpr std::uint64_t kUint32Range =
    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
constexpr size_t kDebugPreviewMaxHeaders = 32;
constexpr size_t kDebugPreviewMaxActions = 16;
constexpr size_t kDebugPreviewMaxFragments = 16;
constexpr size_t kDebugPreviewMaxNameChars = 96;
constexpr size_t kDebugPreviewMaxContentChars = 160;
// Hex escapes consume four display characters per byte. Keeping this preview
// to 24 bytes leaves room for the explicit byte-omission count within the
// 160-character fragment preview.
constexpr size_t kDebugPreviewMaxContentBytes = 24;
constexpr size_t kDebugStringMaxChars = 24 * 1024;

constexpr bool IsAsciiAlpha(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

constexpr bool IsNameBoundary(char value) {
  return IsAsciiAlpha(value) || (value >= '0' && value <= '9') || value == '_';
}

constexpr bool IsNameMiddle(char value) {
  return IsNameBoundary(value) || value == '-' || value == '#';
}

absl::StatusOr<std::uint64_t> JsonUnsigned(const nlohmann::json& value,
                                           std::string_view field) {
  // Safe without a guard: each get is preceded by the check that makes it
  // well-typed, which is the convention in A11's JSON code -- see
  // a11/json_codec.h for where that convention's boundary is.
  if (value.is_number_unsigned()) {
    return value.get<std::uint64_t>();
  }
  if (value.is_number_integer()) {
    const std::int64_t result = value.get<std::int64_t>();
    if (result >= 0) {
      return static_cast<std::uint64_t>(result);
    }
  }
  return absl::InvalidArgumentError(
      absl::StrCat(field, " must be a non-negative integer"));
}

absl::StatusOr<std::string> JsonString(const nlohmann::json& value,
                                       std::string_view field) {
  if (!value.is_string()) {
    return absl::InvalidArgumentError(absl::StrCat(field, " must be a string"));
  }
  // Safe without a guard: the is_string() check above is what makes the get
  // well-typed, and a get that matches the value's type cannot fail.
  return value.get<std::string>();
}

void PackByteMap(MsgpackWriter* absl_nonnull writer,
                 const ByteMap& values) {
  // Sorted, because nlohmann's object type is ordered and a ByteMap is a hash
  // map: sorting is what keeps these bytes identical to the JSON path's, and
  // also the only thing that makes the encoding deterministic at all.
  std::vector<const ByteMap::value_type*> entries;
  entries.reserve(values.size());
  for (const auto& entry : values) {
    entries.push_back(&entry);
  }
  std::sort(entries.begin(), entries.end(),
            [](const ByteMap::value_type* left,
               const ByteMap::value_type* right) {
              return left->first < right->first;
            });
  writer->PackMapHeader(entries.size());
  for (const ByteMap::value_type* entry : entries) {
    writer->PackString(entry->first);
    writer->PackBinary(entry->second);
  }
}

absl::StatusOr<ByteMap> DecodeByteMap(const nlohmann::json& value,
                                      std::string_view field) {
  if (!value.is_object()) {
    return absl::InvalidArgumentError(absl::StrCat(field, " must be a map"));
  }
  ByteMap result;
  for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
    absl::Status name_status = ValidateName(iterator.key());
    if (!name_status.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid key in ", field, ": ", name_status.message()));
    }
    ABSL_ASSIGN_OR_RETURN(std::string bytes,
                          GetBinary(iterator.value(), field));
    result.emplace(iterator.key(), std::move(bytes));
  }
  return result;
}

absl::StatusOr<nlohmann::json> ReadField(MsgpackReader* reader,
                                         std::string_view field) {
  absl::StatusOr<nlohmann::json> value = reader->Read();
  if (!value.ok()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Failed to read ", field, ": ", value.status().message()));
  }
  return value;
}

std::string BoundedText(std::string_view value, size_t limit) {
  if (value.size() <= limit) {
    return std::string(value);
  }
  size_t prefix_size = limit;
  std::string suffix;
  for (int index = 0; index < 3; ++index) {
    const size_t omitted = value.size() - prefix_size;
    suffix = absl::StrCat("... <", omitted, " chars omitted>");
    prefix_size = limit > suffix.size() ? limit - suffix.size() : 0;
  }
  return absl::StrCat(value.substr(0, prefix_size), suffix);
}

std::string DebugChunk(const Chunk& chunk) {
  if (!chunk.ref.empty()) {
    return absl::StrCat(
        "ref='", BoundedText(chunk.ref, kDebugPreviewMaxContentChars), "'");
  }
  if (chunk.data.empty()) {
    return "<empty chunk>";
  }
  const size_t shown =
      std::min(chunk.data.size(), kDebugPreviewMaxContentBytes);
  std::string preview;
  preview.reserve(shown * 4);
  constexpr char kHex[] = "0123456789abcdef";
  for (size_t index = 0; index < shown; ++index) {
    const unsigned char byte = static_cast<unsigned char>(chunk.data[index]);
    preview.push_back('\\');
    preview.push_back('x');
    preview.push_back(kHex[byte >> 4U]);
    preview.push_back(kHex[byte & 0x0fU]);
  }
  if (shown < chunk.data.size()) {
    absl::StrAppend(&preview, " ... <", chunk.data.size() - shown,
                    " bytes omitted>");
  }
  return absl::StrCat("data=<", chunk.data.size(), " bytes> ", preview);
}

}  // namespace

absl::Status ValidateName(std::string_view name) {
  if (name.empty() || name.size() > 255) {
    return absl::InvalidArgumentError(
        "name must contain between 1 and 255 characters");
  }
  if (!IsNameBoundary(name.front()) || !IsNameBoundary(name.back())) {
    return absl::InvalidArgumentError(
        "name must start and end with an ASCII letter, digit, or underscore");
  }
  // One table lookup per character rather than the four comparisons and two
  // calls that the predicate form compiles to. This runs over every id in every
  // record A11 encodes or decodes -- it was 2.19% of the server benchmark's
  // profile once the allocator stopped dominating -- and the table is 256 bytes
  // resolved at compile time.
  static constexpr std::array<bool, 256> kAllowed = [] {
    std::array<bool, 256> allowed{};
    for (int value = 0; value < 256; ++value) {
      const char character = static_cast<char>(value);
      allowed[static_cast<size_t>(value)] = IsNameMiddle(character);
    }
    return allowed;
  }();
  for (const char character : name) {
    if (!kAllowed[static_cast<unsigned char>(character)]) {
      return absl::InvalidArgumentError(
          "name contains a character outside [a-zA-Z0-9-_#]");
    }
  }
  return absl::OkStatus();
}

size_t ChunkMetadata::ApproxBytes() const {
  size_t result = mimetype.size() + 8 + 1;
  for (const auto& [key, value] : attributes) {
    result += key.size() + value.size();
  }
  return result;
}

std::string ChunkMetadata::DebugString() const {
  return absl::StrCat("ChunkMetadata(mimetype='",
                      BoundedText(mimetype, kDebugPreviewMaxNameChars),
                      "', timestamp=", timestamp.has_value() ? "set" : "none",
                      ", attributes=", attributes.size(), ")");
}

absl::Status ChunkMetadata::Validate() const {
  for (const auto& [key, unused] : attributes) {
    (void)unused;
    ABSL_RETURN_IF_ERROR(ValidateName(key));
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> ChunkMetadata::GetAttribute(
    std::string_view key) const {
  const auto found = attributes.find(key);
  if (found == attributes.end()) {
    return absl::NotFoundError(absl::StrCat("Attribute not found: ", key));
  }
  return found->second;
}

absl::Status ChunkMetadata::SetAttribute(std::string key, std::string value) {
  ABSL_RETURN_IF_ERROR(ValidateName(key));
  attributes.insert_or_assign(std::move(key), std::move(value));
  return absl::OkStatus();
}

absl::StatusOr<Bytes> ChunkMetadata::ToMsgpack() const {
  // Validated once, here, for the whole tree: Validate() recurses, so leaving it
  // in ToMsgpackInto() as well re-checked every nested id once per ancestor
  // level. ValidateName was 2.19% of profile, most of it that repetition.
  ABSL_RETURN_IF_ERROR(Validate());
  MsgpackWriter writer;
  ABSL_RETURN_IF_ERROR(ToMsgpackInto(&writer));
  return writer.TakeBytes();
}

absl::Status ChunkMetadata::ToMsgpackInto(MsgpackWriter* absl_nonnull writer) const {
  writer->PackString(mimetype);
  if (timestamp.has_value()) {
    writer->PackInt(absl::ToUnixMicros(*timestamp));
  } else {
    writer->PackNil();
  }
  PackByteMap(writer, attributes);
  return absl::OkStatus();
}

absl::StatusOr<ChunkMetadata> ChunkMetadata::FromMsgpack(
    std::string_view bytes) {
  MsgpackReader reader(bytes);
  ABSL_ASSIGN_OR_RETURN(nlohmann::json mimetype_value,
                        ReadField(&reader, "ChunkMetadata.mimetype"));
  ABSL_ASSIGN_OR_RETURN(std::string mimetype,
                        JsonString(mimetype_value, "ChunkMetadata.mimetype"));
  ABSL_ASSIGN_OR_RETURN(nlohmann::json timestamp_value,
                        ReadField(&reader, "ChunkMetadata.timestamp"));
  std::optional<absl::Time> timestamp;
  if (!timestamp_value.is_null()) {
    if (!timestamp_value.is_number_integer()) {
      return absl::InvalidArgumentError(
          "ChunkMetadata.timestamp must be integer microseconds or null");
    }
    timestamp = absl::FromUnixMicros(timestamp_value.get<std::int64_t>());
  }
  ABSL_ASSIGN_OR_RETURN(nlohmann::json attributes_value,
                        ReadField(&reader, "ChunkMetadata.attributes"));
  ABSL_ASSIGN_OR_RETURN(
      ByteMap attributes,
      DecodeByteMap(attributes_value, "ChunkMetadata.attributes"));
  ABSL_RETURN_IF_ERROR(reader.EnsureFullyConsumed());
  ChunkMetadata result{.mimetype = std::move(mimetype),
                       .timestamp = timestamp,
                       .attributes = std::move(attributes)};
  ABSL_RETURN_IF_ERROR(result.Validate());
  return result;
}

size_t Chunk::ApproxBytes() const {
  // The object's estimate rather than its encoding: this is called for buffer
  // accounting on every put, and encoding a value to find out how big it is
  // would reintroduce exactly the cost the object exists to avoid.
  return ref.size() + data.size() +
         (object != nullptr ? object->ApproxBytes() : 0) +
         (metadata ? metadata->ApproxBytes() : 1) + 5;
}

std::string Chunk::DebugString() const {
  return absl::StrCat("Chunk(", DebugChunk(*this), ")");
}

std::string Chunk::GetMimetype() const {
  return metadata.has_value() ? metadata->mimetype : std::string();
}

bool Chunk::IsEmpty() const {
  // The object counts. A chunk carrying a value has no bytes yet, and reading
  // that as empty would read it as a null stream terminator -- ending a stream
  // on its first value.
  return ref.empty() && data.empty() && object == nullptr;
}

bool Chunk::IsNull() const {
  return IsEmpty() && GetMimetype() == "application/octet-stream";
}

absl::Status Chunk::Validate() const {
  if (!ref.empty() && !data.empty()) {
    return absl::InvalidArgumentError("Only one of ref or data may be set");
  }
  if (object != nullptr && !ref.empty()) {
    return absl::InvalidArgumentError(
        "A chunk carrying an object cannot also reference another node");
  }
  return metadata ? metadata->Validate() : absl::OkStatus();
}

absl::Status Chunk::Materialize() {
  if (object == nullptr) {
    return absl::OkStatus();
  }
  if (!data.empty()) {
    // Bytes already won; drop the value so there is one answer rather than two
    // that could drift apart.
    object.reset();
    return absl::OkStatus();
  }
  ABSL_ASSIGN_OR_RETURN(data, object->Encode());
  if (!metadata.has_value()) {
    metadata = ChunkMetadata{.mimetype = std::string(object->mimetype())};
  } else if (metadata->mimetype.empty()) {
    metadata->mimetype = std::string(object->mimetype());
  }
  object.reset();
  return absl::OkStatus();
}

absl::StatusOr<Bytes> Chunk::ToMsgpack() const {
  // Validated once, here, for the whole tree: Validate() recurses, so leaving it
  // in ToMsgpackInto() as well re-checked every nested id once per ancestor
  // level. ValidateName was 2.19% of profile, most of it that repetition.
  ABSL_RETURN_IF_ERROR(Validate());
  MsgpackWriter writer;
  ABSL_RETURN_IF_ERROR(ToMsgpackInto(&writer));
  return writer.TakeBytes();
}

absl::Status Chunk::ToMsgpackInto(MsgpackWriter* absl_nonnull writer) const {
  if (metadata.has_value()) {
    ABSL_RETURN_IF_ERROR(writer->PackRecord([this](MsgpackWriter* child) {
      return metadata->ToMsgpackInto(child);
    }));
  } else {
    writer->PackNil();
  }
  writer->PackString(ref);
  // The payload itself, copied once into the buffer instead of once into a
  // vector, once into a string and once again on append.
  writer->PackBinary(data);
  return absl::OkStatus();
}

absl::StatusOr<Chunk> Chunk::FromMsgpack(std::string_view bytes) {
  MsgpackReader reader(bytes);
  ABSL_ASSIGN_OR_RETURN(nlohmann::json metadata_value,
                        ReadField(&reader, "Chunk.metadata"));
  std::optional<ChunkMetadata> metadata;
  if (!metadata_value.is_null()) {
    ABSL_ASSIGN_OR_RETURN(Bytes encoded,
                          GetBinary(metadata_value, "Chunk.metadata"));
    ABSL_ASSIGN_OR_RETURN(ChunkMetadata decoded,
                          ChunkMetadata::FromMsgpack(encoded));
    metadata = std::move(decoded);
  }
  ABSL_ASSIGN_OR_RETURN(nlohmann::json ref_value,
                        ReadField(&reader, "Chunk.ref"));
  ABSL_ASSIGN_OR_RETURN(std::string ref, JsonString(ref_value, "Chunk.ref"));
  // The payload, read straight out of the buffer: this is the one field whose
  // size is unbounded, so it is the copy worth not making.
  ABSL_ASSIGN_OR_RETURN(std::string_view data_view, reader.ReadBinaryView());
  std::string data(data_view);
  ABSL_RETURN_IF_ERROR(reader.EnsureFullyConsumed());
  Chunk result{.metadata = std::move(metadata),
               .ref = std::move(ref),
               .data = std::move(data)};
  ABSL_RETURN_IF_ERROR(result.Validate());
  return result;
}

absl::StatusOr<Chunk> MakeStatusChunk(const absl::Status& status,
                                      bool closing) {
  ABSL_ASSIGN_OR_RETURN(std::string bytes, PackStatus(status));
  ChunkMetadata metadata{.mimetype = std::string(kStatusMimetype)};
  if (closing) {
    ABSL_RETURN_IF_ERROR(
        metadata.SetAttribute(std::string(kCloseAttribute), "1"));
  }
  return Chunk{.metadata = std::move(metadata), .data = std::move(bytes)};
}

bool IsStatusChunk(const Chunk& chunk) {
  return chunk.GetMimetype() == kStatusMimetype;
}

bool IsCloseStatusChunk(const Chunk& chunk) {
  return IsStatusChunk(chunk) && chunk.metadata.has_value() &&
         chunk.metadata->attributes.contains(kCloseAttribute);
}

absl::StatusOr<absl::Status> StatusFromStatusChunk(const Chunk& chunk) {
  absl::StatusOr<absl::Status> result;
  if (absl::Status validation = chunk.Validate(); !validation.ok()) {
    result.AssignStatus(std::move(validation));
    return result;
  }
  if (!IsStatusChunk(chunk)) {
    result.AssignStatus(
        absl::InvalidArgumentError("Chunk does not contain a status"));
    return result;
  }
  return UnpackStatus(chunk.data);
}

size_t NodeRef::ApproxBytes() const {
  return id.size() + 4 + (length.has_value() ? 4 : 0) + 1;
}

std::string NodeRef::DebugString() const {
  return absl::StrCat(
      "NodeRef(id='", BoundedText(id, kDebugPreviewMaxNameChars),
      "', offset=", offset,
      ", length=", length.has_value() ? absl::StrCat(*length) : "end", ")");
}

absl::Status NodeRef::Validate() const {
  ABSL_RETURN_IF_ERROR(ValidateName(id));
  if (length.has_value() &&
      (*length > kUint32Range || *length + offset > kUint32Range)) {
    return absl::InvalidArgumentError("Offset + length must not exceed 2^32");
  }
  return absl::OkStatus();
}

absl::StatusOr<Bytes> NodeRef::ToMsgpack() const {
  // Validated once, here, for the whole tree: Validate() recurses, so leaving it
  // in ToMsgpackInto() as well re-checked every nested id once per ancestor
  // level. ValidateName was 2.19% of profile, most of it that repetition.
  ABSL_RETURN_IF_ERROR(Validate());
  MsgpackWriter writer;
  ABSL_RETURN_IF_ERROR(ToMsgpackInto(&writer));
  return writer.TakeBytes();
}

absl::Status NodeRef::ToMsgpackInto(MsgpackWriter* absl_nonnull writer) const {
  writer->PackString(id);
  writer->PackUint(offset);
  if (length.has_value()) {
    writer->PackUint(*length);
  } else {
    writer->PackNil();
  }
  return absl::OkStatus();
}

absl::StatusOr<NodeRef> NodeRef::FromMsgpack(std::string_view bytes) {
  MsgpackReader reader(bytes);
  ABSL_ASSIGN_OR_RETURN(nlohmann::json id_value,
                        ReadField(&reader, "NodeRef.id"));
  ABSL_ASSIGN_OR_RETURN(std::string id, JsonString(id_value, "NodeRef.id"));
  ABSL_ASSIGN_OR_RETURN(nlohmann::json offset_value,
                        ReadField(&reader, "NodeRef.offset"));
  ABSL_ASSIGN_OR_RETURN(std::uint64_t offset,
                        JsonUnsigned(offset_value, "NodeRef.offset"));
  if (offset > std::numeric_limits<std::uint32_t>::max()) {
    return absl::OutOfRangeError("NodeRef.offset exceeds uint32");
  }
  ABSL_ASSIGN_OR_RETURN(nlohmann::json length_value,
                        ReadField(&reader, "NodeRef.length"));
  std::optional<std::uint64_t> length;
  if (!length_value.is_null()) {
    ABSL_ASSIGN_OR_RETURN(std::uint64_t parsed,
                          JsonUnsigned(length_value, "NodeRef.length"));
    length = parsed;
  }
  ABSL_RETURN_IF_ERROR(reader.EnsureFullyConsumed());
  NodeRef result{.id = std::move(id),
                 .offset = static_cast<std::uint32_t>(offset),
                 .length = length};
  ABSL_RETURN_IF_ERROR(result.Validate());
  return result;
}

size_t NodeFragment::ApproxBytes() const {
  const size_t data_size =
      std::visit([](const auto& value) { return value.ApproxBytes(); }, data);
  return id.size() + data_size + (seq.has_value() ? 4 : 1) + 6;
}

std::string NodeFragment::DebugString() const {
  const std::string payload = std::holds_alternative<Chunk>(data)
                                  ? std::get<Chunk>(data).DebugString()
                                  : std::get<NodeRef>(data).DebugString();
  return absl::StrCat("NodeFragment(id='",
                      BoundedText(id, kDebugPreviewMaxNameChars),
                      "', seq=", seq.has_value() ? absl::StrCat(*seq) : "none",
                      ", continued=", continued, ", data=", payload, ")");
}

absl::Status NodeFragment::Validate() const {
  if (!id.empty()) {
    ABSL_RETURN_IF_ERROR(ValidateName(id));
  }
  return std::visit([](const auto& value) { return value.Validate(); }, data);
}

absl::StatusOr<Chunk* absl_nonnull> NodeFragment::GetChunk() {
  if (auto* chunk = std::get_if<Chunk>(&data)) {
    return chunk;
  }
  return absl::FailedPreconditionError("Data is not a Chunk");
}

absl::StatusOr<const Chunk* absl_nonnull> NodeFragment::GetChunk() const {
  if (const auto* chunk = std::get_if<Chunk>(&data)) {
    return chunk;
  }
  return absl::FailedPreconditionError("Data is not a Chunk");
}

absl::StatusOr<NodeRef* absl_nonnull> NodeFragment::GetNodeRef() {
  if (auto* node_ref = std::get_if<NodeRef>(&data)) {
    return node_ref;
  }
  return absl::FailedPreconditionError("Data is not a NodeRef");
}

absl::StatusOr<const NodeRef* absl_nonnull> NodeFragment::GetNodeRef() const {
  if (const auto* node_ref = std::get_if<NodeRef>(&data)) {
    return node_ref;
  }
  return absl::FailedPreconditionError("Data is not a NodeRef");
}

absl::StatusOr<Bytes> NodeFragment::ToMsgpack() const {
  // Validated once, here, for the whole tree: Validate() recurses, so leaving it
  // in ToMsgpackInto() as well re-checked every nested id once per ancestor
  // level. ValidateName was 2.19% of profile, most of it that repetition.
  ABSL_RETURN_IF_ERROR(Validate());
  MsgpackWriter writer;
  ABSL_RETURN_IF_ERROR(ToMsgpackInto(&writer));
  return writer.TakeBytes();
}

absl::Status NodeFragment::ToMsgpackInto(MsgpackWriter* absl_nonnull writer) const {
  writer->PackString(id);
  const bool is_chunk = std::holds_alternative<Chunk>(data);
  writer->PackUint(is_chunk ? 0 : 1);
  ABSL_RETURN_IF_ERROR(writer->PackRecord([this, is_chunk](
                                              MsgpackWriter* child) {
    return is_chunk ? std::get<Chunk>(data).ToMsgpackInto(child)
                    : std::get<NodeRef>(data).ToMsgpackInto(child);
  }));
  if (seq.has_value()) {
    writer->PackUint(*seq);
  } else {
    writer->PackNil();
  }
  writer->PackBool(continued);
  return absl::OkStatus();
}

absl::StatusOr<NodeFragment> NodeFragment::FromMsgpack(std::string_view bytes) {
  MsgpackReader reader(bytes);
  ABSL_ASSIGN_OR_RETURN(nlohmann::json id_value,
                        ReadField(&reader, "NodeFragment.id"));
  ABSL_ASSIGN_OR_RETURN(std::string id,
                        JsonString(id_value, "NodeFragment.id"));
  ABSL_ASSIGN_OR_RETURN(nlohmann::json union_value,
                        ReadField(&reader, "NodeFragment.data index"));
  ABSL_ASSIGN_OR_RETURN(std::uint64_t union_index,
                        JsonUnsigned(union_value, "NodeFragment.data index"));
  if (union_index > 1) {
    return absl::FailedPreconditionError(
        "Invalid union index; expected 0 for Chunk or 1 for NodeRef");
  }
  // The nested record is parsed in place. Copying it out first duplicated the
  // whole chunk -- payload included -- before Chunk::FromMsgpack had even
  // looked at it.
  ABSL_ASSIGN_OR_RETURN(std::string_view encoded, reader.ReadBinaryView());
  std::variant<Chunk, NodeRef> data;
  if (union_index == 0) {
    ABSL_ASSIGN_OR_RETURN(Chunk chunk, Chunk::FromMsgpack(encoded));
    data = std::move(chunk);
  } else {
    ABSL_ASSIGN_OR_RETURN(NodeRef node_ref, NodeRef::FromMsgpack(encoded));
    data = std::move(node_ref);
  }
  ABSL_ASSIGN_OR_RETURN(nlohmann::json seq_value,
                        ReadField(&reader, "NodeFragment.seq"));
  std::optional<std::uint32_t> seq;
  if (!seq_value.is_null()) {
    ABSL_ASSIGN_OR_RETURN(std::uint64_t parsed,
                          JsonUnsigned(seq_value, "NodeFragment.seq"));
    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
      return absl::OutOfRangeError("NodeFragment.seq exceeds uint32");
    }
    seq = static_cast<std::uint32_t>(parsed);
  }
  ABSL_ASSIGN_OR_RETURN(nlohmann::json continued_value,
                        ReadField(&reader, "NodeFragment.continued"));
  if (!continued_value.is_boolean()) {
    return absl::InvalidArgumentError("NodeFragment.continued must be bool");
  }
  const bool continued = continued_value.get<bool>();
  ABSL_RETURN_IF_ERROR(reader.EnsureFullyConsumed());
  NodeFragment result{.id = std::move(id),
                      .data = std::move(data),
                      .seq = seq,
                      .continued = continued};
  ABSL_RETURN_IF_ERROR(result.Validate());
  return result;
}

size_t Port::ApproxBytes() const {
  return name.size() + id.size() + 1;
}

std::string Port::DebugString() const {
  return absl::StrCat("Port(name='",
                      BoundedText(name, kDebugPreviewMaxNameChars), "', id='",
                      BoundedText(id, kDebugPreviewMaxNameChars), "')");
}

absl::Status Port::Validate() const {
  if (!name.empty()) {
    ABSL_RETURN_IF_ERROR(ValidateName(name));
  }
  return id.empty() ? absl::OkStatus() : ValidateName(id);
}

absl::StatusOr<Bytes> Port::ToMsgpack() const {
  // Validated once, here, for the whole tree: Validate() recurses, so leaving it
  // in ToMsgpackInto() as well re-checked every nested id once per ancestor
  // level. ValidateName was 2.19% of profile, most of it that repetition.
  ABSL_RETURN_IF_ERROR(Validate());
  MsgpackWriter writer;
  ABSL_RETURN_IF_ERROR(ToMsgpackInto(&writer));
  return writer.TakeBytes();
}

absl::Status Port::ToMsgpackInto(MsgpackWriter* absl_nonnull writer) const {
  writer->PackString(name);
  writer->PackString(id);
  return absl::OkStatus();
}

absl::StatusOr<Port> Port::FromMsgpack(std::string_view bytes) {
  MsgpackReader reader(bytes);
  ABSL_ASSIGN_OR_RETURN(nlohmann::json name_value,
                        ReadField(&reader, "Port.name"));
  ABSL_ASSIGN_OR_RETURN(std::string name, JsonString(name_value, "Port.name"));
  ABSL_ASSIGN_OR_RETURN(nlohmann::json id_value, ReadField(&reader, "Port.id"));
  ABSL_ASSIGN_OR_RETURN(std::string id, JsonString(id_value, "Port.id"));
  ABSL_RETURN_IF_ERROR(reader.EnsureFullyConsumed());
  Port result{.name = std::move(name), .id = std::move(id)};
  ABSL_RETURN_IF_ERROR(result.Validate());
  return result;
}

size_t ActionMessage::ApproxBytes() const {
  size_t result = id.size() + name.size() + 8;
  for (const Port& port : inputs) {
    result += port.ApproxBytes();
  }
  for (const Port& port : outputs) {
    result += port.ApproxBytes();
  }
  for (const auto& [key, value] : headers) {
    result += key.size() + value.size();
  }
  return result;
}

std::string ActionMessage::DebugString() const {
  return absl::StrCat("ActionMessage(id='",
                      BoundedText(id, kDebugPreviewMaxNameChars), "', name='",
                      BoundedText(name, kDebugPreviewMaxNameChars),
                      "', inputs=", inputs.size(), ", outputs=", outputs.size(),
                      ", headers=", headers.size(), ")");
}

absl::Status ActionMessage::Validate() const {
  if (!id.empty()) {
    ABSL_RETURN_IF_ERROR(ValidateName(id));
  }
  if (!name.empty()) {
    ABSL_RETURN_IF_ERROR(ValidateName(name));
  }
  for (const Port& port : inputs) {
    ABSL_RETURN_IF_ERROR(port.Validate());
  }
  for (const Port& port : outputs) {
    ABSL_RETURN_IF_ERROR(port.Validate());
  }
  for (const auto& [key, unused] : headers) {
    (void)unused;
    ABSL_RETURN_IF_ERROR(ValidateName(key));
  }
  return absl::OkStatus();
}

absl::StatusOr<Bytes> ActionMessage::ToMsgpack() const {
  // Validated once, here, for the whole tree: Validate() recurses, so leaving it
  // in ToMsgpackInto() as well re-checked every nested id once per ancestor
  // level. ValidateName was 2.19% of profile, most of it that repetition.
  ABSL_RETURN_IF_ERROR(Validate());
  MsgpackWriter writer;
  ABSL_RETURN_IF_ERROR(ToMsgpackInto(&writer));
  return writer.TakeBytes();
}

absl::Status ActionMessage::ToMsgpackInto(MsgpackWriter* absl_nonnull writer) const {
  writer->PackString(id);
  writer->PackString(name);
  for (const std::vector<Port>* ports : {&inputs, &outputs}) {
    writer->PackArrayHeader(ports->size());
    for (const Port& port : *ports) {
      ABSL_RETURN_IF_ERROR(writer->PackRecord([&port](MsgpackWriter* child) {
        return port.ToMsgpackInto(child);
      }));
    }
  }
  PackByteMap(writer, headers);
  return absl::OkStatus();
}

absl::StatusOr<ActionMessage> ActionMessage::FromMsgpack(
    std::string_view bytes) {
  MsgpackReader reader(bytes);
  ABSL_ASSIGN_OR_RETURN(nlohmann::json id_value,
                        ReadField(&reader, "ActionMessage.id"));
  ABSL_ASSIGN_OR_RETURN(std::string id,
                        JsonString(id_value, "ActionMessage.id"));
  ABSL_ASSIGN_OR_RETURN(nlohmann::json name_value,
                        ReadField(&reader, "ActionMessage.name"));
  ABSL_ASSIGN_OR_RETURN(std::string name,
                        JsonString(name_value, "ActionMessage.name"));
  auto decode_ports =
      [&reader](std::string_view field) -> absl::StatusOr<std::vector<Port>> {
    ABSL_ASSIGN_OR_RETURN(nlohmann::json values, ReadField(&reader, field));
    if (!values.is_array()) {
      return absl::InvalidArgumentError(absl::StrCat(field, " must be a list"));
    }
    std::vector<Port> result;
    result.reserve(values.size());
    for (const nlohmann::json& value : values) {
      ABSL_ASSIGN_OR_RETURN(Bytes encoded, GetBinary(value, field));
      ABSL_ASSIGN_OR_RETURN(Port port, Port::FromMsgpack(encoded));
      result.push_back(std::move(port));
    }
    return result;
  };
  ABSL_ASSIGN_OR_RETURN(std::vector<Port> inputs,
                        decode_ports("ActionMessage.inputs"));
  ABSL_ASSIGN_OR_RETURN(std::vector<Port> outputs,
                        decode_ports("ActionMessage.outputs"));
  ABSL_ASSIGN_OR_RETURN(nlohmann::json headers_value,
                        ReadField(&reader, "ActionMessage.headers"));
  ABSL_ASSIGN_OR_RETURN(ByteMap headers,
                        DecodeByteMap(headers_value, "ActionMessage.headers"));
  ABSL_RETURN_IF_ERROR(reader.EnsureFullyConsumed());
  ActionMessage result{.id = std::move(id),
                       .name = std::move(name),
                       .inputs = std::move(inputs),
                       .outputs = std::move(outputs),
                       .headers = std::move(headers)};
  ABSL_RETURN_IF_ERROR(result.Validate());
  return result;
}

size_t WireMessage::ApproxBytes() const {
  size_t result = 8;
  for (const NodeFragment& fragment : node_fragments) {
    result += fragment.ApproxBytes();
  }
  for (const ActionMessage& action : actions) {
    result += action.ApproxBytes();
  }
  for (const auto& [key, value] : headers) {
    result += key.size() + value.size();
  }
  return result;
}

std::string WireMessage::DebugString() const {
  std::ostringstream stream;
  stream << "WireMessage(headers={";
  size_t header_count = 0;
  for (const auto& [key, value] : headers) {
    if (header_count == kDebugPreviewMaxHeaders) {
      break;
    }
    if (header_count != 0) {
      stream << ", ";
    }
    stream << '\'' << key << "': <" << value.size() << " bytes>";
    ++header_count;
  }
  if (header_count < headers.size()) {
    stream << ", ... <" << headers.size() - header_count << " more headers>";
  }
  stream << '}';
  if (!node_fragments.empty()) {
    stream << ", node_fragments=" << node_fragments.size() << " [";
    const size_t count =
        std::min(node_fragments.size(), kDebugPreviewMaxFragments);
    for (size_t index = 0; index < count; ++index) {
      if (index != 0) {
        stream << "; ";
      }
      const NodeFragment& fragment = node_fragments[index];
      stream << (fragment.id.empty() ? "<anonymous>" : fragment.id) << ": ";
      if (const Chunk* chunk = std::get_if<Chunk>(&fragment.data)) {
        stream << BoundedText(DebugChunk(*chunk), kDebugPreviewMaxContentChars);
      } else {
        const NodeRef& node_ref = std::get<NodeRef>(fragment.data);
        stream << "node_ref=" << node_ref.id << '[' << node_ref.offset << ':';
        if (node_ref.length) {
          stream << static_cast<std::uint64_t>(node_ref.offset) +
                        *node_ref.length;
        } else {
          stream << "end";
        }
        stream << ']';
      }
    }
    if (count < node_fragments.size()) {
      stream << "; ... <" << node_fragments.size() - count << " more>";
    }
    stream << ']';
  }
  if (!actions.empty()) {
    stream << ", actions=" << actions.size() << " [";
    const size_t count = std::min(actions.size(), kDebugPreviewMaxActions);
    for (size_t index = 0; index < count; ++index) {
      if (index != 0) {
        stream << ", ";
      }
      stream << '\''
             << BoundedText(actions[index].name, kDebugPreviewMaxNameChars)
             << '\'';
    }
    if (count < actions.size()) {
      stream << ", ... <" << actions.size() - count << " more>";
    }
    stream << ']';
  }
  stream << ')';
  return BoundedText(stream.str(), kDebugStringMaxChars);
}

absl::Status WireMessage::Validate() const {
  for (const NodeFragment& fragment : node_fragments) {
    ABSL_RETURN_IF_ERROR(fragment.Validate());
  }
  for (const ActionMessage& action : actions) {
    ABSL_RETURN_IF_ERROR(action.Validate());
  }
  for (const auto& [key, unused] : headers) {
    (void)unused;
    ABSL_RETURN_IF_ERROR(ValidateName(key));
  }
  return absl::OkStatus();
}

absl::StatusOr<Bytes> WireMessage::ToMsgpack() const {
  // Validated once, here, for the whole tree: Validate() recurses, so leaving it
  // in ToMsgpackInto() as well re-checked every nested id once per ancestor
  // level. ValidateName was 2.19% of profile, most of it that repetition.
  ABSL_RETURN_IF_ERROR(Validate());
  MsgpackWriter writer;
  ABSL_RETURN_IF_ERROR(ToMsgpackInto(&writer));
  return writer.TakeBytes();
}

absl::Status WireMessage::ToMsgpackInto(MsgpackWriter* absl_nonnull writer) const {
  writer->PackUint(kVersion);
  writer->PackArrayHeader(node_fragments.size());
  for (const NodeFragment& fragment : node_fragments) {
    ABSL_RETURN_IF_ERROR(writer->PackRecord([&fragment](MsgpackWriter* child) {
      return fragment.ToMsgpackInto(child);
    }));
  }
  writer->PackArrayHeader(actions.size());
  for (const ActionMessage& action : actions) {
    ABSL_RETURN_IF_ERROR(writer->PackRecord([&action](MsgpackWriter* child) {
      return action.ToMsgpackInto(child);
    }));
  }
  PackByteMap(writer, headers);
  return absl::OkStatus();
}

absl::StatusOr<WireMessage> WireMessage::FromMsgpack(std::string_view bytes) {
  MsgpackReader reader(bytes);
  ABSL_ASSIGN_OR_RETURN(nlohmann::json version_value,
                        ReadField(&reader, "WireMessage.version"));
  ABSL_ASSIGN_OR_RETURN(std::uint64_t version,
                        JsonUnsigned(version_value, "WireMessage.version"));
  if (version != kVersion) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid serialized WireMessage version: ", version));
  }
  // Each element is decoded straight out of the source buffer. Materialising
  // the array as JSON first meant a vector allocation and two copies of every
  // element's bytes before its own decoder had seen them.
  auto decode_binary_list = [&reader](std::string_view field, auto decode_one) {
    using Result = decltype(decode_one(std::string_view{}));
    using Value = typename Result::value_type;
    absl::StatusOr<size_t> count = reader.ReadArrayLength();
    if (!count.ok()) {
      return absl::StatusOr<std::vector<Value>>(
          absl::InvalidArgumentError(absl::StrCat(field, " must be a list")));
    }
    std::vector<Value> result;
    result.reserve(*count);
    for (size_t index = 0; index < *count; ++index) {
      absl::StatusOr<std::string_view> encoded = reader.ReadBinaryView();
      if (!encoded.ok()) {
        return absl::StatusOr<std::vector<Value>>(encoded.status());
      }
      Result decoded = decode_one(*encoded);
      if (!decoded.ok()) {
        return absl::StatusOr<std::vector<Value>>(decoded.status());
      }
      result.push_back(std::move(*decoded));
    }
    return absl::StatusOr<std::vector<Value>>(std::move(result));
  };
  ABSL_ASSIGN_OR_RETURN(std::vector<NodeFragment> fragments,
                        decode_binary_list("WireMessage.node_fragments",
                                           NodeFragment::FromMsgpack));
  ABSL_ASSIGN_OR_RETURN(
      std::vector<ActionMessage> actions,
      decode_binary_list("WireMessage.actions", ActionMessage::FromMsgpack));
  ABSL_ASSIGN_OR_RETURN(nlohmann::json headers_value,
                        ReadField(&reader, "WireMessage.headers"));
  ABSL_ASSIGN_OR_RETURN(ByteMap headers,
                        DecodeByteMap(headers_value, "WireMessage.headers"));
  ABSL_RETURN_IF_ERROR(reader.EnsureFullyConsumed());
  WireMessage result{.node_fragments = std::move(fragments),
                     .actions = std::move(actions),
                     .headers = std::move(headers)};
  ABSL_RETURN_IF_ERROR(result.Validate());
  return result;
}

bool IsHalfCloseMessage(const WireMessage& message) {
  return message.actions.empty() && message.node_fragments.empty();
}

WireMessage MakeHalfCloseMessage(ByteMap trailers) {
  return WireMessage{.headers = std::move(trailers)};
}

size_t EmptyWireMessageSize() {
  static const size_t size = [] {
    absl::StatusOr<Bytes> encoded = WireMessage{}.ToMsgpack();
    return encoded.ok() ? encoded->size() : 0;
  }();
  return size;
}

}  // namespace a11::data
