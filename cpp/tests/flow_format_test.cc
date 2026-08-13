// Copyright 2026 The A11 Authors.

#include "a11/flow/format.h"

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
#include "a11/flow/lexer.h"
#include "a11/flow/parser.h"
#include "a11/flow/token.h"

namespace a11::flow {
namespace {

std::string Formatted(std::string_view source) {
  const FormatResult result = Format(source);
  std::vector<std::string> messages;
  for (const Diagnostic& diagnostic : result.diagnostics) {
    messages.push_back(diagnostic.message);
  }
  EXPECT_TRUE(messages.empty()) << absl::StrJoin(messages, "; ");
  return result.formatted;
}

/// Every token as `kind:text`, minus the line breaks.
///
/// The comparison the token-preservation invariant is stated in. Line breaks are
/// left out because they are whitespace that happens to be a token: `if a { b }`
/// has to become three lines, and a formatter that could not add one could not
/// indent anything. What may not change is everything else -- and the tree, which
/// is what [Tree] checks and is the guarantee that actually matters.
///
/// Trailing whitespace inside a comment is the one piece of whitespace that lives
/// inside a token, and the formatter trims it.
std::vector<std::string> Stream(std::string_view source) {
  std::vector<std::string> out;
  for (const Token& token : Lex(source).tokens) {
    if (token.kind == TokenKind::kEnd) break;
    if (token.kind == TokenKind::kNewline) continue;
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

/// The syntax tree, with every position dropped.
///
/// Formatting moves everything, so the offsets differ by construction; what has to
/// be identical is the tree those positions hang off. This is the invariant a
/// person actually cares about: the program after formatting is the same program.
nlohmann::json Tree(std::string_view source) {
  nlohmann::json value = SyntaxToJsonValue("-", Parse(source));
  value.erase("source");
  const auto strip = [](nlohmann::json& node, const auto& self) -> void {
    if (node.is_object()) {
      node.erase("at");
      for (auto& [key, child] : node.items()) self(child, self);
    } else if (node.is_array()) {
      for (auto& child : node) self(child, self);
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
       {root / "examples" / "003-flow-dsl", root / "scripts",
        root / "testdata" / "flow"}) {
    if (!std::filesystem::is_directory(directory)) continue;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.path().extension() == ".flow") paths.push_back(entry.path());
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
            "  out b: string\n"
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
            "  out answer:   string          \"The answer.\"\n"
            "  out sources:  string stream   \"Where it came from.\"\n"
            "  out read:     number\n"
            "\n"
            // A blank line means the author grouped them, so the second group
            // lines up its own columns rather than the first group's.
            "  in  x:  string\n"
            "  out yy: string\n"
            "}\n");
}

TEST(FlowFormat, KeepsTheBreaksTheAuthorWroteAndIndentsThem) {
  // The formatter's one big decision: where a line breaks says what the author
  // thought went together, and that judgement is theirs.
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
  EXPECT_EQ(Formatted(source),
            "flow t {\n"
            "  in  a: string\n"
            "  out b: string\n"
            "  pages\n"
            "    | collect\n"
            "    | join \", \"\n"
            "    -> b\n"
            "  x = run act(p: a)\n"
            "      with \"h\": 1\n"
            "      forward headers \"authorization\"\n"
            "}\n");
}

TEST(FlowFormat, ABracketedGroupIndentsFromTheLineThatOpenedIt) {
  const std::string source =
      "flow t {\n"
      "  in  a: string\n"
      "  llm = call interact(\n"
      "interactions: a | map a11.sdk.Interaction{\n"
      "role: \"user\",\n"
      "content: [to_chunk(it)]\n"
      "},\n"
      "config: {}\n"
      ")\n"
      "}\n";
  EXPECT_EQ(Formatted(source),
            "flow t {\n"
            "  in  a: string\n"
            "  llm = call interact(\n"
            "    interactions: a | map a11.sdk.Interaction{\n"
            "      role: \"user\",\n"
            "      content: [to_chunk(it)]\n"
            "    },\n"
            "    config: {}\n"
            "  )\n"
            "}\n");
}

TEST(FlowFormat, ADescriptionOnItsOwnLineIsIndentedUnderWhatItDescribes) {
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
            "  describe\n"
            "    \"What this is for.\"\n"
            "  in  question: string required\n"
            "    \"What to find out, at length.\"\n"
            "  out answer:   string\n"
            "    \"The answer.\"\n"
            "}\n");
}

TEST(FlowFormat, ATripleQuotedStringIsLeftExactlyAsItWasWritten) {
  // Its interior is content, not code: the formatter indents the line the string
  // starts on and does not touch a byte inside it, because every one of those
  // bytes is in the value.
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
      "  if a { skip a } else if a { skip a } else { skip a }\n"
      "}\n";
  EXPECT_EQ(Formatted(source),
            "flow t {\n"
            "  in  a: string\n"
            "  if a {\n"
            "    skip a\n"
            "  } else if a {\n"
            "    skip a\n"
            "  } else {\n"
            "    skip a\n"
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

TEST(FlowFormat, IsIdempotentAndTokenPreservingOverTheCorpus) {
  // The two invariants, over every flow this repository ships. They are what let
  // `fmt -i` be run without reading the diff: whatever the style decides, the
  // program is the same program, and running it again changes nothing.
  const std::vector<std::filesystem::path> corpus = Corpus();
  ASSERT_FALSE(corpus.empty());
  for (const std::filesystem::path& path : corpus) {
    const std::string source = ReadFile(path);
    const FormatResult once = Format(source);
    ASSERT_TRUE(once.diagnostics.empty()) << path;
    // Compared one token at a time so a failure says *which* token moved rather
    // than printing two thousand of them.
    const std::vector<std::string> before = Stream(source);
    const std::vector<std::string> after = Stream(once.formatted);
    for (size_t index = 0; index < std::min(before.size(), after.size());
         ++index) {
      ASSERT_EQ(before[index], after[index])
          << path << " at token " << index;
    }
    ASSERT_EQ(before.size(), after.size()) << path;
    EXPECT_EQ(Tree(once.formatted), Tree(source)) << path;
    const FormatResult twice = Format(once.formatted);
    EXPECT_EQ(twice.formatted, once.formatted) << path;
    EXPECT_FALSE(twice.changed) << path;
  }
}

TEST(FlowFormat, EveryFlowThisRepositoryShipsIsFormatted) {
  // The corpus is the style's own documentation, so it is held to it. A diff here
  // means either the formatter or the file needs a change, and the diff says
  // which.
  for (const std::filesystem::path& path : Corpus()) {
    const std::string source = ReadFile(path);
    EXPECT_EQ(Format(source).formatted, source) << path;
  }
}

TEST(FlowFormat, AFileThatIsAlreadyFormattedIsNotChanged) {
  const std::string source =
      "flow t {\n"
      "  in  a: string\n"
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

}  // namespace
}  // namespace a11::flow
