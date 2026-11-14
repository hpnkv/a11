/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef A11_FLOW_VOCABULARY_H_
#define A11_FLOW_VOCABULARY_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/container/flat_hash_set.h>
#include <absl/types/span.h>

namespace a11::flow {

/// Word sets and argument shapes shared by language frontends.
/// Words are significant at grammar positions; none are globally reserved.
namespace vocabulary {

/// Lowercases uniformly upper-case words; leaves mixed-case words unchanged.
/// `FOR` and `for` are keywords; `For` is a name.
std::string Canonical(std::string_view word);

/// Whether the word, canonicalised, is written entirely in upper case.
bool IsShouted(std::string_view word);

/// What a stage takes after its name.
enum class StageArgument {
  /// Nothing: `| collect`, `| count`.
  kNone,
  /// A whole number of values: `| first 3`.
  kNumber,
  /// An expression, with `it` bound to the value in hand: `| where it.ok`.
  kExpression,
  /// A string, required: `| strformat "took {}"`.
  kString,
  /// A string, optional: `| join`, `| join ", "`.
  kOptionalString,
  /// An expression, optional, with `it` bound: `| sum`, `| sum it.price`.
  kOptionalExpression,
  /// An optional `by` key with `it` bound, and an optional `desc`:
  /// `| sort`, `| sort by it.score desc`.
  kSortKey,
  /// A literal to start from, a name for what is carried, and the expression
  /// that folds one value into it: `| fold 0 as total, total + it`.
  kFold,
  /// A length of time: `| timeout 30s`, `| pace 100ms`.
  kDuration,
  /// Another stream to read after this one: `| then other`.
  kStream,
  /// A named type: `| as i32`.
  kType,
  /// An optional level word and an optional expression, with `it` bound to the
  /// value in hand: `| log`, `| log warning it.error`.
  kLog,
  /// An optional level word, a format string, and the values to fill it with,
  /// with `it` bound to the value in hand: `| logf "took %s" it.elapsed`.
  kLogFormat,
};

/// One implicit port of the entry flow.
struct EntryPort {
  std::string_view name;
  std::string_view type;
  /// Whether it carries one value rather than a stream.
  bool unary;
  std::string_view description;
};

/// Implicit entry-flow ports in declaration order.
absl::Span<const EntryPort> EntryPorts();

absl::Span<const std::string_view> Stages();

/// What a stage takes, or `nullopt` if the name is not a stage.
std::optional<StageArgument> StageTakes(std::string_view canonical_name);

/// The spelling of a stage argument kind in the output formats.
std::string_view StageArgumentName(StageArgument argument);

/// Documentation for a language word, shared by editor and CLI surfaces.
struct WordDoc {
  /// One-line description shown in a summary.
  std::string_view summary;
  /// Argument description in source terms, or empty for no argument.
  std::string_view takes;
  /// Behavior description as a short Markdown paragraph.
  std::string_view detail;
  /// Single-line examples use newlines; multiline examples use blank lines.
  /// A sentence before a multiline flow introduces its use case.
  std::string_view example;
};

/// Stage documentation, or null for an unknown stage.
const WordDoc* absl_nullable StageDocumentation(
    std::string_view canonical_name);

/// Built-in function documentation, or null for an unknown function.
/// A word may have separate stage and function meanings.
const WordDoc* absl_nullable BuiltinDocumentation(
    std::string_view canonical_name);

/// The grammar role under which a word is documented.
/// A word may appear under multiple roles with one shared description.
enum class WordRole {
  /// A pipeline stage: `| first 3`. Same table as [StageDocumentation].
  kStage,
  /// A built-in function, where it is called. Same table as
  /// [BuiltinDocumentation].
  kBuiltin,
  /// A word that opens a statement: `run`, `value`, `for`. [StatementWords].
  kStatement,
  /// A word that declares something: `flow`, `in`, `header`.
  /// [DeclarationWords].
  kDeclaration,
  /// A word that continues a statement: `else`, `parallel`. [ClauseWords].
  kClause,
  /// A word that may follow a call's `)`: `via`, `timeout`. [ModifierWords].
  kModifier,
  /// A word that opens a pipeline source rather than naming one: `status`,
  /// `zip`. [SourceWords].
  kSource,
  /// What a port says about itself after its type. [PortModifierWords].
  kPortModifier,
  /// What a `struct` field says about itself after its type.
  /// [FieldModifierWords].
  kFieldModifier,
  /// A built-in port type name. [TypeNames].
  kType,
  /// A literal that is a word: `true`, `it`. [ConstantWords].
  kConstant,
  /// An operator that is a word: `and`, `not`. [OperatorWords].
  kOperatorWord,
  /// One of Abseil's canonical status codes. [StatusCodes].
  kStatusCode,
  /// The severity a `log` or `logf` may name. [LogLevels].
  kLogLevel,
  /// A field of a status record, read after a `.`. [StatusFields].
  kStatusField,
  /// A duration suffix: `ms`, `h`. [DurationUnits].
  kDurationUnit,
  /// Punctuation and the operators that are not words, keyed by the symbol
  /// itself: `|`, `->`, `==`, `{`. [OrderedSymbols].
  kSymbol,
};

/// Whether a standard type and its conversion are in the primitive prelude.
bool IsPrimitiveType(std::string_view name);

/// Returns documentation under any role, or null when none exists.
const WordDoc* absl_nullable AnyDocumentation(std::string_view canonical_name);

/// Every role, for something walking all of them.
absl::Span<const WordRole> WordRoles();

/// Role name used by `flow.vocabulary/v1`, such as `port_modifier`.
std::string_view WordRoleName(WordRole role);

/// Words documented under `role`, in listing order.
std::vector<std::string_view> WordsOf(WordRole role);

/// What one word of `role` does, or `nullptr` where that table has no entry.
///
/// The word is the canonical form for every role but [WordRole::kSymbol], where
/// it is the punctuation as written.
const WordDoc* absl_nullable Documentation(WordRole role,
                                           std::string_view canonical_name);

/// Punctuation and non-word operators, in listing order.
absl::Span<const std::string_view> OrderedSymbols();

/// Stages accepted without a leading `|`, such as `hits where it.ok`.
const absl::flat_hash_set<std::string_view>& BareStages();

/// Stages that accept `parallel n` for concurrent value processing.
const absl::flat_hash_set<std::string_view>& ParallelStages();

/// Stages that reduce a whole stream to one value.
const absl::flat_hash_set<std::string_view>& ReducingStages();

/// Stages that select or group stream values.
const absl::flat_hash_set<std::string_view>& PositionalStages();

/// Built-in functions available to flow expressions.
const absl::flat_hash_set<std::string_view>& Builtins();

/// Built-in port type names. Ports also accept registered tags and media types.
const absl::flat_hash_set<std::string_view>& TypeNames();
bool IsNumericType(std::string_view name);

/// Allowed type-parameter counts for a built-in type; empty means none.
absl::Span<const int> TypeParameters(std::string_view canonical_name);

/// The built-in port type names, in the order a listing reads them.
absl::Span<const std::string_view> OrderedTypeNames();

/// The fixed function set, in the order a listing reads it.
absl::Span<const std::string_view> OrderedBuiltins();

/// Words that open a statement, and so are not read as a name there.
const absl::flat_hash_set<std::string_view>& StatementWords();

/// Statement starters in completion order, most common first.
absl::Span<const std::string_view> OrderedStatements();

/// Words inside statements, such as `else`, `parallel`, and `max`.
const absl::flat_hash_set<std::string_view>& ClauseWords();

/// The same, in the order they read.
absl::Span<const std::string_view> OrderedClauseWords();

/// Words that declare something inside a flow.
const absl::flat_hash_set<std::string_view>& DeclarationWords();

/// The declarations in the order they are written in a flow.
absl::Span<const std::string_view> OrderedDeclarations();

/// Words that may follow a call's closing parenthesis.
const absl::flat_hash_set<std::string_view>& ModifierWords();

/// Call modifiers in source order, including `forward headers`.
absl::Span<const std::string_view> OrderedModifiers();

/// Words that open a pipeline source rather than naming one: `status`, `zip`.
const absl::flat_hash_set<std::string_view>& SourceWords();

/// What a port says about itself after its type: `stream`, `required`.
const absl::flat_hash_set<std::string_view>& PortModifierWords();

/// The same, in the order a port writes them.
absl::Span<const std::string_view> OrderedPortModifiers();

/// Modifiers after a `struct` field's type.
const absl::flat_hash_set<std::string_view>& FieldModifierWords();

/// Field modifiers in required source order.
absl::Span<const std::string_view> OrderedFieldModifiers();

/// Abseil's canonical status codes, lower case, which is what `fail` names.
absl::Span<const std::string_view> StatusCodes();

/// Whether a word names a canonical status code, in either case.
bool IsStatusCode(std::string_view word);

/// Log levels accepted by `log` and `logf`, quietest first.
absl::Span<const std::string_view> LogLevels();

/// Whether a word names a log level, in either case.
bool IsLogLevel(std::string_view word);

/// The fields of a status record: what reading an outcome gives.
const absl::flat_hash_set<std::string_view>& StatusFields();

/// The same, in the order a status record is written and read.
absl::Span<const std::string_view> OrderedStatusFields();

/// Literals that are words: `true`, `false`, `null`, `it`.
const absl::flat_hash_set<std::string_view>& ConstantWords();

/// Operators that are words: `and`, `or`, `not`.
const absl::flat_hash_set<std::string_view>& OperatorWords();

/// Duration suffixes a number may carry, shortest unit first.
absl::Span<const std::string_view> DurationUnits();

/// The seconds one of that unit is, or `nullopt` if it is not a unit.
std::optional<double> DurationUnitSeconds(std::string_view unit);

}  // namespace vocabulary
}  // namespace a11::flow

#endif  // A11_FLOW_VOCABULARY_H_
