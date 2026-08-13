// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_EMIT_JSON_H_
#define A11_FLOW_EMIT_JSON_H_

#include <string>
#include <string_view>

#include <absl/types/span.h>
#include <nlohmann/json_fwd.hpp>

#include "a11/flow/complete.h"
#include "a11/flow/diagnostic.h"
#include "a11/flow/format.h"
#include "a11/flow/parser.h"
#include "a11/flow/plan.h"
#include "a11/flow/syntax.h"

namespace a11::flow {

/// The `format` field of each envelope: what a reader checks before parsing.
///
/// A version is bumped when a field changes meaning or disappears. Adding a
/// field is not a version change, so a consumer must ignore fields it does not
/// know -- which is the contract these strings stand for.
inline constexpr std::string_view kDiagnosticsFormat = "flow.diagnostics/v1";
inline constexpr std::string_view kCodesFormat = "flow.codes/v1";
inline constexpr std::string_view kSyntaxFormat = "flow.syntax/v1";
inline constexpr std::string_view kTokensFormat = "flow.tokens/v1";
inline constexpr std::string_view kVocabularyFormat = "flow.vocabulary/v1";
inline constexpr std::string_view kFormatFormat = "flow.format/v1";

/// One diagnostic, as it appears in the envelope.
nlohmann::json DiagnosticToJsonValue(const Diagnostic& diagnostic);

/// A diagnostic read back from its JSON, for frontends that consume the
/// envelope rather than the library -- the IntelliJ plugin, a CI script.
///
/// Unknown fields are ignored and missing ones take their defaults, so a newer
/// producer never breaks an older reader.
Diagnostic DiagnosticFromJsonValue(const nlohmann::json& value);

/// The full `flow.diagnostics/v1` envelope.
///
/// `source` is whatever names the input -- a path, `-` for standard input -- and
/// is echoed so a batch of these can be concatenated and still say what each
/// was about. `counts` is there so a gate can be written without walking the
/// list.
nlohmann::json DiagnosticsToJsonValue(
    std::string_view source, absl::Span<const Diagnostic> diagnostics);

/// The envelope, serialised with a trailing newline and two-space indent.
std::string DiagnosticsToJson(std::string_view source,
                              absl::Span<const Diagnostic> diagnostics);

/// The published code table, as `flow.codes/v1`.
nlohmann::json CodesToJsonValue();

/// Every word set the language gives meaning to, as `flow.vocabulary/v1`.
///
/// What anything generating a static grammar file reads instead of keeping a list
/// of its own, and what `a11 flow syntax` holds an editor definition to. One
/// producer, so the table the Python API sees and the table the standalone tool
/// prints cannot differ.
nlohmann::json VocabularyToJsonValue();

/// A SARIF 2.1.0 log for one file's diagnostics.
///
/// SARIF is what code-scanning services and CI annotators already read, so
/// emitting it means a flow's problems show up in a pull request without anybody
/// writing a converter. The rule metadata comes from [KnownCodes], so every rule
/// in the log is documented by construction.
nlohmann::json DiagnosticsToSarifValue(
    std::string_view source, absl::Span<const Diagnostic> diagnostics);

/// The SARIF log, serialised with a trailing newline.
std::string DiagnosticsToSarif(std::string_view source,
                               absl::Span<const Diagnostic> diagnostics);

/// One syntax node, and everything under it.
///
/// The field names are the ones `a11/flow/syntax.py` gives them, so the tree the
/// two implementations produce can be compared field for field while the Python
/// one is still the reference. Every node carries `kind` and its location; the
/// rest is what that kind holds. A duration is `{"$duration": seconds}` rather
/// than a bare number, so a reader can tell one from a count, and the position
/// lives under `at` rather than beside those fields, because a `repeat` has a
/// `start` of its own.
nlohmann::json NodeToJsonValue(const syntax::Node& node);

/// A constant as the syntax format writes it.
nlohmann::json ConstantToJsonValue(const syntax::Constant& constant);

/// The full `flow.syntax/v1` envelope: the flows a file declares, and what is
/// wrong with it. Both, always: a tree with a mistake in it is still a tree, and
/// that is the whole point of the recovering parser.
nlohmann::json SyntaxToJsonValue(std::string_view source,
                                 const ParseResult& result);

/// The envelope, serialised with a trailing newline and two-space indent.
std::string SyntaxToJson(std::string_view source, const ParseResult& result);

/// The `flow.tokens/v1` envelope: every token, and what it means where it is.
///
/// Lexes and classifies in one pass, so the two halves of a token -- what it *is*
/// (`word`, `->`) and what it *means* (`stage`, `type`) -- cannot disagree about
/// its extent. A client colouring a document needs the second; one driving a
/// lexer of its own -- an IDE that insists on tokenising every character -- needs
/// the first, and the offsets tile the source so the gaps between them are
/// whitespace by construction.
nlohmann::json TokensToJsonValue(std::string_view source_name,
                                 std::string_view source);

/// The envelope, serialised with a trailing newline and two-space indent.
std::string TokensToJson(std::string_view source_name, std::string_view source);

/// The `flow.format/v1` envelope: the formatted text, and what it took.
nlohmann::json FormatToJsonValue(const FormatResult& result);

/// The `flow.completions/v1` envelope: what may be written at an offset.
nlohmann::json CompletionsToJsonValue(const CompleteResult& result);

/// The `flow.plan/v1` envelope: what each flow of a file resolved to.
///
/// The keys are `a11.flow.plan`'s `describe()`, so the plan a reader diffs to see
/// whether a change to a flow changed what it does reads the same whichever
/// implementation produced it.
nlohmann::json PlanToJsonValue(std::string_view source_name,
                               const Program& program);

/// The envelope, serialised with a trailing newline and two-space indent.
std::string PlanToJson(std::string_view source_name, const Program& program);

/// One diagnostic on one line, in the shape editors and compilers have used for
/// decades: `path:line:column: severity: message [code]`.
///
/// `source` may be empty, which drops the leading `path:`.
std::string DiagnosticToText(std::string_view source,
                             const Diagnostic& diagnostic);

}  // namespace a11::flow

#endif  // A11_FLOW_EMIT_JSON_H_
