// Copyright 2026 The A11 Authors.

#include "a11/flow/vocabulary.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/types/span.h>

namespace a11::flow::vocabulary {
namespace {

// Stages in the order a listing reads best: what shortens a stream, what reshapes
// each value, what reduces it to one, what encodes it.
constexpr std::array kStageOrder = {
    std::string_view("first"),  std::string_view("last"),
    std::string_view("drop"),   std::string_view("truncate"),
    std::string_view("batch"),  std::string_view("group"),
    std::string_view("where"),  std::string_view("map"),
    std::string_view("distinct"), std::string_view("then"),
    std::string_view("mime"),   std::string_view("strformat"),
    std::string_view("collect"), std::string_view("count"),
    std::string_view("join"),   std::string_view("text"),
    std::string_view("json"),   std::string_view("packb"),
};

const absl::flat_hash_map<std::string_view, StageArgument>& StageTable() {
  static const auto* table =
      new absl::flat_hash_map<std::string_view, StageArgument>{
          {"first", StageArgument::kNumber},
          {"last", StageArgument::kNumber},
          {"drop", StageArgument::kNumber},
          {"truncate", StageArgument::kNumber},
          {"batch", StageArgument::kNumber},
          {"where", StageArgument::kExpression},
          {"map", StageArgument::kExpression},
          {"group", StageArgument::kExpression},
          {"join", StageArgument::kOptionalString},
          {"strformat", StageArgument::kString},
          {"mime", StageArgument::kString},
          {"then", StageArgument::kStream},
          {"collect", StageArgument::kNone},
          {"count", StageArgument::kNone},
          {"distinct", StageArgument::kNone},
          {"text", StageArgument::kNone},
          {"json", StageArgument::kNone},
          {"packb", StageArgument::kNone},
      };
  return *table;
}

constexpr std::array kStatusCodes = {
    std::string_view("ok"),
    std::string_view("cancelled"),
    std::string_view("unknown"),
    std::string_view("invalid_argument"),
    std::string_view("deadline_exceeded"),
    std::string_view("not_found"),
    std::string_view("already_exists"),
    std::string_view("permission_denied"),
    std::string_view("resource_exhausted"),
    std::string_view("failed_precondition"),
    std::string_view("aborted"),
    std::string_view("out_of_range"),
    std::string_view("unimplemented"),
    std::string_view("internal"),
    std::string_view("unavailable"),
    std::string_view("data_loss"),
    std::string_view("unauthenticated"),
};

// Shortest unit first, which is also the order a duration is formatted in.
constexpr std::array kDurationUnits = {
    std::string_view("ns"), std::string_view("us"), std::string_view("ms"),
    std::string_view("s"),  std::string_view("m"),  std::string_view("h"),
};

const absl::flat_hash_map<std::string_view, double>& DurationTable() {
  static const auto* table = new absl::flat_hash_map<std::string_view, double>{
      {"ns", 1e-9}, {"us", 1e-6}, {"ms", 0.001},
      {"s", 1.0},   {"m", 60.0},  {"h", 3600.0},
  };
  return *table;
}

const absl::flat_hash_set<std::string_view>* MakeSet(
    std::initializer_list<std::string_view> words) {
  return new absl::flat_hash_set<std::string_view>(words);
}

// A set of the words in an ordered table, so a list that has an order and a set
// that answers "is this one of them" cannot disagree about what the words are.
// An entry of two words -- `forward headers` -- contributes both.
const absl::flat_hash_set<std::string_view>* SetOf(
    absl::Span<const std::string_view> words) {
  auto* set = new absl::flat_hash_set<std::string_view>();
  for (const std::string_view word : words) {
    size_t start = 0;
    while (start <= word.size()) {
      const size_t space = word.find(' ', start);
      const size_t end = space == std::string_view::npos ? word.size() : space;
      if (end > start) set->insert(word.substr(start, end - start));
      if (space == std::string_view::npos) break;
      start = space + 1;
    }
  }
  return set;
}

// Types in the order a listing reads: the scalars, then the containers, then the
// escape hatches.
constexpr std::array kTypeOrder = {
    std::string_view("string"),  std::string_view("text"),
    std::string_view("number"),  std::string_view("integer"),
    std::string_view("int"),     std::string_view("bool"),
    std::string_view("boolean"), std::string_view("object"),
    std::string_view("json"),    std::string_view("list"),
    std::string_view("array"),   std::string_view("bytes"),
    std::string_view("any"),
};

// The functions, grouped the way the reference lists them: text, structure,
// then time.
constexpr std::array kBuiltinOrder = {
    std::string_view("len"),        std::string_view("lower"),
    std::string_view("upper"),      std::string_view("trim"),
    std::string_view("text"),       std::string_view("number"),
    std::string_view("bool"),       std::string_view("keys"),
    std::string_view("values"),     std::string_view("get"),
    std::string_view("join"),       std::string_view("split"),
    std::string_view("merge"),      std::string_view("contains"),
    std::string_view("starts-with"), std::string_view("ends-with"),
    std::string_view("replace"),    std::string_view("slice"),
    std::string_view("default"),    std::string_view("to_chunk"),
    std::string_view("from_chunk"), std::string_view("strformat"),
    std::string_view("now"),        std::string_view("duration"),
    std::string_view("time"),       std::string_view("seconds"),
};

// The verbs first, then the barriers, then the blocks: the order somebody
// scanning a list of statements finds what they meant.
constexpr std::array kStatementOrder = {
    std::string_view("run"),    std::string_view("call"),
    std::string_view("try"),    std::string_view("skip"),
    std::string_view("wait"),   std::string_view("drain"),
    std::string_view("cancel"), std::string_view("fail"),
    std::string_view("if"),     std::string_view("for"),
    std::string_view("repeat"), std::string_view("until"),
    std::string_view("while"),  std::string_view("nodes"),
};

// `else` continues an `if`; `parallel` and `max` say how wide a loop runs.
constexpr std::array kClauseOrder = {
    std::string_view("else"),
    std::string_view("parallel"),
    std::string_view("max"),
};

// As a flow is written, top to bottom.
constexpr std::array kDeclarationOrder = {
    std::string_view("flow"),     std::string_view("describe"),
    std::string_view("in"),       std::string_view("out"),
    std::string_view("header"),   std::string_view("as"),
    std::string_view("default"),  std::string_view("stream"),
    std::string_view("required"), std::string_view("node"),
    std::string_view("nodes"),
};

// As they read after a call: what it does with its nodes, then how long, then
// what it waits for, then what it is told.
constexpr std::array kModifierOrder = {
    std::string_view("tee"),  std::string_view("via"),
    std::string_view("timeout"), std::string_view("after"),
    std::string_view("with"), std::string_view("id"),
    std::string_view("forward headers"),
};

constexpr std::array kPortModifierOrder = {
    std::string_view("stream"),
    std::string_view("required"),
};

constexpr std::array kStatusFieldOrder = {
    std::string_view("ok"),
    std::string_view("code"),
    std::string_view("number"),
    std::string_view("message"),
};

}  // namespace

std::string Canonical(std::string_view word) {
  bool has_upper = false;
  bool has_lower = false;
  for (const char letter : word) {
    if (letter >= 'A' && letter <= 'Z') has_upper = true;
    if (letter >= 'a' && letter <= 'z') has_lower = true;
  }
  if (!has_upper || has_lower) return std::string(word);
  std::string lowered(word);
  for (char& letter : lowered) {
    if (letter >= 'A' && letter <= 'Z') letter = static_cast<char>(letter + 32);
  }
  return lowered;
}

bool IsShouted(std::string_view word) {
  bool has_upper = false;
  for (const char letter : word) {
    if (letter >= 'a' && letter <= 'z') return false;
    if (letter >= 'A' && letter <= 'Z') has_upper = true;
  }
  return has_upper;
}

absl::Span<const std::string_view> Stages() {
  return absl::MakeConstSpan(kStageOrder.data(), kStageOrder.size());
}

std::optional<StageArgument> StageTakes(std::string_view canonical_name) {
  const auto found = StageTable().find(canonical_name);
  if (found == StageTable().end()) return std::nullopt;
  return found->second;
}

std::string_view StageArgumentName(StageArgument argument) {
  switch (argument) {
    case StageArgument::kNone:
      return "none";
    case StageArgument::kNumber:
      return "number";
    case StageArgument::kExpression:
      return "expr";
    case StageArgument::kString:
      return "string";
    case StageArgument::kOptionalString:
      return "string?";
    case StageArgument::kStream:
      return "stream";
  }
  return "none";
}

const absl::flat_hash_set<std::string_view>& BareStages() {
  static const auto* words = MakeSet({"then", "where"});
  return *words;
}

const absl::flat_hash_set<std::string_view>& ReducingStages() {
  static const auto* words = MakeSet({"collect", "count", "join"});
  return *words;
}

const absl::flat_hash_set<std::string_view>& PositionalStages() {
  static const auto* words =
      MakeSet({"first", "last", "drop", "batch", "group", "distinct", "count"});
  return *words;
}

const absl::flat_hash_set<std::string_view>& Builtins() {
  static const auto* words = SetOf(OrderedBuiltins());
  return *words;
}

absl::Span<const std::string_view> OrderedBuiltins() {
  return absl::MakeConstSpan(kBuiltinOrder.data(), kBuiltinOrder.size());
}

const absl::flat_hash_set<std::string_view>& TypeNames() {
  static const auto* words = SetOf(OrderedTypeNames());
  return *words;
}

absl::Span<const std::string_view> OrderedTypeNames() {
  return absl::MakeConstSpan(kTypeOrder.data(), kTypeOrder.size());
}

absl::Span<const int> TypeParameters(std::string_view canonical_name) {
  // A list says what it holds; an object says what it maps. Everything else
  // takes none.
  static constexpr std::array<int, 2> kZeroOrOne = {0, 1};
  static constexpr std::array<int, 3> kZeroOneOrTwo = {0, 1, 2};
  static constexpr std::array<int, 1> kNone = {0};
  if (canonical_name == "list" || canonical_name == "array") {
    return absl::MakeConstSpan(kZeroOrOne.data(), kZeroOrOne.size());
  }
  if (canonical_name == "object" || canonical_name == "json") {
    return absl::MakeConstSpan(kZeroOneOrTwo.data(), kZeroOneOrTwo.size());
  }
  return absl::MakeConstSpan(kNone.data(), kNone.size());
}

const absl::flat_hash_set<std::string_view>& StatementWords() {
  static const auto* words = SetOf(OrderedStatements());
  return *words;
}

absl::Span<const std::string_view> OrderedStatements() {
  return absl::MakeConstSpan(kStatementOrder.data(), kStatementOrder.size());
}

const absl::flat_hash_set<std::string_view>& ClauseWords() {
  static const auto* words = SetOf(OrderedClauseWords());
  return *words;
}

absl::Span<const std::string_view> OrderedClauseWords() {
  return absl::MakeConstSpan(kClauseOrder.data(), kClauseOrder.size());
}

const absl::flat_hash_set<std::string_view>& DeclarationWords() {
  static const auto* words = SetOf(OrderedDeclarations());
  return *words;
}

absl::Span<const std::string_view> OrderedDeclarations() {
  return absl::MakeConstSpan(kDeclarationOrder.data(),
                             kDeclarationOrder.size());
}

const absl::flat_hash_set<std::string_view>& ModifierWords() {
  static const auto* words = SetOf(OrderedModifiers());
  return *words;
}

absl::Span<const std::string_view> OrderedModifiers() {
  return absl::MakeConstSpan(kModifierOrder.data(), kModifierOrder.size());
}

const absl::flat_hash_set<std::string_view>& SourceWords() {
  static const auto* words = MakeSet({"status"});
  return *words;
}

const absl::flat_hash_set<std::string_view>& PortModifierWords() {
  static const auto* words = SetOf(OrderedPortModifiers());
  return *words;
}

absl::Span<const std::string_view> OrderedPortModifiers() {
  return absl::MakeConstSpan(kPortModifierOrder.data(),
                             kPortModifierOrder.size());
}

absl::Span<const std::string_view> StatusCodes() {
  return absl::MakeConstSpan(kStatusCodes.data(), kStatusCodes.size());
}

bool IsStatusCode(std::string_view word) {
  const std::string canonical = Canonical(word);
  // A code may be written with dashes where the canonical name has underscores,
  // which is what `plan.status_code` accepts.
  std::string normalised = canonical;
  for (char& letter : normalised) {
    if (letter == '-') letter = '_';
  }
  for (const std::string_view code : kStatusCodes) {
    if (code == normalised) return true;
  }
  return false;
}

const absl::flat_hash_set<std::string_view>& StatusFields() {
  static const auto* words = SetOf(OrderedStatusFields());
  return *words;
}

absl::Span<const std::string_view> OrderedStatusFields() {
  return absl::MakeConstSpan(kStatusFieldOrder.data(),
                             kStatusFieldOrder.size());
}

const absl::flat_hash_set<std::string_view>& ConstantWords() {
  static const auto* words = MakeSet({"true", "false", "null", "it"});
  return *words;
}

const absl::flat_hash_set<std::string_view>& OperatorWords() {
  static const auto* words = MakeSet({"and", "or", "not"});
  return *words;
}

absl::Span<const std::string_view> DurationUnits() {
  return absl::MakeConstSpan(kDurationUnits.data(), kDurationUnits.size());
}

std::optional<double> DurationUnitSeconds(std::string_view unit) {
  const auto found = DurationTable().find(unit);
  if (found == DurationTable().end()) return std::nullopt;
  return found->second;
}

}  // namespace a11::flow::vocabulary
