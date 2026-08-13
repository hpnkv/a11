// Copyright 2026 The A11 Authors.

#include "a11/flow/service.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <nlohmann/json.hpp>

#include "a11/flow/complete.h"
#include "a11/flow/diagnostic.h"
#include "a11/flow/emit_json.h"
#include "a11/flow/format.h"
#include "a11/flow/generate.h"
#include "a11/flow/inspect.h"
#include "a11/flow/offsets.h"
#include "a11/flow/parser.h"
#include "a11/flow/resolve.h"

namespace a11::flow {
namespace {

struct Method {
  std::string_view name;
  std::string_view summary;
};

constexpr std::array kMethods = {
    Method{"check", "every problem in a document, as flow.diagnostics/v1"},
    Method{"tokens", "every token and what it means, as flow.tokens/v1"},
    Method{"parse", "the syntax tree, as flow.syntax/v1"},
    Method{"plan", "what each flow resolved to, as flow.plan/v1"},
    Method{"format", "the document formatted, as flow.format/v1"},
    Method{"complete", "what may be written at `offset`, as flow.completions/v1"},
    Method{"codes", "the published diagnostic codes, as flow.codes/v1"},
    Method{"vocabulary", "every word the language knows, as flow.vocabulary/v1"},
    Method{"syntax", "an editor definition, generated for `target`"},
};

nlohmann::json Failure(const nlohmann::json& request, std::string message) {
  nlohmann::json answer{{"ok", false},
                        {"error", nlohmann::json{{"message", std::move(message)}}}};
  if (request.contains("id")) answer["id"] = request.at("id");
  return answer;
}

nlohmann::json Success(const nlohmann::json& request, nlohmann::json result) {
  nlohmann::json answer{{"ok", true}, {"result", std::move(result)}};
  if (request.contains("id")) answer["id"] = request.at("id");
  return answer;
}

/// Everything wrong with one document: both passes, every problem, in order.
std::vector<Diagnostic> Problems(std::string_view source) {
  const ParseResult parsed = Parse(source);
  ResolveResult resolved = Resolve(source, parsed);
  for (Diagnostic& found : Inspect(source, parsed, resolved)) {
    resolved.diagnostics.push_back(std::move(found));
  }
  SortDiagnostics(resolved.diagnostics);
  return std::move(resolved.diagnostics);
}

}  // namespace

absl::Span<const std::string_view> Methods() {
  static const std::vector<std::string_view>* names = [] {
    auto* list = new std::vector<std::string_view>();
    for (const Method& method : kMethods) list->push_back(method.name);
    return list;
  }();
  return absl::MakeConstSpan(names->data(), names->size());
}

std::string_view MethodSummary(std::string_view method) {
  for (const Method& known : kMethods) {
    if (known.name == method) return known.summary;
  }
  return "";
}

nlohmann::json Handle(const nlohmann::json& request) {
  if (!request.is_object()) {
    return Failure(nlohmann::json::object(), "A request is a JSON object.");
  }
  const std::string method = request.value("method", std::string());
  if (method.empty()) {
    return Failure(request,
                   absl::StrCat("A request says which method it wants (",
                                absl::StrJoin(Methods(), ", "), ")."));
  }
  const std::string source = request.value("source", std::string());
  const std::string path = request.value("path", std::string("-"));

  // Which arithmetic this client counts in. Bytes unless it says otherwise, so
  // nothing that worked before changes; a host whose document buffer is UTF-16 --
  // a JVM plugin, a JavaScript extension -- says `utf16` once per request and
  // every offset in the answer is in its own units. Doing it here rather than in
  // each frontend is the same rule as everything else in this service: the
  // language converts, nobody re-derives.
  OffsetBasis basis = OffsetBasis::kBytes;
  const std::string wanted = request.value("offsets", std::string());
  if (!OffsetBasisFromName(wanted, basis)) {
    return Failure(request, absl::StrCat("`offsets` is 'bytes' or 'utf16', not '",
                                         wanted, "'."));
  }
  // Built once per request and only where it is needed: for a byte-counting
  // client this is the whole cost of the feature.
  const TextIndex index =
      basis == OffsetBasis::kUtf16 ? TextIndex(source) : TextIndex();
  const auto answer = [&](nlohmann::json result) {
    if (basis == OffsetBasis::kUtf16) RebaseToUtf16(result, index);
    return Success(request, std::move(result));
  };

  if (method == "check") {
    return answer(DiagnosticsToJsonValue(path, Problems(source)));
  }
  if (method == "tokens") {
    return answer(TokensToJsonValue(path, source));
  }
  if (method == "parse") {
    return answer(SyntaxToJsonValue(path, Parse(source)));
  }
  if (method == "plan") {
    const ParseResult parsed = Parse(source);
    const ResolveResult resolved = Resolve(source, parsed);
    nlohmann::json result = PlanToJsonValue(path, resolved.program);
    // A plan of a document with an error in it is a partial plan, and a reader
    // that showed it as the whole truth would be lying: the diagnostics travel
    // with it.
    nlohmann::json diagnostics = nlohmann::json::array();
    for (const Diagnostic& diagnostic : resolved.diagnostics) {
      diagnostics.push_back(DiagnosticToJsonValue(diagnostic));
    }
    result["diagnostics"] = diagnostics;
    return answer(std::move(result));
  }
  if (method == "format") {
    return answer(FormatToJsonValue(Format(source)));
  }
  if (method == "complete") {
    if (!request.contains("offset")) {
      return Failure(request, "`complete` takes the `offset` to complete at.");
    }
    const nlohmann::json& offset = request.at("offset");
    if (!offset.is_number_unsigned() && !offset.is_number_integer()) {
      return Failure(request,
                     absl::StrCat("`offset` is a number of ",
                                  OffsetBasisName(basis),
                                  " into the source."));
    }
    const long long at = offset.get<long long>();
    if (at < 0) return Failure(request, "`offset` cannot be negative.");
    // The one offset that travels *in*, so the one that converts the other way:
    // a caret an editor reports in its own units is a byte offset here before
    // anything reads the character in front of it.
    const size_t reached = basis == OffsetBasis::kUtf16
                               ? index.ByteOf(static_cast<size_t>(at))
                               : static_cast<size_t>(at);
    return answer(CompletionsToJsonValue(CompleteAt(source, reached)));
  }
  if (method == "codes") return Success(request, CodesToJsonValue());
  if (method == "vocabulary") return Success(request, VocabularyToJsonValue());
  if (method == "syntax") {
    const std::string name = request.value("target", std::string("sublime"));
    SyntaxTarget target = SyntaxTarget::kSublime;
    if (!SyntaxTargetFromName(name, target)) {
      return Failure(request, absl::StrCat("No editor definition is generated "
                                           "for ", name, "."));
    }
    return Success(request, nlohmann::json{
                                {"target", SyntaxTargetName(target)},
                                {"path", SyntaxTargetPath(target)},
                                {"text", GenerateSyntax(target)},
                            });
  }
  return Failure(request,
                 absl::StrCat(method, " is not a method this speaks (",
                              absl::StrJoin(Methods(), ", "), ")."));
}

}  // namespace a11::flow
