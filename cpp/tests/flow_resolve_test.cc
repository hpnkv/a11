// Copyright 2026 The A11 Authors.

#include "a11/flow/resolve.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <gtest/gtest.h>

#include "a11/flow/parser.h"

namespace a11::flow {
namespace {

ResolveResult Check(std::string_view source) {
  return Resolve(source, Parse(source));
}

std::vector<std::string> Codes(const ResolveResult& result) {
  std::vector<std::string> codes;
  for (const Diagnostic& diagnostic : result.diagnostics) {
    codes.push_back(diagnostic.code);
  }
  return codes;
}

std::string Messages(const ResolveResult& result) {
  std::vector<std::string> messages;
  for (const Diagnostic& diagnostic : result.diagnostics) {
    messages.push_back(diagnostic.message);
  }
  return absl::StrJoin(messages, "; ");
}

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

TEST(FlowResolve, ResolvesAFlowIntoWhatACallerSees) {
  const ResolveResult result = Check(
      "flow research {\n"
      "  describe \"Search and answer.\"\n"
      "  in  question: string required \"What to find out.\"\n"
      "  out answer:   string\n"
      "  out sources:  string stream\n"
      "  header \"x-a11-deadline\" as deadline\n"
      "\n"
      "  nodes fetched {\n"
      "    search = run web-search(query: question) timeout 30s\n"
      "  }\n"
      "  for hit in search.hits parallel 2 {\n"
      "    hit.url -> sources\n"
      "  }\n"
      "  search.summary -> answer\n"
      "}\n");
  ASSERT_TRUE(result.diagnostics.empty()) << Messages(result);
  ASSERT_EQ(result.program.flows.size(), 1u);
  const FlowPlan& plan = result.program.flows[0];

  EXPECT_EQ(plan.name, "research");
  EXPECT_EQ(plan.description, "Search and answer.");
  ASSERT_EQ(plan.ports.size(), 3u);
  EXPECT_EQ(plan.ports[0].name, "question");
  EXPECT_EQ(plan.ports[0].declared, "string");
  EXPECT_TRUE(plan.ports[0].required);
  EXPECT_EQ(plan.ports[0].description, "What to find out.");
  EXPECT_FALSE(plan.ports[2].unary);
  ASSERT_EQ(plan.headers.size(), 1u);
  EXPECT_EQ(plan.headers[0].alias, "deadline");
  EXPECT_EQ(plan.node_maps, (std::vector<std::string>{"fetched"}));

  // The steps, in the order the flow reads: the call, the pipe feeding it, the
  // loop with its body, and the pipe out.
  ASSERT_EQ(plan.steps.size(), 4u);
  EXPECT_EQ(plan.steps[0].kind, "run");
  EXPECT_EQ(plan.steps[0].label, "search");
  EXPECT_EQ(plan.steps[0].action, "web-search");
  EXPECT_EQ(plan.steps[0].mode, "run");
  EXPECT_EQ(plan.steps[0].node_map, "fetched");
  EXPECT_EQ(plan.steps[0].timeout, absl::Seconds(30));
  EXPECT_EQ(plan.steps[1].kind, "pipe");
  EXPECT_EQ(plan.steps[1].destination, "search.query");
  EXPECT_EQ(plan.steps[2].kind, "for");
  ASSERT_EQ(plan.steps[2].bodies.size(), 1u);
  ASSERT_EQ(plan.steps[2].bodies[0].size(), 1u);
  EXPECT_EQ(plan.steps[2].bodies[0][0].label, "hit.url -> sources");
  EXPECT_EQ(plan.steps[3].source, "search.summary");
}

TEST(FlowResolve, ChecksACallsPortsAgainstTheFlowItNames) {
  // Whichever way round they are written: a program is not a header file.
  for (const bool callee_first : {true, false}) {
    const std::string inner =
        "flow inner { in a: string\n out b: string\n a -> b }\n";
    const std::string outer =
        "flow outer { in a: string\n out b: string\n"
        "  x = run inner(nope: a)\n  x.b -> b }\n";
    const ResolveResult result =
        Check(callee_first ? inner + outer : outer + inner);
    EXPECT_EQ(Codes(result),
              (std::vector<std::string>{"flow.name.unknown-port"}));
    EXPECT_NE(Messages(result).find("has no input port 'nope'"),
              std::string::npos)
        << Messages(result);
  }
}

TEST(FlowResolve, SkipACallExpandsToEveryDeclaredOutputOfASiblingFlow) {
  const std::string inner =
      "flow inner { in a: string\n out b: string\n out c: string\n"
      "  a -> b\n a -> c }\n";
  const std::string outer =
      "flow outer { in a: string\n"
      "  x = run inner(a: a)\n  skip x }\n";
  const ResolveResult result = Check(inner + outer);
  ASSERT_TRUE(result.diagnostics.empty()) << Messages(result);
  ASSERT_EQ(result.program.flows.size(), 2u);
  const FlowPlan& plan = result.program.flows[1];
  // The call, the pipe feeding it, then one 'skip' per declared output.
  ASSERT_EQ(plan.steps.size(), 4u);
  EXPECT_EQ(plan.steps[2].kind, "skip");
  EXPECT_EQ(plan.steps[2].source, "x.b");
  EXPECT_EQ(plan.steps[3].kind, "skip");
  EXPECT_EQ(plan.steps[3].source, "x.c");
}

TEST(FlowResolve, SkipACallWhoseRealPortsAreUnknownHereStillResolves) {
  // `unknown-action` names nothing this program declares, so the resolver has
  // no port list to expand -- the same situation as an action from a registry.
  const ResolveResult result = Check(
      "flow f { in a: string\n"
      "  x = run unknown-action(a: a)\n  skip x }\n");
  ASSERT_TRUE(result.diagnostics.empty()) << Messages(result);
  const FlowPlan& plan = result.program.flows[0];
  ASSERT_EQ(plan.steps.size(), 3u);
  EXPECT_EQ(plan.steps[2].kind, "skip");
  EXPECT_EQ(plan.steps[2].label, "x");
  EXPECT_EQ(plan.steps[2].source, "x");
}

TEST(FlowResolve, SkipNamedOutputsOfACallReusesThePortValidationOfADottedReference) {
  const std::string inner =
      "flow inner { in a: string\n out b: string\n a -> b }\n";
  const std::string outer =
      "flow outer { in a: string\n"
      "  x = run inner(a: a)\n  skip nope of x }\n";
  const ResolveResult result = Check(inner + outer);
  EXPECT_EQ(Codes(result), (std::vector<std::string>{"flow.name.unknown-port"}));
  EXPECT_NE(Messages(result).find("has no output port 'nope'"),
            std::string::npos)
      << Messages(result);
}

TEST(FlowResolve, SkipOfACallRequiresACall) {
  const ResolveResult result =
      Check("flow f { in a: string\n  skip nope of a }\n");
  EXPECT_EQ(Codes(result), (std::vector<std::string>{"flow.name.not-a-call"}));
}

TEST(FlowResolve, SkipCombinesAPortAndACallsOutputsInOneStatement) {
  const std::string inner =
      "flow inner { in a: string\n out b: string\n a -> b }\n";
  const std::string outer =
      "flow outer { in a: string\n out done: string\n"
      "  x = run inner(a: a)\n"
      "  skip a,\n"
      "    (b) of x\n"
      "  \"done\" -> done }\n";
  const ResolveResult result = Check(inner + outer);
  ASSERT_TRUE(result.diagnostics.empty()) << Messages(result);
  const FlowPlan& plan = result.program.flows[1];
  // The call, the pipe feeding it, the skip of 'a', the skip of 'x.b', the
  // final pipe.
  ASSERT_EQ(plan.steps.size(), 5u);
  EXPECT_EQ(plan.steps[2].kind, "skip");
  EXPECT_EQ(plan.steps[2].source, "a");
  EXPECT_EQ(plan.steps[3].kind, "skip");
  EXPECT_EQ(plan.steps[3].source, "x.b");
}

TEST(FlowResolve, AFlowIsInItsOwnProgramSoItMayCallItself) {
  const ResolveResult good = Check(
      "flow loop { in a: string\n out b: string\n"
      "  x = run loop(a: a)\n  x.b -> b }\n");
  EXPECT_TRUE(good.diagnostics.empty()) << Messages(good);

  const ResolveResult bad = Check(
      "flow loop { in a: string\n out b: string\n"
      "  x = run loop(nope: a)\n  x.b -> b }\n");
  EXPECT_EQ(Codes(bad), (std::vector<std::string>{"flow.name.unknown-port"}));
}

TEST(FlowResolve, SaysWhatIsWrongInTheWordsThePythonCompilerUses) {
  struct Case {
    std::string_view source;
    std::string_view code;
    std::string_view message;
  };
  // Every message here is `a11/flow/plan.py`'s, because the suite and the
  // documentation already quote them.
  const Case cases[] = {
      {"flow f { out a: string\n missing -> a }", "flow.name.unknown",
       "Unknown name 'missing'"},
      {"flow f { in a: string\n out b: string\n a -> a }",
       "flow.name.not-writable", "cannot be written by this flow"},
      {"flow f { in a: string\n wait nobody }", "flow.name.unknown",
       "Unknown name 'nobody'"},
      {"flow f { in a: string\n s <- a }",
       "flow.barrier.carry-outside-repeat", "no repeat here"},
      {"flow f { in a: string\n until a }",
       "flow.barrier.until-outside-repeat", "no repeat here"},
      {"flow f { in a: string\n repeat s = 1 { until a\n while a } }",
       "flow.barrier.duplicate-until", "already has a stop condition"},
      {"flow f { in a: string\n repeat s = 1 { s <- a\n s <- a } }",
       "flow.barrier.duplicate-carry", "is already carried"},
      {"flow f { in a: string\n repeat s = 1 { t <- a } }",
       "flow.barrier.wrong-carry", "This repeat carries 's', not 't'."},
      {"flow f { in a: wat }", "flow.form.unknown-type",
       "Unknown type 'wat'"},
      {"flow f { in a: list[string, object] }", "flow.form.unknown-type",
       "type parameter"},
      {"flow f { in a: string\n in a: string }", "flow.form.duplicate-port",
       "declared twice"},
      {"flow one { in a: string }\nflow one { in a: string }",
       "flow.form.duplicate-flow", "Flow 'one' is declared twice."},
      {"flow f { out a: bool\n True -> a }", "flow.name.unknown",
       "Unknown name 'True'"},
      {"flow f { out a: string\n fail wat \"x\" }",
       "flow.form.unknown-status-code", "Unknown status code 'wat'"},
      {"flow f { out o: string\n kept = node() in nowhere\n \"x\" -> o }",
       "flow.name.unknown-node-map", "Unknown node map 'nowhere'"},
      {"flow f { in a: string\n x = run t(p: a)\n x -> a }",
       "flow.name.call-as-stream", "name one of its ports"},
      {"flow f { in a: string\n out b: string\n x = run t(p: a)\n x | count -> b }",
       "flow.name.call-as-stream", "name one of its ports"},
      {"flow f { in a: string\n out b: string\n nodes m\n m -> b }",
       "flow.name.not-a-stream", "is a node map, not a stream"},
      {"flow f { in a: string\n out b: string\n b | count -> a }",
       "flow.name.not-a-stream", "is written by this flow, not read"},
      {"flow f { in a: string\n cancel a }", "flow.name.not-a-call",
       "'a' is not a call."},
      {"flow f { in a: string\n out b: string\n a | where it -> b\n it -> b }",
       "flow.name.it-outside-stage", "there is none here"},
      {"flow f { header \"x-h\" as h\n out b: string\n status h -> b }",
       "flow.name.no-status", "'h' has no status"},
      {"flow f { in w: string stream\n for word in w { skip 1 word } }",
       "flow.sequence.skip-count-target", "takes a port or a node"},
      {"flow f { in a: string\n out b: string\n x = run t(p: a)\n a -> x }",
       "flow.name.call-as-stream", "name the port to write"},
      {"flow f { in a: string\n x = wait a\n a -> x }",
       "flow.name.not-writable", "is a barrier, not somewhere to write"},
      {"flow f { in a: string\n x = run t(p: a)\n x = run t(p: a) }",
       "flow.name.taken", "is already taken in this scope"},
      {"flow f { in a: string\n repeat s = a { until a } }",
       "flow.syntax.constant-required", "Expected a constant value here"},
  };
  for (const Case& one : cases) {
    const ResolveResult result = Check(one.source);
    bool found = false;
    for (const Diagnostic& diagnostic : result.diagnostics) {
      if (diagnostic.code == one.code &&
          diagnostic.message.find(one.message) != std::string::npos) {
        found = true;
      }
    }
    EXPECT_TRUE(found) << one.source << " gave " << Messages(result);
  }
}

TEST(FlowResolve, EveryCodeItProducesIsPublished) {
  const std::string_view sources[] = {
      "flow f { out a: string\n missing -> a }",
      "flow f { in a: wat }",
      "flow f { in a: string\n s <- a }",
      "flow f { in a: string\n cancel a }",
      "flow f { in a: string\n x = run t(p: a)\n x -> a }",
      "flow f { in w: string stream\n for word in w { skip 1 word } }",
      "flow f { out a: string\n fail wat \"x\" }",
      "flow one { in a: string }\nflow one { in a: string }",
  };
  for (const std::string_view source : sources) {
    for (const Diagnostic& diagnostic : Check(source).diagnostics) {
      EXPECT_NE(FindCode(diagnostic.code), nullptr)
          << diagnostic.code << " from " << source;
    }
  }
}

TEST(FlowResolve, ReportsEveryProblemRatherThanTheFirst) {
  const ResolveResult result = Check(
      "flow f {\n"
      "  in  a: string\n"
      "  out b: string\n"
      "  missing -> b\n"
      "  alsomissing -> b\n"
      "  cancel a\n"
      "}\n");
  EXPECT_EQ(Codes(result),
            (std::vector<std::string>{"flow.name.unknown", "flow.name.unknown",
                                      "flow.name.not-a-call",
                                      "flow.form.unconditional-cancel"}));
}

TEST(FlowResolve, AFailOrCancelAtTheTopOfABodyIsRefused) {
  // These take no input and wait for nothing, so at the top of a flow's body
  // they run at once and race every other statement: read top to bottom they
  // look like a last resort, and they are the first thing that happens.
  EXPECT_EQ(Codes(Check("flow f {\n  out b: string\n"
                        "  fail internal \"no\"\n}\n")),
            (std::vector<std::string>{"flow.form.unconditional-fail"}));

  // An `if` makes it conditional on something, which is the usual fix.
  EXPECT_TRUE(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                          "  if a == \"\" { fail invalid_argument \"empty\" }\n"
                          "  a -> b\n}\n"))
                  .empty());

  // So does a loop body, and so does an `after`, which is the author saying in
  // the source what it is waiting for.
  EXPECT_TRUE(Codes(Check("flow f {\n  in a: string stream\n  out b: string\n"
                          "  for one in a { fail internal \"no\" }\n"
                          "  a | first 1 -> b\n}\n"))
                  .empty());
  EXPECT_TRUE(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                          "  x = run act(p: a)\n  x.out -> b\n"
                          "  fail internal \"no\" after x\n}\n"))
                  .empty());

  // A `nodes` block is not one of these: it joins the body around it, and does
  // not change when anything in it runs.
  EXPECT_EQ(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                        "  nodes m {\n    cancel a\n  }\n  a -> b\n}\n")),
            (std::vector<std::string>{"flow.name.not-a-call",
                                      "flow.form.unconditional-cancel"}));
}

TEST(FlowResolve, ALogAtTheTopOfABodyIsRefused) {
  // Same rule as `fail` and `cancel`, and for a reason of its own: a log says
  // what just happened, so one that runs before anything else describes
  // something that has not happened yet.
  EXPECT_EQ(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                        "  log \"starting\"\n  a -> b\n}\n")),
            (std::vector<std::string>{"flow.form.unconditional-log"}));
  EXPECT_EQ(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                        "  logf \"starting %s\" a\n  a -> b\n}\n")),
            (std::vector<std::string>{"flow.form.unconditional-logf"}));

  // An `if`, a loop body and an `after` each say when it happens.
  EXPECT_TRUE(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                          "  if a == \"\" { log warning \"empty\" }\n"
                          "  a -> b\n}\n"))
                  .empty());
  EXPECT_TRUE(Codes(Check("flow f {\n  in a: string stream\n  out b: string\n"
                          "  for one in a { log one }\n"
                          "  a | first 1 -> b\n}\n"))
                  .empty());
  EXPECT_TRUE(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                          "  x = run act(p: a)\n  x.out -> b\n"
                          "  log \"done\" after x\n}\n"))
                  .empty());

  // As a stage it is part of a pipeline rather than a statement of its own, so
  // the rule has nothing to say about it.
  EXPECT_TRUE(Codes(Check("flow f {\n  in a: string stream\n"
                          "  out b: string stream\n"
                          "  a | log | logf \"saw %s\" it -> b\n}\n"))
                  .empty());
}

TEST(FlowResolve, ALogNamesALevelOrNothing) {
  EXPECT_EQ(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                        "  if a { log chatty \"hm\" }\n  a -> b\n}\n")),
            (std::vector<std::string>{"flow.form.unknown-log-level"}));
  // A level is only read where one may stand, so `error` after it is the value.
  EXPECT_TRUE(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                          "  if a { log error a }\n  a -> b\n}\n"))
                  .empty());
  // Written with nothing to log, a statement says so rather than logging the
  // level word.
  EXPECT_EQ(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                        "  if a { log warning }\n  a -> b\n}\n")),
            (std::vector<std::string>{"flow.form.log-value"}));
  // A `logf` takes a format, not a value.
  EXPECT_EQ(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                        "  if a { logf a }\n  a -> b\n}\n")),
            (std::vector<std::string>{"flow.form.log-format"}));
}

TEST(FlowResolve, ARaceIsAValueAndWaitingForEveryoneIsNot) {
  // Which of them won is a number, so it goes where a number goes.
  EXPECT_TRUE(Codes(Check("flow f {\n  in a: string\n  out n: int\n"
                          "  x = call other(a: a)\n  y = call other(a: a)\n"
                          "  wait first of x, y -> n\n}\n"
                          "flow other {\n  in a: string\n  out o: string\n"
                          "  a -> o\n}\n"))
                  .empty());
  EXPECT_TRUE(Codes(Check("flow f {\n  in a: string\n  out n: int\n"
                          "  x = call other(a: a)\n  y = call other(a: a)\n"
                          "  let w = wait first of x, y\n  w -> n\n}\n"
                          "flow other {\n  in a: string\n  out o: string\n"
                          "  a -> o\n}\n"))
                  .empty());
  // `wait all of` has no winner, so neither form is a value.
  EXPECT_EQ(Codes(Check("flow f {\n  in a: string\n  out n: int\n"
                        "  x = call other(a: a)\n  y = call other(a: a)\n"
                        "  wait all of x, y -> n\n}\n"
                        "flow other {\n  in a: string\n  out o: string\n"
                        "  a -> o\n}\n")),
            (std::vector<std::string>{"flow.form.wait-all-has-no-value"}));
  EXPECT_EQ(Codes(Check("flow f {\n  in a: string\n  out n: int\n"
                        "  x = call other(a: a)\n  y = call other(a: a)\n"
                        "  let w = wait all of x, y\n  w -> n\n}\n"
                        "flow other {\n  in a: string\n  out o: string\n"
                        "  a -> o\n}\n")),
            (std::vector<std::string>{"flow.form.wait-all-has-no-value"}));
}

TEST(FlowResolve, ALogStageChangesNothingAboutTheStream) {
  // A log is a pass-through, so what the pipeline was carrying it still carries:
  // the shape survives, and so does the proof that there is one value.
  const ResolveResult result =
      Check("struct Hit {\n  url: string\n}\n\n"
            "flow f {\n  in a: string\n  out b: Hit\n"
            "  a | map Hit{url: it} | log it.url | first 1 -> b\n}\n");
  EXPECT_TRUE(Codes(result).empty()) << Messages(result);
}

TEST(FlowResolve, ItInALogStageIsTheValueInHand) {
  EXPECT_TRUE(Codes(Check("flow f {\n  in a: string stream\n"
                          "  out b: string stream\n"
                          "  a | log it -> b\n}\n"))
                  .empty());
  // And means nothing in the statement, which has no value in hand.
  EXPECT_EQ(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                        "  if a { log it }\n  a -> b\n}\n")),
            (std::vector<std::string>{"flow.name.it-outside-stage"}));
}

TEST(FlowResolve, ARepeatSaysWhenItStops) {
  // The cap used to be 16 by default, so a `repeat` whose condition never held
  // did sixteen passes and reported *success*. There is no default now, which
  // makes a loop with nothing to end it something to say out loud.
  EXPECT_EQ(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                        "  repeat n = 0 {\n    a -> b\n  }\n}\n")),
            (std::vector<std::string>{"flow.form.unbounded-repeat"}));

  // Either an `until`/`while` or the author's own `max` is enough.
  EXPECT_TRUE(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                          "  repeat n = 0 {\n    a -> b\n    until true\n"
                          "  }\n}\n"))
                  .empty());
  EXPECT_TRUE(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                          "  repeat n = 0 max 4 {\n    a -> b\n  }\n}\n"))
                  .empty());
}

constexpr std::string_view kShapes = R"(flow shapes {
  in  one:   string required
  in  many:  string stream required
  out out:   string stream

  header "x-a11-deadline" as deadline

  made = node()
  one -> made

  many | collect  -> out
  many | count    -> out
  many | first 1  -> out
  many | first 2  -> out
  one  | text     -> out
  one  | chunk 64 -> out
  zip(one, one)   -> out
  zip(one, many)  -> out
  deadline        -> out
  made            -> out
}
)";

/// Whether the ref the graph labelled `label` carries at most one value.
bool Unary(const ResolvedFlow& flow, std::string_view label) {
  for (const graph::Ref& ref : flow.graph.refs) {
    if (ref.label == label) return ref.unary;
  }
  std::vector<std::string> labels;
  for (const graph::Ref& ref : flow.graph.refs) labels.push_back(ref.label);
  ADD_FAILURE() << "no ref labelled '" << label << "'; the graph has: "
                << absl::StrJoin(labels, " | ");
  return false;
}

TEST(FlowResolve, WorksOutWhichStreamsCarryAtMostOneValue) {
  // What a value read is allowed to *consume*. A claim, so the interesting half
  // is what is not claimed: a node the flow writes from anywhere, and a port that
  // said `stream`, are streams however they are used.
  const ResolveResult result =
      Resolve(kShapes, Parse(kShapes), /*build_graph=*/true);
  ASSERT_TRUE(result.diagnostics.empty())
      << absl::StrJoin(Codes(result), ", ");
  ASSERT_FALSE(result.flows.empty());
  const ResolvedFlow& flow = result.flows.front();

  // Declared, which is the only place the language says it outright.
  EXPECT_TRUE(Unary(flow, "one"));
  EXPECT_FALSE(Unary(flow, "many"));
  EXPECT_TRUE(Unary(flow, "header \"x-a11-deadline\""));
  // A node is written from anywhere, including from inside a loop.
  EXPECT_FALSE(Unary(flow, "made"));
  // The reducing stages make one value out of however many.
  EXPECT_TRUE(Unary(flow, "many | collect"));
  EXPECT_TRUE(Unary(flow, "many | count"));
  // `first 1` is how a pipeline says "the value" out loud; `first 2` is not.
  EXPECT_TRUE(Unary(flow, "many | first 1"));
  EXPECT_FALSE(Unary(flow, "many | first 2"));
  // A per-value stage keeps the count it was given; `chunk` makes more.
  EXPECT_TRUE(Unary(flow, "one | text"));
  EXPECT_FALSE(Unary(flow, "one | chunk 64"));
  // A zip runs until every source has ended, so one round is only certain when
  // every source has at most one value.
  EXPECT_TRUE(Unary(flow, "zip(one, one)"));
  EXPECT_FALSE(Unary(flow, "zip(one, many)"));
}

TEST(FlowResolve, AdvanceOnlyMovesAValueALetBound) {
  EXPECT_EQ(Codes(Check("flow f {\n  in q: string\n  out o: string\n"
                        "  n = node()\n  advance n\n  q -> o\n}\n")),
            (std::vector<std::string>{"flow.name.not-advanceable"}));
  EXPECT_EQ(Codes(Check("flow f {\n  in q: string\n  out o: string\n"
                        "  advance nope\n  q -> o\n}\n")),
            (std::vector<std::string>{"flow.name.unknown"}));
  // And the ordinary shape is clean: a value, then the next one under the same
  // name.
  EXPECT_TRUE(Codes(Check("flow f {\n  in q: string stream required\n"
                          "  out o: string stream\n  let w = q\n"
                          "  strformat(\"%s\", w) -> o\n  advance w\n"
                          "  strformat(\"%s\", w) -> o\n}\n"))
                  .empty());
}

TEST(FlowResolve, APatternIsReadWhereItIsWritten) {
  // A pattern is a literal almost every time, so a typo in one belongs in the
  // editor rather than in the failure the first value triggers.
  EXPECT_EQ(Codes(Check("flow f {\n  in l: string stream\n  out o: json stream\n"
                        "  l | match \"name={name\" -> o\n}\n")),
            (std::vector<std::string>{"flow.form.bad-pattern"}));
  EXPECT_EQ(Codes(Check("flow f {\n  in l: string stream\n  out o: json stream\n"
                        "  l | match \"{a} {a}\" -> o\n}\n")),
            (std::vector<std::string>{"flow.form.bad-pattern"}));
  // And a pattern that reads is left alone.
  EXPECT_TRUE(Codes(Check("flow f {\n  in l: string stream\n"
                          "  out o: json stream\n"
                          "  l | match \"name={name} age={age:int}\" -> o\n}\n"))
                  .empty());
}

TEST(FlowResolve, ALetMayTakeAValueApart) {
  // Both spellings resolve, and each name is its own value.
  EXPECT_TRUE(Codes(Check("flow f {\n  in u: json stream required\n"
                          "  out o: string\n  let name, age = u\n"
                          "  strformat(\"%s%s\", name, age) -> o\n}\n"))
                  .empty());
  // A name taken from another value has no next one of its own, and that is
  // said on the editor path too, where there is no graph to read it from.
  EXPECT_EQ(Codes(Check("flow f {\n  in u: json stream required\n"
                        "  out o: string\n  let name, age = u\n"
                        "  strformat(\"%s%s\", name, age) -> o\n"
                        "  advance name\n}\n")),
            (std::vector<std::string>{"flow.name.not-advanceable"}));
  // A part taking a name something else already has is still a clash, and so is
  // one taking a name from the same `let`: two parts called the same thing would
  // define one and shadow it with the other.
  EXPECT_EQ(Codes(Check("flow f {\n  in u: json stream required\n"
                        "  out o: string\n  let age, age = u\n"
                        "  strformat(\"%s\", age) -> o\n}\n")),
            (std::vector<std::string>{"flow.name.taken"}));
}

TEST(FlowResolve, ChecksAFieldAgainstWhatTheFileSaidTheValueHolds) {
  constexpr std::string_view kHead =
      "struct Source {\n  url:  string required\n  rank: number\n}\n\n"
      "flow f {\n  in  src:   Source required\n"
      "  in  lines: string stream required\n  in  loose: json required\n"
      "  out o:     string stream\n";

  // A port declared with a struct, a value a literal pattern made, and `it` in a
  // stage after a `match`: three ways the file said what a value holds, and one
  // check for all of them.
  EXPECT_EQ(Codes(Check(absl::StrCat(
                kHead, "  strformat(\"%s\", src.urll) -> o\n}\n"))),
            (std::vector<std::string>{"flow.form.unknown-field"}));
  EXPECT_EQ(Codes(Check(absl::StrCat(
                kHead, "  lines | match \"{a}: {b}\" | map it.c -> o\n}\n"))),
            (std::vector<std::string>{"flow.form.unknown-field"}));
  EXPECT_EQ(Codes(Check(absl::StrCat(
                kHead, "  let who = match(\"name={name}\", lines)\n"
                       "  strformat(\"%s\", who.nmae) -> o\n}\n"))),
            (std::vector<std::string>{"flow.form.unknown-field"}));

  // Spelled right, nothing said.
  EXPECT_TRUE(Codes(Check(absl::StrCat(
                        kHead, "  strformat(\"%s\", src.url) -> o\n"
                               "  lines | match \"{a}: {b}\" | map it.a -> o\n"
                               "  let who = match(\"name={name}\", lines)\n"
                               "  strformat(\"%s\", who.name) -> o\n}\n")))
                  .empty());

  // And where the file never said, nothing is checked: a `json` port may hold
  // anything, and `it` with no pattern behind it is anybody's guess. A check that
  // cried wolf here would be worse than no check.
  EXPECT_TRUE(Codes(Check(absl::StrCat(
                        kHead, "  strformat(\"%s\", loose.anything) -> o\n"
                               "  lines | map it.whatever -> o\n}\n")))
                  .empty());
  // A positional pattern names no fields, so it says nothing either.
  EXPECT_TRUE(Codes(Check(absl::StrCat(
                        kHead,
                        "  lines | match \"{}:{}\" | map it.nope -> o\n}\n")))
                  .empty());
  // One level only: a field holding a record of its own says nothing about its
  // keys, so the chain stops rather than guessing.
  EXPECT_TRUE(Codes(Check(absl::StrCat(
                        kHead, "  strformat(\"%s\", src.url.deeper) -> o\n}\n")))
                  .empty());
}

TEST(FlowResolve, FindsNothingWrongWithAnyFlowThisRepositoryShips) {
  // The other half of the parity claim: whatever the resolver says, it says
  // nothing about the flows that already work.
  const std::vector<std::filesystem::path> corpus = Corpus();
  ASSERT_FALSE(corpus.empty());
  for (const std::filesystem::path& path : corpus) {
    const ResolveResult result = Check(ReadFile(path));
    EXPECT_TRUE(result.diagnostics.empty()) << path << ": " << Messages(result);
  }
}

TEST(FlowResolve, CountsWhatReadsAndWritesEachName) {
  // What the inspector is built on: the resolver walks every reference, so it is
  // the pass that knows what nothing ever read.
  const ResolveResult result = Check(
      "flow f {\n"
      "  in  a: string\n"
      "  out b: string\n"
      "  out unused: string\n"
      "  header \"x-h\" as h\n"
      "  x = try run act(p: a)\n"
      "  x.out -> b\n"
      "}\n");
  ASSERT_TRUE(result.diagnostics.empty()) << Messages(result);
  ASSERT_EQ(result.flows.size(), 1u);
  const auto find = [&](std::string_view name) -> const Symbol* {
    for (const Symbol& symbol : result.flows[0].symbols) {
      if (symbol.name == name) return &symbol;
    }
    return nullptr;
  };
  ASSERT_NE(find("a"), nullptr);
  EXPECT_EQ(find("a")->reads, 1);
  EXPECT_EQ(find("b")->writes, 1);
  EXPECT_EQ(find("unused")->writes, 0);
  EXPECT_EQ(find("h")->reads, 0);
  ASSERT_NE(find("x"), nullptr);
  EXPECT_TRUE(find("x")->tolerant);
  // The call's port was read, but nothing read its *status*.
  EXPECT_EQ(find("x")->status_reads, 0);
}

}  // namespace
}  // namespace a11::flow
