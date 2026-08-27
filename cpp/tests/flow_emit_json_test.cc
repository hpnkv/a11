// Copyright 2026 The A11 Authors.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a11/flow/diagnostic.h"
#include "a11/flow/emit_json.h"

namespace a11::flow {
namespace {

Diagnostic Example() {
  Diagnostic diagnostic;
  diagnostic.code = "flow.sequence.redundant-stage";
  diagnostic.severity = Severity::kWeakWarning;
  diagnostic.family = Family::kSequence;
  diagnostic.message =
      "'| collect' leaves exactly one value, so '| last' has nothing to choose "
      "between and does nothing.";
  diagnostic.flow = "research";
  diagnostic.range.start = Position{40, 3, 12};
  diagnostic.range.end = Position{46, 3, 18};
  Fix fix;
  fix.label = "Remove '| last'";
  fix.edits.push_back(Edit{37, 46, ""});
  diagnostic.fixes.push_back(fix);
  return diagnostic;
}

TEST(FlowDiagnosticsJson, CarriesEverythingAFrontendNeeds) {
  const std::vector<Diagnostic> diagnostics = {Example()};
  const nlohmann::json value =
      DiagnosticsToJsonValue("research.flow", diagnostics);

  EXPECT_EQ(value["format"], "flow.diagnostics/v1");
  EXPECT_EQ(value["source"], "research.flow");
  ASSERT_EQ(value["diagnostics"].size(), 1u);

  const nlohmann::json& first = value["diagnostics"][0];
  EXPECT_EQ(first["code"], "flow.sequence.redundant-stage");
  EXPECT_EQ(first["severity"], "weak-warning");
  EXPECT_EQ(first["family"], "sequence");
  EXPECT_EQ(first["flow"], "research");
  // Offsets for an editor, lines and columns for a person: both, always, so a
  // diagnostic that has travelled as JSON needs nothing else to be placed.
  EXPECT_EQ(first["range"]["start"]["offset"], 40);
  EXPECT_EQ(first["range"]["start"]["line"], 3);
  EXPECT_EQ(first["range"]["start"]["column"], 12);
  EXPECT_EQ(first["range"]["end"]["offset"], 46);
  ASSERT_EQ(first["fixes"].size(), 1u);
  EXPECT_EQ(first["fixes"][0]["label"], "Remove '| last'");
  EXPECT_EQ(first["fixes"][0]["edits"][0]["start"], 37);
  EXPECT_EQ(first["fixes"][0]["edits"][0]["end"], 46);
  EXPECT_EQ(first["fixes"][0]["edits"][0]["text"], "");
}

TEST(FlowDiagnosticsJson, CountsBySeverityForAGate) {
  std::vector<Diagnostic> diagnostics;
  Diagnostic error = Example();
  error.severity = Severity::kError;
  Diagnostic warning = Example();
  warning.severity = Severity::kWarning;
  diagnostics = {error, warning, Example(), Example()};

  const nlohmann::json counts =
      DiagnosticsToJsonValue("x.flow", diagnostics)["counts"];
  EXPECT_EQ(counts["error"], 1);
  EXPECT_EQ(counts["warning"], 1);
  EXPECT_EQ(counts["weak-warning"], 2);
  EXPECT_EQ(counts["information"], 0);
}

TEST(FlowDiagnosticsJson, OmitsAnUnknownFlowRatherThanSayingItIsEmpty) {
  Diagnostic diagnostic = Example();
  diagnostic.flow.clear();
  const nlohmann::json value = DiagnosticToJsonValue(diagnostic);
  EXPECT_EQ(value.find("flow"), value.end());
}

TEST(FlowDiagnosticsJson, RoundTripsThroughItsOwnReader) {
  // The plugin and any CI script read the envelope back with this, so the two
  // halves have to agree exactly.
  const Diagnostic original = Example();
  const Diagnostic read =
      DiagnosticFromJsonValue(DiagnosticToJsonValue(original));

  EXPECT_EQ(read.code, original.code);
  EXPECT_EQ(read.severity, original.severity);
  EXPECT_EQ(read.family, original.family);
  EXPECT_EQ(read.message, original.message);
  EXPECT_EQ(read.flow, original.flow);
  EXPECT_EQ(read.range.start.offset, original.range.start.offset);
  EXPECT_EQ(read.range.start.line, original.range.start.line);
  EXPECT_EQ(read.range.start.column, original.range.start.column);
  EXPECT_EQ(read.range.end.offset, original.range.end.offset);
  ASSERT_EQ(read.fixes.size(), 1u);
  EXPECT_EQ(read.fixes[0].label, original.fixes[0].label);
  ASSERT_EQ(read.fixes[0].edits.size(), 1u);
  EXPECT_EQ(read.fixes[0].edits[0].start, 37u);
  EXPECT_EQ(read.fixes[0].edits[0].end, 46u);
  EXPECT_EQ(read.fixes[0].edits[0].text, "");
}

TEST(FlowDiagnosticsJson, ReaderToleratesRubbish) {
  // A reader of a wire format must not throw on one: an older plugin reading a
  // newer tool, or a hand-written file, gets defaults.
  const Diagnostic empty = DiagnosticFromJsonValue(nlohmann::json::object());
  EXPECT_EQ(empty.severity, Severity::kError);
  EXPECT_EQ(empty.range.start.line, 1);
  EXPECT_TRUE(empty.fixes.empty());

  const Diagnostic wrongTypes = DiagnosticFromJsonValue(
      nlohmann::json{{"range", 7}, {"fixes", "no"}, {"code", "flow.x.y"}});
  EXPECT_EQ(wrongTypes.code, "flow.x.y");
  EXPECT_TRUE(wrongTypes.fixes.empty());
}

TEST(FlowDiagnosticsJson, SerialisesWithATrailingNewline) {
  const std::vector<Diagnostic> diagnostics = {Example()};
  const std::string text = DiagnosticsToJson("x.flow", diagnostics);
  EXPECT_TRUE(text.ends_with("}\n"));
  nlohmann::json parsed;
  EXPECT_NO_THROW(parsed = nlohmann::json::parse(text));
  EXPECT_TRUE(parsed.is_object());
}

TEST(FlowCodesJson, PublishesEveryCodeWithItsMeaning) {
  const nlohmann::json value = CodesToJsonValue();
  EXPECT_EQ(value["format"], "flow.codes/v1");
  EXPECT_EQ(value["codes"].size(), KnownCodes().size());
  for (const nlohmann::json& entry : value["codes"]) {
    EXPECT_FALSE(entry["code"].get<std::string>().empty());
    EXPECT_FALSE(entry["summary"].get<std::string>().empty());
    EXPECT_FALSE(entry["family"].get<std::string>().empty());
    EXPECT_FALSE(entry["severity"].get<std::string>().empty());
  }
}

// <repo>/cpp/tests/flow_emit_json_test.cc -> <repo>/testdata/flow/codes.json
std::filesystem::path GoldenCodesPath() {
  return (std::filesystem::path(A11_CPP_SOURCE_ROOT).parent_path() /
          std::filesystem::path(__FILE__))
             .parent_path()
             .parent_path()
             .parent_path() /
         "testdata" / "flow" / "codes.json";
}

TEST(FlowCodesJson, MatchesTheTableEveryLanguageReads) {
  // `testdata/flow/codes.json` is the published code table, in the same spirit
  // as `testdata/serial_tags.json`: the C++ owns it, and the Python CLI and any
  // editor read it back rather than keeping a second list. Regenerate with:
  //
  //   A11_UPDATE_GOLDENS=1 build/ctests/cpp/tests/a11_flow_test
  //       --gtest_filter=FlowCodesJson.MatchesTheTableEveryLanguageReads
  const std::filesystem::path path = GoldenCodesPath();
  const std::string generated = CodesToJsonValue().dump(2) + "\n";

  if (std::getenv("A11_UPDATE_GOLDENS") != nullptr) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << "cannot write " << path;
    out << generated;
    out.close();
    GTEST_SKIP() << "rewrote " << path;
  }

  std::ifstream stream(path);
  ASSERT_TRUE(stream.is_open()) << "cannot open " << path;
  std::stringstream buffer;
  buffer << stream.rdbuf();
  EXPECT_EQ(buffer.str(), generated)
      << "the code table changed; regenerate testdata/flow/codes.json";
}

TEST(FlowSarif, IsAValidLogWithDocumentedRules) {
  const std::vector<Diagnostic> diagnostics = {Example()};
  const nlohmann::json log =
      DiagnosticsToSarifValue("research.flow", diagnostics);

  EXPECT_EQ(log["version"], "2.1.0");
  ASSERT_EQ(log["runs"].size(), 1u);
  const nlohmann::json& run = log["runs"][0];
  EXPECT_EQ(run["tool"]["driver"]["name"], "a11 flow");
  // Every rule the log can reference is described in it, because the rule list
  // comes from the same table the diagnostics do.
  EXPECT_EQ(run["tool"]["driver"]["rules"].size(), KnownCodes().size());
  ASSERT_EQ(run["results"].size(), 1u);
  const nlohmann::json& result = run["results"][0];
  EXPECT_EQ(result["ruleId"], "flow.sequence.redundant-stage");
  // A weak warning is SARIF's `note`: the format has three levels, and this is
  // the one that does not stop a build.
  EXPECT_EQ(result["level"], "note");
  const nlohmann::json& region =
      result["locations"][0]["physicalLocation"]["region"];
  EXPECT_EQ(region["startLine"], 3);
  EXPECT_EQ(region["startColumn"], 12);
  EXPECT_EQ(region["charOffset"], 40);
  EXPECT_EQ(region["charLength"], 6);
  EXPECT_EQ(
      result["locations"][0]["physicalLocation"]["artifactLocation"]["uri"],
      "research.flow");
}

TEST(FlowDiagnosticText, ReadsLikeEveryOtherCompiler) {
  const Diagnostic diagnostic = Example();
  EXPECT_EQ(DiagnosticToText("research.flow", diagnostic),
            "research.flow:3:12: weak-warning: " + diagnostic.message +
                " [flow.sequence.redundant-stage]");
  // Standard input has no path to print.
  EXPECT_EQ(DiagnosticToText("", diagnostic),
            "3:12: weak-warning: " + diagnostic.message +
                " [flow.sequence.redundant-stage]");
}

}  // namespace
}  // namespace a11::flow
