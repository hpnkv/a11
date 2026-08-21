// Copyright 2026 The A11 Authors.

#include "sdk/flow/actions/options.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/strings/strip.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

namespace a11::sdk::flow {
namespace {

/// The units the language writes a duration in, and their spelling.
///
/// A second copy of `a11::flow::ParseDuration`, deliberately. That one lives in
/// a11::flow_runtime -- the half of the language that executes flows -- and
/// depending on it from here would mean a host that wants `read_file` links the
/// whole flow engine to get it, and would point the dependency the wrong way
/// round: the runtime is what *calls* these actions.
///
/// The duplication is bounded to this table and the loop below, and the tests
/// pin both against the spellings the language accepts. If a third copy ever
/// wants writing, the answer is to move this into a11::core rather than to
/// write it again.
struct Unit {
  std::string_view name;
  double seconds;
};

constexpr Unit kUnits[] = {
    {"h", 3600.0}, {"m", 60.0},  {"s", 1.0},
    {"ms", 1e-3},  {"us", 1e-6}, {"ns", 1e-9},
};

/// Names a JSON value the way an error message wants it named.
std::string_view Describe(const nlohmann::json& value) {
  switch (value.type()) {
    case nlohmann::json::value_t::null:
      return "null";
    case nlohmann::json::value_t::object:
      return "an object";
    case nlohmann::json::value_t::array:
      return "a list";
    case nlohmann::json::value_t::string:
      return "a string";
    case nlohmann::json::value_t::boolean:
      return "a boolean";
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned:
      return "an integer";
    case nlohmann::json::value_t::number_float:
      return "a number";
    default:
      return "something else";
  }
}

}  // namespace

absl::StatusOr<Options> Options::Parse(const nlohmann::json* value) {
  if (value == nullptr || value->is_null()) {
    return Options{};
  }
  if (!value->is_object()) {
    return absl::InvalidArgumentError(
        absl::StrCat("options must be an object, got ", Describe(*value)));
  }
  return Options(*value);
}

bool Options::Has(std::string_view key) const {
  return value_.contains(key) && !value_.at(std::string(key)).is_null();
}

absl::Status Options::Wrong(std::string_view key,
                            std::string_view expected) const {
  const nlohmann::json& value = value_.at(std::string(key));
  return absl::InvalidArgumentError(absl::StrCat(path_, ".", key, " must be ",
                                                 expected, ", got ",
                                                 Describe(value)));
}

absl::StatusOr<bool> Options::Bool(std::string_view key, bool fallback) const {
  if (!Has(key)) {
    return fallback;
  }
  const nlohmann::json& value = value_.at(std::string(key));
  if (!value.is_boolean()) {
    return Wrong(key, "a boolean");
  }
  return value.get<bool>();
}

absl::StatusOr<std::int64_t> Options::Int(std::string_view key,
                                          std::int64_t fallback) const {
  if (!Has(key)) {
    return fallback;
  }
  const nlohmann::json& value = value_.at(std::string(key));
  if (!value.is_number_integer() && !value.is_number_unsigned()) {
    return Wrong(key, "an integer");
  }
  return value.get<std::int64_t>();
}

absl::StatusOr<std::int64_t> Options::IntInRange(std::string_view key,
                                                 std::int64_t fallback,
                                                 std::int64_t minimum,
                                                 std::int64_t maximum) const {
  ABSL_ASSIGN_OR_RETURN(const std::int64_t value, Int(key, fallback));
  if (value < minimum || value > maximum) {
    return absl::InvalidArgumentError(
        absl::StrCat(path_, ".", key, " must be between ", minimum, " and ",
                     maximum, ", got ", value));
  }
  return value;
}

absl::StatusOr<std::uint64_t> Options::Bytes(std::string_view key,
                                             std::uint64_t fallback) const {
  if (!Has(key)) {
    return fallback;
  }
  ABSL_ASSIGN_OR_RETURN(const std::int64_t value, Int(key, 0));
  if (value < 0) {
    return absl::InvalidArgumentError(absl::StrCat(
        path_, ".", key, " must not be negative, got ", value));
  }
  return static_cast<std::uint64_t>(value);
}

absl::StatusOr<std::string> Options::String(std::string_view key,
                                            std::string_view fallback) const {
  if (!Has(key)) {
    return std::string(fallback);
  }
  const nlohmann::json& value = value_.at(std::string(key));
  if (!value.is_string()) {
    return Wrong(key, "a string");
  }
  return value.get<std::string>();
}

absl::StatusOr<std::string> Options::Enum(
    std::string_view key, std::string_view fallback,
    const std::vector<std::string_view>& allowed) const {
  ABSL_ASSIGN_OR_RETURN(const std::string value, String(key, fallback));
  if (std::find(allowed.begin(), allowed.end(), value) == allowed.end()) {
    return absl::InvalidArgumentError(
        absl::StrCat(path_, ".", key, " must be one of ",
                     absl::StrJoin(allowed, ", "), ", got '", value, "'"));
  }
  return value;
}

absl::StatusOr<std::vector<std::string>> Options::StringList(
    std::string_view key) const {
  std::vector<std::string> names;
  if (!Has(key)) {
    return names;
  }
  const nlohmann::json& value = value_.at(std::string(key));
  // A bare string counts as a list of one, because `omit: "body"` is what
  // somebody writes when they mean one port and reading it as an error would
  // be pedantry rather than a diagnosis.
  if (value.is_string()) {
    names.push_back(value.get<std::string>());
    return names;
  }
  if (!value.is_array()) {
    return Wrong(key, "a list of strings");
  }
  names.reserve(value.size());
  for (const nlohmann::json& element : value) {
    if (!element.is_string()) {
      return absl::InvalidArgumentError(
          absl::StrCat(path_, ".", key, " must hold strings, got ",
                       Describe(element)));
    }
    names.push_back(element.get<std::string>());
  }
  return names;
}

absl::StatusOr<absl::Duration> Options::Duration(
    std::string_view key, absl::Duration fallback) const {
  if (!Has(key)) {
    return fallback;
  }
  const nlohmann::json& value = value_.at(std::string(key));
  if (value.is_number()) {
    const double seconds = value.get<double>();
    if (seconds < 0) {
      return absl::InvalidArgumentError(absl::StrCat(
          path_, ".", key,
          " must not be negative; leave it out to mean no limit"));
    }
    return absl::Seconds(seconds);
  }
  if (!value.is_string()) {
    return Wrong(key, "a duration");
  }
  ABSL_ASSIGN_OR_RETURN(const absl::Duration parsed,
                        ParseDuration(value.get<std::string>()));
  if (parsed < absl::ZeroDuration()) {
    return absl::InvalidArgumentError(absl::StrCat(
        path_, ".", key,
        " must not be negative; leave it out to mean no limit"));
  }
  return parsed;
}

absl::StatusOr<Options> Options::Object(std::string_view key) const {
  if (!Has(key)) {
    return Options{};
  }
  const nlohmann::json& value = value_.at(std::string(key));
  if (!value.is_object()) {
    return Wrong(key, "an object");
  }
  Options nested(value);
  nested.path_ = absl::StrCat(path_, ".", key);
  return nested;
}

absl::StatusOr<std::vector<std::string>> Options::Omit() const {
  return StringList("omit");
}

absl::StatusOr<absl::Duration> ParseDuration(std::string_view text) {
  const std::string_view trimmed = absl::StripAsciiWhitespace(text);
  const auto refuse = [text]() {
    return absl::InvalidArgumentError(absl::StrCat(
        "'", text,
        "' is not a duration; write one as 30s, 250ms, 1m30s, or a number of "
        "seconds"));
  };
  if (trimmed.empty()) {
    return refuse();
  }
  const std::string lowered = absl::AsciiStrToLower(trimmed);
  if (lowered == "forever" || lowered == "infinite" || lowered == "inf") {
    return absl::InfiniteDuration();
  }

  double total = 0.0;
  double sign = 1.0;
  std::size_t at = 0;
  int pieces = 0;
  while (at < trimmed.size()) {
    if (trimmed[at] == ' ' || trimmed[at] == '\t') {
      ++at;
      continue;
    }
    const std::size_t began = at;
    if (trimmed[at] == '+' || trimmed[at] == '-') {
      ++at;
    }
    const std::size_t whole = at;
    while (at < trimmed.size() && absl::ascii_isdigit(trimmed[at])) {
      ++at;
    }
    if (at == whole) {
      return refuse();
    }
    if (at < trimmed.size() && trimmed[at] == '.') {
      ++at;
      const std::size_t fraction = at;
      while (at < trimmed.size() && absl::ascii_isdigit(trimmed[at])) {
        ++at;
      }
      if (at == fraction) {
        return refuse();
      }
    }
    const std::string_view number = trimmed.substr(began, at - began);
    while (at < trimmed.size() &&
           (trimmed[at] == ' ' || trimmed[at] == '\t')) {
      ++at;
    }
    const std::size_t word = at;
    while (at < trimmed.size() && absl::ascii_isalpha(trimmed[at])) {
      ++at;
    }
    const std::string_view unit = trimmed.substr(word, at - word);
    double scale = 1.0;
    if (!unit.empty()) {
      bool known = false;
      for (const Unit& one : kUnits) {
        if (one.name == unit) {
          scale = one.seconds;
          known = true;
          break;
        }
      }
      if (!known) {
        return refuse();
      }
    }
    // `-1m30s` is a minute and a half, backwards -- not a minute back and half
    // a second forwards. The sign belongs to the whole.
    if (pieces == 0 && absl::StartsWith(number, "-")) {
      sign = -1.0;
    }
    double magnitude = 0.0;
    if (!absl::SimpleAtod(absl::StripPrefix(absl::StripPrefix(number, "-"), "+"),
                          &magnitude)) {
      return refuse();
    }
    total += magnitude * scale;
    ++pieces;
  }
  if (pieces == 0) {
    return refuse();
  }
  return absl::Seconds(sign * total);
}

absl::StatusOr<absl::Time> ParseDeadlineHeader(std::string_view value) {
  // Bare is milliseconds and an `ns` suffix is nanoseconds -- the wire format,
  // whose two units have been read the wrong way round before now.
  const bool nanoseconds = absl::EndsWith(value, "ns");
  const std::string_view digits =
      nanoseconds ? absl::StripSuffix(value, "ns") : value;
  std::int64_t count = 0;
  if (digits.empty() || !absl::SimpleAtoi(digits, &count)) {
    return absl::InvalidArgumentError(absl::StrCat(
        "x-a11-deadline must be a count of milliseconds since the epoch, or "
        "nanoseconds with an 'ns' suffix; got '",
        value, "'"));
  }
  return absl::UnixEpoch() +
         (nanoseconds ? absl::Nanoseconds(count) : absl::Milliseconds(count));
}

}  // namespace a11::sdk::flow
