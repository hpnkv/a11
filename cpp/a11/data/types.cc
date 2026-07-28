// Copyright 2026 The A11 Authors.

#include "a11/data/types.h"

#include <algorithm>
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

bool IsAsciiAlpha(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool IsNameBoundary(char value) {
  return IsAsciiAlpha(value) || (value >= '0' && value <= '9') || value == '_';
}

bool IsNameMiddle(char value) {
  return IsNameBoundary(value) || value == '-' || value == '#';
}

absl::StatusOr<std::uint64_t> JsonUnsigned(const nlohmann::json& value,
                                           std::string_view field) {
  try {
    if (value.is_number_unsigned())
      return value.get<std::uint64_t>();
    if (value.is_number_integer()) {
      const std::int64_t result = value.get<std::int64_t>();
      if (result >= 0)
        return static_cast<std::uint64_t>(result);
    }
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid ", field, ": ", error.what()));
  }
  return absl::InvalidArgumentError(
      absl::StrCat(field, " must be a non-negative integer"));
}

absl::StatusOr<std::string> JsonString(const nlohmann::json& value,
                                       std::string_view field) {
  if (!value.is_string()) {
    return absl::InvalidArgumentError(absl::StrCat(field, " must be a string"));
  }
  try {
    return value.get<std::string>();
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid ", field, ": ", error.what()));
  }
}

nlohmann::json EncodeByteMap(const ByteMap& values) {
  nlohmann::json result = nlohmann::json::object();
  for (const auto& [key, value] : values)
    result[key] = Binary(value);
  return result;
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
    absl::StatusOr<std::string> bytes = GetBinary(iterator.value(), field);
    if (!bytes.ok())
      return bytes.status();
    result.emplace(iterator.key(), std::move(*bytes));
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
  if (value.size() <= limit)
    return std::string(value);
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
  if (chunk.data.empty())
    return "<empty chunk>";
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
  if (!std::all_of(name.begin(), name.end(), IsNameMiddle)) {
    return absl::InvalidArgumentError(
        "name contains a character outside [a-zA-Z0-9-_#]");
  }
  return absl::OkStatus();
}

size_t ChunkMetadata::ApproxBytes() const {
  size_t result = mimetype.size() + 8 + 1;
  for (const auto& [key, value] : attributes)
    result += key.size() + value.size();
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
    absl::Status status = ValidateName(key);
    if (!status.ok())
      return status;
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
  absl::Status status = ValidateName(key);
  if (!status.ok())
    return status;
  attributes.insert_or_assign(std::move(key), std::move(value));
  return absl::OkStatus();
}

absl::StatusOr<Bytes> ChunkMetadata::ToMsgpack() const {
  absl::Status status = Validate();
  if (!status.ok())
    return status;
  MsgpackWriter writer;
  status = writer.Pack(mimetype);
  if (!status.ok())
    return status;
  status = writer.Pack(timestamp.has_value()
                           ? nlohmann::json(absl::ToUnixMicros(*timestamp))
                           : nlohmann::json(nullptr));
  if (!status.ok())
    return status;
  status = writer.Pack(EncodeByteMap(attributes));
  if (!status.ok())
    return status;
  return writer.TakeBytes();
}

absl::StatusOr<ChunkMetadata> ChunkMetadata::FromMsgpack(
    std::string_view bytes) {
  MsgpackReader reader(bytes);
  absl::StatusOr<nlohmann::json> mimetype_value =
      ReadField(&reader, "ChunkMetadata.mimetype");
  if (!mimetype_value.ok())
    return mimetype_value.status();
  absl::StatusOr<std::string> mimetype =
      JsonString(*mimetype_value, "ChunkMetadata.mimetype");
  if (!mimetype.ok())
    return mimetype.status();
  absl::StatusOr<nlohmann::json> timestamp_value =
      ReadField(&reader, "ChunkMetadata.timestamp");
  if (!timestamp_value.ok())
    return timestamp_value.status();
  std::optional<absl::Time> timestamp;
  if (!timestamp_value->is_null()) {
    try {
      if (!timestamp_value->is_number_integer()) {
        return absl::InvalidArgumentError(
            "ChunkMetadata.timestamp must be integer microseconds or null");
      }
      timestamp = absl::FromUnixMicros(timestamp_value->get<std::int64_t>());
    } catch (const std::exception& error) {
      return absl::InvalidArgumentError(error.what());
    }
  }
  absl::StatusOr<nlohmann::json> attributes_value =
      ReadField(&reader, "ChunkMetadata.attributes");
  if (!attributes_value.ok())
    return attributes_value.status();
  absl::StatusOr<ByteMap> attributes =
      DecodeByteMap(*attributes_value, "ChunkMetadata.attributes");
  if (!attributes.ok())
    return attributes.status();
  absl::Status consumed = reader.EnsureFullyConsumed();
  if (!consumed.ok())
    return consumed;
  ChunkMetadata result{.mimetype = std::move(*mimetype),
                       .timestamp = timestamp,
                       .attributes = std::move(*attributes)};
  consumed = result.Validate();
  if (!consumed.ok())
    return consumed;
  return result;
}

size_t Chunk::ApproxBytes() const {
  return ref.size() + data.size() + (metadata ? metadata->ApproxBytes() : 1) +
         5;
}

std::string Chunk::DebugString() const {
  return absl::StrCat("Chunk(", DebugChunk(*this), ")");
}

std::string Chunk::GetMimetype() const {
  return metadata.has_value() ? metadata->mimetype : std::string();
}

bool Chunk::IsEmpty() const {
  return ref.empty() && data.empty();
}

bool Chunk::IsNull() const {
  return IsEmpty() && GetMimetype() == "application/octet-stream";
}

absl::Status Chunk::Validate() const {
  if (!ref.empty() && !data.empty()) {
    return absl::InvalidArgumentError("Only one of ref or data may be set");
  }
  return metadata ? metadata->Validate() : absl::OkStatus();
}

absl::StatusOr<Bytes> Chunk::ToMsgpack() const {
  absl::Status status = Validate();
  if (!status.ok())
    return status;
  MsgpackWriter writer;
  if (metadata.has_value()) {
    absl::StatusOr<Bytes> encoded = metadata->ToMsgpack();
    if (!encoded.ok())
      return encoded.status();
    status = writer.Pack(Binary(*encoded));
  } else {
    status = writer.Pack(nullptr);
  }
  if (!status.ok())
    return status;
  status = writer.Pack(ref);
  if (!status.ok())
    return status;
  status = writer.Pack(Binary(data));
  if (!status.ok())
    return status;
  return writer.TakeBytes();
}

absl::StatusOr<Chunk> Chunk::FromMsgpack(std::string_view bytes) {
  MsgpackReader reader(bytes);
  absl::StatusOr<nlohmann::json> metadata_value =
      ReadField(&reader, "Chunk.metadata");
  if (!metadata_value.ok())
    return metadata_value.status();
  std::optional<ChunkMetadata> metadata;
  if (!metadata_value->is_null()) {
    absl::StatusOr<Bytes> encoded =
        GetBinary(*metadata_value, "Chunk.metadata");
    if (!encoded.ok())
      return encoded.status();
    absl::StatusOr<ChunkMetadata> decoded =
        ChunkMetadata::FromMsgpack(*encoded);
    if (!decoded.ok())
      return decoded.status();
    metadata = std::move(*decoded);
  }
  absl::StatusOr<nlohmann::json> ref_value = ReadField(&reader, "Chunk.ref");
  if (!ref_value.ok())
    return ref_value.status();
  absl::StatusOr<std::string> ref = JsonString(*ref_value, "Chunk.ref");
  if (!ref.ok())
    return ref.status();
  absl::StatusOr<nlohmann::json> data_value = ReadField(&reader, "Chunk.data");
  if (!data_value.ok())
    return data_value.status();
  absl::StatusOr<std::string> data = GetBinary(*data_value, "Chunk.data");
  if (!data.ok())
    return data.status();
  absl::Status consumed = reader.EnsureFullyConsumed();
  if (!consumed.ok())
    return consumed;
  Chunk result{.metadata = std::move(metadata),
               .ref = std::move(*ref),
               .data = std::move(*data)};
  consumed = result.Validate();
  if (!consumed.ok())
    return consumed;
  return result;
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
  absl::Status status = ValidateName(id);
  if (!status.ok())
    return status;
  if (length.has_value() &&
      (*length > kUint32Range || *length + offset > kUint32Range)) {
    return absl::InvalidArgumentError("Offset + length must not exceed 2^32");
  }
  return absl::OkStatus();
}

absl::StatusOr<Bytes> NodeRef::ToMsgpack() const {
  absl::Status status = Validate();
  if (!status.ok())
    return status;
  MsgpackWriter writer;
  status = writer.Pack(id);
  if (!status.ok())
    return status;
  status = writer.Pack(offset);
  if (!status.ok())
    return status;
  status =
      writer.Pack(length ? nlohmann::json(*length) : nlohmann::json(nullptr));
  if (!status.ok())
    return status;
  return writer.TakeBytes();
}

absl::StatusOr<NodeRef> NodeRef::FromMsgpack(std::string_view bytes) {
  MsgpackReader reader(bytes);
  absl::StatusOr<nlohmann::json> id_value = ReadField(&reader, "NodeRef.id");
  if (!id_value.ok())
    return id_value.status();
  absl::StatusOr<std::string> id = JsonString(*id_value, "NodeRef.id");
  if (!id.ok())
    return id.status();
  absl::StatusOr<nlohmann::json> offset_value =
      ReadField(&reader, "NodeRef.offset");
  if (!offset_value.ok())
    return offset_value.status();
  absl::StatusOr<std::uint64_t> offset =
      JsonUnsigned(*offset_value, "NodeRef.offset");
  if (!offset.ok())
    return offset.status();
  if (*offset > std::numeric_limits<std::uint32_t>::max()) {
    return absl::OutOfRangeError("NodeRef.offset exceeds uint32");
  }
  absl::StatusOr<nlohmann::json> length_value =
      ReadField(&reader, "NodeRef.length");
  if (!length_value.ok())
    return length_value.status();
  std::optional<std::uint64_t> length;
  if (!length_value->is_null()) {
    absl::StatusOr<std::uint64_t> parsed =
        JsonUnsigned(*length_value, "NodeRef.length");
    if (!parsed.ok())
      return parsed.status();
    length = *parsed;
  }
  absl::Status consumed = reader.EnsureFullyConsumed();
  if (!consumed.ok())
    return consumed;
  NodeRef result{.id = std::move(*id),
                 .offset = static_cast<std::uint32_t>(*offset),
                 .length = length};
  consumed = result.Validate();
  if (!consumed.ok())
    return consumed;
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
    absl::Status status = ValidateName(id);
    if (!status.ok())
      return status;
  }
  return std::visit([](const auto& value) { return value.Validate(); }, data);
}

absl::StatusOr<Chunk* absl_nonnull> NodeFragment::GetChunk() {
  if (auto* chunk = std::get_if<Chunk>(&data))
    return chunk;
  return absl::FailedPreconditionError("Data is not a Chunk");
}

absl::StatusOr<const Chunk* absl_nonnull> NodeFragment::GetChunk() const {
  if (const auto* chunk = std::get_if<Chunk>(&data))
    return chunk;
  return absl::FailedPreconditionError("Data is not a Chunk");
}

absl::StatusOr<NodeRef* absl_nonnull> NodeFragment::GetNodeRef() {
  if (auto* node_ref = std::get_if<NodeRef>(&data))
    return node_ref;
  return absl::FailedPreconditionError("Data is not a NodeRef");
}

absl::StatusOr<const NodeRef* absl_nonnull> NodeFragment::GetNodeRef() const {
  if (const auto* node_ref = std::get_if<NodeRef>(&data))
    return node_ref;
  return absl::FailedPreconditionError("Data is not a NodeRef");
}

absl::StatusOr<Bytes> NodeFragment::ToMsgpack() const {
  absl::Status status = Validate();
  if (!status.ok())
    return status;
  MsgpackWriter writer;
  status = writer.Pack(id);
  if (!status.ok())
    return status;
  const bool is_chunk = std::holds_alternative<Chunk>(data);
  status = writer.Pack(is_chunk ? 0 : 1);
  if (!status.ok())
    return status;
  absl::StatusOr<Bytes> encoded = is_chunk
                                      ? std::get<Chunk>(data).ToMsgpack()
                                      : std::get<NodeRef>(data).ToMsgpack();
  if (!encoded.ok())
    return encoded.status();
  status = writer.Pack(Binary(*encoded));
  if (!status.ok())
    return status;
  status = writer.Pack(seq ? nlohmann::json(*seq) : nlohmann::json(nullptr));
  if (!status.ok())
    return status;
  status = writer.Pack(continued);
  if (!status.ok())
    return status;
  return writer.TakeBytes();
}

absl::StatusOr<NodeFragment> NodeFragment::FromMsgpack(std::string_view bytes) {
  MsgpackReader reader(bytes);
  absl::StatusOr<nlohmann::json> id_value =
      ReadField(&reader, "NodeFragment.id");
  if (!id_value.ok())
    return id_value.status();
  absl::StatusOr<std::string> id = JsonString(*id_value, "NodeFragment.id");
  if (!id.ok())
    return id.status();
  absl::StatusOr<nlohmann::json> union_value =
      ReadField(&reader, "NodeFragment.data index");
  if (!union_value.ok())
    return union_value.status();
  absl::StatusOr<std::uint64_t> union_index =
      JsonUnsigned(*union_value, "NodeFragment.data index");
  if (!union_index.ok())
    return union_index.status();
  if (*union_index > 1) {
    return absl::FailedPreconditionError(
        "Invalid union index; expected 0 for Chunk or 1 for NodeRef");
  }
  absl::StatusOr<nlohmann::json> data_value =
      ReadField(&reader, "NodeFragment.data");
  if (!data_value.ok())
    return data_value.status();
  absl::StatusOr<Bytes> encoded = GetBinary(*data_value, "NodeFragment.data");
  if (!encoded.ok())
    return encoded.status();
  std::variant<Chunk, NodeRef> data;
  if (*union_index == 0) {
    absl::StatusOr<Chunk> chunk = Chunk::FromMsgpack(*encoded);
    if (!chunk.ok())
      return chunk.status();
    data = std::move(*chunk);
  } else {
    absl::StatusOr<NodeRef> node_ref = NodeRef::FromMsgpack(*encoded);
    if (!node_ref.ok())
      return node_ref.status();
    data = std::move(*node_ref);
  }
  absl::StatusOr<nlohmann::json> seq_value =
      ReadField(&reader, "NodeFragment.seq");
  if (!seq_value.ok())
    return seq_value.status();
  std::optional<std::uint32_t> seq;
  if (!seq_value->is_null()) {
    absl::StatusOr<std::uint64_t> parsed =
        JsonUnsigned(*seq_value, "NodeFragment.seq");
    if (!parsed.ok())
      return parsed.status();
    if (*parsed > std::numeric_limits<std::uint32_t>::max()) {
      return absl::OutOfRangeError("NodeFragment.seq exceeds uint32");
    }
    seq = static_cast<std::uint32_t>(*parsed);
  }
  absl::StatusOr<nlohmann::json> continued_value =
      ReadField(&reader, "NodeFragment.continued");
  if (!continued_value.ok())
    return continued_value.status();
  if (!continued_value->is_boolean()) {
    return absl::InvalidArgumentError("NodeFragment.continued must be bool");
  }
  const bool continued = continued_value->get<bool>();
  absl::Status consumed = reader.EnsureFullyConsumed();
  if (!consumed.ok())
    return consumed;
  NodeFragment result{.id = std::move(*id),
                      .data = std::move(data),
                      .seq = seq,
                      .continued = continued};
  consumed = result.Validate();
  if (!consumed.ok())
    return consumed;
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
  absl::Status status;
  if (!name.empty()) {
    status = ValidateName(name);
    if (!status.ok())
      return status;
  }
  return id.empty() ? absl::OkStatus() : ValidateName(id);
}

absl::StatusOr<Bytes> Port::ToMsgpack() const {
  absl::Status status = Validate();
  if (!status.ok())
    return status;
  MsgpackWriter writer;
  status = writer.Pack(name);
  if (!status.ok())
    return status;
  status = writer.Pack(id);
  if (!status.ok())
    return status;
  return writer.TakeBytes();
}

absl::StatusOr<Port> Port::FromMsgpack(std::string_view bytes) {
  MsgpackReader reader(bytes);
  absl::StatusOr<nlohmann::json> name_value = ReadField(&reader, "Port.name");
  if (!name_value.ok())
    return name_value.status();
  absl::StatusOr<std::string> name = JsonString(*name_value, "Port.name");
  if (!name.ok())
    return name.status();
  absl::StatusOr<nlohmann::json> id_value = ReadField(&reader, "Port.id");
  if (!id_value.ok())
    return id_value.status();
  absl::StatusOr<std::string> id = JsonString(*id_value, "Port.id");
  if (!id.ok())
    return id.status();
  absl::Status consumed = reader.EnsureFullyConsumed();
  if (!consumed.ok())
    return consumed;
  Port result{.name = std::move(*name), .id = std::move(*id)};
  consumed = result.Validate();
  if (!consumed.ok())
    return consumed;
  return result;
}

size_t ActionMessage::ApproxBytes() const {
  size_t result = id.size() + name.size() + 8;
  for (const Port& port : inputs)
    result += port.ApproxBytes();
  for (const Port& port : outputs)
    result += port.ApproxBytes();
  for (const auto& [key, value] : headers)
    result += key.size() + value.size();
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
  absl::Status status;
  if (!id.empty()) {
    status = ValidateName(id);
    if (!status.ok())
      return status;
  }
  if (!name.empty()) {
    status = ValidateName(name);
    if (!status.ok())
      return status;
  }
  for (const Port& port : inputs) {
    status = port.Validate();
    if (!status.ok())
      return status;
  }
  for (const Port& port : outputs) {
    status = port.Validate();
    if (!status.ok())
      return status;
  }
  for (const auto& [key, unused] : headers) {
    (void)unused;
    status = ValidateName(key);
    if (!status.ok())
      return status;
  }
  return absl::OkStatus();
}

absl::StatusOr<Bytes> ActionMessage::ToMsgpack() const {
  absl::Status status = Validate();
  if (!status.ok())
    return status;
  MsgpackWriter writer;
  status = writer.Pack(id);
  if (!status.ok())
    return status;
  status = writer.Pack(name);
  if (!status.ok())
    return status;
  nlohmann::json packed_inputs = nlohmann::json::array();
  for (const Port& port : inputs) {
    absl::StatusOr<Bytes> encoded = port.ToMsgpack();
    if (!encoded.ok())
      return encoded.status();
    packed_inputs.push_back(Binary(*encoded));
  }
  status = writer.Pack(packed_inputs);
  if (!status.ok())
    return status;
  nlohmann::json packed_outputs = nlohmann::json::array();
  for (const Port& port : outputs) {
    absl::StatusOr<Bytes> encoded = port.ToMsgpack();
    if (!encoded.ok())
      return encoded.status();
    packed_outputs.push_back(Binary(*encoded));
  }
  status = writer.Pack(packed_outputs);
  if (!status.ok())
    return status;
  status = writer.Pack(EncodeByteMap(headers));
  if (!status.ok())
    return status;
  return writer.TakeBytes();
}

absl::StatusOr<ActionMessage> ActionMessage::FromMsgpack(
    std::string_view bytes) {
  MsgpackReader reader(bytes);
  absl::StatusOr<nlohmann::json> id_value =
      ReadField(&reader, "ActionMessage.id");
  if (!id_value.ok())
    return id_value.status();
  absl::StatusOr<std::string> id = JsonString(*id_value, "ActionMessage.id");
  if (!id.ok())
    return id.status();
  absl::StatusOr<nlohmann::json> name_value =
      ReadField(&reader, "ActionMessage.name");
  if (!name_value.ok())
    return name_value.status();
  absl::StatusOr<std::string> name =
      JsonString(*name_value, "ActionMessage.name");
  if (!name.ok())
    return name.status();
  auto decode_ports =
      [&reader](std::string_view field) -> absl::StatusOr<std::vector<Port>> {
    absl::StatusOr<nlohmann::json> values = ReadField(&reader, field);
    if (!values.ok())
      return values.status();
    if (!values->is_array()) {
      return absl::InvalidArgumentError(absl::StrCat(field, " must be a list"));
    }
    std::vector<Port> result;
    result.reserve(values->size());
    for (const nlohmann::json& value : *values) {
      absl::StatusOr<Bytes> encoded = GetBinary(value, field);
      if (!encoded.ok())
        return encoded.status();
      absl::StatusOr<Port> port = Port::FromMsgpack(*encoded);
      if (!port.ok())
        return port.status();
      result.push_back(std::move(*port));
    }
    return result;
  };
  absl::StatusOr<std::vector<Port>> inputs =
      decode_ports("ActionMessage.inputs");
  if (!inputs.ok())
    return inputs.status();
  absl::StatusOr<std::vector<Port>> outputs =
      decode_ports("ActionMessage.outputs");
  if (!outputs.ok())
    return outputs.status();
  absl::StatusOr<nlohmann::json> headers_value =
      ReadField(&reader, "ActionMessage.headers");
  if (!headers_value.ok())
    return headers_value.status();
  absl::StatusOr<ByteMap> headers =
      DecodeByteMap(*headers_value, "ActionMessage.headers");
  if (!headers.ok())
    return headers.status();
  absl::Status consumed = reader.EnsureFullyConsumed();
  if (!consumed.ok())
    return consumed;
  ActionMessage result{.id = std::move(*id),
                       .name = std::move(*name),
                       .inputs = std::move(*inputs),
                       .outputs = std::move(*outputs),
                       .headers = std::move(*headers)};
  consumed = result.Validate();
  if (!consumed.ok())
    return consumed;
  return result;
}

size_t WireMessage::ApproxBytes() const {
  size_t result = 8;
  for (const NodeFragment& fragment : node_fragments) {
    result += fragment.ApproxBytes();
  }
  for (const ActionMessage& action : actions)
    result += action.ApproxBytes();
  for (const auto& [key, value] : headers)
    result += key.size() + value.size();
  return result;
}

std::string WireMessage::DebugString() const {
  std::ostringstream stream;
  stream << "WireMessage(headers={";
  size_t header_count = 0;
  for (const auto& [key, value] : headers) {
    if (header_count == kDebugPreviewMaxHeaders)
      break;
    if (header_count != 0)
      stream << ", ";
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
      if (index != 0)
        stream << "; ";
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
      if (index != 0)
        stream << ", ";
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
  absl::Status status;
  for (const NodeFragment& fragment : node_fragments) {
    status = fragment.Validate();
    if (!status.ok())
      return status;
  }
  for (const ActionMessage& action : actions) {
    status = action.Validate();
    if (!status.ok())
      return status;
  }
  for (const auto& [key, unused] : headers) {
    (void)unused;
    status = ValidateName(key);
    if (!status.ok())
      return status;
  }
  return absl::OkStatus();
}

absl::StatusOr<Bytes> WireMessage::ToMsgpack() const {
  absl::Status status = Validate();
  if (!status.ok())
    return status;
  MsgpackWriter writer;
  status = writer.Pack(kVersion);
  if (!status.ok())
    return status;
  nlohmann::json fragments = nlohmann::json::array();
  for (const NodeFragment& fragment : node_fragments) {
    absl::StatusOr<Bytes> encoded = fragment.ToMsgpack();
    if (!encoded.ok())
      return encoded.status();
    fragments.push_back(Binary(*encoded));
  }
  status = writer.Pack(fragments);
  if (!status.ok())
    return status;
  nlohmann::json action_values = nlohmann::json::array();
  for (const ActionMessage& action : actions) {
    absl::StatusOr<Bytes> encoded = action.ToMsgpack();
    if (!encoded.ok())
      return encoded.status();
    action_values.push_back(Binary(*encoded));
  }
  status = writer.Pack(action_values);
  if (!status.ok())
    return status;
  status = writer.Pack(EncodeByteMap(headers));
  if (!status.ok())
    return status;
  return writer.TakeBytes();
}

absl::StatusOr<WireMessage> WireMessage::FromMsgpack(std::string_view bytes) {
  MsgpackReader reader(bytes);
  absl::StatusOr<nlohmann::json> version_value =
      ReadField(&reader, "WireMessage.version");
  if (!version_value.ok())
    return version_value.status();
  absl::StatusOr<std::uint64_t> version =
      JsonUnsigned(*version_value, "WireMessage.version");
  if (!version.ok())
    return version.status();
  if (*version != kVersion) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid serialized WireMessage version: ", *version));
  }
  auto decode_binary_list = [&reader](std::string_view field, auto decode_one) {
    using Result = decltype(decode_one(std::string_view{}));
    using Value = typename Result::value_type;
    absl::StatusOr<nlohmann::json> values = ReadField(&reader, field);
    if (!values.ok())
      return absl::StatusOr<std::vector<Value>>(values.status());
    if (!values->is_array()) {
      return absl::StatusOr<std::vector<Value>>(
          absl::InvalidArgumentError(absl::StrCat(field, " must be a list")));
    }
    std::vector<Value> result;
    result.reserve(values->size());
    for (const nlohmann::json& value : *values) {
      absl::StatusOr<Bytes> encoded = GetBinary(value, field);
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
  absl::StatusOr<std::vector<NodeFragment>> fragments = decode_binary_list(
      "WireMessage.node_fragments", NodeFragment::FromMsgpack);
  if (!fragments.ok())
    return fragments.status();
  absl::StatusOr<std::vector<ActionMessage>> actions =
      decode_binary_list("WireMessage.actions", ActionMessage::FromMsgpack);
  if (!actions.ok())
    return actions.status();
  absl::StatusOr<nlohmann::json> headers_value =
      ReadField(&reader, "WireMessage.headers");
  if (!headers_value.ok())
    return headers_value.status();
  absl::StatusOr<ByteMap> headers =
      DecodeByteMap(*headers_value, "WireMessage.headers");
  if (!headers.ok())
    return headers.status();
  absl::Status consumed = reader.EnsureFullyConsumed();
  if (!consumed.ok())
    return consumed;
  WireMessage result{.node_fragments = std::move(*fragments),
                     .actions = std::move(*actions),
                     .headers = std::move(*headers)};
  consumed = result.Validate();
  if (!consumed.ok())
    return consumed;
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
