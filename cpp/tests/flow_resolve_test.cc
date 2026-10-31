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

#include "a11/flow/module.h"
#include "a11/flow/parser.h"
#include "a11/flow/resolve.h"
#include "a11/flow/single_item.h"

namespace a11::flow {
namespace {

ResolveResult Check(std::string_view source) {
  auto linked = LinkModules(std::string(source), "<test>");
  return Resolve(linked.source, linked.parsed);
}

std::vector<std::string> Codes(const ResolveResult& result) {
  std::vector<std::string> codes;
  codes.reserve(result.diagnostics.size());
  for (const Diagnostic& diagnostic : result.diagnostics) {
    codes.push_back(diagnostic.code);
  }
  return codes;
}

std::string Messages(const ResolveResult& result) {
  std::vector<std::string> messages;
  messages.reserve(result.diagnostics.size());
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
      {"flow f { in a: string\n s <- a }", "flow.barrier.carry-outside-repeat",
       "no repeat here"},
      {"flow f { in a: string\n repeat s = 1 { s <- a\n s <- a } }",
       "flow.barrier.duplicate-carry", "is already carried"},
      {"flow f { in a: string\n repeat s = 1 { t <- a } }",
       "flow.barrier.wrong-carry", "This repeat carries 's', not 't'."},
      {"flow f { in a: wat }", "flow.form.unknown-type", "Unknown type 'wat'"},
      {"flow f { in a: std::list[string, object] }", "flow.form.unknown-type",
       "type parameter"},
      {"flow f { in a: string\n in a: string }", "flow.form.duplicate-port",
       "declared twice"},
      {"flow one { in a: string }\nflow one { in a: string }",
       "flow.sequence.redundant-declaration", "Compatible callable contracts"},
      {"flow f { out a: bool\n True -> a }", "flow.name.unknown",
       "Unknown name 'True'"},
      {"flow f { out a: string\n fail wat \"x\" }",
       "flow.form.unknown-status-code", "Unknown status code 'wat'"},
      {"flow f { out o: string\n kept = node() in nowhere\n \"x\" -> o }",
       "flow.name.unknown-node-map", "Unknown node map 'nowhere'"},
      {"flow f { in a: string\n x = run t(p: a)\n x -> a }",
       "flow.name.call-as-stream", "name one of its ports"},
      {"flow f { in a: string\n out b: string\n x = run t(p: a)\n x | count -> "
       "b }",
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
      {"flow f { in a: string\n repeat s = a {  } until a }",
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

TEST(FlowResolve, PlainPipelineBindingsCoverLiteralAndOperationSources) {
  for (const auto* source :
       {"values", "[1, 2] | flatten", "(values | first 1)", "{\"key\": 1}",
        "{}", "(node()).id", "(run unknown()).output",
        "status (try run unknown())"}) {
    const std::string text =
        absl::StrCat("flow main { in values: int stream\n done = ", source,
                     " -> _\n after done { log \"done\" }\n}");
    for (const bool graph : {false, true}) {
      const auto result = Resolve(text, Parse(text), graph);
      EXPECT_FALSE(result.HasErrors()) << text << "\n" << Messages(result);
    }
  }
  EXPECT_FALSE(Check("flow main { done = [1] | flatten\n}").HasErrors());
  EXPECT_TRUE(Check("flow main { done = [1] -> missing\n}").HasErrors());
}

TEST(FlowResolve, AfterBlocksGateEffectsAndKeepTheirBindingsLocal) {
  for (const bool graph : {false, true}) {
    const std::string source = R"(flow main {
 done = {}
 after done {
   log "done"
   local = run unknown()
   cancel local
   after local { log "nested" }
 }
})";
    const auto result = Resolve(source, Parse(source), graph);
    EXPECT_FALSE(result.HasErrors()) << Messages(result);
  }
  EXPECT_TRUE(
      Check("flow main { done = {}\n after done { hidden = take (1) }\n "
            "log hidden }")
          .HasErrors());
  for (const auto* source :
       {"flow main { after missing { log \"done\" } }",
        "flow main { after {} }",
        "flow main { done = {}\n after done, { log \"done\" } }"}) {
    EXPECT_TRUE(Check(source).HasErrors()) << source;
  }
}

TEST(FlowResolve, PredicatePrefixesRequirePredicatesAndOrderedEvaluation) {
  for (const auto* tail :
       {"take", "take 3", "take while", "take until",
        "take while it parallel 2", "take until it unordered"}) {
    const auto parsed =
        Parse(absl::StrCat("flow main { [1] | flatten | ", tail, " -> _ }"));
    EXPECT_TRUE(parsed.HasErrors()) << tail;
  }
  EXPECT_TRUE(
      Codes(Check("flow main { [1] | flatten | TAKE until it == 1 -> _ }"))
          .empty());
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
      "flow f { out a: string\n _ -> a }",
      "flow f { out a: string\n _ = node()\n \"x\" -> a }",
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

TEST(FlowResolve, EffectsMayWaitForTheirDataArguments) {
  const std::string_view statements[] = {
      "log input",
      "identity = take (input | consume)\n log (node(identity)).id",
      "identified = node(input)\n log identified.id",
      "logf \"value: %s\" (input | consume)",
      "value = take (input | consume)\n logf \"value: %s\" value",
      "first = take (input | consume)\n value = take (first)\n log value",
      "log {\"value\": input}",
      "fail internal input",
      "fail input",
      "abort target unavailable input",
      "log (run produce()).output",
      "log status run produce()",
      "log (wait try run produce()).ok",
  };
  for (const auto statement : statements) {
    const std::string source =
        absl::StrCat("flow produce { out output: string\n \"ok\" -> output }\n",
                     "flow main { in input: string stream\n target = node()\n ",
                     statement, "\n}");
    for (const bool graph : {false, true}) {
      const auto result = Resolve(source, Parse(source), graph);
      EXPECT_FALSE(result.HasErrors()) << statement << "\n" << Messages(result);
    }
  }
}

TEST(FlowResolve, ConstantArgumentsAndHandlesDoNotGateEffects) {
  for (const auto* statement : {
           "log \"constant\"",
           "logf \"value: %s\" 42",
           "value = take (\"constant\")\n log value",
           "first = take (\"constant\")\n value = take (first)\n log value",
           "log ([1] | consume)",
           "log (true or input)",
           "log (false and input)",
           "log target.id",
           "fail internal \"constant\"",
           "abort target unavailable \"constant\"",
           "cancel work",
       }) {
    const std::string source =
        absl::StrCat("flow main { in input: string stream\n target = node()\n",
                     "work = run unknown()\n ", statement, "\n}");
    for (const bool graph : {false, true}) {
      const auto result = Resolve(source, Parse(source), graph);
      bool unconditional = false;
      for (const auto& diagnostic : result.diagnostics) {
        unconditional |=
            diagnostic.code.starts_with("flow.form.unconditional-");
      }
      EXPECT_TRUE(unconditional) << statement << "\n" << Messages(result);
    }
  }
}

TEST(FlowResolve, ALogAtTheTopOfABodyIsRefused) {
  // Same rule as `fail` and `cancel`, and for a reason of its own: a log says
  // what just happened, so one that runs before anything else describes
  // something that has not happened yet.
  EXPECT_EQ(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                        "  log \"starting\"\n  a -> b\n}\n")),
            (std::vector<std::string>{"flow.form.unconditional-log"}));
  EXPECT_EQ(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                        "  logf \"starting %s\" \"a\"\n  a -> b\n}\n")),
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
                          "  w = take (wait first of x, y)\n  w -> n\n}\n"
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
                        "  w = take (wait all of x, y)\n  w -> n\n}\n"
                        "flow other {\n  in a: string\n  out o: string\n"
                        "  a -> o\n}\n")),
            (std::vector<std::string>{"flow.form.wait-all-has-no-value"}));
}

TEST(FlowResolve, ALogStageChangesNothingAboutTheStream) {
  // A log is a pass-through, so what the pipeline was carrying it still
  // carries:
  // the shape survives, and so does the proof that there is one value.
  const ResolveResult result = Check(
      "struct Hit {\n  url: string\n}\n\n"
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

TEST(FlowResolve, ADiscardIsADestinationAndNothingElse) {
  // `-> _` is the whole of what `_` may say.
  const auto codes = [](std::string_view body) {
    return Codes(Check(absl::StrCat("flow f {\n  in a: string stream\n",
                                    "  out b: string stream\n", body, "}\n")));
  };
  EXPECT_TRUE(codes("  a | count -> _\n").empty());
  // Beside a real one, and twice: a discard is a reader like any other, so
  // several of them are several readers and no more remarkable than that.
  EXPECT_TRUE(codes("  a -> b, _\n").empty());
  EXPECT_TRUE(codes("  a -> b\n  a -> _, _\n").empty());

  const std::vector<std::string> read{"flow.name.discard-read"};
  EXPECT_EQ(codes("  _ -> b\n"), read);
  EXPECT_EQ(codes("  _.field -> b\n"), read);
  EXPECT_EQ(codes("  a -> b\n  close _\n"), read);
  EXPECT_EQ(codes("  a -> b\n  wait _\n"), read);
  // With an `after`, since an unconditional `abort` has a complaint of its own.
  EXPECT_EQ(codes("  a -> b\n  abort _ after b\n"), read);
  EXPECT_EQ(codes("  a -> b\n  _ -> b\n"), read);
  EXPECT_EQ(codes("  a | map _ -> b\n"), read);
  EXPECT_EQ(codes("  a | where _ == 1 -> b\n"), read);
  EXPECT_EQ(codes("  for v in _ { v -> b }\n"), read);
  EXPECT_EQ(codes("  a -> b\n  logf info \"%s\" status _ after b\n"), read);

  // And nothing may be called `_`, wherever a name is introduced.
  const std::vector<std::string> bound{"flow.name.discard-bound"};
  EXPECT_EQ(codes("  _ = node()\n  a -> b\n"), bound);
  EXPECT_EQ(codes("  a -> b\n  _ = take (a)\n"), bound);
  EXPECT_EQ(codes("  _ = wait a\n  a -> b\n"), bound);
  EXPECT_EQ(Codes(Check("flow f {\n  in _: string\n  out b: string\n"
                        "  \"x\" -> b\n}\n")),
            bound);
  EXPECT_EQ(Codes(Check("flow f {\n  header \"x\" as _\n  out b: string\n"
                        "  \"x\" -> b\n}\n")),
            bound);
}

TEST(FlowResolve, ARepeatSaysWhenItStops) {
  EXPECT_EQ(Codes(Check("flow f {\n  in a: string\n  out b: string\n"
                        "  repeat n = 0 {\n    a -> b\n  }\n}\n")),
            (std::vector<std::string>{"flow.form.unbounded-repeat"}));

  // Either an `until`/`while` or the author's own `max` is enough.
  EXPECT_TRUE(Codes(Check("flow f {\n  in a: string\n  out b: string\n  repeat "
                          "n = 0 {\n    a -> b\n    \n  } until true\n}\n"))
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
  std::zip(one, one)   -> out
  std::zip(one, many)  -> out
  deadline        -> out
  made            -> out
}
)";

/// Whether the ref the graph labelled `label` carries at most one value.
bool Unary(const ResolvedFlow& flow, std::string_view label) {
  for (const graph::Ref& ref : flow.graph.refs) {
    if (ref.label == label) {
      return ref.unary;
    }
  }
  std::vector<std::string> labels;
  labels.reserve(flow.graph.refs.size());
  for (const graph::Ref& ref : flow.graph.refs) {
    labels.push_back(ref.label);
  }
  ADD_FAILURE() << "no ref labelled '" << label
                << "'; the graph has: " << absl::StrJoin(labels, " | ");
  return false;
}

TEST(FlowResolve, BareListLoopLowersToFlattenWithoutChangingTheSyntaxTree) {
  const std::string source =
      "flow main { out value: string stream\n"
      "for word in [\"a\", \"b\"] { word -> value }\n}";
  const auto parsed = Parse(source);
  const auto resolved = Resolve(source, parsed, true);
  ASSERT_FALSE(resolved.HasErrors()) << Messages(resolved);
  ASSERT_EQ(resolved.flows.size(), 1);
  const auto& graph = resolved.flows.front().graph;
  bool found = false;
  for (const auto& step : graph.steps) {
    if (step.kind != graph::StepKind::kForEach) {
      continue;
    }
    const auto& input = graph.refs.at(step.source);
    EXPECT_EQ(input.kind, graph::RefKind::kDerived);
    EXPECT_EQ(input.stage.name, "flatten");
    found = true;
  }
  EXPECT_TRUE(found);
  const auto again = Resolve(source, parsed, true);
  ASSERT_FALSE(again.HasErrors()) << Messages(again);
  EXPECT_EQ(again.flows.front().graph.refs.size(), graph.refs.size());
}

TEST(FlowResolve, WorksOutWhichStreamsCarryAtMostOneValue) {
  // What a value read is allowed to *consume*. A claim, so the interesting half
  // is what is not claimed: a node the flow writes from anywhere, and a port
  // that said `stream`, are streams however they are used.
  const ResolveResult result = [&] {
    auto linked = LinkModules(std::string(kShapes), "<test>");
    return Resolve(linked.source, linked.parsed, /*build_graph=*/true);
  }();
  ASSERT_TRUE(result.diagnostics.empty()) << absl::StrJoin(Codes(result), ", ");
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

TEST(FlowResolve, DynamicParallelismRetainsParallelSafetyChecks) {
  for (const auto* body :
       {"for n in [1, 2] parallel width { x = n }",
        "for n in [1, 2] parallel width { n -> result } until n == 2",
        "rows | sort parallel width -> result",
        "rows | take until it == 2 parallel width -> result",
        "rows | map (take rows) parallel width -> result",
        "rows | map it parallel it -> result"}) {
    const auto source = absl::StrCat(
        "flow main { in rows: int stream\n out result: int stream\n"
        "width = 2\n x = 0\n",
        body, "\n}");
    EXPECT_TRUE(Check(source).HasErrors()) << source;
  }
  EXPECT_FALSE(Check("flow main { out result: int stream\n width = 2\n"
                     "for n in [1, 2] parallel width { n -> result }\n}")
                   .HasErrors());
}

TEST(FlowResolve, ReassignmentRejectsProtectedBindingsAndParallelUpdates) {
  EXPECT_TRUE(Check("flow f { n = node()\n n = 1 }").HasErrors());
  EXPECT_FALSE(
      Check("flow f { x = 0\n repeat max 3 { x = x + 1 } }").HasErrors());
  EXPECT_TRUE(Check("flow f { x = 0\n for item in ([1] | flatten) parallel 2 { "
                    "x = x + 1 } }")
                  .HasErrors());
  EXPECT_TRUE(Check("flow f { in rows: string stream\n for item in ([1] | "
                    "flatten) parallel 2 { x = take rows } }")
                  .HasErrors());
}

TEST(FlowResolve, ANamedLoopNamesTheLoopAndNotAWaitItGrew) {
  // The trap in binding a loop: `ResolveAfter` makes `kWait` steps of its own,
  // and `ResolveBind` names a statement's *first* step.
  const std::string source =
      "flow f {\n  in w: string stream\n  out o: string stream\n"
      "  taken = node()\n  first = run t()\n"
      "  done = for x in w { x -> taken } after first\n"
      "  close taken after done\n  taken -> o\n}\n";
  // The graph, because the claim is about which *graph* step the name got.
  const ResolveResult result =
      Resolve(source, Parse(source), /*build_graph=*/true);
  EXPECT_TRUE(Codes(result).empty()) << Messages(result);
  ASSERT_FALSE(result.flows.empty());
  bool found = false;
  for (const graph::Step& step : result.flows.front().graph.steps) {
    if (step.label != "done") {
      continue;
    }
    found = true;
    EXPECT_EQ(step.kind, graph::StepKind::kForEach)
        << "'done' named a " << graph::StepKindName(step.kind);
  }
  EXPECT_TRUE(found) << "no step was labelled 'done'";
}

TEST(FlowResolve, AnAfterHoldsTheArgumentsOfTheStatementItIsOn) {
  // `after` holds the *statement*, and an argument is part of the statement: a
  // feed that did not wait would read `now()` while the flow was starting and
  // hand the call a value from before the thing it says it is after.
  const std::string source =
      "flow f {\n  in w: string\n  out o: string\n"
      "  first = run t(p: w)\n  done = wait first\n"
      "  second = run t(p: std::strformat(\"%s\", std::now())) after done\n"
      "  second.out -> o\n}\n";
  const ResolveResult result =
      Resolve(source, Parse(source), /*build_graph=*/true);
  EXPECT_TRUE(Codes(result).empty()) << Messages(result);
  ASSERT_FALSE(result.flows.empty());
  const graph::FlowGraph& flow = result.flows.front().graph;
  graph::StepId barrier = graph::kNone;
  for (graph::StepId step = 0; step < flow.steps.size(); ++step) {
    if (flow.steps[step].label == "done") {
      barrier = step;
    }
  }
  ASSERT_NE(barrier, graph::kNone);
  bool found = false;
  for (const graph::Step& step : flow.steps) {
    if (step.kind != graph::StepKind::kPipe ||
        !step.label.ends_with("-> second.p")) {
      continue;
    }
    found = true;
    EXPECT_EQ(step.after, std::vector<graph::StepId>{barrier}) << step.label;
  }
  EXPECT_TRUE(found) << "no feed step for second.p";
  // And the plan a reader is shown says the same thing.
  for (const StepPlan& step : result.flows.front().plan.steps) {
    if (step.kind == "pipe" && step.destination == "second.p") {
      EXPECT_EQ(step.after, std::vector<std::string>{"done"});
    }
  }
}

TEST(FlowResolve, AnArgumentMayNotReadTheCallItsStatementWaitsFor) {
  // Nothing could run: the feed will not read `first.out` until `first` has
  // finished, and `first` cannot finish while nothing reads `first.out`.
  EXPECT_EQ(Codes(Check("flow f {\n  in w: string\n  out o: string\n"
                        "  first = run t(p: w)\n"
                        "  second = run t(p: first.out) after first\n"
                        "  second.out -> o\n}\n")),
            (std::vector<std::string>{"flow.barrier.after-reads-subject"}));
  // The same pipeline without the `after` is the ordinary way to write it.
  EXPECT_TRUE(Codes(Check("flow f {\n  in w: string\n  out o: string\n"
                          "  first = run t(p: w)\n"
                          "  second = run t(p: first.out)\n"
                          "  second.out -> o\n}\n"))
                  .empty());
  // Waiting for something else while reading this one is fine.
  EXPECT_TRUE(Codes(Check("flow f {\n  in w: string\n  out o: string\n"
                          "  first = run t(p: w)\n  other = run t(p: w)\n"
                          "  second = run t(p: first.out) after other\n"
                          "  second.out -> o\n}\n"))
                  .empty());
}

TEST(FlowResolve, APatternIsReadWhereItIsWritten) {
  // A pattern is a literal almost every time, so a typo in one belongs in the
  // editor rather than in the failure the first value triggers.
  EXPECT_EQ(
      Codes(Check("flow f {\n  in l: string stream\n  out o: std::json stream\n"
                  "  l | match \"name={name\" -> o\n}\n")),
      (std::vector<std::string>{"flow.form.bad-pattern"}));
  EXPECT_EQ(
      Codes(Check("flow f {\n  in l: string stream\n  out o: std::json stream\n"
                  "  l | match \"{a} {a}\" -> o\n}\n")),
      (std::vector<std::string>{"flow.form.bad-pattern"}));
  // And a pattern that reads is left alone.
  EXPECT_TRUE(
      Codes(
          Check("flow f {\n  in l: string stream\n  out o: std::json stream\n  "
                "l | match \"name={name} age={age:int}\" -> o\n}\n"))
          .empty());
}

TEST(FlowResolve, InitializersCanDestructureOneValue) {
  // Both spellings resolve, and each name is its own value.
  EXPECT_TRUE(Codes(Check("flow f {\n  in u: std::json stream required\n  out "
                          "o: string\n  name, age = take (u)\n  "
                          "std::strformat(\"%s%s\", name, age) -> o\n}\n"))
                  .empty());
  // A part taking a name something else already has is still a clash, and so is
  // one taking a name from the same `value`: two parts called the same thing
  // would define one and shadow it with the other.
  EXPECT_EQ(
      Codes(Check(
          "flow f {\n  in u: std::json stream required\n  out o: string\n  "
          "age, age = take (u)\n  std::strformat(\"%s\", age) -> o\n}\n")),
      (std::vector<std::string>{"flow.name.taken"}));
}

TEST(FlowResolve, ChecksAFieldAgainstWhatTheFileSaidTheValueHolds) {
  constexpr std::string_view kHead =
      "struct Source {\n  url:  string required\n  rank: number\n}\n\nflow f "
      "{\n  in  src:   Source required\n  in  lines: string stream required\n  "
      "in  loose: std::json required\n  out o:     string stream\n";

  // A port declared with a struct, a value a literal pattern made, and `it` in
  // a stage after a `match`: three ways the file said what a value holds, and
  // one check for all of them.
  EXPECT_EQ(Codes(Check(absl::StrCat(
                kHead, "  std::strformat(\"%s\", src.urll) -> o\n}\n"))),
            (std::vector<std::string>{"flow.form.unknown-field"}));
  EXPECT_EQ(Codes(Check(absl::StrCat(
                kHead, "  lines | match \"{a}: {b}\" | map it.c -> o\n}\n"))),
            (std::vector<std::string>{"flow.form.unknown-field"}));
  EXPECT_EQ(Codes(Check(absl::StrCat(
                kHead,
                "  who = take (std::match(\"name={name}\", lines))\n  "
                "std::strformat(\"%s\", who.nmae) -> o\n}\n"))),
            (std::vector<std::string>{"flow.form.unknown-field"}));

  // Spelled right, nothing said.
  EXPECT_TRUE(Codes(Check(absl::StrCat(
                        kHead,
                        "  std::strformat(\"%s\", src.url) -> o\n  lines | "
                        "match \"{a}: {b}\" | map it.a -> o\n  who = take "
                        "(std::match(\"name={name}\", lines))\n  "
                        "std::strformat(\"%s\", who.name) -> o\n}\n")))
                  .empty());

  // And where the file never said, nothing is checked: a `json` port may hold
  // anything, and `it` with no pattern behind it is anybody's guess. A check
  // that cried wolf here would be worse than no check.
  EXPECT_TRUE(
      Codes(Check(absl::StrCat(kHead,
                               "  std::strformat(\"%s\", loose.anything) -> "
                               "o\n  lines | map it.whatever -> o\n}\n")))
          .empty());
  // A positional pattern names no fields, so it says nothing either.
  EXPECT_TRUE(
      Codes(Check(absl::StrCat(
                kHead, "  lines | match \"{}:{}\" | map it.nope -> o\n}\n")))
          .empty());
  // One level only: a field holding a record of its own says nothing about its
  // keys, so the chain stops rather than guessing.
  EXPECT_TRUE(
      Codes(Check(absl::StrCat(
                kHead, "  std::strformat(\"%s\", src.url.deeper) -> o\n}\n")))
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
  // What the inspector is built on: the resolver walks every reference, so it
  // is
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
      if (symbol.name == name) {
        return &symbol;
      }
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

TEST(FlowResolve, OptionalBindingRequiresProvenanceAndFreshLocalNames) {
  for (const auto body :
       {"if item = {has_value: true, value: 1} { item -> _ }",
        "if item = {present: true, value: 1} { item -> _ }",
        "if item = 1 { item -> _ }",
        "item = 1\nif item = std::option({has_value: false}) { item -> _ }",
        "if item = std::option({has_value: false}) {} else { item -> _ }",
        "if item = std::option({has_value: false}) {}\nitem -> _"}) {
    const auto result = Check(absl::StrCat("flow main {\n", body, "\n}"));
    EXPECT_TRUE(result.HasErrors()) << body << ": " << Messages(result);
  }
  const auto good = Check(R"(
flow main {
 record = {present: true, has_value: false}
 if record.present == true { record.has_value -> _ }
 if item = std::option({has_value: true, value: 0}) { item -> _ }
}
)");
  EXPECT_FALSE(good.HasErrors()) << Messages(good);
}

TEST(FlowResolve, OptionalBindingInfersPayloadTypeInItsSuccessBranch) {
  const auto result = Check(R"(
struct Row { name: string required }
flow main { in rows: Row stream
 out names: string stream
 if row = try take rows { row.name -> names }
}
)");
  ASSERT_FALSE(result.HasErrors()) << Messages(result);
  auto found = std::find_if(
      result.flows.front().symbols.begin(), result.flows.front().symbols.end(),
      [](const auto& symbol) { return symbol.name == "row"; });
  ASSERT_NE(found, result.flows.front().symbols.end());
  ASSERT_TRUE(found->value_type.has_value());
  EXPECT_EQ(found->value_type->name, "Row");
  EXPECT_GT(found->visibility.end, found->visibility.start);
  EXPECT_EQ(result.flows.front().plan.steps.back().optional_binding, "row");
}

TEST(FlowResolve, OptionalDefaultsInferThePayloadType) {
  const auto result = Check(R"(
struct Row { name: string default "unknown" }
flow main {
  in rows: Row stream
  in stored: std::option[Row]

  direct = try take rows or Row{}
  candidate = take stored
  reused = candidate or Row{}
  untyped = std::option({has_value: false}) or Row{}
  direct.name -> _
  reused.name -> _
  untyped.name -> _
}
)");
  ASSERT_FALSE(result.HasErrors()) << Messages(result);
  int checked = 0;
  for (const auto& symbol : result.flows.front().symbols) {
    if (symbol.name == "direct" || symbol.name == "reused" ||
        symbol.name == "untyped") {
      ASSERT_TRUE(symbol.value_type.has_value()) << symbol.name;
      EXPECT_EQ(symbol.value_type->name, "Row") << symbol.name;
      ++checked;
    }
  }
  EXPECT_EQ(checked, 3);
}

TEST(FlowResolve, ExplicitOptionFieldsRetainTheirTypes) {
  const auto result = Check(R"(
struct Row { name: string required }
flow main { in rows: Row stream
 candidate = try take rows
 flag = candidate.has_value
 payload = candidate.value
 if nested = std::option({has_value: true, value: candidate}) {
   if row = nested { row.name -> _ }
 }
}
)");
  ASSERT_FALSE(result.HasErrors()) << Messages(result);
  for (const auto& symbol : result.flows.front().symbols) {
    if (symbol.name == "flag" || symbol.name == "payload") {
      ASSERT_TRUE(symbol.value_type.has_value());
      EXPECT_EQ(symbol.value_type->name,
                symbol.name == "flag" ? "bool" : "Row");
    }
  }
}

TEST(FlowResolve, ContinuationChainsFlattenWithoutIntermediateNodes) {
  const std::string source = R"(
flow main {
  out result: int stream
  tail = 3 -> 2 -> 1 -> result
  alias = tail
  4 -> alias
}
)";
  const auto result = Resolve(source, Parse(source), true);
  ASSERT_FALSE(result.HasErrors()) << Messages(result);
  const auto& graph = result.flows.front().graph;
  EXPECT_EQ(
      std::count_if(graph.steps.begin(), graph.steps.end(),
                    [](auto& s) { return s.kind == graph::StepKind::kPipe; }),
      2);
  EXPECT_EQ(
      std::count_if(graph.refs.begin(), graph.refs.end(),
                    [](auto& r) { return r.kind == graph::RefKind::kSink; }),
      1);
  EXPECT_EQ(
      std::count_if(graph.refs.begin(), graph.refs.end(),
                    [](auto& r) { return r.kind == graph::RefKind::kNode; }),
      0);
  EXPECT_EQ(result.flows.front().plan.steps.front().serial_sources,
            (std::vector<std::string>{"1", "2", "3"}));
}

TEST(FlowResolve, ContinuationsRejectSourcesPayloadsAndAmbiguousBroadcasts) {
  for (const auto text :
       {"tail -> result", "(tail) | count -> result", "(1 -> result) -> result",
        "{handle: tail} -> result", "std::len(tail) -> result",
        "1 -> result, other -> result"}) {
    const auto result = Check(absl::StrCat(
        "flow main { out result: int stream\n out other: int stream\n"
        " tail = 0 -> result\n",
        text, "\n}"));
    EXPECT_TRUE(result.HasErrors()) << text << ": " << Messages(result);
  }
}

TEST(FlowResolve, ContinuationCompletionCyclesHaveRelatedLocations) {
  for (const auto body : {"tail = 0 -> result\nwait all\n1 -> tail",
                          "tail = 0 -> result\n1 -> tail after tail",
                          "tail = 0 -> result\nfor x in [1] | flatten {\nwait "
                          "tail\nx -> tail\n}"}) {
    const auto result = Check(
        absl::StrCat("flow main { out result: int stream\n", body, "\n}"));
    auto found =
        std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
                     [](const auto& d) {
                       return d.code == "flow.barrier.continuation-cycle";
                     });
    ASSERT_NE(found, result.diagnostics.end()) << Messages(result);
    EXPECT_GE(found->related.size(), 2);
  }
}

}  // namespace

TEST(FlowResolve, RejectsAnInvocationDeadlineDependingOnItsOwnResult) {
  const auto resolved = Check(R"(
flow main {
  work = run fetch() deadline (take work.finished_at)
}
)");
  EXPECT_TRUE(
      std::any_of(resolved.diagnostics.begin(), resolved.diagnostics.end(),
                  [](const auto& diagnostic) {
                    return diagnostic.code == "flow.barrier.expression-cycle";
                  }));
}

TEST(FlowResolve, ReassignmentKeepsTheDeclaredType) {
  EXPECT_TRUE(Check("flow f { int n = 1\n string n = \"two\" }").HasErrors());
  EXPECT_TRUE(
      Check("flow f { int a, b = [1, 2]\n string a, b = [\"x\", \"y\"] }")
          .HasErrors());
}

TEST(FlowResolve, QualifiedUnknownCallsAreErrorsAtTheTarget) {
  const auto result = Check("flow f { run nested::test() }");
  ASSERT_TRUE(result.HasErrors());
  const auto found = std::find_if(
      result.diagnostics.begin(), result.diagnostics.end(),
      [](const auto& d) { return d.code == "flow.name.unknown-call"; });
  ASSERT_NE(found, result.diagnostics.end());
  EXPECT_EQ(found->range.start.offset, 13u);
  EXPECT_EQ(found->range.end.offset, 25u);
}

TEST(FlowResolve, PipelineCallsRequireAnUnambiguousKnownInterface) {
  for (const auto* source : {"flow main { 1 | run missing() -> _ }",
                             "declare flow f {in a: int\n in b: int\n out x: "
                             "int}\nflow main {1 | run f() -> _}",
                             "declare flow f {in a: int\n out x: int}\nflow "
                             "main {1 | run f(a: 2) -> _}",
                             "declare flow f {in a: int\n out x: int\n out y: "
                             "int}\nflow main {1 | run f() -> _}",
                             "declare flow f {in a: object\n out x: int}\nflow "
                             "main {1 | run f() -> _}"}) {
    const auto checked = Check(source);
    EXPECT_TRUE(checked.HasErrors()) << source;
    EXPECT_TRUE(std::any_of(
        checked.diagnostics.begin(), checked.diagnostics.end(),
        [](const auto& d) { return d.code == "flow.form.pipeline-call"; }))
        << source;
  }
}

TEST(FlowResolve, PipelineCallOrderingAllowsStatusReadsAndRejectsOutputCycles) {
  const auto good = Check(R"(
flow clean { in input: string stream
 out output: string stream
 input -> output
}
flow main { in items: string stream
 work = run clean(input: "ready")
 items | run clean() after work | map work.status.ok -> _
}
)");
  EXPECT_FALSE(good.HasErrors()) << Messages(good);
  const auto bad = Check(R"(
flow clean { in input: string stream
 out output: string stream
 input -> output
}
flow main {
 work = run clean(input: "ready")
 work.output | run clean() after work -> _
}
)");
  EXPECT_TRUE(std::any_of(bad.diagnostics.begin(), bad.diagnostics.end(),
                          [](const auto& d) {
                            return d.code == "flow.barrier.after-reads-subject";
                          }));
}

TEST(FlowResolve, ScopeBarriersRejectValuesModifiersAndStreamingCycles) {
  for (const auto* statement :
       {"value = wait all", "wait all timeout 1s",
        "wait all deadline std::now()", "wait all after prior"}) {
    EXPECT_TRUE(
        Parse(absl::StrCat("flow main { prior = {}\n", statement, "\n}"))
            .HasErrors())
        << statement;
  }
  for (const auto* body :
       {"work = run producer()\n wait all\n work.output -> _",
        "n = node()\n \"x\" -> n\n wait all\n n -> _",
        "n = node()\n n -> _\n wait all\n \"x\" -> n",
        "work = run transform()\n wait all\n \"x\" -> work.input",
        "alias = \"x\" | run transform()\n alias -> _\n wait all\n alias -> "
        "_"}) {
    auto result = Check(
        absl::StrCat("flow producer { out output: string stream\n \"x\" "
                     "-> output }\nflow transform { in input: string stream\n"
                     "out output: string stream\n input -> output }\n"
                     "flow main {\n",
                     body, "\n}"));
    EXPECT_TRUE(std::any_of(
        result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& d) { return d.code == "flow.barrier.scope-cycle"; }))
        << Messages(result);
  }
  EXPECT_FALSE(Check("flow main { in input: string stream\n alias = input\n "
                     "wait all\n alias -> _ }")
                   .HasErrors());
  EXPECT_FALSE(Check("flow transform { in input: string stream\n"
                     "out output: string stream\n input -> output }\n"
                     "flow main { alias = \"x\" | run transform()\n"
                     "wait all\n alias -> _ }")
                   .HasErrors());
  EXPECT_FALSE(Check("flow main { a = run noop()\n b = run noop()\n wait all "
                     "of a, b\n if true "
                     "{ log \"free\" } }")
                   .HasErrors());
}

TEST(FlowResolve, SingleItemEvidenceUsesWritesAndActivationOwnership) {
  const auto parsed = Parse(R"(
flow main {
 out result: int stream
 nodes private {
  n = node()
  if true { 1 -> n } else { 2 -> n }
  n -> result
 }
}
)");
  const auto resolved = Resolve("", parsed, true);
  ASSERT_FALSE(resolved.HasErrors());
  const auto& graph = resolved.flows[0].graph;
  ASSERT_TRUE(CheckSingleItemStructure(graph));
  auto analysis = AnalyseSingleItems(graph);
  const auto node = std::find_if(
      graph.refs.begin(), graph.refs.end(),
      [](const auto& ref) { return ref.kind == graph::RefKind::kNode; });
  ASSERT_NE(node, graph.refs.end());
  const auto id = static_cast<size_t>(node - graph.refs.begin());
  EXPECT_FALSE(node->unary);
  EXPECT_TRUE(analysis.refs[id].eligible);
  EXPECT_EQ(analysis.refs[id].upper, 1);
  EXPECT_EQ(analysis.refs[id].writers.size(), 2u);
  EXPECT_FALSE(analysis.refs[id].pump_free);
  EXPECT_FALSE(AnalyseSingleItems(graph, nullptr, 0).refs[id].eligible);
}

TEST(FlowResolve, SingleItemEvidenceRejectsUnknownAndCompetingProducers) {
  for (const auto* body :
       {"1 -> n\n 2 -> n", "1 -> n\n 2 -> n\n 3 -> n",
        "for x in [1, 2] | flatten { x -> n }", "run writer(id: n.id)",
        "other = node(\"unknown\")\n 1 -> n", "run external()\n 1 -> n",
        "input -> n"}) {
    SCOPED_TRACE(body);
    const auto parsed = Parse(
        absl::StrCat("flow main { in input: int\n nodes private { n = node()\n",
                     body, "\n n -> _\n} }"));
    const auto resolved = Resolve("", parsed, true);
    const auto& graph = resolved.flows[0].graph;
    const auto facts = AnalyseSingleItems(graph);
    for (size_t id = 0; id < graph.refs.size(); ++id) {
      if (graph.refs[id].kind == graph::RefKind::kNode) {
        EXPECT_FALSE(facts.refs[id].eligible);
      }
    }
  }
}

TEST(FlowResolve, SingleItemVerifierRejectsBrokenStructuralPremises) {
  const auto parsed = Parse("flow main { n = node()\n 1 -> n\n n -> _ }");
  const auto resolved = Resolve("", parsed, true);
  auto graph = resolved.flows[0].graph;
  ASSERT_TRUE(CheckSingleItemStructure(graph));
  graph.bodies[graph.root].steps.push_back(
      graph.bodies[graph.root].steps.front());
  EXPECT_FALSE(CheckSingleItemStructure(graph));
  const auto facts = AnalyseSingleItems(graph);
  EXPECT_FALSE(facts.complete);
  EXPECT_TRUE(std::none_of(facts.refs.begin(), facts.refs.end(),
                           [](const auto& fact) { return fact.eligible; }));
}

TEST(FlowResolve, SingleItemOptionalBudgetKeepsTheBaselineValid) {
  const auto parsed = Parse("flow main { 42 -> _ }");
  const auto resolved = Resolve("", parsed, true);
  CompilationLimits limits;
  limits.max_work = 0;
  CompilationBudget budget(limits);
  const auto facts = AnalyseSingleItems(resolved.flows[0].graph, &budget);
  EXPECT_FALSE(facts.complete);
  EXPECT_FALSE(budget.exhausted());
  EXPECT_FALSE(budget.Check());
  EXPECT_TRUE(budget.exhausted());
}

}  // namespace a11::flow
