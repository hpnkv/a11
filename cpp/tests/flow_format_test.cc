// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a11/flow/emit_json.h"
#include "a11/flow/format.h"
#include "a11/flow/lexer.h"
#include "a11/flow/parser.h"
#include "a11/flow/token.h"
#include "a11/utf8.h"

namespace a11::flow {
namespace {

std::string Formatted(std::string_view source) {
  const FormatResult result = Format(source);
  std::vector<std::string> messages;
  messages.reserve(result.diagnostics.size());
  for (const Diagnostic& diagnostic : result.diagnostics) {
    messages.push_back(diagnostic.message);
  }
  EXPECT_TRUE(messages.empty()) << absl::StrJoin(messages, "; ");
  return result.formatted;
}

/// Every token as `kind:text`, minus the line breaks.
///
/// Corpus checks omit strings and grouping parentheses, which string wrapping
/// changes. Tree checks compare the resulting values and expression structure.
/// Comment comparisons omit trailing whitespace, which the formatter trims.
std::vector<std::string> Stream(std::string_view source,
                                bool include_strings_and_groups = true) {
  std::vector<std::string> out;
  for (const Token& token : Lex(source).tokens) {
    if (token.kind == TokenKind::kEnd) {
      break;
    }
    if (token.kind == TokenKind::kNewline ||
        token.kind == TokenKind::kSemicolon) {
      continue;
    }
    if (!include_strings_and_groups && (token.kind == TokenKind::kString ||
                                        token.kind == TokenKind::kLeftParen ||
                                        token.kind == TokenKind::kRightParen)) {
      continue;
    }
    std::string text(token.text);
    if (token.kind == TokenKind::kComment) {
      while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
      }
    }
    out.push_back(absl::StrCat(KindName(token.kind), ":", text));
  }
  return out;
}

// The syntax tree, with every position dropped. This is the invariant a person
// actually cares about: the program after formatting is the same program.
/// The syntax tree, with every position dropped.
///
/// Formatting moves everything, so the offsets differ by construction; what
/// has to be identical is the tree those positions hang off. This is the
/// invariant a person actually cares about: the program after formatting is
/// the same program.
nlohmann::json Tree(std::string_view source) {
  nlohmann::json value = SyntaxToJsonValue("-", Parse(source));
  value.erase("source");
  value.erase("diagnostics");
  const auto strip = [](nlohmann::json& node, const auto& self) -> void {
    if (node.is_object()) {
      node.erase("at");
      for (auto& [key, child] : node.items()) {
        self(child, self);
      }
    } else if (node.is_array()) {
      for (auto& child : node) {
        self(child, self);
      }
    }
  };
  strip(value, strip);
  return value;
}

/// The corpus: every flow this repository ships.
std::vector<std::filesystem::path> Corpus() {
  const std::filesystem::path root =
      (std::filesystem::path(A11_CPP_SOURCE_ROOT).parent_path() /
       std::filesystem::path(__FILE__))
          .parent_path()
          .parent_path()
          .parent_path();
  std::vector<std::filesystem::path> paths;
  for (const std::filesystem::path& directory :
       {root / "examples" / "003-flow-dsl",
        root / "examples" / "004-deep-research", root / "scripts",
        root / "testdata" / "flow"}) {
    if (!std::filesystem::is_directory(directory)) {
      continue;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.path().extension() == ".flow") {
        paths.push_back(entry.path());
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path);
  std::stringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

TEST(FlowFormat, DynamicParallelCountsPreserveSyntaxAndAreIdempotent) {
  const std::string source = R"(
flow main {
in workers:int
in rows:int stream
out values:int stream
for n in [1,2] parallel(workers+1){ n->values }
rows | map it+1 parallel workers unordered ->values
}
)";
  const auto formatted = Formatted(source);
  EXPECT_EQ(Stream(source), Stream(formatted));
  EXPECT_EQ(Tree(source), Tree(formatted));
  EXPECT_EQ(Formatted(formatted), formatted);
}

TEST(FlowFormat, PortGroupsKeepDescriptionsAndLeadingCommentsTogether) {
  const std::string source =
      "flow main {\n"
      "in value: string\n"
      "\"Input description.\"\n"
      "# Output description.\n"
      "out result: string\n"
      "}\n";
  const auto formatted = Formatted(source);
  EXPECT_EQ(formatted,
            "flow main {\n"
            "  in  value: string \"Input description.\"\n"
            "\n"
            "  # Output description.\n"
            "  out result: string\n"
            "}\n");
  EXPECT_EQ(Tree(source), Tree(formatted));
  EXPECT_EQ(Formatted(formatted), formatted);
}

TEST(FlowFormat, PortGroupsAreSeparatedWithoutColumnAlignment) {
  const std::string source =
      "flow main {\nIN value:string\nOUT result:string\n}\n";
  FormatOptions options;
  options.align_ports = false;
  const auto formatted = Format(source, options);
  ASSERT_TRUE(formatted.diagnostics.empty());
  EXPECT_EQ(formatted.formatted,
            "flow main {\n  IN value: string\n\n  OUT result: string\n}\n");
  EXPECT_EQ(Tree(source), Tree(formatted.formatted));
  EXPECT_EQ(Format(formatted.formatted, options).formatted,
            formatted.formatted);
}

TEST(FlowFormat, OneDirectionOfPortsNeedsNoEmptyGroupSeparator) {
  for (const auto* direction : {"in", "out"}) {
    const auto source = absl::StrCat("flow main {\n", direction, " a:string\n",
                                     direction, " b:string\n}\n");
    const auto formatted = Formatted(source);
    EXPECT_EQ(formatted.find("\n\n"), std::string::npos);
    EXPECT_EQ(Tree(source), Tree(formatted));
  }
}

TEST(FlowFormat, PlainPipelineBindingsPreserveTheirCompletionGroup) {
  const std::string source = R"(flow main{in values:int stream
out kept:int stream
forwarded=values
|take until it==3
->kept,_
after forwarded{log "done"}
})";
  const auto result = Formatted(source);
  EXPECT_EQ(Tree(result), Tree(source));
  EXPECT_EQ(Stream(result), Stream(source));
  EXPECT_EQ(Formatted(result), result);
  EXPECT_NE(result.find("forwarded = values"), std::string::npos);
}

TEST(FlowFormat, PredicatePrefixesAndAfterBlocksPreserveTheirSyntax) {
  const std::string source =
      R"(flow main{done={ [1,2,3]|flatten|take until it==3->_ }
cleanup=after done{log "done"
 after done{log "nested"}}
})";
  const auto result = Formatted(source);
  EXPECT_EQ(Stream(result), Stream(source));
  EXPECT_FALSE(Parse(result).HasErrors()) << result;
  EXPECT_EQ(Formatted(result), result);
  EXPECT_NE(result.find("take until"), std::string::npos);
  EXPECT_NE(result.find("after done {"), std::string::npos);
}

TEST(FlowFormat, AfterBlocksAreDistinctFromContinuedCallModifiers) {
  const std::string source = R"(flow main {
 first=run work()
 second=run work()
 after first,

 second
 {
  log "finished"
 }
 third=run work()
   after first
 after third { log "last" }
})";
  const auto result = Formatted(source);
  EXPECT_FALSE(Parse(result).HasErrors()) << result;
  EXPECT_EQ(Tree(result), Tree(source));
  EXPECT_EQ(Stream(result), Stream(source));
  EXPECT_EQ(Formatted(result), result);
}

TEST(FlowFormat, IndentsBlocksAndSpacesTokensTheOneWay) {
  const std::string source =
      "flow t{\n"
      "in a:string required\n"
      "out b : string\n"
      "x=run act( p : a,q:\"s\" )   timeout 250ms\n"
      "a|truncate 200|where it!=\"\"->b\n"
      "if(a|count)>0{\n"
      "fail not_found \"gone\"\n"
      "}\n"
      "}\n";
  EXPECT_EQ(Formatted(source),
            "flow t {\n"
            "  in  a: string required\n"
            "\n"
            "  out b: string\n"
            "\n"
            "  x = run act(p: a, q: \"s\") timeout 250ms\n"
            "  a | truncate 200 | where it != \"\" -> b\n"
            "  if (a | count) > 0 {\n"
            "    fail not_found \"gone\"\n"
            "  }\n"
            "}\n");
}

TEST(FlowFormat, LinesUpARunOfPortDeclarations) {
  const std::string source =
      "flow t {\n"
      "  in question: string required \"What to find out.\"\n"
      "  out answer: string \"The answer.\"\n"
      "  out sources: string stream \"Where it came from.\"\n"
      "  out read: number\n"
      "\n"
      "  in  x: string\n"
      "  out yy: string\n"
      "}\n";
  EXPECT_EQ(Formatted(source),
            "flow t {\n"
            "  in  question: string required \"What to find out.\"\n"
            "\n"
            "  out answer:  string        \"The answer.\"\n"
            "  out sources: string stream \"Where it came from.\"\n"
            "  out read:    number\n"
            "\n"
            "  in  x: string\n"
            "\n"
            "  out yy: string\n"
            "}\n");
}

TEST(FlowFormat, CompactsShortPipelinesAndCallModifiers) {
  const std::string source =
      "flow t {\n"
      "  in  a: string\n"
      "  out b: string\n"
      "pages\n"
      "| collect\n"
      "| join \", \"\n"
      "-> b\n"
      "  x = run act(p: a)\n"
      "with \"h\": 1\n"
      "forward headers \"authorization\"\n"
      "}\n";
  EXPECT_EQ(
      Formatted(source),
      "flow t {\n"
      "  in  a: string\n"
      "\n"
      "  out b: string\n"
      "\n"
      "  pages | collect | join \", \" -> b\n"
      "  x = run act(p: a) with \"h\": 1 forward headers \"authorization\"\n"
      "}\n");
}

TEST(FlowFormat, ABracketedGroupIndentsFromTheLineThatOpenedIt) {
  const std::string source =
      "flow t {\n  in  a: string\n  llm = call interact(\ninteractions: a | "
      "map a11::sdk::Interaction{\nrole: \"user\",\ncontent: "
      "[std::to_chunk(it)]\n},\nconfig: {}\n)\n}\n";
  EXPECT_EQ(
      Formatted(source),
      "flow t {\n  in  a: string\n\n  llm = call interact(\n    interactions: "
      "a "
      "| map a11::sdk::Interaction{\n      role: \"user\", content: "
      "[std::to_chunk(it)]\n    },\n    config: {}\n  )\n}\n");
}

TEST(FlowFormat, CompactsShortFoldArguments) {
  const std::string source =
      "flow t {\n"
      "  in  n: number stream\n"
      "  out o: number\n"
      "n | fold 0 as total,\n"
      "total + it\n"
      "-> o\n"
      "}\n";
  EXPECT_EQ(Formatted(source),
            "flow t {\n"
            "  in  n: number stream\n"
            "\n"
            "  out o: number\n"
            "\n"
            "  n | fold 0 as total, total + it -> o\n"
            "}\n");
}

TEST(FlowFormat, ShortDescriptionsUseTheDeclarationLine) {
  const std::string source =
      "flow t {\n"
      "describe\n"
      "\"What this is for.\"\n"
      "  in  question: string required\n"
      "\"What to find out, at length.\"\n"
      "  out answer: string\n"
      "        \"The answer.\"\n"
      "}\n";
  EXPECT_EQ(Formatted(source),
            "flow t {\n"
            "  describe \"What this is for.\"\n"
            "  in  question: string required \"What to find out, at length.\"\n"
            "\n"
            "  out answer: string \"The answer.\"\n"
            "}\n");
}

TEST(FlowFormat, ATripleQuotedStringIsLeftExactlyAsItWasWritten) {
  // Its interior is content, not code: the formatter indents the line the
  // string starts on and does not touch a byte inside it, because every one of
  // those bytes is in the value.
  const std::string source =
      "flow t {\n"
      "describe \"\"\"\n"
      "    What this is for.\n"
      "      Indented further.\n"
      "    \"\"\"\n"
      "  in a: string\n"
      "}\n";
  EXPECT_EQ(Formatted(source),
            "flow t {\n"
            "  describe \"\"\"\n"
            "    What this is for.\n"
            "      Indented further.\n"
            "    \"\"\"\n"
            "  in  a: string\n"
            "}\n");
}

TEST(FlowFormat, CommentsStayWhereTheyWere) {
  const std::string source =
      "# What this file is.\n"
      "flow t {\n"
      "  # Why this port.\n"
      "  in  a: string\n"
      "  a -> b   # and why this line\n"
      "}\n";
  // Two spaces before a comment that shares its line, whatever was there.
  EXPECT_EQ(Formatted(source),
            "# What this file is.\n"
            "flow t {\n"
            "  # Why this port.\n"
            "  in  a: string\n"
            "\n"
            "  a -> b  # and why this line\n"
            "}\n");
}

TEST(FlowFormat, CollapsesBlankLinesWithoutLosingTheOneThatSeparates) {
  const std::string source =
      "flow t {\n"
      "\n"
      "  in  a: string\n"
      "\n"
      "\n"
      "\n"
      "  a -> b\n"
      "\n"
      "}\n"
      "\n"
      "\n"
      "flow u { in a: string }\n";
  EXPECT_EQ(Formatted(source),
            "flow t {\n"
            "  in  a: string\n"
            "\n"
            "  a -> b\n"
            "}\n"
            "\n"
            "flow u {\n"
            "  in  a: string\n"
            "}\n");
}

TEST(FlowFormat, AnElseKeepsItsPlaceOnTheClosingLine) {
  const std::string source =
      "flow t {\n"
      "  in  a: string\n"
      "  if a { skip 1 a } else if a { skip 1 a } else { skip 1 a }\n"
      "}\n";
  EXPECT_EQ(Formatted(source),
            "flow t {\n"
            "  in  a: string\n"
            "\n"
            "  if a {\n"
            "    skip 1 a\n"
            "  } else if a {\n"
            "    skip 1 a\n"
            "  } else {\n"
            "    skip 1 a\n"
            "  }\n"
            "}\n");
}

TEST(FlowFormat, LeavesAFileItCannotReadExactlyAsItIs) {
  const std::string source = "flow t {\n  in a: string\n  a | wat -> b\n}\n";
  const FormatResult result = Format(source);
  EXPECT_EQ(result.formatted, source);
  EXPECT_FALSE(result.changed);
  EXPECT_TRUE(result.edits.empty());
  ASSERT_FALSE(result.diagnostics.empty());
  EXPECT_EQ(result.diagnostics[0].code, "flow.form.unknown-stage");
}

TEST(FlowFormat, TheEditIsTrimmedToWhatActuallyDiffers) {
  const std::string source = "flow t {\n  in  a: string\n  a  ->  b\n}\n";
  const FormatResult result = Format(source);
  ASSERT_TRUE(result.changed);
  ASSERT_EQ(result.edits.size(), 1u);
  const Edit& edit = result.edits.front();
  // Only the middle of the third line moved, and the edit says so rather than
  // replacing the file: an editor applying this keeps the cursor and the folds.
  EXPECT_GT(edit.start, 20u);
  EXPECT_LT(edit.end, source.size());
  std::string applied = source;
  applied.replace(edit.start, edit.end - edit.start, edit.text);
  EXPECT_EQ(applied, result.formatted);
}

TEST(FlowFormat, IsIdempotentAndMeaningPreservingOverTheCorpus) {
  // The two invariants, over every flow this repository ships. They are what
  // let `fmt -i` be run without reading the diff: whatever the style decides,
  // the program is the same program, and running it again changes nothing.
  const std::vector<std::filesystem::path> corpus = Corpus();
  ASSERT_FALSE(corpus.empty());
  for (const std::filesystem::path& path : corpus) {
    const std::string source = ReadFile(path);
    const FormatResult once = Format(source);
    ASSERT_TRUE(once.diagnostics.empty()) << path;
    // Compared one token at a time so a failure says *which* token moved rather
    // than printing two thousand of them.
    const std::vector<std::string> before = Stream(source, false);
    const std::vector<std::string> after = Stream(once.formatted, false);
    for (size_t index = 0; index < std::min(before.size(), after.size());
         ++index) {
      ASSERT_EQ(before[index], after[index]) << path << " at token " << index;
    }
    ASSERT_EQ(before.size(), after.size()) << path;
    EXPECT_EQ(Tree(once.formatted), Tree(source)) << path;
    const FormatResult twice = Format(once.formatted);
    EXPECT_EQ(twice.formatted, once.formatted) << path;
    EXPECT_FALSE(twice.changed) << path;
  }
}

TEST(FlowFormat, EveryFlowThisRepositoryShipsIsFormatted) {
  // The corpus is the style's own documentation, so it is held to it. A diff
  // here means either the formatter or the file needs a change, and the diff
  // says which.
  for (const std::filesystem::path& path : Corpus()) {
    const std::string source = ReadFile(path);
    EXPECT_EQ(Format(source).formatted, source) << path;
  }
}

TEST(FlowFormat, CorpusWhitespaceVariantsHaveOneLayout) {
  for (const auto& path : Corpus()) {
    const std::string source = ReadFile(path);
    const auto expected = Formatted(source);
    std::string variant;
    size_t previous = 0;
    for (const auto& token : Lex(source).tokens) {
      if (token.kind == TokenKind::kNewline || token.kind == TokenKind::kEnd) {
        continue;
      }
      const auto gap =
          std::string_view(source).substr(previous, token.start - previous);
      if (gap.find('\n') != std::string_view::npos) {
        variant.append(std::count(gap.begin(), gap.end(), '\n') > 1
                           ? "\n\n\n\t  "
                           : "\n\t  ");
      } else if (!gap.empty()) {
        variant.append("\t  ");
      }
      variant.append(token.text);
      previous = token.end;
    }
    ASSERT_FALSE(Parse(variant).HasErrors()) << path;
    EXPECT_EQ(Tree(source), Tree(variant)) << path;
    EXPECT_EQ(Formatted(variant), expected) << path;
  }
}

TEST(FlowFormat, AFileThatIsAlreadyFormattedIsNotChanged) {
  const std::string source =
      "flow t {\n"
      "  in  a: string\n"
      "\n"
      "  out b: string\n"
      "\n"
      "  a | count -> b\n"
      "}\n";
  const FormatResult result = Format(source);
  EXPECT_FALSE(result.changed);
  EXPECT_EQ(result.formatted, source);
  EXPECT_TRUE(result.edits.empty());
}

TEST(FlowFormat, EndsTheFileWithOneNewlineAndNoTrailingSpace) {
  EXPECT_EQ(Formatted("flow t { in a: string }   "),
            "flow t {\n  in  a: string\n}\n");
  EXPECT_EQ(Formatted("\n\n\nflow t { in a: string }\n\n\n"),
            "flow t {\n  in  a: string\n}\n");
}

TEST(FlowFormat, OperationsDeclarationsAndModulesPreserveTokensAndGrouping) {
  const std::string source = R"(import "library" as lib
declare flow render exported { in data: object
out text: string stream }
flow f exported {
  out result: string stream
  outcome=take (wait try run check())
  finished=take (close node("existing"))
  (run lib::render(data:{"ok":outcome.ok}) via scratch).text|consume->result
}
)";
  const auto formatted = Format(source);
  ASSERT_EQ(FirstError(formatted.diagnostics), nullptr);
  EXPECT_EQ(Stream(source), Stream(formatted.formatted));
  EXPECT_EQ(Tree(source), Tree(formatted.formatted));
  EXPECT_FALSE(Format(formatted.formatted).changed);
  EXPECT_NE(formatted.formatted.find("wait try run check()"),
            std::string::npos);
}

TEST(FlowFormat, ContextPreservesPredicateAndCountExpressions) {
  const std::string source = R"(flow f{
in rows:string stream
out excerpts:list[string] stream
rows|context std::contains(it,"error") before(1+2) after 0 numbered base(1 + 0)->excerpts
}
)";
  const auto formatted = Format(source);
  ASSERT_EQ(FirstError(formatted.diagnostics), nullptr);
  EXPECT_EQ(Stream(source), Stream(formatted.formatted));
  EXPECT_EQ(Tree(source), Tree(formatted.formatted));
  EXPECT_FALSE(Format(formatted.formatted).changed);
}

TEST(FlowFormat, WrapsExpressionsAndContextWithinTheRequestedWidth) {
  const std::string source = R"(
flow excerpts {
  in rows: string required stream

  out groups: list[string] stream

  rows | context std::contains(it, "error") before (1 + 2) after (3 + 4) -> groups
  if std::len(take rows) > 100 or std::len(take rows) > 200 { "long" -> _ }
}
)";
  const auto result = Format(source, {.line_width = 60});
  ASSERT_EQ(FirstError(result.diagnostics), nullptr);
  EXPECT_EQ(Tree(source), Tree(result.formatted));
  EXPECT_EQ(Format(result.formatted, {.line_width = 60}).formatted,
            result.formatted);
  std::istringstream lines(result.formatted);
  std::string line;
  while (std::getline(lines, line)) {
    EXPECT_LE(line.size(), 60u) << line;
  }
  EXPECT_NE(result.formatted.find("string stream required"), std::string::npos);
}

TEST(FlowFormat, CanonicalizesFieldConstraintsAndPortModifiers) {
  const std::string source = R"(
struct Finding {
  rule: string one of ["raw-pointer-nullability", "status-discard"] required
  line: int 1..2147483647 required
  tags: list[string] unique required
  score: number default 0.5 0..1
}
flow main {
  in rows: Finding required stream

  out results: Finding stream required

  rows -> results
}
)";
  for (bool align : {false, true}) {
    const auto result = Format(source, {.align_ports = align});
    EXPECT_TRUE(result.changed);
    EXPECT_EQ(Tree(source), Tree(result.formatted));
    EXPECT_EQ(Format(result.formatted, {.align_ports = align}).formatted,
              result.formatted);
    EXPECT_NE(result.formatted.find("required one of"), std::string::npos);
    EXPECT_NE(result.formatted.find("int required 1..2147483647"),
              std::string::npos);
    EXPECT_NE(result.formatted.find("list[string] required unique"),
              std::string::npos);
    EXPECT_NE(result.formatted.find("number 0..1 default 0.5"),
              std::string::npos);
    for (const auto& diagnostic : Parse(result.formatted).diagnostics) {
      EXPECT_NE(diagnostic.code, "flow.form.field-modifier-order");
    }
  }
}

TEST(FlowFormat, WrapsCompoundCallModifiersWithoutSplittingTheirKeywords) {
  const std::string source = R"(
flow main {
  in sources: string stream

  out results: object stream

  sources | map(run review_cpp_contract_file(source: it, options: {} ) forward headers "x-a11-llm-*").review parallel settings.max_concurrency unordered -> results
}
)";
  const auto result = Format(source);
  EXPECT_EQ(Tree(source), Tree(result.formatted));
  EXPECT_NE(result.formatted.find("forward headers"), std::string::npos);
  EXPECT_EQ(Format(result.formatted).formatted, result.formatted);
  std::istringstream lines(result.formatted);
  std::string line;
  while (std::getline(lines, line)) {
    EXPECT_LE(line.size(), 80u) << line;
  }
}

TEST(FlowFormat, PrefersExpandedPipelineCalls) {
  const std::string source = R"(flow review {
  results = sources
    | map (run review_cpp_contract_file(source: it, options: settings
  ) forward headers "x-a11-llm-*"
  ).review parallel settings.max_concurrency unordered
}
)";
  const std::string expected = R"(flow review {
  results = sources
    | map (
      run review_cpp_contract_file(source: it, options: settings)
      forward headers "x-a11-llm-*"
    ).review parallel settings.max_concurrency unordered
}
)";
  EXPECT_EQ(Formatted(source), expected);
  EXPECT_EQ(Tree(source), Tree(expected));
  EXPECT_FALSE(Format(expected).changed);
}

TEST(FlowFormat, CompactsShortNestedValues) {
  const std::string source = R"(flow f {
  value = std::list(
    [{
      "items": [
        1,
        2
      ]
    }]
  )
}
)";
  const std::string expected =
      "flow f {\n  value = std::list([{\"items\": [1, 2]}])\n}\n";
  EXPECT_EQ(Formatted(source), expected);
  EXPECT_EQ(Tree(source), Tree(expected));
  EXPECT_FALSE(Format(expected).changed);
}

TEST(FlowFormat, CompactsShortBlockConditions) {
  const std::string source = R"(flow f {
  if true or
false {
log "first"
} else if false or
true {
if true or
false {
log "nested"
}
} else {
log "last"
}
}
)";
  const std::string expected = R"(flow f {
  if true or false {
    log "first"
  } else if false or true {
    if true or false {
      log "nested"
    }
  } else {
    log "last"
  }
}
)";
  EXPECT_EQ(Formatted(source), expected);
  EXPECT_EQ(Tree(source), Tree(expected));
  EXPECT_FALSE(Format(expected).changed);
}

TEST(FlowFormat, WrappedClosersUseTheirOpeningIndentWithCustomWidths) {
  const std::string source = R"(flow review {
  results = sources
    | map (run review_cpp_contract_file(source: it, options: {"limit": [1, 2, 3]}) forward headers "x-a11-llm-*").review parallel settings.max_concurrency unordered
  if std::len(results) > 100 or std::len(results) > 200 { log "many results" }
}
)";
  for (int indent : {2, 4}) {
    for (size_t width : {40u, 60u, 80u}) {
      const FormatOptions options{.indent = indent, .line_width = width};
      const auto result = Format(source, options);
      ASSERT_EQ(FirstError(result.diagnostics), nullptr);
      EXPECT_EQ(Tree(source), Tree(result.formatted));
      EXPECT_FALSE(Format(result.formatted, options).changed);
      std::vector<size_t> openers;
      for (const auto& token : Lex(result.formatted).tokens) {
        const size_t newline = result.formatted.rfind('\n', token.start);
        const size_t line = newline == std::string::npos ? 0 : newline + 1;
        const size_t content = result.formatted.find_first_not_of(' ', line);
        if (token.kind == TokenKind::kLeftParen ||
            token.kind == TokenKind::kLeftBracket ||
            token.kind == TokenKind::kLeftBrace) {
          openers.push_back(content - line);
        } else if (token.kind == TokenKind::kRightParen ||
                   token.kind == TokenKind::kRightBracket ||
                   token.kind == TokenKind::kRightBrace) {
          ASSERT_FALSE(openers.empty());
          if (token.start == content) {
            EXPECT_EQ(content - line, openers.back()) << result.formatted;
          }
          openers.pop_back();
        }
      }
      EXPECT_TRUE(openers.empty());
    }
  }
}

TEST(FlowFormat, EquivalentWhitespaceHasOneLayout) {
  const std::vector<std::string> programs{
      R"(flow review {
in sources: string stream;
out results: object stream;
results = sources | map (run review_cpp_contract_file(source: it, options: settings) forward headers "x-a11-llm-*").review parallel settings.max_concurrency unordered;
})",
      R"(mod outer {
struct Record { first: string; second: list[int]; }
flow main { in value: Record; out result: string;
values = [{"first": std::upper(value.first), "second": [1, 2, 3]}];
if true or false { value.first -> result; } else { fail invalid_argument "Missing value."; }
}
})",
      R"(flow main {
in rows: int stream;
out result: int;
rows | fold 0 as total, total + it -> result;
run other(value: 1) timeout 1s forward headers "x-a11-*";
})"};
  for (const auto& source : programs) {
    const auto expected = Formatted(source);
    for (std::string_view gap : {" ", "\n", "\r\n\t", "  \n\n  "}) {
      std::string variant;
      for (const auto& token : Lex(source).tokens) {
        if (token.kind != TokenKind::kNewline &&
            token.kind != TokenKind::kEnd) {
          variant.append(token.text);
          variant.append(gap);
        }
      }
      ASSERT_FALSE(Parse(variant).HasErrors()) << variant;
      EXPECT_EQ(Tree(source), Tree(variant));
      EXPECT_EQ(Formatted(variant), expected) << variant;
    }
    EXPECT_FALSE(Format(expected).changed);
  }
}

TEST(FlowFormat, TrailingCommasRequestExpandedItems) {
  const std::string source = R"(flow f {
  compact = {"items": [1, 2]}
  expanded = {"items": [1, 2,],}
}
)";
  const std::string expected = R"(flow f {
  compact = {"items": [1, 2]}
  expanded = {
    "items": [
      1,
      2,
    ],
  }
}
)";
  EXPECT_EQ(Formatted(source), expected);
  EXPECT_EQ(Tree(source), Tree(expected));
  EXPECT_FALSE(Format(expected).changed);
}

TEST(FlowFormat, ExpandedArgumentsArePackedTogetherOrOnePerLine) {
  const std::string packed =
      "flow main { value = run process(first: 1, second: 2, third: 3) }";
  const std::string vertical =
      "flow main { value = run process(first: 1, second: 2, third: 3,) }";
  const FormatOptions options{.line_width = 40};
  const auto together = Format(packed, options);
  EXPECT_EQ(together.formatted, R"(flow main {
  value = run process(
    first: 1, second: 2, third: 3
  )
}
)");
  const auto separate = Format(vertical, options);
  EXPECT_EQ(separate.formatted, R"(flow main {
  value = run process(
    first: 1,
    second: 2,
    third: 3,
  )
}
)");
  EXPECT_EQ(Tree(packed), Tree(together.formatted));
  EXPECT_EQ(Tree(vertical), Tree(separate.formatted));
  EXPECT_FALSE(Format(together.formatted, options).changed);
  EXPECT_FALSE(Format(separate.formatted, options).changed);
}

TEST(FlowFormat, NestedLiteralDelimitersShareLines) {
  const std::string source = R"(flow main {
  value = std::list([{"first_long_field_name": 1, "second_long_field_name": 2}])
}
)";
  const std::string expected = R"(flow main {
  value = std::list([{
    "first_long_field_name": 1,
    "second_long_field_name": 2
  }])
}
)";
  const auto result = Format(source, {.line_width = 40});
  EXPECT_EQ(result.formatted, expected);
  EXPECT_EQ(Tree(source), Tree(result.formatted));
  EXPECT_FALSE(Format(result.formatted, {.line_width = 40}).changed);
}

TEST(FlowFormat, LongArgumentExpressionsWrapWithinExpandedCalls) {
  const std::string source = R"(flow main {
  work = run process(value: first_input_value + second_input_value + third_input_value + fourth_input_value, options: {"enabled": true})
}
)";
  const auto result = Format(source, {.line_width = 50});
  ASSERT_EQ(FirstError(result.diagnostics), nullptr);
  EXPECT_EQ(Tree(source), Tree(result.formatted));
  EXPECT_FALSE(Format(result.formatted, {.line_width = 50}).changed);
  std::istringstream lines(result.formatted);
  std::string line;
  while (std::getline(lines, line)) {
    EXPECT_LE(line.size(), 50u) << line;
  }
}

TEST(FlowFormat, SignificantStatementBoundariesRemainDistinct) {
  const std::string source = R"(flow f {
  fail invalid_argument
  "A separate transfer." -> output
}
)";
  const std::string explicit_boundary = R"(flow f { fail invalid_argument;
"A separate transfer." -> output; })";
  EXPECT_EQ(Tree(source), Tree(explicit_boundary));
  EXPECT_EQ(Formatted(source), Formatted(explicit_boundary));
  EXPECT_EQ(Tree(source), Tree(Formatted(source)));
  const std::string message =
      "flow f { fail invalid_argument\n\t\"An attached message.\" }";
  EXPECT_EQ(Formatted(message),
            "flow f {\n  fail invalid_argument \"An attached message.\"\n}\n");
}

TEST(FlowFormat, UnbrokenUnicodeAndEscapeRunsRespectStringWidth) {
  for (std::string_view atom : {"abcd", "東京", "😀", R"(\"\\\0)"}) {
    std::string literal;
    for (int count = 0; count < 40; ++count) {
      literal.append(atom);
    }
    const std::string source =
        absl::StrCat("flow f {\n log \"", literal, "\"\n}\n");
    const auto result = Format(source, {.line_width = 40});
    ASSERT_EQ(FirstError(result.diagnostics), nullptr);
    EXPECT_EQ(Tree(source), Tree(result.formatted));
    EXPECT_FALSE(Format(result.formatted, {.line_width = 40}).changed);
    std::istringstream lines(result.formatted);
    std::string line;
    while (std::getline(lines, line)) {
      const size_t columns = static_cast<size_t>(
          std::count_if(line.begin(), line.end(),
                        [](char c) { return !utf8::IsContinuation(c); }));
      EXPECT_LE(columns, 40u) << line;
    }
  }
}

TEST(FlowFormat, ModulesUseNamespaceIndentationAndKeepStructFields) {
  const std::string source = R"(mod outer {
  struct Record {
  title: string required
  count: int default 0
  }
  mod inner {
    flow main {
    in value: Record
    out result: string
    value.title -> result
    }
  }
}
)";
  const std::string expected = R"(mod outer {
struct Record {
  title: string required
  count: int default 0
}

mod inner {
flow main {
  in  value: Record

  out result: string

  value.title -> result
}
}
}
)";
  EXPECT_EQ(Formatted(source), expected);
  EXPECT_EQ(Tree(source), Tree(expected));
  EXPECT_FALSE(Format(expected).changed);
}

TEST(FlowFormat, DeclaredModulesKeepTheirDeclarationPrefixTogether) {
  const std::string source = R"(declare
mod library {
declare flow work { in value: string; out result: string; }
})";
  const std::string expected = R"(declare mod library {
declare flow work {
  in  value: string

  out result: string
}
}
)";
  EXPECT_EQ(Formatted(source), expected);
  EXPECT_EQ(Tree(source), Tree(expected));
  EXPECT_FALSE(Format(expected).changed);
}

TEST(FlowFormat, ExpandedGroupsKeepCommentsInsideTheirDelimiters) {
  const std::string source = R"(flow f {
  values = [1, # first item
  # second item
  2,
  # final note
  ]
  value = {"text": std::upper(
  # the text
  "hello"
  ),}
}
)";
  const std::string expected = R"(flow f {
  values = [
    1,  # first item
    # second item
    2,
    # final note
  ]
  value = {
    "text": std::upper(
      # the text
      "hello"
    ),
  }
}
)";
  EXPECT_EQ(Formatted(source), expected);
  EXPECT_EQ(Tree(source), Tree(expected));
  EXPECT_FALSE(Format(expected).changed);
}

TEST(FlowFormat, ReordersModifiersAroundACommentedMultilineEnumeration) {
  const std::string source = R"(
struct S {
  mode: string one of ["first", # the first mode
    "second"] required
}
)";
  const auto result = Format(source);
  EXPECT_EQ(Tree(source), Tree(result.formatted));
  EXPECT_NE(result.formatted.find("string required one of"), std::string::npos);
  EXPECT_NE(result.formatted.find("# the first mode"), std::string::npos);
  EXPECT_EQ(Format(result.formatted).formatted, result.formatted);
}

TEST(FlowFormat, WrapsUnbrokenStringsAndPreservesLongComments) {
  const std::string literal(100, 'x');
  const std::string source =
      absl::StrCat("flow f {\n out text: string\n \"", literal,
                   "\" -> text\n}\n# ", literal, "\n");
  const auto result = Format(source);
  EXPECT_EQ(Tree(source), Tree(result.formatted));
  EXPECT_EQ(Stream(source, false), Stream(result.formatted, false));
  EXPECT_NE(result.formatted.find(absl::StrCat("# ", literal)),
            std::string::npos);
  EXPECT_NE(result.formatted.find("\"\n"), std::string::npos);
  EXPECT_EQ(Format(result.formatted).formatted, result.formatted);
}

TEST(FlowFormat, SplitsLongStringsIntoAdjacentLiterals) {
  const std::string source = R"(flow main {
  message = "This message contains enough words to wrap across several lines without changing its value."
  log message
  fail invalid_argument "The selected repository directory must contain a readable configuration file."
}
)";
  const auto result = Format(source, {.line_width = 48});
  ASSERT_EQ(FirstError(result.diagnostics), nullptr);
  EXPECT_EQ(Tree(source), Tree(result.formatted));
  EXPECT_EQ(result.formatted, R"(flow main {
  message = (
    "This message contains enough words to wrap"
    " across several lines without changing its"
    " value."
  )
  log message
  fail (invalid_argument) (
    "The selected repository directory must"
    " contain a readable configuration file."
  )
}
)");
  EXPECT_EQ(Format(result.formatted, {.line_width = 48}).formatted,
            result.formatted);
  ASSERT_EQ(result.edits.size(), 1u);
  std::string applied = source;
  applied.replace(result.edits[0].start,
                  result.edits[0].end - result.edits[0].start,
                  result.edits[0].text);
  EXPECT_EQ(applied, result.formatted);
}

TEST(FlowFormat, SplitsStringsInsideGroupsAndDescriptions) {
  const std::string source = R"(struct Settings {
  root: string "The repository directory containing the source files and configuration to inspect."
}
flow main {
  describe "Inspect repository source files and report findings with their original source locations."
  in settings: Settings "Settings describing which repository and configuration to inspect for this run."
  out text: string
  std::strformat("The repository {} contains source files that can be inspected and checked for findings.", settings.root) -> text
  ["A list item containing enough words to wrap across several separate string literals."] -> _
}
)";
  const auto result = Format(source, {.line_width = 48});
  ASSERT_EQ(FirstError(result.diagnostics), nullptr);
  EXPECT_EQ(Tree(source), Tree(result.formatted));
  EXPECT_EQ(Format(result.formatted, {.line_width = 48}).formatted,
            result.formatted);
  std::istringstream lines(result.formatted);
  std::string line;
  while (std::getline(lines, line)) {
    EXPECT_LE(line.size(), 48u) << line;
  }
}

TEST(FlowFormat, StringSplitsPreserveEscapesWhitespaceAndUnicode) {
  for (
      const auto* literal :
      {R"("Escaped quotes \"hello world\" and slashes \\ remain intact when this sentence wraps.")",
       R"("Tabs\tand newlines\n and nul\0 remain escaped when this longer string wraps at spaces.")",
       R"("Repeated   spaces and an escaped\ space retain their exact values after the string wraps.  ")",
       R"("Unicode café 東京 and 😀 remain intact when this sentence wraps across multiple lines.")",
       R"("A first fragment " "and a second fragment form one long string that wraps across lines.")"}) {
    SCOPED_TRACE(literal);
    const auto source = absl::StrCat("flow main {\n log ", literal, "\n}\n");
    const auto result = Format(source, {.line_width = 40});
    ASSERT_EQ(FirstError(result.diagnostics), nullptr);
    EXPECT_EQ(Tree(source), Tree(result.formatted));
    EXPECT_EQ(Format(result.formatted, {.line_width = 40}).formatted,
              result.formatted);
    EXPECT_NE(result.formatted.find("log (\n"), std::string::npos);
  }
}

TEST(FlowFormat, StringSplitsRespectCommentsAndExistingBreaks) {
  const std::string source = R"(flow main {
  log "A long message with enough words to wrap across several lines of source text." # keep this comment
  log (
    "An existing fragment."
    # keep this boundary
    "Another long fragment with enough words to wrap into multiple adjacent string literals."
  )
}
)";
  const auto result = Format(source, {.line_width = 48});
  ASSERT_EQ(FirstError(result.diagnostics), nullptr);
  EXPECT_EQ(Tree(source), Tree(result.formatted));
  EXPECT_EQ(Format(result.formatted, {.line_width = 48}).formatted,
            result.formatted);
  EXPECT_NE(result.formatted.find(
                "\"An existing fragment.\"\n    # keep this boundary\n"),
            std::string::npos);
  EXPECT_NE(result.formatted.find(")  # keep this comment"), std::string::npos);
  EXPECT_NE(result.formatted.find("\"Another long fragment with enough words"),
            std::string::npos);
  EXPECT_EQ(result.formatted.find(
                "\"Another long fragment with enough words to wrap into"),
            std::string::npos);
}

TEST(FlowFormat, StringSplitsKeepStatusCodesAndLogLevelsSeparate) {
  for (const auto* prefix : {"fail invalid_argument ", "log warning ",
                             "1 | log warning ", "abort n invalid_argument "}) {
    const auto source = absl::StrCat(
        "flow main {\n n = node()\n ", prefix,
        "\"A message with enough words to require several adjacent string "
        "literals after formatting.\"",
        std::string_view(prefix).starts_with("1 |") ? " -> _" : "", "\n}\n");
    SCOPED_TRACE(source);
    const auto result = Format(source, {.line_width = 48});
    ASSERT_EQ(FirstError(result.diagnostics), nullptr);
    EXPECT_EQ(Tree(source), Tree(result.formatted));
    EXPECT_EQ(Format(result.formatted, {.line_width = 48}).formatted,
              result.formatted);
    EXPECT_NE(result.formatted.find("(\n"), std::string::npos);
  }
}

TEST(FlowFormat, LeavesStringsIntactWhenWrappingIsDisabled) {
  const std::string source = R"(flow main {
  log "This message contains enough words to wrap across several lines without changing its value."
}
)";
  const auto result = Format(source, {.line_width = 0});
  EXPECT_EQ(result.formatted, source);
  EXPECT_EQ(Stream(result.formatted), Stream(source));
}

TEST(FlowFormat, SemicolonsFormatAsNewlines) {
  const std::string source =
      ";;struct Settings { root: string; count: int };"
      "declare flow inspect { in value: string; out result: string };"
      "flow main { describe \"Inspect a repository.\";"
      "in settings: Settings; out result: string;"
      "a = run inspect(value: settings.root);"
      "if settings.count < 1 { log \"empty; repository\"; log \"done\" };"
      "a.result | map std::trim(it) -> result; wait all; };";
  std::string multiline = source;
  bool quoted = false;
  for (char& c : multiline) {
    if (c == '"') {
      quoted = !quoted;
    } else if (c == ';' && !quoted) {
      c = '\n';
    }
  }
  const auto result = Format(source);
  ASSERT_EQ(FirstError(result.diagnostics), nullptr);
  EXPECT_EQ(Tree(source), Tree(multiline));
  EXPECT_EQ(result.formatted, Formatted(multiline));
  EXPECT_EQ(Format(result.formatted).formatted, result.formatted);
  EXPECT_NE(result.formatted.find("empty; repository"), std::string::npos);
}

}  // namespace
}  // namespace a11::flow
