// Copyright 2026 The A11 Authors.

#include "a11/json_codec.h"

#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <nlohmann/json.hpp>

#include "a11/utf8.h"

namespace a11 {
namespace {

using Json = nlohmann::json;

}  // namespace

absl::StatusOr<Json> ParseJson(std::string_view encoded,
                               std::string_view what) {
  // nlohmann's own non-throwing overload: `allow_exceptions = false` hands back
  // a discarded value instead of raising. It also discards the reason, and the
  // reason is most of what makes a parse error actionable -- so the parse is
  // repeated through the throwing overload only when the first one failed,
  // where the cost does not matter and the message does.
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
    const std::vector<std::uint8_t> encoded = Json::to_msgpack(value);
    return std::string(reinterpret_cast<const char*>(encoded.data()),
                       encoded.size());
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
  const auto* first = reinterpret_cast<const std::uint8_t*>(encoded.data());
  const auto* last = first + encoded.size();
  // Same shape as ParseJson: the cheap non-throwing decode first, and the
  // throwing one only to recover the reason for a failure.
  Json value = Json::from_msgpack(first, last, /*strict=*/true,
                                  /*allow_exceptions=*/false);
  if (!value.is_discarded()) {
    return value;
  }
  try {
    return Json::from_msgpack(first, last, true, true);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid ", what, " MessagePack data: ", error.what()));
  } catch (...) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Invalid ", what, " MessagePack data raised a non-standard exception"));
  }
}

}  // namespace a11
