// Copyright 2026 The A11 Authors.

#include "a11/data/msgpack.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <nlohmann/json.hpp>

#include "a11/json_codec.h"
#include "a11/status.h"

namespace a11::data {
namespace {

constexpr int kMaxNesting = 256;

absl::Status Need(size_t position, size_t count, size_t total) {
  if (position > total || count > total - position) {
    return absl::InvalidArgumentError("Truncated MessagePack data");
  }
  return absl::OkStatus();
}

absl::StatusOr<std::uint64_t> ReadUnsigned(std::string_view bytes,
                                           size_t* position, size_t width) {
  ABSL_RETURN_IF_ERROR(Need(*position, width, bytes.size()));
  std::uint64_t result = 0;
  for (size_t index = 0; index < width; ++index) {
    result =
        (result << 8U) | static_cast<unsigned char>(bytes[*position + index]);
  }
  *position += width;
  return result;
}

absl::Status SkipValue(std::string_view bytes, size_t* position, int depth) {
  if (depth > kMaxNesting) {
    return absl::ResourceExhaustedError("MessagePack nesting is too deep");
  }
  ABSL_RETURN_IF_ERROR(Need(*position, 1, bytes.size()));
  const std::uint8_t marker = static_cast<std::uint8_t>(bytes[(*position)++]);

  if (marker <= 0x7f || marker >= 0xe0 || marker == 0xc0 || marker == 0xc2 ||
      marker == 0xc3) {
    return absl::OkStatus();
  }
  if ((marker & 0xe0U) == 0xa0U) {
    const size_t length = marker & 0x1fU;
    const absl::Status status = Need(*position, length, bytes.size());
    if (status.ok()) {
      *position += length;
    }
    return status;
  }
  if ((marker & 0xf0U) == 0x90U || (marker & 0xf0U) == 0x80U) {
    const bool is_map = (marker & 0xf0U) == 0x80U;
    std::uint64_t count = marker & 0x0fU;
    if (is_map) {
      count *= 2;
    }
    for (std::uint64_t index = 0; index < count; ++index) {
      ABSL_RETURN_IF_ERROR(SkipValue(bytes, position, depth + 1));
    }
    return absl::OkStatus();
  }

  size_t fixed_width = 0;
  switch (marker) {
    case 0xc4:
    case 0xd9:
      fixed_width = 1;
      break;
    case 0xc5:
    case 0xda:
      fixed_width = 2;
      break;
    case 0xc6:
    case 0xdb:
      fixed_width = 4;
      break;
    case 0xca:
      fixed_width = 4;
      break;
    case 0xcb:
      fixed_width = 8;
      break;
    case 0xcc:
    case 0xd0:
      fixed_width = 1;
      break;
    case 0xcd:
    case 0xd1:
      fixed_width = 2;
      break;
    case 0xce:
    case 0xd2:
      fixed_width = 4;
      break;
    case 0xcf:
    case 0xd3:
      fixed_width = 8;
      break;
    case 0xd4:
      fixed_width = 2;  // one-byte payload plus extension type
      break;
    case 0xd5:
      fixed_width = 3;
      break;
    case 0xd6:
      fixed_width = 5;
      break;
    case 0xd7:
      fixed_width = 9;
      break;
    case 0xd8:
      fixed_width = 17;
      break;
    default:
      break;
  }
  if (fixed_width != 0) {
    const absl::Status status = Need(*position, fixed_width, bytes.size());
    if (status.ok()) {
      *position += fixed_width;
    }
    return status;
  }

  if (marker == 0xdc || marker == 0xdd || marker == 0xde || marker == 0xdf) {
    const size_t width = (marker == 0xdc || marker == 0xde) ? 2 : 4;
    ABSL_ASSIGN_OR_RETURN(std::uint64_t count,
                          ReadUnsigned(bytes, position, width));
    const bool is_map = marker == 0xde || marker == 0xdf;
    if (is_map) {
      if (count > std::numeric_limits<std::uint64_t>::max() / 2) {
        return absl::ResourceExhaustedError("MessagePack map is too large");
      }
      count *= 2;
    }
    for (std::uint64_t index = 0; index < count; ++index) {
      ABSL_RETURN_IF_ERROR(SkipValue(bytes, position, depth + 1));
    }
    return absl::OkStatus();
  }

  if (marker == 0xc7 || marker == 0xc8 || marker == 0xc9) {
    const size_t width = marker == 0xc7 ? 1 : marker == 0xc8 ? 2 : 4;
    ABSL_ASSIGN_OR_RETURN(std::uint64_t length,
                          ReadUnsigned(bytes, position, width));
    if (length > std::numeric_limits<size_t>::max() - 1) {
      return absl::ResourceExhaustedError("MessagePack extension is too large");
    }
    const absl::Status status =
        Need(*position, static_cast<size_t>(length) + 1, bytes.size());
    if (status.ok()) {
      *position += static_cast<size_t>(length) + 1;
    }
    return status;
  }

  if (marker == 0xc4 || marker == 0xc5 || marker == 0xc6 || marker == 0xd9 ||
      marker == 0xda || marker == 0xdb) {
    // Handled above through a length field, not a fixed payload width.
    return absl::InternalError("Unreachable MessagePack length marker");
  }

  return absl::InvalidArgumentError(
      absl::StrCat("Unsupported MessagePack marker: ", marker));
}

// Length-prefixed binary and string markers need payload-aware handling. This
// wrapper handles them before delegating the remaining marker classes.
absl::Status ScanValue(std::string_view bytes, size_t* position, int depth) {
  ABSL_RETURN_IF_ERROR(Need(*position, 1, bytes.size()));
  const std::uint8_t marker = static_cast<std::uint8_t>(bytes[*position]);
  if (marker == 0xc4 || marker == 0xc5 || marker == 0xc6 || marker == 0xd9 ||
      marker == 0xda || marker == 0xdb) {
    ++*position;
    const size_t width = (marker == 0xc4 || marker == 0xd9)   ? 1
                         : (marker == 0xc5 || marker == 0xda) ? 2
                                                              : 4;
    ABSL_ASSIGN_OR_RETURN(std::uint64_t length,
                          ReadUnsigned(bytes, position, width));
    if (length > std::numeric_limits<size_t>::max()) {
      return absl::ResourceExhaustedError("MessagePack value is too large");
    }
    const absl::Status status =
        Need(*position, static_cast<size_t>(length), bytes.size());
    if (status.ok()) {
      *position += static_cast<size_t>(length);
    }
    return status;
  }

  // SkipValue recursively calls itself, so recursively encountered length
  // markers must also route through ScanValue. The array/map implementation is
  // duplicated here to preserve that routing.
  if ((marker & 0xf0U) == 0x90U || (marker & 0xf0U) == 0x80U ||
      marker == 0xdc || marker == 0xdd || marker == 0xde || marker == 0xdf) {
    if (depth > kMaxNesting) {
      return absl::ResourceExhaustedError("MessagePack nesting is too deep");
    }
    ++*position;
    std::uint64_t count = 0;
    bool is_map = false;
    if ((marker & 0xf0U) == 0x90U || (marker & 0xf0U) == 0x80U) {
      count = marker & 0x0fU;
      is_map = (marker & 0xf0U) == 0x80U;
    } else {
      const size_t width = (marker == 0xdc || marker == 0xde) ? 2 : 4;
      ABSL_ASSIGN_OR_RETURN(std::uint64_t parsed,
                            ReadUnsigned(bytes, position, width));
      count = parsed;
      is_map = marker == 0xde || marker == 0xdf;
    }
    if (is_map) {
      if (count > std::numeric_limits<std::uint64_t>::max() / 2) {
        return absl::ResourceExhaustedError("MessagePack map is too large");
      }
      count *= 2;
    }
    for (std::uint64_t index = 0; index < count; ++index) {
      ABSL_RETURN_IF_ERROR(ScanValue(bytes, position, depth + 1));
    }
    return absl::OkStatus();
  }
  return SkipValue(bytes, position, depth);
}

}  // namespace

absl::Status MsgpackWriter::Pack(const nlohmann::json& value) {
  ABSL_ASSIGN_OR_RETURN(const std::string encoded,
                        PackMsgpack(value, "a value"));
  bytes_.append(encoded);
  return absl::OkStatus();
}

absl::StatusOr<nlohmann::json> MsgpackReader::Read() {
  const size_t begin = position_;
  absl::Status status = ScanValue(bytes_, &position_, 0);
  if (!status.ok()) {
    position_ = begin;
    return status;
  }
  absl::StatusOr<nlohmann::json> value = UnpackMsgpack(
      std::string_view(bytes_).substr(begin, position_ - begin), "a value");
  if (!value.ok()) {
    position_ = begin;
    return value.status();
  }
  return value;
}

absl::StatusOr<std::string_view> MsgpackReader::ReadBinaryView() {
  const size_t begin = position_;
  ABSL_RETURN_IF_ERROR(Need(position_, 1, bytes_.size()));
  const auto marker = static_cast<std::uint8_t>(bytes_[position_]);
  if (marker != 0xc4 && marker != 0xc5 && marker != 0xc6) {
    position_ = begin;
    return absl::InvalidArgumentError(
        "Expected MessagePack binary data at this field");
  }
  ++position_;
  const size_t width = marker == 0xc4 ? 1 : (marker == 0xc5 ? 2 : 4);
  absl::StatusOr<std::uint64_t> length =
      ReadUnsigned(bytes_, &position_, width);
  if (!length.ok()) {
    position_ = begin;
    return length.status();
  }
  if (*length > std::numeric_limits<size_t>::max()) {
    position_ = begin;
    return absl::ResourceExhaustedError("MessagePack binary is too large");
  }
  const auto size = static_cast<size_t>(*length);
  const absl::Status available = Need(position_, size, bytes_.size());
  if (!available.ok()) {
    position_ = begin;
    return available;
  }
  const std::string_view view = bytes_.substr(position_, size);
  position_ += size;
  return view;
}

absl::StatusOr<size_t> MsgpackReader::ReadArrayLength() {
  const size_t begin = position_;
  ABSL_RETURN_IF_ERROR(Need(position_, 1, bytes_.size()));
  const auto marker = static_cast<std::uint8_t>(bytes_[position_]);
  if ((marker & 0xf0U) == 0x90U) {
    ++position_;
    return static_cast<size_t>(marker & 0x0fU);
  }
  if (marker != 0xdc && marker != 0xdd) {
    position_ = begin;
    return absl::InvalidArgumentError(
        "Expected a MessagePack array at this field");
  }
  ++position_;
  absl::StatusOr<std::uint64_t> count =
      ReadUnsigned(bytes_, &position_, marker == 0xdc ? 2 : 4);
  if (!count.ok()) {
    position_ = begin;
    return count.status();
  }
  if (*count > std::numeric_limits<size_t>::max()) {
    position_ = begin;
    return absl::ResourceExhaustedError("MessagePack array is too large");
  }
  return static_cast<size_t>(*count);
}

absl::Status MsgpackReader::EnsureFullyConsumed() const {
  if (position_ != bytes_.size()) {
    return absl::InvalidArgumentError("Extra data after deserialization");
  }
  return absl::OkStatus();
}

nlohmann::json Binary(std::string_view bytes) {
  return nlohmann::json::binary(
      std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}

absl::StatusOr<std::string> GetBinary(const nlohmann::json& value,
                                      std::string_view field_name) {
  if (!value.is_binary()) {
    return absl::InvalidArgumentError(
        absl::StrCat(field_name, " must be MessagePack binary data"));
  }
  const auto& bytes = value.get_binary();
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

absl::StatusOr<std::string> PackStatus(const absl::Status& status) {
  MsgpackWriter writer;
  ABSL_RETURN_IF_ERROR(writer.Pack(static_cast<int>(status.code())));
  ABSL_RETURN_IF_ERROR(writer.Pack(std::string(status.message())));
  ABSL_RETURN_IF_ERROR(writer.Pack(StatusDetails(status)));
  return writer.TakeBytes();
}

absl::StatusOr<absl::Status> UnpackStatus(std::string_view bytes) {
  MsgpackReader reader(bytes);
  absl::StatusOr<nlohmann::json> code = reader.Read();
  if (!code.ok()) {
    absl::StatusOr<absl::Status> result;
    result.AssignStatus(code.status());
    return result;
  }
  absl::StatusOr<nlohmann::json> message = reader.Read();
  if (!message.ok()) {
    absl::StatusOr<absl::Status> result;
    result.AssignStatus(message.status());
    return result;
  }
  absl::StatusOr<nlohmann::json> details = reader.Read();
  if (!details.ok()) {
    absl::StatusOr<absl::Status> result;
    result.AssignStatus(details.status());
    return result;
  }
  absl::Status consumed = reader.EnsureFullyConsumed();
  if (!consumed.ok()) {
    absl::StatusOr<absl::Status> result;
    result.AssignStatus(std::move(consumed));
    return result;
  }
  if (!code->is_number_integer() || !message->is_string() ||
      (!details->is_null() && !details->is_array())) {
    absl::StatusOr<absl::Status> result;
    result.AssignStatus(absl::InvalidArgumentError(
        "MessagePack does not contain a valid Status"));
    return result;
  }
  const std::int64_t raw_code = code->get<std::int64_t>();
  if (raw_code < 0 || raw_code > 16) {
    absl::StatusOr<absl::Status> result;
    result.AssignStatus(
        absl::InvalidArgumentError("MessagePack status code is not canonical"));
    return result;
  }
  nlohmann::json detail_array =
      details->is_null() ? nlohmann::json::array() : std::move(*details);
  for (const auto& detail : detail_array) {
    if (!detail.is_object()) {
      absl::StatusOr<absl::Status> result;
      result.AssignStatus(
          absl::InvalidArgumentError("Each Status detail must be an object"));
      return result;
    }
  }
  return absl::StatusOr<absl::Status>(
      std::in_place,
      MakeStatus(static_cast<absl::StatusCode>(raw_code),
                 message->get<std::string>(), std::move(detail_array)));
}

}  // namespace a11::data
