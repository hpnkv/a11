// Copyright 2026 The A11 Authors.

#include "a11/flow/parser.h"

#include <cstdlib>
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
#include "a11/flow/syntax.h"

namespace a11::flow {
namespace {

using syntax::As;
using syntax::NodeKind;

std::vector<std::string> Codes(const ParseResult& result) {
  std::vector<std::string> codes;
  for (const Diagnostic& diagnostic : result.diagnostics) {
    codes.push_back(diagnostic.code);
  }
  return codes;
}

std::vector<std::string> Messages(const ParseResult& result) {
  std::vector<std::string> messages;
  for (const Diagnostic& diagnostic : result.diagnostics) {
    messages.push_back(diagnostic.message);
  }
  return messages;
}

/// The kinds of a flow's statements, which is the shape a test usually means.
std::vector<std::string> BodyKinds(const syntax::FlowDeclaration& flow) {
  std::vector<std::string> kinds;
  for (const syntax::NodePtr& statement : flow.body) {
    kinds.emplace_back(syntax::NodeKindName(statement->kind));
  }
  return kinds;
}

// A flow using most of the grammar at once. Deliberately one test rather than
// thirty: what matters is that a realistic flow comes out with the shape it was
// written in, and a per-construct test says less than this does.
constexpr std::string_view kResearch = R"flow(
# Look something up, then say what was found.
flow research {
  describe "Search, read and summarise."
  in  question: string required "What to look up"
  out answer:   string
  header "x-a11-budget" as budget default 3 "How many pages to read"

  nodes scratch

  hits = run web-search(query: question) timeout 30s via scratch
  for hit in hits.results parallel 2 {
    page = try call web-fetch(url: hit.url)
      with "accept": "text/html"
      forward headers "authorization"
    page.text | truncate 200 | where it != "" -> pages
  }
  drain pages after hits
  repeat state = {"round": 0} max 6 {
    step = run think(state: state)
    state <- step.next
    until step.next.done
  }
  if (pages | count) > 0 {
    pages | join ", " -> answer
  } else {
    fail not_found "nothing to read"
  }
}
)flow";

TEST(FlowParser, ReadsAWholeFlowWithTheShapeItWasWrittenIn) {
  const ParseResult result = Parse(kResearch);
  ASSERT_TRUE(result.diagnostics.empty())
      << absl::StrJoin(Messages(result), "; ");
  ASSERT_EQ(result.flows.size(), 1u);
  const syntax::FlowDeclaration& flow = *result.flows.front();

  EXPECT_EQ(flow.name.text, "research");
  EXPECT_EQ(flow.description, "Search, read and summarise.");

  ASSERT_EQ(flow.ports.size(), 2u);
  EXPECT_EQ(flow.ports[0]->name.text, "question");
  EXPECT_EQ(flow.ports[0]->direction, syntax::PortDirection::kInput);
  EXPECT_EQ(flow.ports[0]->type.ToString(), "string");
  EXPECT_TRUE(flow.ports[0]->required);
  EXPECT_TRUE(flow.ports[0]->unary);
  EXPECT_EQ(flow.ports[0]->description, "What to look up");
  EXPECT_EQ(flow.ports[1]->direction, syntax::PortDirection::kOutput);

  ASSERT_EQ(flow.headers.size(), 1u);
  EXPECT_EQ(flow.headers[0]->name, "x-a11-budget");
  EXPECT_EQ(flow.headers[0]->alias.text, "budget");
  ASSERT_TRUE(flow.headers[0]->has_default);
  EXPECT_EQ(flow.headers[0]->default_value.integer, 3);

  EXPECT_EQ(BodyKinds(flow), (std::vector<std::string>{"nodes", "bind",
                                                       "for-each", "drain",
                                                       "repeat", "if"}));

  // The call, its modifiers, and the pipeline feeding a port.
  const auto* bind = As<syntax::Bind>(flow.body[1].get());
  ASSERT_NE(bind, nullptr);
  const auto* call = As<syntax::CallExpression>(bind->value.get());
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->action, "web-search");
  EXPECT_EQ(call->mode, "run");
  EXPECT_FALSE(call->tolerant);
  ASSERT_EQ(call->args.size(), 1u);
  EXPECT_EQ(call->args[0].port.text, "query");
  EXPECT_EQ(call->modifiers->timeout, absl::Seconds(30));
  EXPECT_EQ(call->modifiers->node_map.text, "scratch");

  const auto* loop = As<syntax::ForEach>(flow.body[2].get());
  ASSERT_NE(loop, nullptr);
  EXPECT_EQ(loop->variable.text, "hit");
  EXPECT_EQ(loop->parallel, 2);
  ASSERT_EQ(loop->body.size(), 2u);
  const auto* inner = As<syntax::Bind>(loop->body[0].get());
  ASSERT_NE(inner, nullptr);
  const auto* tolerant = As<syntax::CallExpression>(inner->value.get());
  ASSERT_NE(tolerant, nullptr);
  EXPECT_TRUE(tolerant->tolerant);
  EXPECT_EQ(tolerant->mode, "call");
  ASSERT_EQ(tolerant->modifiers->headers.size(), 1u);
  EXPECT_EQ(tolerant->modifiers->headers[0].first, "accept");
  EXPECT_EQ(tolerant->modifiers->forward,
            (std::vector<std::string>{"authorization"}));

  const auto* pipe = As<syntax::Pipe>(loop->body[1].get());
  ASSERT_NE(pipe, nullptr);
  ASSERT_EQ(pipe->pipeline->stages.size(), 2u);
  EXPECT_EQ(pipe->pipeline->stages[0]->name, "truncate");
  EXPECT_EQ(pipe->pipeline->stages[0]->number, 200);
  EXPECT_TRUE(pipe->pipeline->stages[0]->is_integer);
  EXPECT_EQ(pipe->pipeline->stages[1]->name, "where");
  EXPECT_EQ(pipe->pipeline->stages[1]->argument->kind, NodeKind::kBinary);
  ASSERT_EQ(pipe->targets.size(), 1u);

  const auto* drain = As<syntax::Drain>(flow.body[3].get());
  ASSERT_NE(drain, nullptr);
  ASSERT_EQ(drain->after.size(), 1u);
  EXPECT_EQ(drain->after[0].text, "hits");

  const auto* repeat = As<syntax::Repeat>(flow.body[4].get());
  ASSERT_NE(repeat, nullptr);
  EXPECT_EQ(repeat->variable.text, "state");
  EXPECT_EQ(repeat->max_iterations, 6);
  // `{"round": 0}` is a value here even though a `{` opens a block two tokens
  // later: a repeat's start is an expression, and the block is what follows it.
  EXPECT_EQ(repeat->start->kind, NodeKind::kObjectLiteral);
  EXPECT_EQ(BodyKinds(*result.flows.front()).size(), flow.body.size());

  const auto* branch = As<syntax::If>(flow.body[5].get());
  ASSERT_NE(branch, nullptr);
  EXPECT_EQ(branch->condition->kind, NodeKind::kBinary);
  ASSERT_EQ(branch->then_body.size(), 1u);
  ASSERT_EQ(branch->else_body.size(), 1u);
  EXPECT_EQ(branch->else_body[0]->kind, NodeKind::kFail);
}

TEST(FlowParser, ANoteAboutTheOnlyPlaceAWordMeansTwoThings) {
  // `then` and `where` may go without their pipe; a word spelled like a stage
  // with nothing after it is a name.
  const ParseResult bare = Parse("flow f { in a: string\n a where it -> b }");
  ASSERT_TRUE(bare.diagnostics.empty());
  const auto* pipe = As<syntax::Pipe>(bare.flows[0]->body[0].get());
  ASSERT_NE(pipe, nullptr);
  ASSERT_EQ(pipe->pipeline->stages.size(), 1u);

  const ParseResult name = Parse("flow f { in a: string\n where -> b }");
  ASSERT_TRUE(name.diagnostics.empty());
  const auto* named = As<syntax::Pipe>(name.flows[0]->body[0].get());
  ASSERT_NE(named, nullptr);
  EXPECT_TRUE(named->pipeline->stages.empty());
  EXPECT_EQ(named->pipeline->source->kind, NodeKind::kName);

  // And a statement word left of a `->` is a name too.
  const ParseResult keyword = Parse("flow f { in a: string\n skip -> b }");
  ASSERT_TRUE(keyword.diagnostics.empty());
  EXPECT_EQ(keyword.flows[0]->body[0]->kind, NodeKind::kPipe);
}

TEST(FlowParser, ADescriptionMayStandOnTheLineBelowWhatItDescribes) {
  // Prose that says anything runs past the width of the declaration it belongs
  // to, so it may go underneath it -- at any indentation, or none.
  const ParseResult result = Parse(
      "flow f {\n"
      "  describe\n"
      "    \"What this flow is for.\"\n"
      "  in  question: string required\n"
      "      \"What to find out, at length.\"\n"
      "  out answer: string\n"
      "\"not indented at all\"\n"
      "  header \"x-a11-budget\" as budget default 3\n"
      "    \"How many pages to read.\"\n"
      "}\n");
  ASSERT_TRUE(result.diagnostics.empty())
      << absl::StrJoin(Messages(result), "; ");
  ASSERT_EQ(result.flows.size(), 1u);
  const syntax::FlowDeclaration& flow = *result.flows.front();
  EXPECT_EQ(flow.description, "What this flow is for.");
  ASSERT_EQ(flow.ports.size(), 2u);
  EXPECT_EQ(flow.ports[0]->description, "What to find out, at length.");
  EXPECT_TRUE(flow.ports[0]->required);
  EXPECT_EQ(flow.ports[1]->description, "not indented at all");
  ASSERT_EQ(flow.headers.size(), 1u);
  EXPECT_EQ(flow.headers[0]->description, "How many pages to read.");
  EXPECT_TRUE(flow.body.empty());
}

TEST(FlowParser, AStringWithSomethingAfterItIsAStatementAndNotADescription) {
  // What makes the form above unambiguous: a description is *alone* on its line.
  const ParseResult result = Parse(
      "flow f {\n"
      "  in  a: string\n"
      "  out b: string\n"
      "  \"a literal\" -> b\n"
      "}\n");
  ASSERT_TRUE(result.diagnostics.empty())
      << absl::StrJoin(Messages(result), "; ");
  EXPECT_EQ(result.flows[0]->ports[0]->description, "");
  ASSERT_EQ(result.flows[0]->body.size(), 1u);
  EXPECT_EQ(result.flows[0]->body[0]->kind, NodeKind::kPipe);
}

TEST(FlowParser, ATripleQuotedStringHoldsLineBreaksAndLosesItsIndentation) {
  const ParseResult result = Parse(
      "flow f {\n"
      "  describe \"\"\"\n"
      "    What this flow is for.\n"
      "\n"
      "      An indented line, kept indented.\n"
      "    \"\"\"\n"
      "  in a: string\n"
      "  out b: string\n"
      "  \"\"\"a value, in a statement\"\"\" -> b\n"
      "}\n");
  ASSERT_TRUE(result.diagnostics.empty())
      << absl::StrJoin(Messages(result), "; ");
  const syntax::FlowDeclaration& flow = *result.flows.front();
  // The blank first line and the whitespace-only last one go; the indentation
  // every remaining line shares comes off, and what one line has *extra* stays.
  EXPECT_EQ(flow.description,
            "What this flow is for.\n\n  An indented line, kept indented.");
  const auto* pipe = As<syntax::Pipe>(flow.body[0].get());
  ASSERT_NE(pipe, nullptr);
  const auto* literal = As<syntax::Literal>(pipe->pipeline->source.get());
  ASSERT_NE(literal, nullptr);
  EXPECT_EQ(literal->value.text, "a value, in a statement");
}

TEST(FlowParser, KeywordsMayBeShoutedAndMixedCaseIsAName) {
  const ParseResult shouted =
      Parse("FLOW f { IN a: STRING REQUIRED\n OUT b: STRING\n a -> b }");
  ASSERT_TRUE(shouted.diagnostics.empty());
  ASSERT_EQ(shouted.flows.size(), 1u);
  EXPECT_TRUE(shouted.flows[0]->ports[0]->required);

  const ParseResult mixed = Parse("flow f { out a: bool\n True -> a }");
  ASSERT_TRUE(mixed.diagnostics.empty());
  const auto* pipe = As<syntax::Pipe>(mixed.flows[0]->body[0].get());
  ASSERT_NE(pipe, nullptr);
  // `TRUE` is the literal; `True` is a name, and resolving it is what fails.
  EXPECT_EQ(pipe->pipeline->source->kind, NodeKind::kName);
}

TEST(FlowParser, AProblemCostsItsOwnLineAndNothingMore) {
  // The property the whole recovering design exists for: a mistake in the middle
  // of a flow does not hide what is around it.
  const ParseResult result = Parse(
      "flow f {\n"
      "  in  a: string\n"
      "  out b: string\n"
      "  a | wat -> b\n"
      "  a -> b\n"
      "}\n");
  EXPECT_EQ(Codes(result), (std::vector<std::string>{"flow.form.unknown-stage"}));
  ASSERT_EQ(result.flows.size(), 1u);
  // Both statements are there, and the ports around them too.
  EXPECT_EQ(result.flows[0]->ports.size(), 2u);
  EXPECT_EQ(BodyKinds(*result.flows[0]),
            (std::vector<std::string>{"pipe", "pipe"}));
}

TEST(FlowParser, ReportsEveryProblemRatherThanTheFirst) {
  const ParseResult result = Parse(
      "flow f {\n"
      "  in  a: string\n"
      "  a | wat -> b\n"
      "  a | nope -> b\n"
      "  nothing(1) -> b\n"
      "}\n");
  EXPECT_EQ(Codes(result),
            (std::vector<std::string>{"flow.form.unknown-stage",
                                      "flow.form.unknown-stage",
                                      "flow.form.unknown-builtin"}));
}

TEST(FlowParser, SaysWhatIsMissingInTheWordsThePythonCompilerUses) {
  struct Case {
    std::string_view source;
    std::string_view code;
    std::string_view message;
  };
  // The sentences are the reference implementation's, because they are what the
  // suite and the documentation already quote.
  const Case cases[] = {
      {"flow f { in a: stream string\n }", "flow.form.port-modifier-order",
       "'stream' follows the type: write 'a: TYPE stream'"},
      {"flow f { in a: list[string\n }", "flow.syntax.unexpected",
       "Expected ']'"},
      {"flow f { in a: string\n out b: string\n a -> b b -> b }",
       "flow.syntax.statement-end", "one statement per line"},
      {"flow f { in a: string", "flow.syntax.unclosed", "missing its closing"},
      {"flow wrong { out o: string\n x = run thing(a: \"b\") forward \"x\"\n"
       " x.out -> o }",
       "flow.syntax.unexpected", "Expected 'headers'"},
      {"flow wrong { out o: string stream\n n = node\n n -> o }",
       "flow.form.node-parentheses", "takes parentheses"},
      {"flow wrong { in w: string stream\n skip 0 w }",
       "flow.form.count-not-positive", "counts whole values"},
      {"flow wrong { in w: string stream\n skip 1.5 w }",
       "flow.form.count-not-positive", "counts whole values"},
      {"flow f { in a: string\n x = 5 }", "flow.syntax.unexpected",
       "Expected 'run' or 'call', found '5'."},
      {"flow f { in a: string\n a | first -> b }",
       "flow.form.stage-argument", "Expected a count for 'first'"},
      {"flow f { header \"x-a\" default a }", "flow.syntax.constant-required",
       "Expected a constant value."},
      {"# nothing here\n", "flow.syntax.unexpected",
       "A flow file must declare at least one flow."},
  };
  for (const Case& one : cases) {
    const ParseResult result = Parse(one.source);
    ASSERT_FALSE(result.diagnostics.empty()) << one.source;
    bool found = false;
    for (const Diagnostic& diagnostic : result.diagnostics) {
      if (diagnostic.code == one.code &&
          diagnostic.message.find(one.message) != std::string::npos) {
        found = true;
      }
    }
    EXPECT_TRUE(found) << one.source << " gave "
                       << absl::StrJoin(Messages(result), "; ");
  }
}

TEST(FlowParser, EveryDiagnosticCodeItProducesIsPublished) {
  // The contract `a11 flow codes` and the documentation stand on: a code a
  // toolchain can see is a code the table explains.
  const std::string_view sources[] = {
      "flow f { in a: stream string\n }",
      "flow f { in a: list[string\n }",
      "flow f { in a: string\n a -> b b -> b }",
      "flow f { in a: string",
      "flow f { out o: string\n n = node\n n -> o }",
      "flow f { in w: string stream\n skip 0 w }",
      "flow f { in a: string\n a | wat -> b }",
      "flow f { in a: string\n a | first -> b }",
      "flow f { out o: string\n nothing(1) -> o }",
      "flow f { header \"x-a\" default a }",
      "flow f { out o: string\n x = run t(a: \"b\") headers \"x\"\n x.o -> o }",
      "not a flow at all",
      "flow f { in a: string\n [1, 2 }",
  };
  for (const std::string_view source : sources) {
    for (const Diagnostic& diagnostic : Parse(source).diagnostics) {
      EXPECT_NE(FindCode(diagnostic.code), nullptr)
          << diagnostic.code << " from " << source;
    }
  }
}

TEST(FlowParser, StopsRatherThanSpinsOnTextThatIsNotAFlowAtAll) {
  // Every one of these used to be a way to hang a hand-written parser. The
  // guarantee is only that parsing terminates and says something.
  const std::string_view sources[] = {
      "",          "}",         "{",        "flow",     "flow f",
      "flow f {",  "flow f {\n a",  "flow f { a -> }",
      "flow f { a | }",         "flow f { a( }", "flow f { in }",
      "flow f { in a: }",       "flow f { for }", "flow f { if { } }",
      "flow f { repeat }",      "flow f { x = }", "flow f { \"s\" }",
      "flow f { a -> b, }",     "flow f { {\"a\": } }", ",,,,",
      "flow f { a | then }",    "flow f { status }", "flow f { a[ }",
  };
  for (const std::string_view source : sources) {
    const ParseResult result = Parse(source);
    // Something was wrong with every one of these, and it was said out loud.
    EXPECT_TRUE(result.HasErrors()) << source;
    EXPECT_NE(result.FirstError(), nullptr) << source;
  }
}

TEST(FlowParser, TheFirstErrorIsWhatAStrictCallerRefusesWith) {
  const ParseResult result = Parse("flow f { in a: string\n a | wat -> b\n }");
  const Diagnostic* first = result.FirstError();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->code, "flow.form.unknown-stage");
  // The position the Python `FlowSyntaxError` carries: 1-based, at the word.
  EXPECT_EQ(first->range.start.line, 2);
  EXPECT_EQ(first->range.start.column, 6);
  EXPECT_EQ(first->flow, "f");
}

TEST(FlowParser, ALexProblemIsReportedAndParsingCarriesOn) {
  const ParseResult result = Parse(
      "flow f {\n"
      "  in  a: string\n"
      "  a -> b\n"
      "  \"unterminated -> a\n"
      "  a -> b\n"
      "}\n");
  EXPECT_EQ(Codes(result),
            (std::vector<std::string>{"flow.syntax.unterminated-string",
                                      "flow.syntax.unexpected"}));
  ASSERT_EQ(result.flows.size(), 1u);
  // The line after the broken string is still a statement.
  EXPECT_EQ(result.flows[0]->body.back()->kind, NodeKind::kPipe);
}

TEST(FlowParser, TheSyntaxEnvelopeIsWhatTheFormatSays) {
  const ParseResult result =
      Parse("flow f { in a: string\n out b: string\n a | truncate 5 -> b }");
  const nlohmann::json value = SyntaxToJsonValue("f.flow", result);
  EXPECT_EQ(value["format"], "flow.syntax/v1");
  EXPECT_EQ(value["source"], "f.flow");
  EXPECT_TRUE(value["diagnostics"].empty());
  ASSERT_EQ(value["flows"].size(), 1u);

  const nlohmann::json& flow = value["flows"][0];
  EXPECT_EQ(flow["kind"], "flow");
  EXPECT_EQ(flow["name"], "f");
  EXPECT_EQ(flow["ports"][0]["direction"], "inputs");
  EXPECT_EQ(flow["ports"][0]["type"]["name"], "string");
  const nlohmann::json& stage = flow["body"][0]["pipeline"]["stages"][0];
  EXPECT_EQ(stage["name"], "truncate");
  EXPECT_EQ(stage["takes"], "number");
  EXPECT_EQ(stage["arg"], 5);
  // Every node says where it is, in bytes and in line and column -- under `at`,
  // clear of the fields a node of its own kind has.
  EXPECT_EQ(flow["at"]["start"], 0);
  EXPECT_EQ(flow["at"]["line"], 1);
}

TEST(FlowParser, ADurationIsTaggedSoItIsNotReadAsACount) {
  const ParseResult result =
      Parse("flow f { in a: string\n x = run t(a: a) timeout 250ms\n"
            " x.o -> a }");
  const nlohmann::json value = SyntaxToJsonValue("-", result);
  const nlohmann::json& modifiers = value["flows"][0]["body"][0]["value"]
                                         ["modifiers"];
  EXPECT_EQ(modifiers["timeout"]["$duration"], 0.25);
}

TEST(FlowParser, ReadsEveryFlowInOneFileAndSaysWhichOneAProblemIsIn) {
  const ParseResult result = Parse(
      "flow first { in a: string\n a -> b }\n"
      "flow second { in a: string\n a | wat -> b }\n");
  ASSERT_EQ(result.flows.size(), 2u);
  EXPECT_EQ(result.flows[0]->name.text, "first");
  EXPECT_EQ(result.flows[1]->name.text, "second");
  ASSERT_EQ(result.diagnostics.size(), 1u);
  EXPECT_EQ(result.diagnostics[0].flow, "second");
}

// <repo>/cpp/tests/flow_parser_test.cc -> <repo>/testdata/flow/<name>
std::filesystem::path GoldenPath(std::string_view name) {
  return (std::filesystem::path(A11_CPP_SOURCE_ROOT).parent_path() /
          std::filesystem::path(__FILE__))
             .parent_path()
             .parent_path()
             .parent_path() /
         "testdata" / "flow" / name;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream.is_open()) return "";
  std::stringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

TEST(FlowSyntaxJson, MatchesTheGoldenEveryLanguageReads) {
  // `testdata/flow/syntax.json` is the syntax format pinned against one small
  // flow, in the same spirit as `testdata/flow/codes.json`: the C++ owns it, and
  // a frontend in another language reads it back to check its own decoder.
  // Regenerate with
  //
  //   A11_UPDATE_GOLDENS=1 build/ctests/cpp/tests/a11_flow_test \
  //       --gtest_filter=FlowSyntaxJson.MatchesTheGoldenEveryLanguageReads
  const std::filesystem::path source_path = GoldenPath("example.flow");
  const std::string source = ReadFile(source_path);
  ASSERT_FALSE(source.empty()) << "cannot read " << source_path;

  const ParseResult result = Parse(source);
  ASSERT_TRUE(result.diagnostics.empty())
      << absl::StrJoin(Messages(result), "; ");
  const std::string generated =
      SyntaxToJson("testdata/flow/example.flow", result);

  const std::filesystem::path path = GoldenPath("syntax.json");
  if (std::getenv("A11_UPDATE_GOLDENS") != nullptr) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << "cannot write " << path;
    out << generated;
    out.close();
    GTEST_SKIP() << "rewrote " << path;
  }
  EXPECT_EQ(ReadFile(path), generated)
      << "the syntax format changed; regenerate testdata/flow/syntax.json";
}

TEST(FlowParser, ConstantFoldingIsWhatTheGrammarsConstantPositionsUse) {
  const ParseResult result =
      Parse("flow f { header \"x-a\" as a default [1, {\"k\": \"v\"}, 1.5]\n"
            " in q: string }");
  ASSERT_TRUE(result.diagnostics.empty()) << absl::StrJoin(Messages(result), "; ");
  const syntax::HeaderDeclaration& header = *result.flows[0]->headers[0];
  ASSERT_TRUE(header.has_default);
  ASSERT_EQ(header.default_value.kind, syntax::Constant::Kind::kList);
  ASSERT_EQ(header.default_value.items.size(), 3u);
  EXPECT_EQ(header.default_value.items[0].integer, 1);
  EXPECT_EQ(header.default_value.items[1].kind,
            syntax::Constant::Kind::kObject);
  EXPECT_EQ(header.default_value.items[2].kind,
            syntax::Constant::Kind::kDouble);
}

}  // namespace
}  // namespace a11::flow
