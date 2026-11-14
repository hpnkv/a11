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

/// Envelope format identifiers. Readers ignore unknown fields in a version.
inline constexpr std::string_view kDiagnosticsFormat = "flow.diagnostics/v1";
inline constexpr std::string_view kCodesFormat = "flow.codes/v1";
inline constexpr std::string_view kSyntaxFormat = "flow.syntax/v1";
inline constexpr std::string_view kTokensFormat = "flow.tokens/v1";
inline constexpr std::string_view kVocabularyFormat = "flow.vocabulary/v1";
inline constexpr std::string_view kFormatFormat = "flow.format/v1";

/// One diagnostic, as it appears in the envelope.
nlohmann::json DiagnosticToJsonValue(const Diagnostic& diagnostic);

/// Reads a diagnostic, ignoring unknown fields and defaulting missing ones.
Diagnostic DiagnosticFromJsonValue(const nlohmann::json& value);

/// The `flow.diagnostics/v1` envelope, with source and severity counts.
nlohmann::json DiagnosticsToJsonValue(std::string_view source,
                                      absl::Span<const Diagnostic> diagnostics);

/// The envelope, serialised with a trailing newline and two-space indent.
std::string DiagnosticsToJson(std::string_view source,
                              absl::Span<const Diagnostic> diagnostics);

/// The published code table, as `flow.codes/v1`.
nlohmann::json CodesToJsonValue();

/// The language word sets as `flow.vocabulary/v1`.
nlohmann::json VocabularyToJsonValue();

/// A SARIF 2.1.0 log for one file, with rule metadata from [KnownCodes].
nlohmann::json DiagnosticsToSarifValue(
    std::string_view source, absl::Span<const Diagnostic> diagnostics);

/// The SARIF log, serialised with a trailing newline.
std::string DiagnosticsToSarif(std::string_view source,
                               absl::Span<const Diagnostic> diagnostics);

/// A syntax node and its subtree. Every node carries `kind` and `at`.
/// Durations use `{"$duration": seconds}`.
nlohmann::json NodeToJsonValue(const syntax::Node& node);

/// A constant as the syntax format writes it.
nlohmann::json ConstantToJsonValue(const syntax::Constant& constant);

/// The `flow.syntax/v1` envelope, including partial trees and diagnostics.
nlohmann::json SyntaxToJsonValue(std::string_view source,
                                 const ParseResult& result);

/// The envelope, serialised with a trailing newline and two-space indent.
std::string SyntaxToJson(std::string_view source, const ParseResult& result);

/// The `flow.tokens/v1` envelope, with token kinds and semantic classes.
/// Token offsets cover the source; unclassified spans are whitespace.
nlohmann::json TokensToJsonValue(std::string_view source_name,
                                 std::string_view source);

/// The envelope, serialised with a trailing newline and two-space indent.
std::string TokensToJson(std::string_view source_name, std::string_view source);

/// The `flow.format/v1` envelope: the formatted text, and what it took.
nlohmann::json FormatToJsonValue(const FormatResult& result);

/// The `flow.completions/v1` envelope: what may be written at an offset.
nlohmann::json CompletionsToJsonValue(const CompleteResult& result);

/// The `flow.plan/v1` envelope for resolved flows.
nlohmann::json PlanToJsonValue(std::string_view source_name,
                               const Program& program);

/// A resolved shape. With `program`, referenced shapes appear under `nested`.
/// A self-reference appears once, in `struct`.
nlohmann::json DtoToJsonValue(const DtoPlan& dto,
                              const Program* absl_nullable program = nullptr);

/// The envelope, serialised with a trailing newline and two-space indent.
std::string PlanToJson(std::string_view source_name, const Program& program);

/// `path:line:column: severity: message [code]` on one line.
/// An empty `source` omits the path prefix.
std::string DiagnosticToText(std::string_view source,
                             const Diagnostic& diagnostic);

}  // namespace a11::flow

#endif  // A11_FLOW_EMIT_JSON_H_
