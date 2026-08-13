// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_VOCABULARY_H_
#define A11_FLOW_VOCABULARY_H_

#include <optional>
#include <string>
#include <string_view>

#include <absl/base/nullability.h>
#include <absl/container/flat_hash_set.h>
#include <absl/types/span.h>

namespace a11::flow {

/// The words the language gives meaning to, and what each one may be given.
///
/// This is the one table. The Python parser, the Sublime syntax and the IntelliJ
/// plugin each used to keep a copy, and `a11/flow/tests/test_editor_support.py`
/// existed to catch them drifting; everything now reads these instead, generating
/// what a static grammar file needs rather than restating it.
///
/// There are no reserved words: a word is only significant where the grammar puts
/// it, which is why these are sets to be consulted at a position rather than a
/// list of keywords the lexer could stamp on sight.
namespace vocabulary {

/// The lower-case form of a uniformly-cased word, or the word unchanged.
///
/// `FOR` and `for` are the keyword; `For` is a name. The rule is the compiler's
/// (`a11.flow.lexer.canonical`), and applying it in one place is what keeps every
/// surface agreeing about it.
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
  /// Another stream to read after this one: `| then other`.
  kStream,
};

/// Every pipeline stage, in the order they read best in a listing.
absl::Span<const std::string_view> Stages();

/// What a stage takes, or `nullopt` if the name is not a stage.
std::optional<StageArgument> StageTakes(std::string_view canonical_name);

/// The spelling of a stage argument kind in the output formats.
std::string_view StageArgumentName(StageArgument argument);

/// What one word of the language does, as reference an editor can show.
///
/// Here rather than in each editor for the reason everything else in this file is
/// here: there are four frontends and one language. A hover, the popup beside a
/// completion list, and `a11-flow vocabulary` are three questions with one answer,
/// and a copy of this in the IntelliJ plugin would be a copy that goes stale the
/// first time a stage learns a new argument.
///
/// Written as reference rather than as a gloss. "stage, takes number" is what the
/// editor used to say about `truncate`, and it answers nothing a reader did not
/// already know from looking at the line.
struct WordDoc {
  /// One line: what it does. A sentence, because it is shown as one.
  std::string_view summary;
  /// What it takes, spelled the way a reader writes it rather than as a type
  /// name: "a count", "an expression, with `it` bound to the value in hand".
  /// Empty where it takes nothing.
  std::string_view takes;
  /// How it behaves, and the caveat if it has one. Markdown, a short paragraph.
  std::string_view detail;
  /// One line of Flow showing it in use. Reference without an example is a
  /// definition, and a definition is what the reader is trying to get past.
  std::string_view example;
};

/// What a stage does, or `nullptr` where the name is not a stage.
///
/// Every stage in [Stages] has an entry, which `FlowVocabulary.EveryWordIs
/// Documented` pins: a stage added to the grammar without reference text is a
/// hole a test finds rather than one a reader finds.
const WordDoc* absl_nullable StageDocumentation(std::string_view canonical_name);

/// What a built-in function does, or `nullptr` where the name is not one.
///
/// A word that is both a stage and a function has an entry in each table, because
/// they do different things: `| text` re-writes every value of a stream and
/// `text(x)` re-writes one value. Which one a position means is the highlighter's
/// judgement ([SemanticKind]), so whatever asks here has already been told.
const WordDoc* absl_nullable BuiltinDocumentation(
    std::string_view canonical_name);

/// The two stages that may be written without their leading `|`.
///
/// `history then asked` and `hits where it.ok` read as words joining two things
/// rather than as a transformation applied to a stream. Both take an operand,
/// which is what tells the stage from a port of the same name.
const absl::flat_hash_set<std::string_view>& BareStages();

/// The stages that read a whole stream and yield exactly one value.
///
/// Their arithmetic is what makes some sequences impossible rather than merely
/// odd: after one of these there is a single value, so `| drop 2` yields nothing
/// and `| count` is 1 however long the stream was.
const absl::flat_hash_set<std::string_view>& ReducingStages();

/// The stages that choose *which* values of a stream to keep, or group them,
/// rather than reshaping each one. These say nothing about a single value.
const absl::flat_hash_set<std::string_view>& PositionalStages();

/// The language's fixed function set. No user code, ever: a flow stays data.
const absl::flat_hash_set<std::string_view>& Builtins();

/// The built-in port type names.
///
/// Not the whole of what may stand where a type does: a port may name a type by
/// the tag a serialisation registry knows it by (`a11.sdk.AudioBuffer`), say what
/// a generic one holds (`list[a11.NodeFragment]`), or quote a mimetype.
const absl::flat_hash_set<std::string_view>& TypeNames();

/// How many type parameters a built-in type may be given.
///
/// Empty means none are allowed. A `list` says what it holds, an `object` what it
/// maps; a tag is already concrete.
absl::Span<const int> TypeParameters(std::string_view canonical_name);

/// The built-in port type names, in the order a listing reads them.
absl::Span<const std::string_view> OrderedTypeNames();

/// The fixed function set, in the order a listing reads it.
absl::Span<const std::string_view> OrderedBuiltins();

/// Words that open a statement, and so are not read as a name there.
const absl::flat_hash_set<std::string_view>& StatementWords();

/// The same, in the order they read: what a statement may begin with, most
/// common first. What something offering them at a caret writes down the list.
absl::Span<const std::string_view> OrderedStatements();

/// Words that stand inside a statement without opening one.
///
/// `else` continues an `if`, and `parallel` and `max` say how wide a loop runs.
/// They are keywords where the grammar puts them and nowhere else, which is why
/// they are not in [StatementWords]: a word there is one that stops being a name
/// at the head of a statement, and none of these ever stands there. They are
/// here so that everything colouring a flow colours them, which is what a list
/// living only in the parser cost.
const absl::flat_hash_set<std::string_view>& ClauseWords();

/// The same, in the order they read.
absl::Span<const std::string_view> OrderedClauseWords();

/// Words that declare something inside a flow.
const absl::flat_hash_set<std::string_view>& DeclarationWords();

/// The declarations in the order they are written in a flow.
absl::Span<const std::string_view> OrderedDeclarations();

/// Words that may follow a call's closing parenthesis.
const absl::flat_hash_set<std::string_view>& ModifierWords();

/// The modifiers in the order they read after a call, `forward headers` being
/// the one that is two words. [ModifierWords] is these split on the space, so a
/// modifier added here is a modifier everywhere.
absl::Span<const std::string_view> OrderedModifiers();

/// Words that open a pipeline source rather than naming one: `status`, `zip`.
const absl::flat_hash_set<std::string_view>& SourceWords();

/// What a port says about itself after its type: `stream`, `required`.
const absl::flat_hash_set<std::string_view>& PortModifierWords();

/// The same, in the order a port writes them.
absl::Span<const std::string_view> OrderedPortModifiers();

/// What a `struct` field says about itself after its type.
///
/// A superset of [PortModifierWords] in spirit rather than in fact -- a field is
/// never a `stream`, and a port never has a `default` -- so the two tables are
/// kept apart and each position consults its own.
const absl::flat_hash_set<std::string_view>& FieldModifierWords();

/// The same, in the order a field writes them, which is the order they are
/// required to be written in.
absl::Span<const std::string_view> OrderedFieldModifiers();

/// Abseil's canonical status codes, lower case, which is what `fail` names.
absl::Span<const std::string_view> StatusCodes();

/// Whether a word names a canonical status code, in either case.
bool IsStatusCode(std::string_view word);

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
