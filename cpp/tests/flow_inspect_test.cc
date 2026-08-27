// Copyright 2026 The A11 Authors.

#include <string>
#include <string_view>
#include <vector>

#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <gtest/gtest.h>

#include "a11/flow/inspect.h"
#include "a11/flow/parser.h"
#include "a11/flow/resolve.h"

namespace a11::flow {
namespace {

std::vector<Diagnostic> Findings(std::string_view source) {
  const ParseResult parsed = Parse(source);
  const ResolveResult resolved = Resolve(source, parsed);
  return Inspect(source, parsed, resolved);
}

std::vector<std::string> Codes(std::string_view source) {
  std::vector<std::string> codes;
  for (const Diagnostic& diagnostic : Findings(source)) {
    codes.push_back(diagnostic.code);
  }
  return codes;
}

std::string Messages(std::string_view source) {
  std::vector<std::string> messages;
  for (const Diagnostic& diagnostic : Findings(source)) {
    messages.push_back(diagnostic.message);
  }
  return absl::StrJoin(messages, "; ");
}

TEST(FlowInspect, SaysWhenNothingUsesSomethingTheFlowDeclared) {
  struct Case {
    std::string_view source;
    std::string_view code;
    std::string_view severity;
  };

  const Case cases[] = {
      // A `try` is a promise to handle a failure; not reading the status is the
      // flow breaking that promise.
      {"flow f { in a: string\n out b: string\n x = try run act(p: a)\n"
       " x.out -> b }",
       "flow.unused.try-status", "weak-warning"},
      // An `out` port nothing writes: a caller reading it gets nothing.
      {"flow f { in a: string\n out b: string\n out forgotten: string\n"
       " a -> b }",
       "flow.unused.output-port", "warning"},
      {"flow f { header \"x-h\" as h\n in a: string\n out b: string\n"
       " a -> b }",
       "flow.unused.header", "weak-warning"},
      {"flow f { in a: string\n out b: string\n nodes empty\n a -> b }",
       "flow.unused.node-map", "weak-warning"},
      {"flow f { in a: string stream\n out b: string\n"
       " for word in a { \"x\" -> b } }",
       "flow.unused.loop-variable", "weak-warning"},
      {"flow f { in a: string\n out b: string\n x = run act(p: a)\n"
       " done = wait x\n x.out -> b }",
       "flow.unused.barrier-name", "weak-warning"},
  };
  for (const Case& one : cases) {
    bool found = false;
    for (const Diagnostic& diagnostic : Findings(one.source)) {
      if (diagnostic.code == one.code &&
          SeverityName(diagnostic.severity) == one.severity) {
        found = true;
      }
    }
    EXPECT_TRUE(found) << one.source << " gave " << Messages(one.source);
  }
}

TEST(FlowInspect, ATryWhoseStatusIsReadIsFine) {
  // Three ways of reading it, and each one has to count -- otherwise the fix
  // the finding suggests does not clear it.
  for (const std::string_view read :
       {"  wait x\n", "  s = wait x\n  s -> c\n", "  status x -> c\n"}) {
    const std::string source = absl::StrCat(
        "flow f {\n  in a: string\n  out b: string\n"
        "  out c: object\n  x = try run act(p: a)\n",
        read, "  x.out -> b\n}\n");
    for (const Diagnostic& diagnostic : Findings(source)) {
      EXPECT_NE(diagnostic.code, "flow.unused.try-status") << source;
    }
  }
}

TEST(FlowInspect, TheIndexEveryLoopBindsIsNotSomethingTheAuthorForgot) {
  const std::vector<std::string> codes = Codes(
      "flow f { in a: string stream\n out b: string\n"
      " for word in a { word -> b } }");
  for (const std::string& code : codes) {
    EXPECT_NE(code, "flow.unused.loop-variable");
  }
}

TEST(FlowInspect, WorksOutWhatAStageCanPossiblyDo) {
  // The one fact everything here follows from: after a reducing stage there is
  // exactly one value, whatever the stream held before it.
  EXPECT_EQ(Codes("flow f { in a: string stream\n out b: string\n"
                  " a | collect | drop 3 -> b }"),
            (std::vector<std::string>{"flow.sequence.impossible"}));
  EXPECT_NE(Messages("flow f { in a: string stream\n out b: string\n"
                     " a | collect | drop 3 -> b }")
                .find("'| collect' left one"),
            std::string::npos);

  EXPECT_EQ(Codes("flow f { in a: string stream\n out b: number\n"
                  " a | count | count -> b }"),
            (std::vector<std::string>{"flow.sequence.impossible"}));
  EXPECT_EQ(Codes("flow f { in a: string stream\n out b: string\n"
                  " a | collect | first 1 -> b }"),
            (std::vector<std::string>{"flow.sequence.redundant-stage"}));
  EXPECT_EQ(Codes("flow f { in a: string stream\n out b: string\n"
                  " a | distinct | distinct -> b }"),
            (std::vector<std::string>{"flow.sequence.redundant-stage"}));
  EXPECT_EQ(Codes("flow f { in a: string stream\n out b: string\n"
                  " a | first 0 -> b }"),
            (std::vector<std::string>{"flow.sequence.impossible"}));

  // And the sequences that are perfectly ordinary.
  for (const std::string_view fine :
       {"a | first 3 | truncate 200 -> b", "a | collect | truncate 20 -> b",
        R"(a | where it != "" | map it | join "," -> b)",
        "a | group it | map join(it, \" \") -> b",
        "a | batch 4 | collect -> b"}) {
    const std::string source = absl::StrCat(
        "flow f { in a: string stream\n out b: string\n ", fine, " }");
    EXPECT_EQ(Codes(source), (std::vector<std::string>{})) << source;
  }
}

TEST(FlowInspect, LooksInsideAPipelineWrittenAsAValue) {
  EXPECT_EQ(Codes("flow f { in a: string stream\n out b: string\n"
                  " if (a | collect | count) > 0 { \"x\" -> b } }"),
            (std::vector<std::string>{"flow.sequence.impossible"}));
}

TEST(FlowInspect, SaysWhenACountIsNotANumberOfAnything) {
  EXPECT_EQ(Codes("flow f { in a: string stream\n out b: string\n"
                  " for word in a parallel 0 { word -> b } }"),
            (std::vector<std::string>{"flow.form.count-not-positive"}));
  EXPECT_EQ(Codes("flow f { in a: string\n out b: string\n"
                  " repeat max 0 { a -> b\n until a } }"),
            (std::vector<std::string>{"flow.form.count-not-positive"}));
}

TEST(FlowInspect, SaysWhenABarrierCannotDoAnything) {
  EXPECT_EQ(Codes("flow f { in a: string\n out b: string\n"
                  " x = run act(p: a)\n wait x\n wait x\n x.out -> b }"),
            (std::vector<std::string>{"flow.barrier.duplicate"}));
  // Both of these give the `cancel` an `after` or a branch, because a `cancel`
  // at the top of a body races everything and the language refuses one -- and
  // the inspector says nothing at all about a flow that does not resolve.
  EXPECT_EQ(Codes("flow f { in a: string\n out b: string\n"
                  " x = run act(p: a)\n w = wait x\n cancel x after w\n"
                  " x.out -> b }"),
            (std::vector<std::string>{"flow.barrier.cancel-after-wait"}));
  // A `cancel` *before* the wait is the ordinary way round.
  EXPECT_EQ(Codes("flow f { in a: string\n out b: string\n"
                  " x = run act(p: a)\n if a == \"stop\" { cancel x }\n"
                  " wait x\n x.out -> b }"),
            (std::vector<std::string>{}));
}

TEST(FlowInspect, SaysWhenAClockIsReadAtAMomentNothingPinsDown) {
  // The mistake this is here for: written last, run first, so what it reports
  // is the flow starting rather than how long the call took.
  EXPECT_EQ(Codes("flow f { in a: string\n out b: string\n"
                  " started = node()\n now() -> started\n"
                  " x = run act(p: a)\n wait x\n"
                  " strformat(\"%s\", now() - started) -> b }"),
            (std::vector<std::string>{"flow.barrier.unordered-clock"}));
  // The same statement, ordered: this is what the flow meant.
  EXPECT_EQ(Codes("flow f { in a: string\n out b: string\n"
                  " started = node()\n now() -> started\n"
                  " x = run act(p: a)\n done = wait x\n"
                  " strformat(\"%s\", now() - started) -> b after done }"),
            (std::vector<std::string>{}));
  // An argument is part of its statement, so it is asked the same question.
  EXPECT_EQ(Codes("flow f { in a: string\n out b: string\n"
                  " started = node()\n now() -> started\n"
                  " x = run act(p: strformat(\"%s\", now() - started))\n"
                  " x.out -> b }"),
            (std::vector<std::string>{"flow.barrier.unordered-clock"}));
  // Stamping the start is the other half of the idiom, and needs no moment.
  EXPECT_EQ(Codes("flow f { out b: string\n started = node()\n"
                  " now() -> started\n started -> b }"),
            (std::vector<std::string>{}));
  // A header is there before the first statement, so reading the clock against
  // one says nothing about when this ran.
  EXPECT_EQ(Codes("flow f { out b: string\n header \"x-a11-deadline\" as by\n"
                  " strformat(\"%s\", time(by) - now()) -> b }"),
            (std::vector<std::string>{}));
}

TEST(FlowInspect, SaysWhenOneStatementReadsOneStreamTwiceForAValue) {
  const std::vector<std::string> both{"flow.barrier.unordered-clock",
                                      "flow.barrier.value-read-twice"};
  EXPECT_EQ(Codes("flow f { in a: string\n out b: string\n"
                  " started = node()\n now() -> started\n"
                  " x = run act(p: a)\n wait x\n"
                  " strformat(\"%s %s\", started, now() - started) -> b }"),
            both);
  // Ordered, and still two reads: the `after` says when the statement runs and
  // cannot say which read inside it goes first.
  EXPECT_EQ(Codes("flow f { in a: string\n out b: string\n"
                  " started = node()\n now() -> started\n"
                  " x = run act(p: a)\n done = wait x\n"
                  " strformat(\"%s %s\", started, started) -> b after done }"),
            (std::vector<std::string>{"flow.barrier.value-read-twice"}));
  // A port that did not say `stream` provably carries one value, so it is
  // shared rather than taken and reading it twice is reading one value twice.
  EXPECT_EQ(Codes("flow f { in a: string\n out b: string\n"
                  " strformat(\"%s %s\", a, a) -> b }"),
            (std::vector<std::string>{}));
}

TEST(FlowInspect, SaysWhenAWaitEndsANodeRatherThanWaitingForIt) {
  EXPECT_EQ(Codes("flow f { in a: string\n out b: string\n"
                  " seen = node()\n"
                  " w = run act(p: a) with \"x-a11-progress-node\": seen.id\n"
                  " skip w.out\n seen -> b\n wait seen }"),
            (std::vector<std::string>{"flow.barrier.wait-lends-node"}));
  // Naming the step that fills it is the whole fix, and the documented idiom.
  EXPECT_EQ(Codes("flow f { in a: string\n out b: string\n"
                  " seen = node()\n"
                  " w = run act(p: a) with \"x-a11-progress-node\": seen.id\n"
                  " skip w.out\n seen -> b\n drain seen after w }"),
            (std::vector<std::string>{}));
  // A node this flow writes is closed by its own last writer, so waiting for
  // it is a wait.
  EXPECT_EQ(Codes("flow f { in a: string\n out b: string\n"
                  " seen = node()\n a -> seen\n seen -> b\n wait seen }"),
            (std::vector<std::string>{}));
}

TEST(FlowInspect, SaysNothingAboutAFlowThatDoesNotResolve) {
  // An unknown name means every fact about what uses what is unreliable, and a
  // page of "nothing uses this" on top of the real problem is noise.
  EXPECT_EQ(Codes("flow f { in a: string\n out b: string\n missing -> b }"),
            (std::vector<std::string>{}));
}

TEST(FlowInspect, EveryCodeItProducesIsPublished) {
  const std::string_view sources[] = {
      "flow f { in a: string\n out b: string\n x = try run act(p: a)\n"
      " x.out -> b }",
      "flow f { in a: string\n out b: string\n out forgotten: string\n"
      " a -> b }",
      "flow f { in a: string stream\n out b: string\n a | collect | drop 3 -> "
      "b }",
      "flow f { in a: string stream\n out b: string\n"
      " for w in a parallel 0 { w -> b } }",
      "flow f { in a: string\n out b: string\n nodes empty\n a -> b }",
  };
  for (const std::string_view source : sources) {
    for (const Diagnostic& diagnostic : Findings(source)) {
      EXPECT_NE(FindCode(diagnostic.code), nullptr)
          << diagnostic.code << " from " << source;
      // A finding is never an error: every one of these compiles and runs.
      EXPECT_NE(diagnostic.severity, Severity::kError) << diagnostic.code;
    }
  }
}

}  // namespace
}  // namespace a11::flow
