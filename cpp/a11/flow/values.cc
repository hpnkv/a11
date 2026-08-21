// Copyright 2026 The A11 Authors.

#include "a11/flow/values.h"

#include "a11/flow/internal/pattern.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <regex>
#include <set>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/strings/ascii.h>
#include <absl/strings/escaping.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <absl/strings/str_join.h>
#include <absl/strings/str_split.h>
#include <absl/time/clock.h>
#include <nlohmann/json.hpp>

#include "a11/data/serialization.h"
#include "a11/flow/vocabulary.h"
#include "a11/time.h"

namespace a11::flow {
namespace {

absl::Status Invalid(std::string_view message) {
  return absl::InvalidArgumentError(message);
}

/// The unit table, largest first: the compact rendering walks it in this order,
/// and a `%(spec)` looks a unit up in it. The same units a duration may be
/// *written* in, because what a flow reads back it has to be able to write.
struct Unit {
  std::string_view name;
  double seconds;
};

constexpr Unit kUnits[] = {
    {"h", 3600.0}, {"m", 60.0},   {"s", 1.0},
    {"ms", 1e-3},  {"us", 1e-6},  {"ns", 1e-9},
};

/// A double as Python's `repr` writes it: the shortest text that reads back as
/// the same number, with a `.0` where it would otherwise look like an integer.
///
/// The spelling is part of the contract -- `text 3.0` has always been `"3.0"`
/// -- and the round-trip search is what `repr` itself does.
std::string FormatDouble(double value) {
  if (std::isnan(value)) return "nan";
  if (std::isinf(value)) return value > 0 ? "inf" : "-inf";
  if (value == 0.0) return std::signbit(value) ? "-0.0" : "0.0";
  // How many significant digits it takes to read the same number back.
  int digits = 17;
  std::string scientific;
  for (int precision = 1; precision <= 17; ++precision) {
    scientific = absl::StrFormat("%.*e", precision - 1, value);
    if (std::strtod(scientific.c_str(), nullptr) == value) {
      digits = precision;
      break;
    }
  }
  const size_t marker = scientific.find('e');
  const int exponent =
      static_cast<int>(std::strtol(scientific.c_str() + marker + 1, nullptr, 10));
  // `repr` switches to an exponent outside this window, and only outside it:
  // `%g` would switch as soon as the exponent reached the digit count, which
  // renders 90.0 as `9e+01` and is not what the language has ever written.
  if (exponent < -4 || exponent >= 16) return scientific;
  std::string fixed =
      absl::StrFormat("%.*f", std::max(0, digits - 1 - exponent), value);
  if (fixed.find('.') == std::string::npos) return absl::StrCat(fixed, ".0");
  while (fixed.size() > 1 && fixed.back() == '0' &&
         fixed[fixed.size() - 2] != '.') {
    fixed.pop_back();
  }
  return fixed;
}

/// A number without a pointless trailing `.0`, which is what a `%(ms)d`-style
/// unit conversion wants: a bare count, ready to go into a metric.
std::string TrimNumber(double value) {
  if (std::isfinite(value) && value == std::floor(value) &&
      std::abs(value) < 1e18) {
    return absl::StrCat(static_cast<std::int64_t>(value));
  }
  return FormatDouble(value);
}

/// How many code points a string holds.
///
/// `len` counts characters, not bytes: the Python reference counted them and a
/// flow trimming a model's answer to a length means the length a person sees.
size_t Utf8Length(std::string_view text) {
  size_t count = 0;
  for (const char letter : text) {
    if ((static_cast<unsigned char>(letter) & 0xC0) != 0x80) ++count;
  }
  return count;
}

/// The byte offset `index` code points into `text`, clamped to its end.
size_t Utf8Offset(std::string_view text, size_t index) {
  size_t seen = 0;
  for (size_t at = 0; at < text.size(); ++at) {
    if ((static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) continue;
    if (seen == index) return at;
    ++seen;
  }
  return text.size();
}

/// `text` between two code-point positions, as Python's slicing would cut it.
std::string Utf8Slice(std::string_view text, std::int64_t start,
                      std::optional<std::int64_t> stop) {
  const auto length = static_cast<std::int64_t>(Utf8Length(text));
  auto resolve = [length](std::int64_t at) {
    if (at < 0) at += length;
    return std::clamp<std::int64_t>(at, 0, length);
  };
  const std::int64_t from = resolve(start);
  const std::int64_t to = stop.has_value() ? resolve(*stop) : length;
  if (to <= from) return {};
  const size_t begin = Utf8Offset(text, static_cast<size_t>(from));
  const size_t end = Utf8Offset(text, static_cast<size_t>(to));
  return std::string(text.substr(begin, end - begin));
}

/// A string as a JSON string literal, escaped the way `json.dumps` escapes one.
///
/// `ensure_ascii` is the default there, so anything outside ASCII becomes a
/// `\uXXXX` escape -- surrogate pair and all -- and a rendered object is the
/// same text whichever engine rendered it.
void AppendJsonString(std::string_view text, std::string& out) {
  out.push_back('"');
  size_t at = 0;
  while (at < text.size()) {
    const auto letter = static_cast<unsigned char>(text[at]);
    if (letter == '"' || letter == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(letter));
      ++at;
      continue;
    }
    if (letter == '\n') { out += "\\n"; ++at; continue; }
    if (letter == '\r') { out += "\\r"; ++at; continue; }
    if (letter == '\t') { out += "\\t"; ++at; continue; }
    if (letter == '\b') { out += "\\b"; ++at; continue; }
    if (letter == '\f') { out += "\\f"; ++at; continue; }
    if (letter < 0x20) {
      absl::StrAppendFormat(&out, "\\u%04x", letter);
      ++at;
      continue;
    }
    if (letter < 0x80) {
      out.push_back(static_cast<char>(letter));
      ++at;
      continue;
    }
    // One code point, then the escape it is written as. A code point above the
    // basic plane is a surrogate pair, which is what `json.dumps` writes.
    size_t width = 1;
    std::uint32_t point = letter;
    if ((letter & 0xE0) == 0xC0) {
      width = 2;
      point = letter & 0x1Fu;
    } else if ((letter & 0xF0) == 0xE0) {
      width = 3;
      point = letter & 0x0Fu;
    } else if ((letter & 0xF8) == 0xF0) {
      width = 4;
      point = letter & 0x07u;
    }
    if (at + width > text.size()) {
      // Malformed tail: written as the replacement character rather than
      // dropped, so the output is still valid JSON.
      out += "\\ufffd";
      break;
    }
    for (size_t more = 1; more < width; ++more) {
      point = (point << 6) | (static_cast<unsigned char>(text[at + more]) & 0x3Fu);
    }
    at += width;
    if (point > 0xFFFF) {
      point -= 0x10000;
      absl::StrAppendFormat(&out, "\\u%04x\\u%04x", 0xD800 + (point >> 10),
                            0xDC00 + (point & 0x3FF));
    } else {
      absl::StrAppendFormat(&out, "\\u%04x", point);
    }
  }
  out.push_back('"');
}

void AppendJson(const Value& value, std::string& out);

void AppendJsonObject(const Value::Pairs& pairs, std::string& out) {
  // Sorted, because `json.dumps(sort_keys=True)` is what rendered a value
  // before and a flow's own output should not change under a port.
  std::vector<const std::pair<std::string, Value>*> ordered;
  ordered.reserve(pairs.size());
  for (const auto& pair : pairs) ordered.push_back(&pair);
  std::sort(ordered.begin(), ordered.end(),
            [](const auto* left, const auto* right) {
              return left->first < right->first;
            });
  out.push_back('{');
  bool first = true;
  for (const auto* pair : ordered) {
    if (!first) out += ", ";
    first = false;
    AppendJsonString(pair->first, out);
    out += ": ";
    AppendJson(pair->second, out);
  }
  out.push_back('}');
}

void AppendJson(const Value& value, std::string& out) {
  switch (value.kind()) {
    case Value::Kind::kNull:
      out += "null";
      return;
    case Value::Kind::kBool:
      out += value.boolean() ? "true" : "false";
      return;
    case Value::Kind::kInteger:
      absl::StrAppend(&out, value.integer());
      return;
    case Value::Kind::kDouble:
      if (std::isnan(value.number())) {
        out += "NaN";
      } else if (std::isinf(value.number())) {
        out += value.number() > 0 ? "Infinity" : "-Infinity";
      } else {
        out += FormatDouble(value.number());
      }
      return;
    case Value::Kind::kString:
      AppendJsonString(value.text(), out);
      return;
    case Value::Kind::kList: {
      out.push_back('[');
      bool first = true;
      for (const Value& item : value.items()) {
        if (!first) out += ", ";
        first = false;
        AppendJson(item, out);
      }
      out.push_back(']');
      return;
    }
    case Value::Kind::kObject:
      AppendJsonObject(value.pairs(), out);
      return;
    default:
      // Bytes, durations, instants, chunks and host values are not JSON. The
      // reference passed `default=str`, which rendered whatever the value's own
      // text was; that is exactly `AsText`, so it is what goes in.
      AppendJsonString(AsText(value), out);
      return;
  }
}

/// The canonical casing of a word the language gives meaning to.
///
/// Every significant word may be written in lower case or upper case but not
/// mixed, so an all-upper word folds down and anything else is left as written.
std::string Canonical(std::string_view word) {
  return std::string(vocabulary::Canonical(word));
}

bool NumericEqual(const Value& left, const Value& right) {
  const bool integral = (left.kind() == Value::Kind::kInteger ||
                         left.kind() == Value::Kind::kBool) &&
                        (right.kind() == Value::Kind::kInteger ||
                         right.kind() == Value::Kind::kBool);
  if (integral) {
    auto whole = [](const Value& value) -> std::int64_t {
      return value.kind() == Value::Kind::kBool ? (value.boolean() ? 1 : 0)
                                                : value.integer();
    };
    return whole(left) == whole(right);
  }
  return AsDouble(left) == AsDouble(right);
}

/// Whether the value is one that compares with a number rather than as text.
bool CountsAsNumber(const Value& value) {
  return value.IsNumber() || value.kind() == Value::Kind::kBool;
}

/// A bridge over A11's own registry, for the tool and the C++ tests.
class NativeBridge final : public HostBridge {
 public:
  absl::StatusOr<Value> Coerce(std::string_view tag,
                               const Value& value) override {
    // The C++ registry is keyed by `typeid`, so a tag names a type this process
    // was compiled with rather than one it can look up and construct. Saying so
    // is the honest answer: a flow casting to a registered type wants the host
    // that registered it, which for A11 means the Python bindings.
    return absl::UnimplementedError(absl::StrCat(
        "Nothing here knows the type '", tag,
        "'. A tag names a type a serialization registry has been told about, so"
        " the module defining it has to be imported where the flow runs."));
  }

  absl::StatusOr<Value> FromChunk(const data::Chunk& chunk) override {
    const std::string mimetype = chunk.GetMimetype();
    if (absl::StrContains(mimetype, "json") || mimetype.empty()) {
      return AsJson(Value::String(chunk.data));
    }
    if (absl::StartsWith(mimetype, "text/")) {
      return Value::String(chunk.data);
    }
    return Value::Bytes(chunk.data);
  }

  absl::StatusOr<data::Chunk> ToChunk(const Value& value,
                                      std::string_view mimetype) override {
    data::Chunk chunk;
    data::ChunkMetadata metadata;
    if (value.kind() == Value::Kind::kBytes) {
      metadata.mimetype =
          mimetype.empty() ? "application/octet-stream" : std::string(mimetype);
      chunk.data = value.text();
    } else {
      metadata.mimetype =
          mimetype.empty() ? "application/json" : std::string(mimetype);
      chunk.data = JsonText(value);
    }
    chunk.metadata = std::move(metadata);
    return chunk;
  }
};

}  // namespace

// --- Value -------------------------------------------------------------------

Value Value::Bool(bool value) {
  Value made;
  made.kind_ = Kind::kBool;
  made.boolean_ = value;
  return made;
}

Value Value::Integer(std::int64_t value) {
  Value made;
  made.kind_ = Kind::kInteger;
  made.integer_ = value;
  return made;
}

Value Value::Double(double value) {
  Value made;
  made.kind_ = Kind::kDouble;
  made.number_ = value;
  return made;
}

Value Value::String(std::string value) {
  Value made;
  made.kind_ = Kind::kString;
  made.text_ = std::make_shared<const std::string>(std::move(value));
  return made;
}

Value Value::Bytes(data::Bytes value) {
  Value made;
  made.kind_ = Kind::kBytes;
  made.text_ = std::make_shared<const std::string>(std::move(value));
  return made;
}

Value Value::List(std::vector<Value> items) {
  Value made;
  made.kind_ = Kind::kList;
  made.items_ = std::make_shared<const std::vector<Value>>(std::move(items));
  return made;
}

Value Value::Object(Pairs pairs) {
  Value made;
  made.kind_ = Kind::kObject;
  made.pairs_ = std::make_shared<const Pairs>(std::move(pairs));
  return made;
}

Value Value::Duration(absl::Duration value) {
  Value made;
  made.kind_ = Kind::kDuration;
  made.duration_ = value;
  return made;
}

Value Value::Time(absl::Time value) {
  Value made;
  made.kind_ = Kind::kTime;
  made.time_ = value;
  return made;
}

Value Value::Chunk(data::Chunk chunk) {
  Value made;
  made.kind_ = Kind::kChunk;
  made.chunk_ = std::make_shared<const data::Chunk>(std::move(chunk));
  return made;
}

Value Value::Host(std::shared_ptr<const HostObject> object) {
  if (object == nullptr) return Null();
  Value made;
  made.kind_ = Kind::kHost;
  made.host_ = std::move(object);
  return made;
}

Value Value::Of(const syntax::Constant& constant) {
  switch (constant.kind) {
    case syntax::Constant::Kind::kNull:
      return Null();
    case syntax::Constant::Kind::kBool:
      return Bool(constant.boolean);
    case syntax::Constant::Kind::kInteger:
      return Integer(constant.integer);
    case syntax::Constant::Kind::kDouble:
      return Double(constant.number);
    case syntax::Constant::Kind::kString:
      return String(constant.text);
    case syntax::Constant::Kind::kDuration:
      return Duration(constant.duration);
    case syntax::Constant::Kind::kList: {
      std::vector<Value> items;
      items.reserve(constant.items.size());
      for (const syntax::Constant& item : constant.items) {
        items.push_back(Of(item));
      }
      return List(std::move(items));
    }
    case syntax::Constant::Kind::kObject: {
      Pairs pairs;
      pairs.reserve(constant.pairs.size());
      for (const auto& [key, item] : constant.pairs) {
        pairs.emplace_back(key, Of(item));
      }
      return Object(std::move(pairs));
    }
  }
  return Null();
}

const Value* absl_nullable Value::Get(std::string_view key) const {
  if (kind_ != Kind::kObject) return nullptr;
  for (const auto& [name, value] : *pairs_) {
    if (name == key) return &value;
  }
  return nullptr;
}

bool operator==(const Value& left, const Value& right) {
  if (CountsAsNumber(left) && CountsAsNumber(right)) {
    return NumericEqual(left, right);
  }
  if (left.kind() != right.kind()) return false;
  switch (left.kind()) {
    case Value::Kind::kNull:
      return true;
    case Value::Kind::kBool:
      return left.boolean() == right.boolean();
    case Value::Kind::kInteger:
      return left.integer() == right.integer();
    case Value::Kind::kDouble:
      return left.number() == right.number();
    case Value::Kind::kString:
    case Value::Kind::kBytes:
      return left.text() == right.text();
    case Value::Kind::kList:
      return left.items() == right.items();
    case Value::Kind::kObject: {
      if (left.pairs().size() != right.pairs().size()) return false;
      // By key, not by order: two objects with the same pairs written in a
      // different order are the same value, as they are in the reference.
      for (const auto& [key, value] : left.pairs()) {
        const Value* other = right.Get(key);
        if (other == nullptr || !(*other == value)) return false;
      }
      return true;
    }
    case Value::Kind::kDuration:
      return left.duration() == right.duration();
    case Value::Kind::kTime:
      return left.time() == right.time();
    case Value::Kind::kChunk:
      return left.chunk() == right.chunk();
    case Value::Kind::kHost:
      return left.host().Equals(right.host());
  }
  return false;
}

std::unique_ptr<HostBridge> NativeHostBridge() {
  return std::make_unique<NativeBridge>();
}

// --- Reading values ----------------------------------------------------------

Value Lookup(const Value& value, const Value& key) {
  switch (value.kind()) {
    case Value::Kind::kNull:
      return Value::Null();
    case Value::Kind::kObject: {
      if (key.kind() != Value::Kind::kString) return Value::Null();
      const Value* found = value.Get(key.text());
      return found == nullptr ? Value::Null() : *found;
    }
    case Value::Kind::kList: {
      if (!CountsAsNumber(key) || key.kind() == Value::Kind::kDouble) {
        return Value::Null();
      }
      const auto& items = value.items();
      auto at = key.kind() == Value::Kind::kBool
                    ? static_cast<std::int64_t>(key.boolean())
                    : key.integer();
      if (at < 0) at += static_cast<std::int64_t>(items.size());
      if (at < 0 || at >= static_cast<std::int64_t>(items.size())) {
        return Value::Null();
      }
      return items[static_cast<size_t>(at)];
    }
    case Value::Kind::kHost:
      if (key.kind() == Value::Kind::kString) {
        return value.host().Field(key.text());
      }
      return value.host().Element(key);
    default:
      // A string is not a container of values here: `text.length` is nothing,
      // and answering nothing is what lets a flow ask without checking first.
      return Value::Null();
  }
}

bool Truthy(const Value& value) {
  switch (value.kind()) {
    case Value::Kind::kNull:
      return false;
    case Value::Kind::kBool:
      return value.boolean();
    case Value::Kind::kInteger:
      return value.integer() != 0;
    case Value::Kind::kDouble:
      return value.number() != 0.0;
    case Value::Kind::kString:
    case Value::Kind::kBytes:
      return !value.text().empty();
    case Value::Kind::kList:
      return !value.items().empty();
    case Value::Kind::kObject:
      return !value.pairs().empty();
    case Value::Kind::kHost:
      return value.host().Truthy();
    default:
      // A duration, an instant and a chunk are objects in the reference, and an
      // object with no `__bool__` is true. Zero is a length, not a nothing.
      return true;
  }
}

std::string AsText(const Value& value) {
  switch (value.kind()) {
    case Value::Kind::kNull:
      return {};
    case Value::Kind::kString:
    case Value::Kind::kBytes:
      return value.text();
    case Value::Kind::kBool:
      return value.boolean() ? "true" : "false";
    case Value::Kind::kInteger:
      return absl::StrCat(value.integer());
    case Value::Kind::kDouble:
      return FormatDouble(value.number());
    case Value::Kind::kDuration:
      return DurationText(value.duration());
    case Value::Kind::kTime:
      return TimeText(value.time());
    case Value::Kind::kChunk:
      // The payload, not a description of the wrapper: a chunk in a flow is a
      // value that has not been decoded yet, and asking for its text is asking
      // for what it holds.
      return value.chunk().data;
    case Value::Kind::kHost:
      return value.host().Text();
    case Value::Kind::kList:
    case Value::Kind::kObject:
      return JsonText(value);
  }
  return {};
}

Value AsNumber(const Value& value) {
  switch (value.kind()) {
    case Value::Kind::kBool:
      return Value::Integer(value.boolean() ? 1 : 0);
    case Value::Kind::kInteger:
    case Value::Kind::kDouble:
      return value;
    case Value::Kind::kDuration:
      return Value::Double(DurationSeconds(value.duration()));
    case Value::Kind::kTime: {
      const absl::StatusOr<std::int64_t> nanoseconds =
          TimeNanosecondsSinceEpoch(value.time());
      if (!nanoseconds.ok()) return Value::Integer(0);
      return Value::Double(static_cast<double>(*nanoseconds) / 1e9);
    }
    case Value::Kind::kString:
    case Value::Kind::kBytes: {
      const std::string trimmed(absl::StripAsciiWhitespace(value.text()));
      if (trimmed.empty()) return Value::Integer(0);
      std::string_view digits = trimmed;
      if (absl::StartsWith(digits, "-")) digits.remove_prefix(1);
      const bool whole =
          !digits.empty() &&
          std::all_of(digits.begin(), digits.end(), absl::ascii_isdigit);
      char* end = nullptr;
      if (whole) {
        const long long parsed = std::strtoll(trimmed.c_str(), &end, 10);
        if (end != nullptr && *end == '\0') return Value::Integer(parsed);
      }
      const double parsed = std::strtod(trimmed.c_str(), &end);
      if (end == nullptr || *end != '\0') return Value::Integer(0);
      return Value::Double(parsed);
    }
    case Value::Kind::kNull:
      return Value::Integer(0);
    case Value::Kind::kList:
      return Value::Integer(static_cast<std::int64_t>(value.items().size()));
    case Value::Kind::kObject:
      return Value::Integer(static_cast<std::int64_t>(value.pairs().size()));
    case Value::Kind::kChunk:
      return Value::Integer(
          static_cast<std::int64_t>(value.chunk().data.size()));
    case Value::Kind::kHost: {
      const std::optional<size_t> size = value.host().Size();
      return Value::Integer(
          size.has_value() ? static_cast<std::int64_t>(*size) : 0);
    }
  }
  return Value::Integer(0);
}

double AsDouble(const Value& value) {
  const Value number = AsNumber(value);
  return number.kind() == Value::Kind::kInteger
             ? static_cast<double>(number.integer())
             : number.number();
}

/// One hole's text, read as the hole said to read it.
Value AsHole(pattern::HoleType type, std::string_view text) {
  const Value written = Value::String(std::string(text));
  switch (type) {
    case pattern::HoleType::kInt:
    case pattern::HoleType::kNumber:
      return AsNumber(written);
    case pattern::HoleType::kBool:
      // The matcher only ever hands `true` or `false` here, in some case.
      return Value::Bool(absl::EqualsIgnoreCase(text, "true"));
    case pattern::HoleType::kDuration:
      return Value::Duration(AsDuration(written));
    case pattern::HoleType::kTime:
      return Value::Time(AsTime(written));
    case pattern::HoleType::kJson:
      return AsJson(written);
    case pattern::HoleType::kString:
    case pattern::HoleType::kWord:
    case pattern::HoleType::kLine:
    case pattern::HoleType::kRest:
      return written;
  }
  return written;
}

absl::StatusOr<Value> MatchPattern(std::string_view text,
                                   std::string_view subject) {
  const pattern::Compiled compiled = pattern::Compile(text);
  if (!compiled.ok()) {
    return absl::InvalidArgumentError(
        absl::StrCat("The pattern '", text, "' cannot be read: ",
                     compiled.error));
  }
  return MatchCompiled(compiled.pattern, subject);
}

Value MatchCompiled(const pattern::Pattern& compiled,
                    std::string_view subject) {
  const std::optional<std::vector<pattern::Capture>> captures =
      pattern::Match(compiled, subject);
  if (!captures.has_value()) return Value::Null();

  if (!compiled.AllNamed()) {
    std::vector<Value> items;
    items.reserve(captures->size());
    for (size_t index = 0; index < captures->size(); ++index) {
      items.push_back(
          AsHole(compiled.holes[index].type, (*captures)[index].text));
    }
    return Value::List(std::move(items));
  }
  Value::Pairs pairs;
  pairs.reserve(captures->size());
  for (size_t index = 0; index < captures->size(); ++index) {
    const pattern::Hole& hole = compiled.holes[index];
    pairs.emplace_back(hole.name, AsHole(hole.type, (*captures)[index].text));
  }
  return Value::Object(std::move(pairs));
}

Value AsJson(const Value& value) {
  if (!value.IsTextlike()) return value;
  const nlohmann::json parsed =
      nlohmann::json::parse(value.text(), nullptr, false);
  if (parsed.is_discarded()) return value;
  const std::function<Value(const nlohmann::json&)> convert =
      [&convert](const nlohmann::json& node) -> Value {
    switch (node.type()) {
      case nlohmann::json::value_t::null:
        return Value::Null();
      case nlohmann::json::value_t::boolean:
        return Value::Bool(node.get<bool>());
      case nlohmann::json::value_t::number_integer:
      case nlohmann::json::value_t::number_unsigned:
        return Value::Integer(node.get<std::int64_t>());
      case nlohmann::json::value_t::number_float:
        return Value::Double(node.get<double>());
      case nlohmann::json::value_t::string:
        return Value::String(node.get<std::string>());
      case nlohmann::json::value_t::array: {
        std::vector<Value> items;
        items.reserve(node.size());
        for (const nlohmann::json& item : node) items.push_back(convert(item));
        return Value::List(std::move(items));
      }
      case nlohmann::json::value_t::object: {
        Value::Pairs pairs;
        for (const auto& [key, item] : node.items()) {
          pairs.emplace_back(key, convert(item));
        }
        return Value::Object(std::move(pairs));
      }
      default:
        return Value::Null();
    }
  };
  return convert(parsed);
}

Value Truncate(const Value& value, std::int64_t size) {
  const std::int64_t kept = std::max<std::int64_t>(size, 0);
  switch (value.kind()) {
    case Value::Kind::kString:
      return Value::String(Utf8Slice(value.text(), 0, kept));
    case Value::Kind::kBytes:
      return Value::Bytes(value.text().substr(
          0, std::min<size_t>(value.text().size(),
                              static_cast<size_t>(kept))));
    case Value::Kind::kObject: {
      const Value::Pairs& pairs = value.pairs();
      Value::Pairs head(
          pairs.begin(),
          pairs.begin() + std::min<size_t>(pairs.size(),
                                           static_cast<size_t>(kept)));
      return Value::Object(std::move(head));
    }
    case Value::Kind::kList: {
      const std::vector<Value>& items = value.items();
      std::vector<Value> head(
          items.begin(),
          items.begin() + std::min<size_t>(items.size(),
                                          static_cast<size_t>(kept)));
      return Value::List(std::move(head));
    }
    default:
      return value;
  }
}

std::string JsonText(const Value& value) {
  std::string out;
  AppendJson(value, out);
  return out;
}

// --- Times and durations -----------------------------------------------------

double DurationSeconds(absl::Duration value) {
  if (value == absl::InfiniteDuration()) {
    return std::numeric_limits<double>::infinity();
  }
  if (value == -absl::InfiniteDuration()) {
    return -std::numeric_limits<double>::infinity();
  }
  return absl::ToDoubleSeconds(value);
}

absl::Duration SecondsDuration(double total) {
  if (std::isinf(total)) {
    return total > 0 ? absl::InfiniteDuration() : -absl::InfiniteDuration();
  }
  return absl::Seconds(total);
}

std::optional<absl::Duration> ParseDuration(std::string_view text) {
  const std::string_view trimmed = absl::StripAsciiWhitespace(text);
  if (trimmed.empty()) return std::nullopt;
  const std::string lowered = absl::AsciiStrToLower(trimmed);
  if (lowered == "forever" || lowered == "infinite" || lowered == "inf") {
    return absl::InfiniteDuration();
  }
  double total = 0.0;
  double sign = 1.0;
  size_t at = 0;
  int pieces = 0;
  while (at < trimmed.size()) {
    if (trimmed[at] == ' ' || trimmed[at] == '\t') {
      ++at;
      continue;
    }
    const size_t began = at;
    if (trimmed[at] == '+' || trimmed[at] == '-') ++at;
    const size_t whole = at;
    while (at < trimmed.size() && absl::ascii_isdigit(trimmed[at])) ++at;
    if (at == whole) return std::nullopt;
    if (at < trimmed.size() && trimmed[at] == '.') {
      ++at;
      const size_t fraction = at;
      while (at < trimmed.size() && absl::ascii_isdigit(trimmed[at])) ++at;
      if (at == fraction) return std::nullopt;
    }
    const std::string number(trimmed.substr(began, at - began));
    while (at < trimmed.size() && (trimmed[at] == ' ' || trimmed[at] == '\t')) {
      ++at;
    }
    const size_t word = at;
    while (at < trimmed.size() && absl::ascii_isalpha(trimmed[at])) ++at;
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
      if (!known) return std::nullopt;
    }
    // `-1m30s` is a minute and a half, backwards, not a minute back and half a
    // second forwards: the sign belongs to the whole.
    if (pieces == 0 && absl::StartsWith(number, "-")) sign = -1.0;
    total += std::abs(std::strtod(number.c_str(), nullptr)) * scale;
    ++pieces;
  }
  if (pieces == 0) return std::nullopt;
  return SecondsDuration(sign * total);
}

absl::Duration AsDuration(const Value& value) {
  if (value.kind() == Value::Kind::kDuration) return value.duration();
  if (value.IsTextlike()) {
    const std::optional<absl::Duration> parsed = ParseDuration(value.text());
    if (parsed.has_value()) return *parsed;
  }
  return SecondsDuration(AsDouble(value));
}

std::optional<absl::Time> ParseTime(std::string_view text) {
  const std::string_view trimmed = absl::StripAsciiWhitespace(text);
  if (trimmed.empty()) return std::nullopt;
  // The spellings RFC 3339 allows and `TimeText` writes, and the one a
  // timestamp with no zone arrives as: that one is UTC here, which is the zone
  // every instant this language writes is in.
  static constexpr std::string_view kPatterns[] = {
      "%Y-%m-%dT%H:%M:%E*S%Ez", "%Y-%m-%dT%H:%M:%E*S",
      "%Y-%m-%d %H:%M:%E*S%Ez", "%Y-%m-%d %H:%M:%E*S",
      "%Y-%m-%d",
  };
  for (const std::string_view pattern : kPatterns) {
    absl::Time when;
    std::string error;
    if (absl::ParseTime(pattern, trimmed, absl::UTCTimeZone(), &when, &error)) {
      return when;
    }
  }
  return std::nullopt;
}

absl::Time AsTime(const Value& value) {
  if (value.kind() == Value::Kind::kTime) return value.time();
  if (value.IsTextlike()) {
    const std::optional<absl::Time> parsed = ParseTime(value.text());
    if (parsed.has_value()) return *parsed;
  }
  return absl::UnixEpoch() + SecondsDuration(AsDouble(value));
}

std::string DurationText(absl::Duration value, std::string_view spec) {
  if (value == absl::InfiniteDuration() || value == -absl::InfiniteDuration()) {
    return "forever";
  }
  // Counted in nanoseconds, which is what a duration is: `1500us + 500ns`
  // renders as `1500.5us`, and not as float arithmetic's opinion of it.
  const absl::StatusOr<std::int64_t> exact = DurationNanoseconds(value);
  const std::int64_t total = exact.ok() ? *exact : 0;
  for (const Unit& unit : kUnits) {
    if (spec == unit.name) {
      return TrimNumber(static_cast<double>(total) / (unit.seconds * 1e9));
    }
  }
  if (total == 0) return "0s";
  const std::string sign = total < 0 ? "-" : "";
  std::int64_t left = std::abs(total);
  std::string pieces;
  bool coarse = false;
  for (const Unit& unit : kUnits) {
    // Microseconds of an hour are noise; microseconds of a millisecond and a
    // half are the value. So the fine units are dropped only once a whole
    // second or more has already been written.
    if ((unit.name == "us" || unit.name == "ns") && coarse) break;
    if (unit.seconds >= 1.0 && !pieces.empty()) coarse = true;
    const auto step = static_cast<std::int64_t>(std::llround(unit.seconds * 1e9));
    const std::int64_t count = left / step;
    if (count != 0) {
      absl::StrAppend(&pieces, count, unit.name);
      left -= count * step;
    }
    if (left <= 0) break;
  }
  return absl::StrCat(sign, pieces.empty() ? "0s" : pieces);
}

std::string TimeText(absl::Time value, std::string_view spec) {
  const absl::StatusOr<std::int64_t> nanoseconds =
      TimeNanosecondsSinceEpoch(value);
  if (spec == "epoch") {
    return TrimNumber(nanoseconds.ok() ? static_cast<double>(*nanoseconds) / 1e9
                                       : 0.0);
  }
  if (!spec.empty()) {
    return absl::FormatTime(spec, value, absl::UTCTimeZone());
  }
  const bool whole = nanoseconds.ok() && *nanoseconds % 1000000000 == 0;
  return absl::FormatTime(
      whole ? "%Y-%m-%dT%H:%M:%SZ" : "%Y-%m-%dT%H:%M:%E6SZ", value,
      absl::UTCTimeZone());
}

// --- Formatting --------------------------------------------------------------

namespace {

/// A value with a `%(SPEC)` applied.
///
/// Only times and durations have anything a spec could mean -- printf's own
/// flags and precision cover the rest -- so anything else is handed on
/// untouched rather than a spec being invented for it.
Value WithSpec(const Value& value, std::string_view spec) {
  if (value.kind() == Value::Kind::kDuration) {
    return Value::String(DurationText(value.duration(), spec));
  }
  if (value.kind() == Value::Kind::kTime) {
    return Value::String(TimeText(value.time(), spec));
  }
  return value;
}

/// One value through one printf conversion, coercing rather than failing.
std::string Printf(const Value& value, std::string_view flags,
                   std::string_view width,
                   std::optional<std::string_view> precision,
                   char conversion) {
  if (conversion == 'i') conversion = 'd';  // printf's synonym for the same.
  const bool numeric = std::string_view("difeEgGxXo").find(conversion) !=
                       std::string_view::npos;
  std::string pattern = "%";
  for (const char flag : flags) {
    // `#` and `0` mean nothing beside a string, and the reference's `%` raised
    // on them rather than rendering; dropping them keeps the value visible.
    if (!numeric && (flag == '#' || flag == '0')) continue;
    pattern.push_back(flag);
  }
  absl::StrAppend(&pattern, width);
  if (precision.has_value()) absl::StrAppend(&pattern, ".", *precision);
  std::string out(64, '\0');
  int written = 0;
  if (conversion == 'd' || conversion == 'x' || conversion == 'X' ||
      conversion == 'o') {
    absl::StrAppend(&pattern, "ll", std::string_view(&conversion, 1));
    const auto number = static_cast<long long>(AsDouble(value));
    written = std::snprintf(out.data(), out.size(), pattern.c_str(), number);
    if (written >= static_cast<int>(out.size())) {
      out.resize(static_cast<size_t>(written) + 1);
      written = std::snprintf(out.data(), out.size(), pattern.c_str(), number);
    }
  } else if (numeric) {
    pattern.push_back(conversion);
    const double number = AsDouble(value);
    written = std::snprintf(out.data(), out.size(), pattern.c_str(), number);
    if (written >= static_cast<int>(out.size())) {
      out.resize(static_cast<size_t>(written) + 1);
      written = std::snprintf(out.data(), out.size(), pattern.c_str(), number);
    }
  } else {
    pattern.push_back('s');
    const std::string text = AsText(value);
    written = std::snprintf(out.data(), out.size(), pattern.c_str(),
                            text.c_str());
    if (written >= static_cast<int>(out.size())) {
      out.resize(static_cast<size_t>(written) + 1);
      written = std::snprintf(out.data(), out.size(), pattern.c_str(),
                              text.c_str());
    }
  }
  if (written < 0) return AsText(value);
  out.resize(static_cast<size_t>(written));
  return out;
}

}  // namespace

std::string Strformat(const Value& format, absl::Span<const Value> arguments) {
  const std::string text = AsText(format);
  std::string out;
  size_t at = 0;
  size_t next = 0;
  while (at < text.size()) {
    if (text[at] != '%') {
      out.push_back(text[at++]);
      continue;
    }
    if (at + 1 < text.size() && text[at + 1] == '%') {
      out.push_back('%');
      at += 2;
      continue;
    }
    // `%[N$][(spec)][flags][width][.precision]conversion`: printf's own shape
    // with one addition, the parenthesised spec.
    size_t cursor = at + 1;
    std::optional<size_t> chosen;
    const size_t digits = cursor;
    while (cursor < text.size() && absl::ascii_isdigit(text[cursor])) ++cursor;
    if (cursor > digits && cursor < text.size() && text[cursor] == '$') {
      chosen = static_cast<size_t>(
          std::strtoll(text.substr(digits, cursor - digits).c_str(), nullptr,
                       10));
      ++cursor;
    } else {
      cursor = at + 1;
    }
    std::string_view spec;
    if (cursor < text.size() && text[cursor] == '(') {
      const size_t closes = text.find(')', cursor);
      if (closes == std::string::npos) {
        out.push_back('%');
        ++at;
        continue;
      }
      spec = std::string_view(text).substr(cursor + 1, closes - cursor - 1);
      cursor = closes + 1;
    }
    const size_t flags = cursor;
    while (cursor < text.size() &&
           std::string_view("-+ 0#").find(text[cursor]) !=
               std::string_view::npos) {
      ++cursor;
    }
    const size_t width = cursor;
    while (cursor < text.size() && absl::ascii_isdigit(text[cursor])) ++cursor;
    const size_t width_end = cursor;
    std::optional<std::string_view> precision;
    if (cursor < text.size() && text[cursor] == '.') {
      const size_t began = ++cursor;
      while (cursor < text.size() && absl::ascii_isdigit(text[cursor])) ++cursor;
      if (cursor == began) {
        out.push_back('%');
        ++at;
        continue;
      }
      precision = std::string_view(text).substr(began, cursor - began);
    }
    if (cursor >= text.size() ||
        std::string_view("sdifeEgGxXo").find(text[cursor]) ==
            std::string_view::npos) {
      // A `%` that starts nothing recognisable is left alone, so `"100% done"`
      // says what it looks like.
      out.push_back('%');
      ++at;
      continue;
    }
    const char conversion = text[cursor];
    const size_t index = chosen.has_value() ? *chosen - 1 : next;
    if (!chosen.has_value()) ++next;
    if (index >= arguments.size()) {
      // Left as it was written: a visible `%3$s` in the output is easier to
      // diagnose than a flow that died formatting a log line.
      out.append(text, at, cursor + 1 - at);
      at = cursor + 1;
      continue;
    }
    Value value = arguments[index];
    if (!spec.empty()) value = WithSpec(value, spec);
    out += Printf(value, std::string_view(text).substr(flags, width - flags),
                  std::string_view(text).substr(width, width_end - width),
                  precision, conversion);
    at = cursor + 1;
  }
  return out;
}

// --- Builtins ----------------------------------------------------------------

namespace {

const Value& Argument(absl::Span<const Value> arguments, size_t index) {
  static const Value kMissing;
  return index < arguments.size() ? arguments[index] : kMissing;
}

/// Whether `container` holds `member`, as `in` and `contains` decide it.
bool Contains(const Value& container, const Value& member) {
  switch (container.kind()) {
    case Value::Kind::kObject:
      return member.kind() == Value::Kind::kString &&
             container.Get(member.text()) != nullptr;
    case Value::Kind::kList:
      return std::any_of(container.items().begin(), container.items().end(),
                         [&member](const Value& item) { return item == member; });
    case Value::Kind::kString:
    case Value::Kind::kBytes:
      return absl::StrContains(container.text(), AsText(member));
    default:
      return false;
  }
}

/// The endings `starts-with`/`ends-with` were given: one, or a list of them.
///
/// A list of candidates is one question, not three: a piece that ends a
/// sentence ends with any of `[".", "?", "!"]`.
std::vector<std::string> Endings(const Value& value) {
  std::vector<std::string> options;
  if (value.kind() == Value::Kind::kList) {
    for (const Value& item : value.items()) options.push_back(AsText(item));
    return options;
  }
  options.push_back(AsText(value));
  return options;
}

}  // namespace

absl::StatusOr<Value> CallBuiltin(std::string_view name,
                                  absl::Span<const Value> arguments,
                                  HostBridge* absl_nullable bridge) {
  const Value& first = Argument(arguments, 0);
  if (name == "match") {
    return MatchPattern(AsText(first), AsText(Argument(arguments, 1)));
  }
  if (name == "strformat") {
    return Value::String(Strformat(
        first, arguments.empty() ? arguments : arguments.subspan(1)));
  }
  if (name == "now") return Value::Time(a11::Now());
  if (name == "duration") return Value::Duration(AsDuration(first));
  if (name == "time") return Value::Time(AsTime(first));
  if (name == "seconds") {
    return Value::Double(DurationSeconds(AsDuration(first)));
  }
  if (name == "len") {
    switch (first.kind()) {
      case Value::Kind::kString:
        return Value::Integer(static_cast<std::int64_t>(
            Utf8Length(first.text())));
      case Value::Kind::kBytes:
        return Value::Integer(static_cast<std::int64_t>(first.text().size()));
      case Value::Kind::kList:
        return Value::Integer(static_cast<std::int64_t>(first.items().size()));
      case Value::Kind::kObject:
        return Value::Integer(static_cast<std::int64_t>(first.pairs().size()));
      case Value::Kind::kHost: {
        const std::optional<size_t> size = first.host().Size();
        return Value::Integer(
            size.has_value() ? static_cast<std::int64_t>(*size) : 0);
      }
      default:
        return Value::Integer(0);
    }
  }
  if (name == "lower") return Value::String(absl::AsciiStrToLower(AsText(first)));
  if (name == "upper") return Value::String(absl::AsciiStrToUpper(AsText(first)));
  if (name == "trim") {
    return Value::String(std::string(absl::StripAsciiWhitespace(AsText(first))));
  }
  // Base64, both alphabets, both ways.
  //
  // Encoding gives *text*, because that is the whole point of encoding: it is
  // what a JSON field or a header can carry. Decoding gives *bytes*, because
  // that is what was encoded -- a flow that wants them as text says so with
  // `text(..)`, rather than this guessing that the bytes were a string.
  // Padding is written and, on the way back, not required: the web-safe
  // alphabet is routinely sent without it.
  if (name == "b64encode") {
    return Value::String(absl::Base64Escape(AsText(first)));
  }
  if (name == "b64urlencode") {
    return Value::String(absl::WebSafeBase64Escape(AsText(first)));
  }
  if (name == "b64decode" || name == "b64urldecode") {
    const std::string encoded = AsText(first);
    std::string decoded;
    const bool ok = name == "b64decode"
                        ? absl::Base64Unescape(encoded, &decoded)
                        : absl::WebSafeBase64Unescape(encoded, &decoded);
    if (!ok) {
      return absl::InvalidArgumentError(
          absl::StrCat(name, " was given text that is not base64."));
    }
    return Value::Bytes(data::Bytes(std::move(decoded)));
  }
  if (name == "text") return Value::String(AsText(first));
  if (name == "number") return AsNumber(first);
  if (name == "bool") return Value::Bool(Truthy(first));
  if (name == "keys") {
    std::vector<Value> keys;
    if (first.kind() == Value::Kind::kObject) {
      std::set<std::string> sorted;
      for (const auto& [key, unused] : first.pairs()) sorted.insert(key);
      for (const std::string& key : sorted) keys.push_back(Value::String(key));
    }
    return Value::List(std::move(keys));
  }
  if (name == "values") {
    std::vector<Value> found;
    if (first.kind() == Value::Kind::kObject) {
      std::vector<const std::pair<std::string, Value>*> ordered;
      for (const auto& pair : first.pairs()) ordered.push_back(&pair);
      std::sort(ordered.begin(), ordered.end(),
                [](const auto* left, const auto* right) {
                  return left->first < right->first;
                });
      for (const auto* pair : ordered) found.push_back(pair->second);
    } else if (first.kind() == Value::Kind::kList) {
      found = first.items();
    }
    return Value::List(std::move(found));
  }
  if (name == "get") {
    const Value found = Lookup(first, Argument(arguments, 1));
    return found.IsNull() ? Argument(arguments, 2) : found;
  }
  if (name == "join") {
    const std::string separator = AsText(Argument(arguments, 1));
    if (first.kind() != Value::Kind::kList) return Value::String(AsText(first));
    std::vector<std::string> pieces;
    pieces.reserve(first.items().size());
    for (const Value& item : first.items()) pieces.push_back(AsText(item));
    return Value::String(absl::StrJoin(pieces, separator));
  }
  if (name == "split") {
    const Value& separator = Argument(arguments, 1);
    const std::string text = AsText(first);
    std::vector<Value> pieces;
    if (Truthy(separator)) {
      for (std::string_view piece : absl::StrSplit(text, AsText(separator))) {
        pieces.push_back(Value::String(std::string(piece)));
      }
    } else {
      // No separator splits on runs of whitespace and keeps nothing empty,
      // which is what `str.split()` with no argument does.
      for (std::string_view piece :
           absl::StrSplit(text, absl::ByAnyChar(" \t\n\r\f\v"),
                          absl::SkipEmpty())) {
        pieces.push_back(Value::String(std::string(piece)));
      }
    }
    return Value::List(std::move(pieces));
  }
  if (name == "merge") {
    Value::Pairs merged;
    for (const Value& value : arguments) {
      if (value.kind() != Value::Kind::kObject) continue;
      for (const auto& [key, item] : value.pairs()) {
        auto found = std::find_if(merged.begin(), merged.end(),
                                  [&key](const auto& pair) {
                                    return pair.first == key;
                                  });
        if (found == merged.end()) {
          merged.emplace_back(key, item);
        } else {
          found->second = item;
        }
      }
    }
    return Value::Object(std::move(merged));
  }
  if (name == "contains") {
    return Value::Bool(Contains(first, Argument(arguments, 1)));
  }
  if (name == "starts-with" || name == "ends-with") {
    const std::string text = AsText(first);
    const std::vector<std::string> options = Endings(Argument(arguments, 1));
    const bool front = name == "starts-with";
    for (const std::string& option : options) {
      if (front ? absl::StartsWith(text, option)
                : absl::EndsWith(text, option)) {
        return Value::Bool(true);
      }
    }
    return Value::Bool(false);
  }
  if (name == "replace") {
    std::string text = AsText(first);
    const std::string from = AsText(Argument(arguments, 1));
    const std::string to = AsText(Argument(arguments, 2));
    if (from.empty()) return Value::String(std::move(text));
    std::string out;
    size_t at = 0;
    while (true) {
      const size_t found = text.find(from, at);
      if (found == std::string::npos) break;
      out.append(text, at, found - at);
      out += to;
      at = found + from.size();
    }
    out.append(text, at, std::string::npos);
    return Value::String(std::move(out));
  }
  if (name == "slice") {
    const auto start = static_cast<std::int64_t>(
        AsDouble(Argument(arguments, 1)));
    const Value& stop = Argument(arguments, 2);
    std::optional<std::int64_t> end;
    if (!stop.IsNull()) end = static_cast<std::int64_t>(AsDouble(stop));
    if (first.kind() == Value::Kind::kString) {
      return Value::String(Utf8Slice(first.text(), start, end));
    }
    if (first.kind() == Value::Kind::kBytes) {
      const auto length = static_cast<std::int64_t>(first.text().size());
      auto resolve = [length](std::int64_t at) {
        if (at < 0) at += length;
        return std::clamp<std::int64_t>(at, 0, length);
      };
      const std::int64_t from = resolve(start);
      const std::int64_t to = end.has_value() ? resolve(*end) : length;
      if (to <= from) return Value::Bytes({});
      return Value::Bytes(first.text().substr(static_cast<size_t>(from),
                                              static_cast<size_t>(to - from)));
    }
    if (first.kind() == Value::Kind::kList) {
      const auto length = static_cast<std::int64_t>(first.items().size());
      auto resolve = [length](std::int64_t at) {
        if (at < 0) at += length;
        return std::clamp<std::int64_t>(at, 0, length);
      };
      const std::int64_t from = resolve(start);
      const std::int64_t to = end.has_value() ? resolve(*end) : length;
      if (to <= from) return Value::List({});
      return Value::List(std::vector<Value>(
          first.items().begin() + from, first.items().begin() + to));
    }
    return first;
  }
  if (name == "default") {
    const bool empty =
        first.IsNull() ||
        (first.kind() == Value::Kind::kString && first.text().empty()) ||
        (first.kind() == Value::Kind::kList && first.items().empty()) ||
        (first.kind() == Value::Kind::kObject && first.pairs().empty());
    return empty ? Argument(arguments, 1) : first;
  }
  if (name == "to_chunk") {
    if (bridge == nullptr) {
      return Invalid("to_chunk needs the host that knows how a value is written.");
    }
    ABSL_ASSIGN_OR_RETURN(
        data::Chunk chunk,
        bridge->ToChunk(first, AsText(Argument(arguments, 1))));
    return Value::Chunk(std::move(chunk));
  }
  if (name == "from_chunk") {
    // Anything already decoded is already what this asks for.
    if (first.kind() != Value::Kind::kChunk) return first;
    if (bridge == nullptr) {
      return Invalid("from_chunk needs the host that knows how a value is read.");
    }
    return bridge->FromChunk(first.chunk());
  }
  return Invalid(absl::StrCat("Unknown function '", name, "'."));
}

namespace {

/// The items `...value` contributes to a list literal.
///
/// A list gives its items. Anything else gives itself, one item -- spreading a
/// value that is not a list is not a mistake worth ending a flow over, and
/// treating it as one thing is what a reader would expect from `[...a, b]`
/// where `a` turned out to be a single record.
std::vector<Value> SpreadItems(const Value& value) {
  if (value.kind() == Value::Kind::kList) return value.items();
  if (value.IsNull()) return {};
  return {value};
}

/// The pairs `...value` contributes to an object literal.
///
/// A mapping gives its pairs, and a host object gives whatever it says its
/// fields are -- which is what lets a record that arrived as a registered type,
/// or as a shape, be spread into a new one. Anything else gives nothing: there
/// are no keys to take, and inventing one would be inventing data.
Value::Pairs SpreadPairs(const Value& value) {
  if (value.kind() == Value::Kind::kObject) return value.pairs();
  if (value.kind() == Value::Kind::kHost) {
    Value::Pairs pairs;
    // A host object has no way to list its fields, so this goes through its
    // text: a model renders as its JSON, which is exactly the mapping wanted.
    const Value decoded = AsJson(Value::String(AsText(value)));
    if (decoded.kind() == Value::Kind::kObject) return decoded.pairs();
    return pairs;
  }
  return {};
}

}  // namespace

// --- Coercion ----------------------------------------------------------------

namespace {

/// A field's value, or nothing where the value being coerced has none.
///
/// Reads out of a mapping and out of a host object alike: a value that arrived
/// as a registered type still has fields, and a shape built from one should not
/// have to be told which sort of thing it came from.
const Value* absl_nullable FieldOf(const Value& value, std::string_view name,
                                   Value& scratch) {
  if (value.kind() == Value::Kind::kObject) return value.Get(name);
  if (value.kind() == Value::Kind::kHost) {
    scratch = value.host().Field(name);
    return scratch.IsNull() ? nullptr : &scratch;
  }
  return nullptr;
}

/// How long a value is, for a range that bounds a length.
size_t Extent(const Value& value) {
  switch (value.kind()) {
    case Value::Kind::kString:
      return Utf8Length(value.text());
    case Value::Kind::kBytes:
      return value.text().size();
    case Value::Kind::kList:
      return value.items().size();
    case Value::Kind::kObject:
      return value.pairs().size();
    default:
      return 0;
  }
}

/// Whether a range bounds this type's *length* rather than its magnitude.
bool BoundsLength(std::string_view type) {
  return type == "string" || type == "text" || type == "bytes" ||
         type == "list" || type == "array" || type == "object" ||
         type == "json";
}

/// The number a bound is, as a double, which is enough for every bound there
/// is: a length is a count and a duration compares by its seconds.
double BoundNumber(const syntax::Constant& bound) {
  if (bound.kind == syntax::Constant::Kind::kDuration) {
    return DurationSeconds(bound.duration);
  }
  return bound.AsDouble();
}

/// What a value compares as against a range on a field of this type.
double Magnitude(const Value& value, std::string_view type) {
  if (BoundsLength(type)) return static_cast<double>(Extent(value));
  if (value.kind() == Value::Kind::kDuration) {
    return DurationSeconds(value.duration());
  }
  if (value.kind() == Value::Kind::kTime) {
    return absl::ToDoubleSeconds(value.time() - absl::UnixEpoch());
  }
  return AsDouble(value);
}

/// The value a constant is, for a default and for an allowed value.
Value OfConstant(const syntax::Constant& constant) { return Value::Of(constant); }

absl::Status FieldError(std::string_view path, std::string what) {
  return absl::InvalidArgumentError(
      absl::StrCat(path, ": ", std::move(what)));
}

/// One field of a shape, coerced and checked, with the path for a message.
absl::StatusOr<Value> CoerceField(const FieldPlan& field, const Value& given,
                                  const CoerceContext& context,
                                  std::string_view path);

/// `value` made a value of the type `field` gives, following a shape it names.
absl::StatusOr<Value> CoerceFieldType(const FieldPlan& field,
                                      const Value& given,
                                      const CoerceContext& context,
                                      std::string_view path) {
  // A shape the program declared: validated rather than handed off, and by name
  // rather than by tag, which is what makes it outrank one.
  if (!field.dto_name.empty() && context.shapes != nullptr) {
    const DtoPlan* nested = context.shapes->Dto(field.dto_name);
    if (nested != nullptr) {
      absl::StatusOr<Value> made = CoerceShape(*nested, given, context);
      if (!made.ok()) {
        return FieldError(path, std::string(made.status().message()));
      }
      return *std::move(made);
    }
  }
  if (field.type == "list" || field.type == "array") {
    std::vector<Value> items;
    if (given.kind() == Value::Kind::kList) {
      items = given.items();
    } else if (!given.IsNull()) {
      items.push_back(given);
    }
    if (!field.element.empty()) {
      const DtoPlan* element =
          field.element_dto_name.empty() || context.shapes == nullptr
              ? nullptr
              : context.shapes->Dto(field.element_dto_name);
      for (size_t index = 0; index < items.size(); ++index) {
        const std::string inner = absl::StrCat(path, "[", index, "]");
        if (element != nullptr) {
          absl::StatusOr<Value> made =
              CoerceShape(*element, items[index], context);
          if (!made.ok()) {
            return FieldError(inner, std::string(made.status().message()));
          }
          items[index] = *std::move(made);
          continue;
        }
        syntax::TypeExpression as;
        as.name = field.element;
        absl::StatusOr<Value> made = Coerce(items[index], as, context);
        if (!made.ok()) return made.status();
        items[index] = *std::move(made);
      }
    }
    return Value::List(std::move(items));
  }
  syntax::TypeExpression as;
  as.name = field.type;
  return Coerce(given, as, context);
}

absl::StatusOr<Value> CoerceField(const FieldPlan& field, const Value& given,
                                  const CoerceContext& context,
                                  std::string_view path) {
  absl::StatusOr<Value> made = CoerceFieldType(field, given, context, path);
  if (!made.ok()) return made;
  const Value& held = *made;

  if (field.has_enumeration) {
    bool allowed = false;
    std::vector<std::string> spelled;
    for (const syntax::Constant& one : field.enumeration) {
      const Value candidate = OfConstant(one);
      spelled.push_back(AsText(candidate));
      if (candidate == held) allowed = true;
    }
    if (!allowed) {
      return FieldError(path, absl::StrCat("'", AsText(held),
                                          "' is not one of ",
                                          absl::StrJoin(spelled, ", "), "."));
    }
  }
  if (field.has_pattern && held.IsTextlike()) {
    // JSON Schema patterns use ECMA-262. Compile them through Flow's exception
    // boundary because std::regex reports invalid patterns by throwing.
    absl::StatusOr<std::regex> pattern =
        internal::CompilePattern(field.pattern);
    if (!pattern.ok()) {
      return FieldError(path, absl::StrCat("the pattern '", field.pattern,
                                          "' is not a regular expression."));
    }
    if (!std::regex_search(held.text(), *pattern)) {
      return FieldError(path, absl::StrCat("'", held.text(),
                                          "' does not match '", field.pattern,
                                          "'."));
    }
  }
  if (!field.range.Empty()) {
    const bool length = BoundsLength(field.type);
    const double have = Magnitude(held, field.type);
    const std::string_view unit = length ? " long" : "";
    if (field.range.has_minimum &&
        have < BoundNumber(field.range.minimum) - 1e-9) {
      return FieldError(
          path, absl::StrCat(length ? "is " : "is ", AsText(AsNumber(
                                 Value::Double(have))),
                             unit, ", and the least allowed is ",
                             AsText(OfConstant(field.range.minimum)), "."));
    }
    if (field.range.has_maximum &&
        have > BoundNumber(field.range.maximum) + 1e-9) {
      return FieldError(
          path, absl::StrCat("is ", AsText(AsNumber(Value::Double(have))), unit,
                             ", and the most allowed is ",
                             AsText(OfConstant(field.range.maximum)), "."));
    }
  }
  if (field.unique && held.kind() == Value::Kind::kList) {
    const std::vector<Value>& items = held.items();
    for (size_t index = 0; index < items.size(); ++index) {
      for (size_t other = index + 1; other < items.size(); ++other) {
        if (items[index] == items[other]) {
          return FieldError(path, absl::StrCat("holds '", AsText(items[index]),
                                              "' twice, and its items are "
                                              "unique."));
        }
      }
    }
  }
  return made;
}

}  // namespace

absl::StatusOr<Value> CoerceShape(const DtoPlan& shape, const Value& value,
                                  const CoerceContext& context) {
  if (value.kind() != Value::Kind::kObject &&
      value.kind() != Value::Kind::kHost) {
    // Shapes accept records; a bare value cannot identify its target field.
    return absl::InvalidArgumentError(absl::StrCat(
        shape.name, " is a record of ", absl::StrJoin(shape.FieldNames(), ", "),
        ", and this is ", value.IsNull() ? "nothing" : AsText(value), "."));
  }

  Value::Pairs pairs;
  pairs.reserve(shape.fields.size());
  for (const FieldPlan& field : shape.fields) {
    Value scratch;
    const Value* given = FieldOf(value, field.name, scratch);
    if (given == nullptr || given->IsNull()) {
      if (field.has_default) {
        // Validate defaults through the same field constraints as input values.
        ABSL_ASSIGN_OR_RETURN(
            Value made,
            CoerceField(field, OfConstant(field.default_value), context,
                        field.name));
        pairs.emplace_back(field.name, std::move(made));
        continue;
      }
      if (field.required) {
        return absl::InvalidArgumentError(
            absl::StrCat(shape.name, " requires '", field.name,
                         "', and this does not give it."));
      }
      // Optional fields without defaults remain absent.
      continue;
    }
    ABSL_ASSIGN_OR_RETURN(
        Value made, CoerceField(field, *given, context, field.name));
    pairs.emplace_back(field.name, std::move(made));
  }

  // The fields the shape declares, in the order it declared them, and nothing
  // else: see the note on [CoerceShape] about extra keys.
  Value made = Value::Object(std::move(pairs));
  if (context.bridge == nullptr || context.shapes == nullptr) return made;
  return context.bridge->Adopt(shape, *context.shapes, made);
}

absl::StatusOr<Value> Coerce(const Value& value,
                             const syntax::TypeExpression& type,
                             const CoerceContext& context) {
  const std::string lowered = Canonical(type.name);
  if (lowered == "string" || lowered == "text") {
    return Value::String(AsText(value));
  }
  if (lowered == "number") return AsNumber(value);
  if (lowered == "integer" || lowered == "int") {
    return Value::Integer(static_cast<std::int64_t>(AsDouble(value)));
  }
  if (lowered == "bool" || lowered == "boolean") {
    return Value::Bool(Truthy(value));
  }
  if (lowered == "duration") return Value::Duration(AsDuration(value));
  if (lowered == "time") return Value::Time(AsTime(value));
  if (lowered == "bytes") {
    if (value.kind() == Value::Kind::kBytes) return value;
    return Value::Bytes(AsText(value));
  }
  if (lowered == "list" || lowered == "array") {
    std::vector<Value> items;
    if (value.kind() == Value::Kind::kList) {
      items = value.items();
    } else {
      items.push_back(value);
    }
    if (!type.parameters.empty()) {
      for (Value& item : items) {
        ABSL_ASSIGN_OR_RETURN(item,
                              Coerce(item, type.parameters.front(), context));
      }
    }
    return Value::List(std::move(items));
  }
  if (lowered == "object" || lowered == "json") {
    // A mapping is already what this asks for, and anything else is left as it
    // is rather than wrapped in an invented key.
    return value;
  }
  if (lowered == "any") return value;
  // A shape this program declared, before a registry is asked: a `struct`
  // outranks a tag of the same name, because what the file says about the name
  // is what the file means by it. Its own spelling, not the canonical one:
  // `struct` names are not keywords.
  if (context.shapes != nullptr) {
    if (const DtoPlan* shape = context.shapes->Dto(type.name);
        shape != nullptr) {
      return CoerceShape(*shape, value, context);
    }
  }
  if (context.bridge == nullptr) {
    return Invalid(absl::StrCat(
        "Nothing here knows the type '", type.name,
        "'. A tag names a type a serialization registry has been told about, so"
        " the module defining it has to be imported where the flow runs."));
  }
  return context.bridge->Coerce(type.name, value);
}

// --- Evaluation --------------------------------------------------------------

namespace {

absl::StatusOr<Value> Arithmetic(std::string_view op, const Value& left,
                                 const Value& right) {
  if (left.IsTimelike() || right.IsTimelike()) {
    const bool left_time = left.kind() == Value::Kind::kTime;
    const bool right_time = right.kind() == Value::Kind::kTime;
    if (left_time && right_time) {
      if (op == "-") return Value::Duration(left.time() - right.time());
      return Invalid("Two instants can be subtracted, but not added.");
    }
    if (left_time) {
      const absl::Duration other = AsDuration(right);
      return Value::Time(op == "+" ? left.time() + other : left.time() - other);
    }
    if (right_time) {
      if (op == "+") return Value::Time(right.time() + AsDuration(left));
      return Invalid("An instant cannot be subtracted from a duration.");
    }
    const absl::Duration first = AsDuration(left);
    const absl::Duration second = AsDuration(right);
    return Value::Duration(op == "+" ? first + second : first - second);
  }
  const Value first = AsNumber(left);
  const Value second = AsNumber(right);
  if (first.kind() == Value::Kind::kInteger &&
      second.kind() == Value::Kind::kInteger) {
    return Value::Integer(op == "+" ? first.integer() + second.integer()
                                    : first.integer() - second.integer());
  }
  const double one = AsDouble(first);
  const double two = AsDouble(second);
  return Value::Double(op == "+" ? one + two : one - two);
}

/// Where the two sit relative to one another: -1, 0 or 1.
///
/// Two instants, or two durations, compare as themselves: turning them into
/// text would order `9s` after `10s`, and into numbers would lose the
/// difference between an instant and a length. Otherwise a number compares as a
/// number and everything else as text, so a flow never dies on `"3" < 5`.
int OrderImpl(const Value& left, const Value& right) {
  if (left.IsTimelike() && right.IsTimelike()) {
    const double one = AsDouble(left);
    const double two = AsDouble(right);
    return one < two ? -1 : (one > two ? 1 : 0);
  }
  if (CountsAsNumber(left) || CountsAsNumber(right)) {
    const double one = AsDouble(left);
    const double two = AsDouble(right);
    return one < two ? -1 : (one > two ? 1 : 0);
  }
  const std::string one = AsText(left);
  const std::string two = AsText(right);
  return one < two ? -1 : (one > two ? 1 : 0);
}

const Value* absl_nullable Bound(const syntax::Node& node,
                                 const EvalContext& context) {
  if (context.bound == nullptr) return nullptr;
  const auto found = context.bound->find(&node);
  return found == context.bound->end() ? nullptr : &found->second;
}

}  // namespace

int Order(const Value& left, const Value& right) {
  return OrderImpl(left, right);
}

absl::StatusOr<Value> Add(const Value& left, const Value& right) {
  return Arithmetic("+", left, right);
}

absl::StatusOr<Value> Evaluate(const syntax::Node& node,
                               const EvalContext& context) {
  // A name the resolver bound to a stream reads as that stream's first value,
  // whatever shape the expression around it has. Checked first, because
  // `x.field` is a field of a value and `call.port` is a stream, and only the
  // resolver knows which of the two was written.
  if (const Value* found = Bound(node, context); found != nullptr) {
    return *found;
  }
  switch (node.kind) {
    case syntax::NodeKind::kLiteral:
      return Value::Of(static_cast<const syntax::Literal&>(node).value);
    case syntax::NodeKind::kIt:
      return context.has_it ? context.it : Value::Null();
    case syntax::NodeKind::kListLiteral: {
      const auto& literal = static_cast<const syntax::ListLiteral&>(node);
      std::vector<Value> items;
      items.reserve(literal.items.size());
      for (const syntax::NodePtr& item : literal.items) {
        if (const auto* spread = syntax::As<syntax::Spread>(item.get());
            spread != nullptr) {
          ABSL_ASSIGN_OR_RETURN(Value held,
                                Evaluate(*spread->value, context));
          for (Value& inner : SpreadItems(held)) {
            items.push_back(std::move(inner));
          }
          continue;
        }
        ABSL_ASSIGN_OR_RETURN(Value value, Evaluate(*item, context));
        items.push_back(std::move(value));
      }
      return Value::List(std::move(items));
    }
    case syntax::NodeKind::kObjectLiteral: {
      const auto& literal = static_cast<const syntax::ObjectLiteral&>(node);
      Value::Pairs pairs;
      pairs.reserve(literal.pairs.size());
      // A later key wins over one a spread brought in, which is what makes
      // `{...it, "tags": [..]}` an override and `{"tags": [..], ...it}` not.
      const auto put = [&pairs](std::string_view key, Value value) {
        for (auto& [existing, held] : pairs) {
          if (existing == key) {
            held = std::move(value);
            return;
          }
        }
        pairs.emplace_back(std::string(key), std::move(value));
      };
      for (const auto& [key, item] : literal.pairs) {
        if (const auto* spread = syntax::As<syntax::Spread>(item.get());
            spread != nullptr) {
          ABSL_ASSIGN_OR_RETURN(Value held,
                                Evaluate(*spread->value, context));
          for (auto& [inner_key, inner] : SpreadPairs(held)) {
            put(inner_key, std::move(inner));
          }
          continue;
        }
        ABSL_ASSIGN_OR_RETURN(Value value, Evaluate(*item, context));
        put(key, std::move(value));
      }
      return Value::Object(std::move(pairs));
    }
    case syntax::NodeKind::kSpread:
      // Only ever an item of a literal, and each of those reads it itself. One
      // standing anywhere else is a mistake the resolver already reported; this
      // is what keeps it from being read as the thing it was spreading.
      return absl::InvalidArgumentError(
          "A spread belongs in a list or an object literal.");
    case syntax::NodeKind::kAttr: {
      const auto& attr = static_cast<const syntax::Attr&>(node);
      ABSL_ASSIGN_OR_RETURN(Value base, Evaluate(*attr.base, context));
      return Lookup(base, Value::String(attr.name));
    }
    case syntax::NodeKind::kIndex: {
      const auto& index = static_cast<const syntax::Index&>(node);
      ABSL_ASSIGN_OR_RETURN(Value base, Evaluate(*index.base, context));
      ABSL_ASSIGN_OR_RETURN(Value key, Evaluate(*index.index, context));
      return Lookup(base, key);
    }
    case syntax::NodeKind::kBuiltin: {
      const auto& builtin = static_cast<const syntax::Builtin&>(node);
      std::vector<Value> arguments;
      arguments.reserve(builtin.args.size());
      for (const syntax::NodePtr& argument : builtin.args) {
        ABSL_ASSIGN_OR_RETURN(Value value, Evaluate(*argument, context));
        arguments.push_back(std::move(value));
      }
      return CallBuiltin(builtin.name, arguments, context.bridge);
    }
    case syntax::NodeKind::kTypedValue: {
      const auto& typed = static_cast<const syntax::TypedValue&>(node);
      ABSL_ASSIGN_OR_RETURN(Value value, Evaluate(*typed.value, context));
      return Coerce(value, typed.type, context.Coercion());
    }
    case syntax::NodeKind::kUnary: {
      const auto& unary = static_cast<const syntax::Unary&>(node);
      ABSL_ASSIGN_OR_RETURN(Value value, Evaluate(*unary.operand, context));
      return Value::Bool(!Truthy(value));
    }
    case syntax::NodeKind::kBinary: {
      const auto& binary = static_cast<const syntax::Binary&>(node);
      if (binary.op == "and" || binary.op == "or") {
        // The value, not a boolean: `a or b` is `a` when `a` holds, which is
        // what makes `name or "unknown"` read as a default.
        ABSL_ASSIGN_OR_RETURN(Value left, Evaluate(*binary.left, context));
        const bool holds = Truthy(left);
        if ((binary.op == "and") != holds) return left;
        return Evaluate(*binary.right, context);
      }
      ABSL_ASSIGN_OR_RETURN(Value left, Evaluate(*binary.left, context));
      ABSL_ASSIGN_OR_RETURN(Value right, Evaluate(*binary.right, context));
      if (binary.op == "==") return Value::Bool(left == right);
      if (binary.op == "!=") return Value::Bool(!(left == right));
      if (binary.op == "in") return Value::Bool(Contains(right, left));
      if (binary.op == "+" || binary.op == "-") {
        return Arithmetic(binary.op, left, right);
      }
      const int order = OrderImpl(left, right);
      if (binary.op == "<") return Value::Bool(order < 0);
      if (binary.op == "<=") return Value::Bool(order <= 0);
      if (binary.op == ">") return Value::Bool(order > 0);
      if (binary.op == ">=") return Value::Bool(order >= 0);
      return Invalid(absl::StrCat("Unknown operator '", binary.op, "'."));
    }
    case syntax::NodeKind::kName:
    case syntax::NodeKind::kOutcome:
    case syntax::NodeKind::kPipelineValue:
      // Every one of these is a stream, and a stream is read before the
      // expression runs. Reaching one here means nothing bound it, which for a
      // name the resolver could not place is the same nothing a missing key is.
      return Value::Null();
    default:
      return Invalid(absl::StrCat("Cannot evaluate a ",
                                  syntax::NodeKindName(node.kind), "."));
  }
}

// --- Statuses as data --------------------------------------------------------

Value StatusRecord(const absl::Status& status) {
  const auto code = static_cast<size_t>(status.code());
  const absl::Span<const std::string_view> names = vocabulary::StatusCodes();
  std::string name = code < names.size()
                         ? absl::AsciiStrToUpper(names[code])
                         : absl::StrCat("CODE_", code);
  Value::Pairs pairs;
  pairs.emplace_back("ok", Value::Bool(status.ok()));
  pairs.emplace_back("code", Value::String(std::move(name)));
  pairs.emplace_back("number", Value::Integer(static_cast<std::int64_t>(code)));
  pairs.emplace_back("message", Value::String(std::string(status.message())));
  return Value::Object(std::move(pairs));
}

std::optional<absl::StatusCode> StatusCodeOf(const Value& value) {
  if (value.kind() == Value::Kind::kBool) return std::nullopt;
  if (value.kind() == Value::Kind::kInteger) {
    const std::int64_t number = value.integer();
    if (number < 0 ||
        number >= static_cast<std::int64_t>(vocabulary::StatusCodes().size())) {
      return std::nullopt;
    }
    return static_cast<absl::StatusCode>(number);
  }
  if (value.kind() == Value::Kind::kString) {
    std::string wanted =
        Canonical(absl::StripAsciiWhitespace(value.text()));
    for (char& letter : wanted) {
      if (letter == '-') letter = '_';
    }
    const absl::Span<const std::string_view> names = vocabulary::StatusCodes();
    for (size_t at = 0; at < names.size(); ++at) {
      if (names[at] == wanted) return static_cast<absl::StatusCode>(at);
    }
  }
  return std::nullopt;
}

absl::Status StatusOfRecord(const Value& record) {
  const Value* code = record.Get("code");
  std::optional<absl::StatusCode> resolved =
      code == nullptr ? std::nullopt : StatusCodeOf(*code);
  if (!resolved.has_value()) {
    const Value* number = record.Get("number");
    if (number != nullptr) resolved = StatusCodeOf(*number);
  }
  const Value* message = record.Get("message");
  return absl::Status(resolved.value_or(absl::StatusCode::kUnknown),
                      message == nullptr ? "" : AsText(*message));
}

}  // namespace a11::flow
