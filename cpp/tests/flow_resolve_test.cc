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
       "Unknown port type 'wat'"},
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
                                      "flow.name.not-a-call"}));
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
