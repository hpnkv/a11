// Copyright 2026 The A11 Authors.

#include <algorithm>
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
#include "a11/flow/parser.h"
#include "a11/flow/syntax.h"
#include "absl/strings/match.h"

namespace a11::flow {
namespace {

using syntax::As;
using syntax::NodeKind;

std::vector<std::string> Codes(const ParseResult& result) {
  std::vector<std::string> codes;
  codes.reserve(result.diagnostics.size());
  for (const Diagnostic& diagnostic : result.diagnostics) {
    codes.push_back(diagnostic.code);
  }
  return codes;
}

std::vector<std::string> Messages(const ParseResult& result) {
  std::vector<std::string> messages;
  messages.reserve(result.diagnostics.size());
  for (const Diagnostic& diagnostic : result.diagnostics) {
    messages.push_back(diagnostic.message);
  }
  return messages;
}

/// The kinds of a flow's statements, which is the shape a test usually means.
std::vector<std::string> BodyKinds(const syntax::FlowDeclaration& flow) {
  std::vector<std::string> kinds;
  kinds.reserve(flow.body.size());
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

// --- the entry flow ----------------------------------------------------------

TEST(FlowParser, ANamelessFlowIsTheEntryPoint) {
  const ParseResult parsed = Parse("flow {\n  describe \"a program\"\n}\n");
  EXPECT_TRUE(Codes(parsed).empty()) << absl::StrJoin(Codes(parsed), ", ");
  ASSERT_EQ(parsed.flows.size(), 1u);
  EXPECT_TRUE(parsed.flows[0]->entry);
  EXPECT_TRUE(parsed.flows[0]->name.text.empty());
}

TEST(FlowParser, ANamedFlowIsNotAnEntryPoint) {
  const ParseResult parsed = Parse("flow named { }\n");
  ASSERT_EQ(parsed.flows.size(), 1u);
  EXPECT_FALSE(parsed.flows[0]->entry);
  EXPECT_EQ(parsed.flows[0]->name.text, "named");
}

TEST(FlowParser, AFileMayHoldBothAnEntryFlowAndNamedOnes) {
  const ParseResult parsed =
      Parse("flow helper { }\nflow {\n}\nflow other { }\n");
  EXPECT_TRUE(Codes(parsed).empty()) << absl::StrJoin(Codes(parsed), ", ");
  ASSERT_EQ(parsed.flows.size(), 3u);
  EXPECT_FALSE(parsed.flows[0]->entry);
  EXPECT_TRUE(parsed.flows[1]->entry);
  EXPECT_FALSE(parsed.flows[2]->entry);
}

TEST(FlowParser, AnUnclosedEntryFlowSaysSoWithoutQuotingAnEmptyName) {
  const ParseResult parsed = Parse("flow {\n  in x: string\n");
  ASSERT_FALSE(parsed.diagnostics.empty());
  const std::vector<std::string> messages = Messages(parsed);
  bool mentioned = false;
  for (const std::string& message : messages) {
    // "Flow '' is missing its closing" would read as a file that forgot a name.
    mentioned = mentioned || absl::StrContains(message, "entry flow");
    EXPECT_EQ(message.find("Flow ''"), std::string::npos) << message;
  }
  EXPECT_TRUE(mentioned) << absl::StrJoin(messages, " | ");
}

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

  EXPECT_EQ(BodyKinds(flow),
            (std::vector<std::string>{"nodes", "bind", "for-each", "drain",
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
  EXPECT_EQ(loop->variable().text, "hit");
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

TEST(FlowParser, LogAndLogfReadTheSameTailAsAStatementAndAsAStage) {
  const ParseResult result = Parse(
      "flow f {\n"
      "  in a: string stream\n"
      "  out b: string stream\n"
      "  if a { log warning it2 }\n"
      "  a | log | logf \"saw %s, %s\" it, a -> b\n"
      "  log \"done\" after b\n"
      "}\n");
  ASSERT_EQ(result.flows.size(), 1u);
  const syntax::FlowDeclaration& flow = *result.flows.front();

  // The statement inside the `if`: a level and a value.
  const auto* branch = As<syntax::If>(flow.body[0].get());
  ASSERT_NE(branch, nullptr);
  const auto* logged = As<syntax::Log>(branch->then_body.front().get());
  ASSERT_NE(logged, nullptr);
  EXPECT_EQ(logged->tail.level.text, "warning");
  EXPECT_FALSE(logged->tail.has_format);
  ASSERT_EQ(logged->tail.arguments.size(), 1u);

  // The two stages: `log` with nothing written, and `logf` with a format and
  // two values to fill it.
  const auto* pipe = As<syntax::Pipe>(flow.body[1].get());
  ASSERT_NE(pipe, nullptr);
  ASSERT_EQ(pipe->pipeline->stages.size(), 2u);
  const syntax::Stage& bare = *pipe->pipeline->stages[0];
  EXPECT_EQ(bare.name, "log");
  EXPECT_EQ(bare.takes, vocabulary::StageArgument::kLog);
  EXPECT_TRUE(bare.log.level.Empty());
  EXPECT_TRUE(bare.log.arguments.empty());
  const syntax::Stage& formatted = *pipe->pipeline->stages[1];
  EXPECT_EQ(formatted.name, "logf");
  EXPECT_EQ(formatted.takes, vocabulary::StageArgument::kLogFormat);
  EXPECT_TRUE(formatted.log.has_format);
  EXPECT_EQ(formatted.log.format, "saw %s, %s");
  EXPECT_EQ(formatted.log.arguments.size(), 2u);

  // A trailing `after`, as `fail` takes one.
  const auto* last = As<syntax::Log>(flow.body[2].get());
  ASSERT_NE(last, nullptr);
  ASSERT_EQ(last->after.size(), 1u);
  EXPECT_EQ(last->after[0].text, "b");
}

TEST(FlowParser, LogIsStillANameWhereTheGrammarWantsOne) {
  // Making a word open a statement makes it a keyword *there* and nowhere else.
  // A port called `log`, a pipeline sourced from it, and a step bound to the
  // name all still read as they did, because a statement word followed by `->`,
  // `|`, `=`, `<-`, `.` or `[` is a name.
  const ParseResult result = Parse(
      "flow f {\n"
      "  in a: string stream\n"
      "  out log: string stream\n"
      "  a -> log\n"
      "  log | count -> n\n"
      "}\n");
  EXPECT_TRUE(result.diagnostics.empty())
      << absl::StrJoin(Messages(result), "; ");
  ASSERT_EQ(result.flows.size(), 1u);
  const syntax::FlowDeclaration& flow = *result.flows.front();
  EXPECT_EQ(flow.ports[1]->name.text, "log");
  EXPECT_EQ(BodyKinds(flow), (std::vector<std::string>{"pipe", "pipe"}));
}

TEST(FlowParser, UnderscoreOnItsOwnIsTheDiscardAndNotAName) {
  const ParseResult result = Parse(
      "flow f {\n"
      "  in a: string stream\n"
      "  out b: string stream\n"
      "  a -> b, _\n"
      "  a | count -> _\n"
      "}\n");
  EXPECT_TRUE(result.diagnostics.empty())
      << absl::StrJoin(Messages(result), "; ");
  ASSERT_EQ(result.flows.size(), 1u);
  const syntax::FlowDeclaration& flow = *result.flows.front();
  ASSERT_EQ(flow.body.size(), 2u);
  const auto* tee = As<syntax::Pipe>(flow.body[0].get());
  ASSERT_NE(tee, nullptr);
  ASSERT_EQ(tee->targets.size(), 2u);
  EXPECT_EQ(tee->targets[0]->kind, NodeKind::kName);
  EXPECT_EQ(tee->targets[1]->kind, NodeKind::kDiscard);
  const auto* counted = As<syntax::Pipe>(flow.body[1].get());
  ASSERT_NE(counted, nullptr);
  ASSERT_EQ(counted->targets.size(), 1u);
  EXPECT_EQ(counted->targets[0]->kind, NodeKind::kDiscard);

  // Only `_` exactly: an underscore is an ordinary letter in a name, so a name
  // that merely holds one -- or starts with one -- is still a name.
  const ParseResult named = Parse(
      "flow f {\n"
      "  in _a: string stream\n"
      "  out b_c: string stream\n"
      "  _a -> b_c\n"
      "}\n");
  EXPECT_TRUE(named.diagnostics.empty())
      << absl::StrJoin(Messages(named), "; ");
  ASSERT_EQ(named.flows.size(), 1u);
  const auto* piped = As<syntax::Pipe>(named.flows.front()->body[0].get());
  ASSERT_NE(piped, nullptr);
  EXPECT_EQ(piped->targets[0]->kind, NodeKind::kName);
  EXPECT_EQ(As<syntax::Name>(piped->targets[0].get())->name, "b_c");
}

TEST(FlowParser, SkipTakesSeveralSubjectsAndACallsOutputsByName) {
  const ParseResult result = Parse(
      "flow f {\n"
      "  in  our_input: string\n"
      "  act1 = run action1(text: our_input)\n"
      "  act2 = run action2(text: our_input)\n"
      "  act3 = run action3(text: our_input)\n"
      "  act4 = run action4(text: our_input)\n"
      "  skip our_input,\n"
      "    act1,\n"
      "    (o1, o2) of act2,\n"
      "    (o1, o2 of act3),\n"
      "    act4.o1, act4.o2\n"
      "}\n");
  ASSERT_TRUE(result.diagnostics.empty())
      << absl::StrJoin(Messages(result), "; ");
  ASSERT_EQ(result.flows.size(), 1u);
  const syntax::FlowDeclaration& flow = *result.flows.front();
  ASSERT_EQ(flow.body.size(), 5u);
  const auto* skip = As<syntax::Skip>(flow.body.back().get());
  ASSERT_NE(skip, nullptr);
  ASSERT_EQ(skip->targets.size(), 6u);

  // `our_input`: an ordinary pipeline target.
  EXPECT_NE(skip->targets[0].pipeline, nullptr);
  EXPECT_TRUE(skip->targets[0].call.Empty());

  // `act1`: a bare call name, which the parser leaves as an ordinary
  // pipeline -- only the resolver knows it names a call.
  EXPECT_NE(skip->targets[1].pipeline, nullptr);
  EXPECT_EQ(As<syntax::Name>(skip->targets[1].pipeline->source.get())->name,
            "act1");

  // `(o1, o2) of act2`.
  EXPECT_EQ(skip->targets[2].pipeline, nullptr);
  EXPECT_EQ(skip->targets[2].call.text, "act2");
  ASSERT_EQ(skip->targets[2].outputs.size(), 2u);
  EXPECT_EQ(skip->targets[2].outputs[0].text, "o1");
  EXPECT_EQ(skip->targets[2].outputs[1].text, "o2");

  // `(o1, o2 of act3)`.
  EXPECT_EQ(skip->targets[3].pipeline, nullptr);
  EXPECT_EQ(skip->targets[3].call.text, "act3");
  ASSERT_EQ(skip->targets[3].outputs.size(), 2u);
  EXPECT_EQ(skip->targets[3].outputs[0].text, "o1");
  EXPECT_EQ(skip->targets[3].outputs[1].text, "o2");

  // `act4.o1`, `act4.o2`: ordinary dotted references.
  EXPECT_NE(skip->targets[4].pipeline, nullptr);
  EXPECT_EQ(skip->targets[4].pipeline->source->kind, NodeKind::kAttr);
  EXPECT_NE(skip->targets[5].pipeline, nullptr);
  EXPECT_EQ(skip->targets[5].pipeline->source->kind, NodeKind::kAttr);
}

TEST(FlowParser,
     SkipNamesAWholeOutputGroupWithNoParenthesesOnlyAsTheWholeStatement) {
  const ParseResult result = Parse(
      "flow f {\n"
      "  act = run action(text: \"x\")\n"
      "  skip o1, o2 of act\n"
      "}\n");
  ASSERT_TRUE(result.diagnostics.empty())
      << absl::StrJoin(Messages(result), "; ");
  const auto* skip = As<syntax::Skip>(result.flows[0]->body.back().get());
  ASSERT_NE(skip, nullptr);
  ASSERT_EQ(skip->targets.size(), 1u);
  EXPECT_EQ(skip->targets[0].call.text, "act");
  ASSERT_EQ(skip->targets[0].outputs.size(), 2u);
  EXPECT_EQ(skip->targets[0].outputs[0].text, "o1");
  EXPECT_EQ(skip->targets[0].outputs[1].text, "o2");
}

TEST(FlowParser, SkipStillReadsAParenthesizedPipelineAsAPlainTarget) {
  const ParseResult result = Parse(
      "flow f {\n"
      "  in  rows: string stream\n"
      "  skip (rows | count)\n"
      "}\n");
  ASSERT_TRUE(result.diagnostics.empty())
      << absl::StrJoin(Messages(result), "; ");
  const auto* skip = As<syntax::Skip>(result.flows[0]->body.back().get());
  ASSERT_NE(skip, nullptr);
  ASSERT_EQ(skip->targets.size(), 1u);
  ASSERT_NE(skip->targets[0].pipeline, nullptr);
  EXPECT_TRUE(skip->targets[0].call.Empty());
  EXPECT_EQ(skip->targets[0].pipeline->source->kind, NodeKind::kPipelineValue);
}

TEST(FlowParser, SkipStillTakesACountedSingleReference) {
  const ParseResult result = Parse(
      "flow f {\n"
      "  in  rows: string stream\n"
      "  skip 1 rows\n"
      "}\n");
  ASSERT_TRUE(result.diagnostics.empty())
      << absl::StrJoin(Messages(result), "; ");
  const auto* skip = As<syntax::Skip>(result.flows[0]->body.back().get());
  ASSERT_NE(skip, nullptr);
  ASSERT_EQ(skip->count, 1);
  ASSERT_EQ(skip->targets.size(), 1u);
  ASSERT_NE(skip->targets[0].pipeline, nullptr);
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

TEST(FlowParser, TryFrontsThreeDifferentThingsToldApartByWhatFollows) {
  // `try` is the one word in the language that fronts three shapes, and the
  // test exists because getting the lookahead off by one silently reclassified
  // every `try run` in the repository as a pipe -- `Peek(0)` is `Current()`, so
  // asking about "the next word" from 0 asks about the `try` itself.
  struct Case {
    std::string_view body;
    syntax::NodeKind kind;
  };

  for (const Case& one : {
           Case{"  x = try run t(p: a)\n  skip x\n", syntax::NodeKind::kBind},
           Case{"  try run t(p: a)\n", syntax::NodeKind::kCallStatement},
           Case{"  try call t(p: a)\n", syntax::NodeKind::kCallStatement},
           Case{"  try a -> o\n", syntax::NodeKind::kPipe},
           Case{"  try a | first 1 -> o\n", syntax::NodeKind::kPipe},
           Case{"  try { a -> o }\n", syntax::NodeKind::kBlock},
       }) {
    const ParseResult result =
        Parse(absl::StrCat("flow f {\n  in a: string stream\n"
                           "  out o: string stream\n",
                           one.body, "}\n"));
    ASSERT_TRUE(result.diagnostics.empty())
        << one.body << " gave " << absl::StrJoin(Messages(result), "; ");
    ASSERT_EQ(result.flows.size(), 1u) << one.body;
    ASSERT_FALSE(result.flows.front()->body.empty()) << one.body;
    EXPECT_EQ(result.flows.front()->body.front()->kind, one.kind)
        << one.body << " read as "
        << syntax::NodeKindName(result.flows.front()->body.front()->kind);
  }
}

TEST(FlowParser, ALoopMayBeNamedAndMayCarryAnAfter) {
  const ParseResult result = Parse(
      "flow f {\n  in w: string stream\n  out o: string stream\n"
      "  taken = node()\n  first = run t()\n"
      "  done = for x in w { x -> taken } after first\n"
      "  drain taken after done\n  taken -> o\n  skip first\n}\n");
  ASSERT_TRUE(result.diagnostics.empty())
      << absl::StrJoin(Messages(result), "; ");
  ASSERT_EQ(result.flows.size(), 1u);
  // The bind, whose value is the loop -- not a call, which is what `name = for`
  // used to be misread as.
  const syntax::Node* bound = result.flows.front()->body[2].get();
  ASSERT_EQ(bound->kind, syntax::NodeKind::kBind);
  const auto* bind = syntax::As<syntax::Bind>(bound);
  ASSERT_NE(bind->value, nullptr);
  ASSERT_EQ(bind->value->kind, syntax::NodeKind::kForEach);
  const auto* loop = syntax::As<syntax::ForEach>(bind->value.get());
  ASSERT_EQ(loop->after.size(), 1u);
  EXPECT_EQ(loop->after.front().text, "first");
}

TEST(FlowParser, InsideBracketsALineBreakIsWhitespace) {
  // A break inside `{ }`, `[ ]` or `( )` ends nothing -- the closing bracket is
  // what ends it -- so an operator may begin the next line.
  //
  // This used to be true only straight after a `,`, because the loops reading a
  // comma-separated list skip newlines themselves: `{"a": 1,\n "b": 2}` parsed
  // and `{"a": x\n or y}` was `Expected }, found 'or'`. One rule now, and it
  // matters most for a `scan` carrying a record, which is where the language's
  // longest expressions are.
  for (const std::string_view wrapped : {
           "  l | map {\"a\": starts-with(it, \"B\")\n"
           "       or ends-with(it, \"E\")} -> o\n",
           "  l | map {\"a\": len(it) > 1\n       and len(it) < 9} -> o\n",
           "  l | map {\"a\": len(it)\n       > 1} -> o\n",
           "  l | map {\"a\": len(it)\n       + 1} -> o\n",
           "  l | map [len(it) > 1\n       and len(it) < 9] -> o\n",
           "  l | map (len(it)\n       + 1) -> o\n",
       }) {
    const ParseResult result =
        Parse(absl::StrCat("flow f {\n  in l: string stream\n"
                           "  out o: json stream\n",
                           wrapped, "}\n"));
    EXPECT_TRUE(result.diagnostics.empty())
        << wrapped << " gave " << absl::StrJoin(Messages(result), "; ");
  }
}

TEST(FlowParser, OutsideBracketsALineBreakStillEndsTheStatement) {
  // The property the change above must not cost. A break outside brackets ends
  // the statement, which is what stops a `where` on the line below from being
  // read as a continuation of the pipe above it -- so an operator alone on the
  // next line is still an error rather than a continuation.
  const ParseResult result = Parse(
      "flow f {\n  in l: string stream\n  out o: string stream\n"
      "  l where starts-with(it, \"B\")\n"
      "  or ends-with(it, \"E\") -> o\n}\n");
  EXPECT_FALSE(result.diagnostics.empty());
}

TEST(FlowParser, AStringWithSomethingAfterItIsAStatementAndNotADescription) {
  // What makes the form above unambiguous: a description is *alone* on its
  // line.
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
  // The property the whole recovering design exists for: a mistake in the
  // middle of a flow does not hide what is around it.
  const ParseResult result = Parse(
      "flow f {\n"
      "  in  a: string\n"
      "  out b: string\n"
      "  a | wat -> b\n"
      "  a -> b\n"
      "}\n");
  EXPECT_EQ(Codes(result),
            (std::vector<std::string>{"flow.form.unknown-stage"}));
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
      {"flow f { in a: string\n a | first -> b }", "flow.form.stage-argument",
       "Expected a count for 'first'"},
      // The tail a stage may carry, and what each part of it needs.
      {"flow f { in w: string stream\n out o: string stream\n"
       " w | collect parallel 4 -> o }",
       "flow.form.stage-not-parallel", "nothing for 'parallel' to run at once"},
      {"flow f { in w: string stream\n out o: string stream\n"
       " w | map it unordered -> o }",
       "flow.form.unordered-without-parallel", "needs 'parallel n'"},
      {"flow f { in w: string stream\n out o: string stream\n out b: json "
       "stream\n w | map it into b -> o }",
       "flow.form.into-without-try", "has to be a 'try'"},
      {"flow f { in w: number stream\n out o: number\n"
       " w | fold it as total, total + it -> o }",
       "flow.form.fold-start", "starts from a literal"},
      {"flow f { in w: number stream\n out o: number\n"
       " w | fold 0, it -> o }",
       "flow.form.fold-name", "names what it carries"},
      {"flow f { header \"x-a\" default a }", "flow.syntax.constant-required",
       "Expected a constant value."},
      {"# nothing here\n", "flow.syntax.unexpected",
       "A flow file must declare at least one flow or struct."},
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
      "",
      "}",
      "{",
      "flow",
      "flow f",
      "flow f {",
      "flow f {\n a",
      "flow f { a -> }",
      "flow f { a | }",
      "flow f { a( }",
      "flow f { in }",
      "flow f { in a: }",
      "flow f { for }",
      "flow f { if { } }",
      "flow f { repeat }",
      "flow f { x = }",
      "flow f { \"s\" }",
      "flow f { a -> b, }",
      "flow f { {\"a\": } }",
      ",,,,",
      "flow f { a | then }",
      "flow f { status }",
      "flow f { a[ }",
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

// A block and an `abort` are statements like any other, and both were once
// missing from the switches that name a node and write one out: the envelope
// carried a `block` with no body at all, and `abort` came back as "error".
// Deep nesting is a diagnostic, not a stack overflow: the parse is recursive
// descent and A11's fibres have small fixed stacks, so the depth a document can
// reach has to be a constant of the parser rather than a property of the input.
TEST(FlowParser, RefusesToDescendPastItsNestingBound) {
  std::string deep = "flow f {\n  out o: json\n  ";
  deep.append(400, '[');
  deep.append(400, ']');
  deep += " -> o\n}\n";
  const ParseResult result = Parse(deep);
  const std::vector<std::string> codes = Codes(result);
  EXPECT_NE(
      std::find(codes.begin(), codes.end(), "flow.syntax.nesting-too-deep"),
      codes.end())
      << absl::StrJoin(codes, ", ");
  // Once, not once per level.
  EXPECT_EQ(
      std::count(codes.begin(), codes.end(), "flow.syntax.nesting-too-deep"),
      1);
}

TEST(FlowParser, TheEnvelopeCarriesABlockBodyAndNamesAnAbort) {
  const ParseResult result = Parse(
      "flow f { in a: string stream\n out o: string stream\n"
      " try { a -> o\n abort o }\n}\n");
  ASSERT_TRUE(result.diagnostics.empty())
      << absl::StrJoin(Messages(result), "; ");
  const nlohmann::json value = SyntaxToJsonValue("-", result);
  const nlohmann::json& block = value["flows"][0]["body"][0];
  EXPECT_EQ(block["kind"], "block");
  EXPECT_EQ(block["tolerant"], true);
  ASSERT_EQ(block["body"].size(), 2u);
  EXPECT_EQ(block["body"][0]["kind"], "pipe");
  EXPECT_EQ(block["body"][1]["kind"], "abort");
}

TEST(FlowParser, ADurationIsTaggedSoItIsNotReadAsACount) {
  const ParseResult result = Parse(
      "flow f { in a: string\n x = run t(a: a) timeout 250ms\n"
      " x.o -> a }");
  const nlohmann::json value = SyntaxToJsonValue("-", result);
  const nlohmann::json& modifiers =
      value["flows"][0]["body"][0]["value"]["modifiers"];
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
  if (!stream.is_open()) {
    return "";
  }
  std::stringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

TEST(FlowSyntaxJson, MatchesTheGoldenEveryLanguageReads) {
  // `testdata/flow/syntax.json` is the syntax format pinned against one small
  // flow, in the same spirit as `testdata/flow/codes.json`: the C++ owns it,
  // and a frontend in another language reads it back to check its own decoder.
  // Regenerate with
  //
  //   A11_UPDATE_GOLDENS=1 build/ctests/cpp/tests/a11_flow_test
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
  const ParseResult result = Parse(
      "flow f { header \"x-a\" as a default [1, {\"k\": \"v\"}, 1.5]\n"
      " in q: string }");
  ASSERT_TRUE(result.diagnostics.empty())
      << absl::StrJoin(Messages(result), "; ");
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

/// The first node of a flow's body, for a test that only wants the expression.
const syntax::Node* absl_nullable FirstStatement(const ParseResult& result) {
  if (result.flows.empty() || result.flows[0]->body.empty()) {
    return nullptr;
  }
  return result.flows[0]->body[0].get();
}

TEST(FlowParser, StringsWrittenNextToEachOtherAreOneString) {
  // Prose that says anything outgrows the line it is written on, and `+` at run
  // time is the wrong tool for something that is a constant.
  const ParseResult result = Parse(
      "flow f {\n"
      "  describe \"one \" \"two \" \"three\"\n"
      "  in  a: string required \"first \" \"second\"\n"
      "  out b: string\n"
      "  \"x \" \"y\" -> b\n"
      "}\n");
  ASSERT_TRUE(Messages(result).empty())
      << absl::StrJoin(Messages(result), "; ");
  EXPECT_EQ(result.flows[0]->description, "one two three");
  EXPECT_EQ(result.flows[0]->ports[0]->description, "first second");

  const auto* pipe = As<syntax::Pipe>(FirstStatement(result));
  ASSERT_NE(pipe, nullptr);
  const auto* literal = As<syntax::Literal>(pipe->pipeline->source.get());
  ASSERT_NE(literal, nullptr);
  EXPECT_EQ(literal->value.text, "x y");
}

TEST(FlowParser, ADescriptionOnItsOwnLineMayBeARunToo) {
  const ParseResult result = Parse(
      "flow f {\n"
      "  in a: string required\n"
      "    \"first \" \"second\"\n"
      "  out b: string\n"
      "  a -> b\n"
      "}\n");
  ASSERT_TRUE(Messages(result).empty())
      << absl::StrJoin(Messages(result), "; ");
  EXPECT_EQ(result.flows[0]->ports[0]->description, "first second");
}

TEST(FlowParser, ALiteralMaySpreadAnotherIntoItself) {
  const ParseResult result = Parse(
      "flow f {\n"
      "  in a: json stream required\n"
      "  out b: json stream\n"
      "  a | map {...it, \"tag\": 1} -> b\n"
      "  a | map [...it, 2] -> b\n"
      "}\n");
  ASSERT_TRUE(Messages(result).empty())
      << absl::StrJoin(Messages(result), "; ");

  const auto* pipe = As<syntax::Pipe>(FirstStatement(result));
  ASSERT_NE(pipe, nullptr);
  const auto* object =
      As<syntax::ObjectLiteral>(pipe->pipeline->stages[0]->argument.get());
  ASSERT_NE(object, nullptr);
  ASSERT_EQ(object->pairs.size(), 2u);
  // A spread has no key: what it brings in keeps its own.
  EXPECT_TRUE(object->pairs[0].first.empty());
  const auto* spread = As<syntax::Spread>(object->pairs[0].second.get());
  ASSERT_NE(spread, nullptr);
  EXPECT_EQ(spread->value->kind, NodeKind::kIt);
  EXPECT_EQ(object->pairs[1].first, "tag");

  // `...` is the same thing as `...`, in a list as in an object.
  const auto* second = As<syntax::Pipe>(result.flows[0]->body[1].get());
  ASSERT_NE(second, nullptr);
  const auto* list =
      As<syntax::ListLiteral>(second->pipeline->stages[0]->argument.get());
  ASSERT_NE(list, nullptr);
  ASSERT_EQ(list->items.size(), 2u);
  EXPECT_EQ(list->items[0]->kind, NodeKind::kSpread);
}

TEST(FlowParser, ASpreadOfConstantsIsStillAConstant) {
  // A header's default has to be a constant, and splicing values that are all
  // known here is folding rather than running anything. A later key wins, so
  // the result is a mapping and not a list of pairs with a duplicate in it.
  const ParseResult result = Parse(
      "flow f {\n"
      "  header \"x-a\" as a default {...{\"p\": 1, \"q\": 2}, \"q\": 3}\n"
      "  header \"x-b\" as b default [...[1, 2], 3]\n"
      "  in x: string required\n  out y: string\n  x -> y\n}\n");
  ASSERT_TRUE(Messages(result).empty())
      << absl::StrJoin(Messages(result), "; ");
  const syntax::Constant& object = result.flows[0]->headers[0]->default_value;
  ASSERT_EQ(object.pairs.size(), 2u);
  EXPECT_EQ(object.pairs[0].first, "p");
  EXPECT_EQ(object.pairs[1].first, "q");
  EXPECT_EQ(object.pairs[1].second.integer, 3);
  EXPECT_EQ(result.flows[0]->headers[1]->default_value.items.size(), 3u);
}

TEST(FlowParser, ATypeMayBeWrittenWithTrailingBrackets) {
  const ParseResult result = Parse(
      "flow f {\n  in a: string[] required\n  in b: list[string] required\n"
      "  in c: a11.Chunk[][] required\n  out d: string\n  \"\" -> d\n}\n");
  ASSERT_TRUE(Messages(result).empty())
      << absl::StrJoin(Messages(result), "; ");
  const syntax::TypeExpression& sugared = result.flows[0]->ports[0]->type;
  const syntax::TypeExpression& spelled = result.flows[0]->ports[1]->type;
  EXPECT_EQ(sugared.name, "list");
  EXPECT_EQ(sugared.parameters[0].name, "string");
  EXPECT_EQ(spelled.name, "list");
  // The same type; only how it reads back differs, so a file formatted twice
  // says what its author wrote.
  EXPECT_TRUE(sugared.sugared);
  EXPECT_FALSE(spelled.sugared);
  EXPECT_EQ(sugared.ToString(), "string[]");
  EXPECT_EQ(spelled.ToString(), "list[string]");
  // Each `[]` wraps what was read so far, so a list of lists is two of them.
  EXPECT_EQ(result.flows[0]->ports[2]->type.ToString(), "a11.Chunk[][]");
}

TEST(FlowParser, ReadsADtoBesideAFlow) {
  const ParseResult result = Parse(
      "struct S {\n  describe \"a shape\"\n"
      "  a: string required matching \"^x\" \"why\"\n"
      "  b: number 0..1 default 0.5\n"
      "  c: string one of [\"p\", \"q\"]\n}\n"
      "flow f {\n  in x: S required\n  out y: string\n  x.a -> y\n}\n");
  ASSERT_TRUE(Messages(result).empty())
      << absl::StrJoin(Messages(result), "; ");
  ASSERT_EQ(result.dtos.size(), 1u);
  ASSERT_EQ(result.flows.size(), 1u);

  const syntax::DtoDeclaration& shape = *result.dtos[0];
  EXPECT_EQ(shape.name.text, "S");
  EXPECT_EQ(shape.description, "a shape");
  ASSERT_EQ(shape.fields.size(), 3u);
  // A keyword's quoted argument is one literal, so the description after it is
  // still a description rather than more of the pattern.
  EXPECT_EQ(shape.fields[0]->pattern, "^x");
  EXPECT_EQ(shape.fields[0]->description, "why");
  EXPECT_TRUE(shape.fields[1]->has_default);
  EXPECT_EQ(shape.fields[2]->enumeration.size(), 2u);
}

TEST(FlowParser, AFileMayDeclareOnlyShapes) {
  const ParseResult result = Parse("struct S {\n  a: string required\n}\n");
  EXPECT_TRUE(Messages(result).empty())
      << absl::StrJoin(Messages(result), "; ");
  EXPECT_EQ(result.dtos.size(), 1u);
  // And a file that declares nothing at all still says so.
  EXPECT_FALSE(Messages(Parse("x -> y\n")).empty());
}

TEST(FlowParser, TellsABlockFromARecordAtTheHeadOfAStatement) {
  // Both are statements and both begin with `{`. A record's keys are strings
  // followed by `:`, and a spread is only ever a record's; anything else opens
  // statements. Getting this wrong turns `{"a": 1} -> out` into a block, which
  // is how the values tests found it.
  const auto kind = [](std::string_view source) {
    const ParseResult result = Parse(source);
    if (result.flows.empty() || result.flows.front()->body.empty()) {
      return "none";
    }
    return syntax::NodeKindName(result.flows.front()->body.front()->kind)
        .data();
  };
  EXPECT_STREQ(kind("flow f {\n  out o: json\n  {\"a\": 1} -> o\n}\n"), "pipe");
  EXPECT_STREQ(kind("flow f {\n  out o: json\n  {} -> o\n}\n"), "pipe");
  EXPECT_STREQ(kind("flow f {\n  in i: json\n  out o: json\n"
                    "  {…i, \"a\": 1} -> o\n}\n"),
               "pipe");
  // A block whose first statement writes a string starts the same way and is
  // not a record, because no `:` follows.
  EXPECT_STREQ(kind("flow f {\n  out o: string\n  { \"one\" -> o }\n}\n"),
               "block");
  EXPECT_STREQ(kind("flow f {\n  out o: string\n  { skip o }\n}\n"), "block");
  EXPECT_STREQ(kind("flow f {\n  out o: string\n  try { skip o }\n}\n"),
               "block");
}

}  // namespace
}  // namespace a11::flow
