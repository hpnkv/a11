// Copyright 2026 The A11 Authors.

#include "a11/json_codec.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <nlohmann/json.hpp>

#include "a11/utf8.h"

namespace a11 {
namespace {

using Json = nlohmann::json;

/// Nesting depth a document may reach, matching data/msgpack.h's scanner.
constexpr int kMaxMsgpackNesting = 256;

/**
 * MessagePack bytes to a JSON value, marker by marker.
 *
 * The representation matches nlohmann::json::from_msgpack: positive fixint and
 * uint8..uint64 land unsigned, negative fixint and int8..int64 signed, float32
 * widens to double, str is a string, bin is a binary, ext is a binary carrying
 * its subtype byte, and a map key that is not a MessagePack string is an error.
 * MsgpackDecodeTest compares the two decoders over every size class.
 *
 * String and binary payloads are appended in one call each. nlohmann's
 * binary_reader::get_string and get_binary copy a payload with one push_back
 * per byte, which prices a decode per payload byte rather than per field; see
 * `data/chunk_codec` in bench/.
 */
class MsgpackDecoder {
 public:
  explicit MsgpackDecoder(std::string_view bytes) : bytes_(bytes) {}

  /// Decode one value, advancing the cursor past it.
  absl::StatusOr<Json> Decode(int depth);

  /// Bytes consumed so far.
  [[nodiscard]] size_t position() const { return position_; }

 private:
  absl::Status Need(size_t count) const {
    if (position_ > bytes_.size() || count > bytes_.size() - position_) {
      return absl::InvalidArgumentError("truncated");
    }
    return absl::OkStatus();
  }

  /// Read the big-endian integer MessagePack uses for every length and number.
  template <typename Unsigned>
  absl::StatusOr<Unsigned> ReadBigEndian() {
    ABSL_RETURN_IF_ERROR(Need(sizeof(Unsigned)));
    Unsigned value = 0;
    for (size_t index = 0; index < sizeof(Unsigned); ++index) {
      value = static_cast<Unsigned>(static_cast<Unsigned>(value << 8U) |
                                    static_cast<unsigned char>(
                                        bytes_[position_ + index]));
    }
    position_ += sizeof(Unsigned);
    return value;
  }

  /// A view of the next @p length payload bytes, advancing past them.
  absl::StatusOr<std::string_view> ReadPayload(std::uint64_t length) {
    if (length > std::numeric_limits<size_t>::max()) {
      return absl::ResourceExhaustedError("value is too large");
    }
    const auto count = static_cast<size_t>(length);
    ABSL_RETURN_IF_ERROR(Need(count));
    const std::string_view payload = bytes_.substr(position_, count);
    position_ += count;
    return payload;
  }

  /// The length field of a str marker, or an error for any other marker.
  absl::StatusOr<std::uint64_t> ReadStringLength(std::uint8_t marker);
  absl::StatusOr<std::string> DecodeString(std::uint8_t marker);
  absl::StatusOr<Json> DecodeBinary(std::uint8_t marker);
  absl::StatusOr<Json> DecodeArray(std::uint64_t count, int depth);
  absl::StatusOr<Json> DecodeObject(std::uint64_t count, int depth);

  std::string_view bytes_;
  size_t position_ = 0;
};

absl::StatusOr<std::uint64_t> MsgpackDecoder::ReadStringLength(
    std::uint8_t marker) {
  if ((marker & 0xe0U) == 0xa0U) {  // fixstr
    return marker & 0x1fU;
  }
  switch (marker) {
    case 0xd9:
      return ReadBigEndian<std::uint8_t>();
    case 0xda:
      return ReadBigEndian<std::uint16_t>();
    case 0xdb:
      return ReadBigEndian<std::uint32_t>();
    default:
      return absl::InvalidArgumentError(
          absl::StrCat("expected a string, found marker 0x",
                       absl::Hex(marker, absl::kZeroPad2)));
  }
}

absl::StatusOr<std::string> MsgpackDecoder::DecodeString(std::uint8_t marker) {
  ABSL_ASSIGN_OR_RETURN(const std::uint64_t length, ReadStringLength(marker));
  ABSL_ASSIGN_OR_RETURN(const std::string_view payload, ReadPayload(length));
  return std::string(payload);
}

absl::StatusOr<Json> MsgpackDecoder::DecodeBinary(std::uint8_t marker) {
  std::uint64_t length = 0;
  bool has_subtype = false;
  switch (marker) {
    case 0xc4: {
      ABSL_ASSIGN_OR_RETURN(length, ReadBigEndian<std::uint8_t>());
      break;
    }
    case 0xc5: {
      ABSL_ASSIGN_OR_RETURN(length, ReadBigEndian<std::uint16_t>());
      break;
    }
    case 0xc6: {
      ABSL_ASSIGN_OR_RETURN(length, ReadBigEndian<std::uint32_t>());
      break;
    }
    // ext carries its length before the subtype byte; fixext has the subtype
    // byte and a length the marker names.
    case 0xc7: {
      ABSL_ASSIGN_OR_RETURN(length, ReadBigEndian<std::uint8_t>());
      has_subtype = true;
      break;
    }
    case 0xc8: {
      ABSL_ASSIGN_OR_RETURN(length, ReadBigEndian<std::uint16_t>());
      has_subtype = true;
      break;
    }
    case 0xc9: {
      ABSL_ASSIGN_OR_RETURN(length, ReadBigEndian<std::uint32_t>());
      has_subtype = true;
      break;
    }
    case 0xd4:
      length = 1;
      has_subtype = true;
      break;
    case 0xd5:
      length = 2;
      has_subtype = true;
      break;
    case 0xd6:
      length = 4;
      has_subtype = true;
      break;
    case 0xd7:
      length = 8;
      has_subtype = true;
      break;
    case 0xd8:
      length = 16;
      has_subtype = true;
      break;
    default:
      return absl::InternalError("Unreachable MessagePack binary marker");
  }
  std::uint8_t subtype = 0;
  if (has_subtype) {
    ABSL_ASSIGN_OR_RETURN(subtype, ReadBigEndian<std::uint8_t>());
  }
  ABSL_ASSIGN_OR_RETURN(const std::string_view payload, ReadPayload(length));
  std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
  return has_subtype ? Json::binary(std::move(bytes), subtype)
                     : Json::binary(std::move(bytes));
}

absl::StatusOr<Json> MsgpackDecoder::DecodeArray(std::uint64_t count,
                                                 int depth) {
  Json value = Json::array();
  for (std::uint64_t index = 0; index < count; ++index) {
    ABSL_ASSIGN_OR_RETURN(Json element, Decode(depth + 1));
    value.emplace_back(std::move(element));
  }
  return value;
}

absl::StatusOr<Json> MsgpackDecoder::DecodeObject(std::uint64_t count,
                                                  int depth) {
  Json value = Json::object();
  for (std::uint64_t index = 0; index < count; ++index) {
    ABSL_RETURN_IF_ERROR(Need(1));
    const auto marker = static_cast<std::uint8_t>(bytes_[position_++]);
    ABSL_ASSIGN_OR_RETURN(const std::string key, DecodeString(marker));
    ABSL_ASSIGN_OR_RETURN(Json element, Decode(depth + 1));
    // Assignment rather than emplace, so a repeated key keeps the last value
    // the way from_msgpack's SAX consumer does.
    value[key] = std::move(element);
  }
  return value;
}

absl::StatusOr<Json> MsgpackDecoder::Decode(int depth) {
  if (depth > kMaxMsgpackNesting) {
    return absl::ResourceExhaustedError("nesting is too deep");
  }
  ABSL_RETURN_IF_ERROR(Need(1));
  const auto marker = static_cast<std::uint8_t>(bytes_[position_++]);

  if (marker <= 0x7f) {  // positive fixint
    return Json(static_cast<std::uint64_t>(marker));
  }
  if (marker >= 0xe0) {  // negative fixint
    return Json(static_cast<std::int64_t>(static_cast<std::int8_t>(marker)));
  }
  if ((marker & 0xe0U) == 0xa0U) {  // fixstr
    ABSL_ASSIGN_OR_RETURN(std::string text, DecodeString(marker));
    return Json(std::move(text));
  }
  if ((marker & 0xf0U) == 0x90U) {  // fixarray
    return DecodeArray(marker & 0x0fU, depth);
  }
  if ((marker & 0xf0U) == 0x80U) {  // fixmap
    return DecodeObject(marker & 0x0fU, depth);
  }

  switch (marker) {
    case 0xc0:
      return Json(nullptr);
    case 0xc2:
      return Json(false);
    case 0xc3:
      return Json(true);
    case 0xc4:
    case 0xc5:
    case 0xc6:
    case 0xc7:
    case 0xc8:
    case 0xc9:
    case 0xd4:
    case 0xd5:
    case 0xd6:
    case 0xd7:
    case 0xd8:
      return DecodeBinary(marker);
    case 0xca: {
      ABSL_ASSIGN_OR_RETURN(const std::uint32_t bits,
                            ReadBigEndian<std::uint32_t>());
      return Json(static_cast<double>(std::bit_cast<float>(bits)));
    }
    case 0xcb: {
      ABSL_ASSIGN_OR_RETURN(const std::uint64_t bits,
                            ReadBigEndian<std::uint64_t>());
      return Json(std::bit_cast<double>(bits));
    }
    case 0xcc: {
      ABSL_ASSIGN_OR_RETURN(const std::uint8_t number,
                            ReadBigEndian<std::uint8_t>());
      return Json(static_cast<std::uint64_t>(number));
    }
    case 0xcd: {
      ABSL_ASSIGN_OR_RETURN(const std::uint16_t number,
                            ReadBigEndian<std::uint16_t>());
      return Json(static_cast<std::uint64_t>(number));
    }
    case 0xce: {
      ABSL_ASSIGN_OR_RETURN(const std::uint32_t number,
                            ReadBigEndian<std::uint32_t>());
      return Json(static_cast<std::uint64_t>(number));
    }
    case 0xcf: {
      ABSL_ASSIGN_OR_RETURN(const std::uint64_t number,
                            ReadBigEndian<std::uint64_t>());
      return Json(number);
    }
    case 0xd0: {
      ABSL_ASSIGN_OR_RETURN(const std::uint8_t raw,
                            ReadBigEndian<std::uint8_t>());
      return Json(static_cast<std::int64_t>(static_cast<std::int8_t>(raw)));
    }
    case 0xd1: {
      ABSL_ASSIGN_OR_RETURN(const std::uint16_t raw,
                            ReadBigEndian<std::uint16_t>());
      return Json(static_cast<std::int64_t>(static_cast<std::int16_t>(raw)));
    }
    case 0xd2: {
      ABSL_ASSIGN_OR_RETURN(const std::uint32_t raw,
                            ReadBigEndian<std::uint32_t>());
      return Json(static_cast<std::int64_t>(static_cast<std::int32_t>(raw)));
    }
    case 0xd3: {
      ABSL_ASSIGN_OR_RETURN(const std::uint64_t raw,
                            ReadBigEndian<std::uint64_t>());
      return Json(static_cast<std::int64_t>(raw));
    }
    case 0xd9:
    case 0xda:
    case 0xdb: {
      ABSL_ASSIGN_OR_RETURN(std::string text, DecodeString(marker));
      return Json(std::move(text));
    }
    case 0xdc: {
      ABSL_ASSIGN_OR_RETURN(const std::uint16_t count,
                            ReadBigEndian<std::uint16_t>());
      return DecodeArray(count, depth);
    }
    case 0xdd: {
      ABSL_ASSIGN_OR_RETURN(const std::uint32_t count,
                            ReadBigEndian<std::uint32_t>());
      return DecodeArray(count, depth);
    }
    case 0xde: {
      ABSL_ASSIGN_OR_RETURN(const std::uint16_t count,
                            ReadBigEndian<std::uint16_t>());
      return DecodeObject(count, depth);
    }
    case 0xdf: {
      ABSL_ASSIGN_OR_RETURN(const std::uint32_t count,
                            ReadBigEndian<std::uint32_t>());
      return DecodeObject(count, depth);
    }
    default:
      return absl::InvalidArgumentError(
          absl::StrCat("unsupported marker 0x",
                       absl::Hex(marker, absl::kZeroPad2)));
  }
}

}  // namespace

absl::StatusOr<Json> ParseJson(std::string_view encoded,
                               std::string_view what) {
  // nlohmann's own non-throwing overload: `allow_exceptions = false` hands back
  // a discarded value instead of raising.
  Json value = Json::parse(encoded.begin(), encoded.end(), nullptr,
                           /*allow_exceptions=*/false,
                           /*ignore_comments=*/false);
  if (!value.is_discarded()) {
    return value;
  }
  try {
    return Json::parse(encoded.begin(), encoded.end(), nullptr, true, false);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Failed to parse ", what, ": ", error.what()));
  } catch (...) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Failed to parse ", what, " with a non-standard exception"));
  }
}

bool IsValidUtf8(std::string_view text) {
  return utf8::IsValid(text);
}

const Json* FindUnencodableString(const Json& value) {
  std::vector<const Json*> pending{&value};
  while (!pending.empty()) {
    const Json* one = pending.back();
    pending.pop_back();
    if (one->is_string()) {
      if (!IsValidUtf8(one->get_ref<const std::string&>())) {
        return one;
      }
      continue;
    }
    if (one->is_object() || one->is_array()) {
      for (const auto& element : *one) {
        pending.push_back(&element);
      }
    }
  }
  return nullptr;
}

absl::StatusOr<std::string> DumpJson(const Json& value, std::string_view what) {
  // Checked before nlohmann is asked, because nlohmann's answer to this one is
  // `std::abort()` in every `-fno-exceptions` TU and the linker picks which
  // instantiation of `dump()` survives. See the header for the whole story.
  if (FindUnencodableString(value) != nullptr) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Failed to serialize ", what,
        ": it holds bytes that are not valid UTF-8, which JSON has no spelling "
        "for. MessagePack carries them exactly, and base64 carries them "
        "through JSON."));
  }
  // No non-throwing form of a strict dump exists: error_handler_t::strict is
  // the request to be told, and being told means a throw. Kept for everything
  // that is not a bad string, where a throw is still a throw here.
  try {
    return value.dump(-1, ' ', false, Json::error_handler_t::strict);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Failed to serialize ", what, ": ", error.what()));
  } catch (...) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Failed to serialize ", what, " with a non-standard exception"));
  }
}

std::string DumpJsonLossy(const Json& value) {
  // Cannot fail: replace substitutes U+FFFD for anything that is not valid
  // UTF-8, which is why this one needs no Status and no try.
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

absl::StatusOr<std::string> PackMsgpack(const Json& value,
                                        std::string_view what) {
  // to_msgpack has no allow_exceptions overload, and it does raise: a string
  // field holding invalid UTF-8 is type_error.316.
  try {
    // The string-backed output adapter, so the writer appends into the buffer
    // this function returns. The vector-returning overload is one copy wider.
    std::string encoded;
    Json::to_msgpack(value, nlohmann::detail::output_adapter<char>(encoded));
    return encoded;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Failed to encode ", what, " as MessagePack: ", error.what()));
  } catch (...) {
    return absl::InvalidArgumentError(
        absl::StrCat("Failed to encode ", what,
                     " as MessagePack with a non-standard exception"));
  }
}

absl::StatusOr<Json> UnpackMsgpack(std::string_view encoded,
                                   std::string_view what) {
  MsgpackDecoder decoder(encoded);
  absl::StatusOr<Json> value = decoder.Decode(0);
  if (!value.ok()) {
    return absl::Status(
        value.status().code(),
        absl::StrCat("Invalid ", what, " MessagePack data: ",
                     value.status().message()));
  }
  // Strict, as from_msgpack's `strict` argument was: one record is one value.
  if (decoder.position() != encoded.size()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Invalid ", what, " MessagePack data: ",
        encoded.size() - decoder.position(), " trailing bytes"));
  }
  return value;
}

}  // namespace a11
