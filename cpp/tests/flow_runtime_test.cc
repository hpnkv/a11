// Copyright 2026 The A11 Authors.

// What the native runtime is *for*: running flows. Each case is a whole flow
// compiled, registered against real actions and run to completion, because the
// things that go wrong in a dataflow runtime -- a reader that was never counted,
// a node nobody closed, a pass of a loop that saw the wrong value -- do not show
// up in a unit test of a part.

#include "a11/flow/runtime.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"

namespace a11::flow {
namespace {

using Values = std::vector<std::string>;
using PortValues = std::map<std::string, Values>;

data::Chunk JsonChunk(std::string_view json) {
  return data::Chunk{
      .metadata = data::ChunkMetadata{.mimetype =
                                          std::string(data::kJsonMimetype)},
      .data = std::string(json)};
}

/// One flow run, and everything a test wants to know about it.
struct Outcome {
  absl::Status status;
  PortValues outputs;
};

/// An action that writes every value of its input twice, upper-cased.
actions::ActionSchema TwiceSchema() {
  return actions::ActionSchema{
      .name = "twice",
      .inputs = {{"text", actions::ActionPortSchema{.name = "text",
                                                    .type = "str"}}},
      .outputs = {{"out", actions::ActionPortSchema{.name = "out",
                                                    .type = "str"}}},
  };
}

actions::ActionHandler TwiceHandler() {
  return actions::MakeAsyncActionHandler(
      [](std::shared_ptr<actions::Action> action) -> absl::Status {
        ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<nodes::AsyncNode> input,
                              action->GetInput("text"));
        ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<nodes::AsyncNode> output,
                              action->GetOutput("out"));
        while (true) {
          ABSL_ASSIGN_OR_RETURN(const std::optional<data::Chunk> chunk,
                                input->NextChunk().Await());
          if (!chunk.has_value()) break;
          if (chunk->IsNull()) continue;
          const std::string shouted = absl::AsciiStrToUpper(chunk->data);
          for (int at = 0; at < 2; ++at) {
            ABSL_RETURN_IF_ERROR(
                output->PutChunk(JsonChunk(shouted)).Await().status());
          }
        }
        ABSL_RETURN_IF_ERROR(output->PutNullFinal().Await().status());
        return output->DrainAndClose().Await().status();
      });
}

/// An action that fails, for the flows that are about a failure.
actions::ActionSchema BoomSchema() {
  return actions::ActionSchema{
      .name = "boom",
      .outputs = {{"out", actions::ActionPortSchema{.name = "out",
                                                    .type = "str"}}},
  };
}

actions::ActionHandler BoomHandler() {
  return actions::MakeAsyncActionHandler(
      [](std::shared_ptr<actions::Action>) -> absl::Status {
        return absl::NotFoundError("nothing there");
      });
}

std::shared_ptr<actions::ActionRegistry> TestRegistry() {
  auto registry = std::make_shared<actions::ActionRegistry>();
  EXPECT_TRUE(registry->Register("twice", TwiceSchema(), TwiceHandler()).ok());
  EXPECT_TRUE(registry->Register("boom", BoomSchema(), BoomHandler()).ok());
  return registry;
}

Values Collect(const std::shared_ptr<nodes::AsyncNode>& node) {
  Values found;
  while (true) {
    absl::StatusOr<std::optional<data::Chunk>> chunk =
        node->NextChunk().Await(absl::Now() + absl::Seconds(5));
    if (!chunk.ok() || !chunk->has_value()) break;
    if ((*chunk)->IsNull()) continue;
    found.push_back((*chunk)->data);
  }
  return found;
}

/// Compile a flow, run it once against the test registry, and read its outputs.
Outcome RunFlow(std::string_view source, std::string_view name,
            const PortValues& inputs = {},
            const std::map<std::string, std::string>& headers = {}) {
  Outcome outcome;
  absl::StatusOr<std::shared_ptr<CompiledProgram>> program =
      CompiledProgram::Compile(std::string(source), "test.flow");
  if (!program.ok()) {
    outcome.status = program.status();
    return outcome;
  }
  const ResolvedFlow* flow = (*program)->Flow(name);
  if (flow == nullptr) {
    outcome.status = absl::NotFoundError(absl::StrCat("no flow ", name));
    return outcome;
  }
  absl::StatusOr<actions::ActionSchema> schema = FlowSchema(flow->plan);
  if (!schema.ok()) {
    outcome.status = schema.status();
    return outcome;
  }
  absl::StatusOr<actions::ActionHandler> handler = MakeHandler(*program, name);
  if (!handler.ok()) {
    outcome.status = handler.status();
    return outcome;
  }
  absl::StatusOr<std::shared_ptr<nodes::NodeMap>> map =
      nodes::NodeMap::Create();
  if (!map.ok()) {
    outcome.status = map.status();
    return outcome;
  }
  absl::StatusOr<std::shared_ptr<actions::Action>> action =
      actions::Action::Create(*schema, "flow", *handler, *map, nullptr, nullptr,
                              TestRegistry());
  if (!action.ok()) {
    outcome.status = action.status();
    return outcome;
  }
  for (const auto& [name, value] : headers) {
    EXPECT_TRUE((*action)->SetHeader(name, value).ok());
  }
  // Every output node before the flow starts, so a value produced early is
  // waiting rather than lost.
  std::map<std::string, std::shared_ptr<nodes::AsyncNode>> outputs;
  for (const auto& [port, unused] : schema->outputs) {
    outputs[port] = *(*action)->GetOutput(port, false);
  }
  outcome.status = (*action)->Run().status();
  if (!outcome.status.ok()) return outcome;
  for (const auto& [port, unused] : schema->inputs) {
    const std::shared_ptr<nodes::AsyncNode> node =
        *(*action)->GetInput(port, false);
    const auto given = inputs.find(port);
    if (given != inputs.end()) {
      for (const std::string& value : given->second) {
        EXPECT_TRUE(node->PutChunk(JsonChunk(value)).Await().ok());
      }
    }
    EXPECT_TRUE(node->PutNullFinal().Await().ok());
    EXPECT_TRUE(node->DrainAndClose().Await().ok());
  }
  outcome.status =
      (*action)->Wait(absl::Seconds(20)).Await(absl::Now() + absl::Seconds(30))
          .status();
  for (auto& [port, node] : outputs) outcome.outputs[port] = Collect(node);
  return outcome;
}

// --- The cases ---------------------------------------------------------------

TEST(FlowRuntimeTest, PipesAPortIntoAPort) {
  const Outcome outcome = RunFlow(R"(
flow copy {
  in  words: string stream
  out said:  string stream
  words -> said
}
)",
                              "copy", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("said"), Values({"\"a\"", "\"b\""}));
}

TEST(FlowRuntimeTest, StagesReshapeAStream) {
  const Outcome outcome = RunFlow(R"(
flow shape {
  in  words: string stream
  out kept:  string stream
  out total: integer
  words | first 2 -> kept
  words | count -> total
}
)",
                              "shape",
                              {{"words", {"\"a\"", "\"b\"", "\"c\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("kept"), Values({"\"a\"", "\"b\""}));
  EXPECT_EQ(outcome.outputs.at("total"), Values({"3"}));
}

TEST(FlowRuntimeTest, RunsACallAndReadsItsOutput) {
  const Outcome outcome = RunFlow(R"(
flow shout {
  in  words:   string stream
  out loudest: string stream
  say = run twice(text: words)
  say.out -> loudest
}
)",
                              "shout", {{"words", {"\"hi\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("loudest"), Values({"\"HI\"", "\"HI\""}));
}

TEST(FlowRuntimeTest, ANodeOfItsOwnIsWrittenAndReadBack) {
  const Outcome outcome = RunFlow(R"(
flow keep {
  in  words: string stream
  out all:   json
  seen = node()
  words -> seen
  seen | collect -> all
}
)",
                              "keep", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("all"), Values({"[\"a\", \"b\"]"}));
}

TEST(FlowRuntimeTest, SkipDrainsAnOutputNobodyWants) {
  const Outcome outcome = RunFlow(R"(
flow ignore {
  in  words: string stream
  out done:  string
  say = run twice(text: words)
  skip say.out
  "finished" -> done
}
)",
                              "ignore", {{"words", {"\"a\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("done"), Values({"\"finished\""}));
}

TEST(FlowRuntimeTest, ForEachRunsABodyPerValue) {
  const Outcome outcome = RunFlow(R"(
flow each {
  in  words: string stream
  out lines: string stream
  for word in words {
    word | strformat "<%s>" -> lines
  }
}
)",
                              "each", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("lines"), Values({"\"<a>\"", "\"<b>\""}));
}

TEST(FlowRuntimeTest, ForEachSeesTheSameOuterValueEveryPass) {
  // The materialisation rule: `prefix` is read inside the loop, so it is
  // buffered once in the scope that owns it and replayed to every pass.
  const Outcome outcome = RunFlow(R"(
flow prefixed {
  in  prefix: string
  in  words:  string stream
  out lines:  string stream
  for word in words {
    strformat("%s:%s", prefix, word) -> lines
  }
}
)",
                              "prefixed",
                              {{"prefix", {"\"p\""}},
                               {"words", {"\"a\"", "\"b\"", "\"c\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("lines"),
            Values({"\"p:a\"", "\"p:b\"", "\"p:c\""}));
}

TEST(FlowRuntimeTest, ALoopReadsAnOuterStreamBeforeItCloses) {
  // The buffer a materialised ref goes through used to read its source to the
  // *end* before handing out a single value, so a loop whose body read anything
  // from outside it could not start until that stream was finished.
  //
  // Here it cannot be finished until the loop has run: `held` gets its second
  // value from `seen`, and `seen` is written only by the loop's *second* pass.
  // The pass matters -- a pass cannot block itself, since every step of a body
  // runs concurrently, so the dependency has to cross passes to be real. With
  // the old buffer, pass one waits for `held` to end, `held` waits for pass two,
  // and pass two waits for pass one.
  //
  // The bare `wait held timeout 2s` is what makes that a failing test rather
  // than a hanging one: the timeout fails a step, which stops the monitor and
  // ends the run with `deadline_exceeded`.
  const Outcome outcome = RunFlow(R"(
flow woven {
  in  items: string stream
  out out:   string stream

  held = node()
  seen = node()

  "first" -> held
  seen | first 1 | strformat "second after %s" -> held
  wait held timeout 2s

  for one in items {
    held | first 1 -> out
    if index > 0 {
      one -> seen
    }
  }
}
)",
                              "woven", {{"items", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  // Every pass replays from the start, which is what replaying means, so both
  // see the value that was there when the buffer was first read.
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"first\"", "\"first\""}));
}

TEST(FlowRuntimeTest, TwoValueReadsOfOneStreamAreTwoValues) {
  // Reading a stream where a value is expected used to take its *first* value and
  // silently throw the rest away, however many places read it. The value reads of
  // one ref now share a single view of it and take turns: two reads are two
  // values. Which turn each gets is not defined, so this asserts the set rather
  // than the order -- `after` is what a flow that cares about the order writes.
  const Outcome outcome = RunFlow(R"(
flow twice {
  in  words: string stream
  out out:   string stream

  strformat("%s", words) -> out
  strformat("%s", words) -> out
}
)",
                              "twice", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  Values seen = outcome.outputs.at("out");
  std::sort(seen.begin(), seen.end());
  EXPECT_EQ(seen, Values({"\"a\"", "\"b\""}));
}

TEST(FlowRuntimeTest, OneExpressionNamingAValueTwiceReadsItOnce) {
  // The turn is per ref per evaluation, not per mention: naming `word` twice in
  // one `strformat` must not take two values off the stream.
  const Outcome outcome = RunFlow(R"(
flow doubled {
  in  words: string stream
  out out:   string stream

  let word = words
  strformat("%s%s", word, word) -> out
}
)",
                              "doubled", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"aa\""}));
}

TEST(FlowRuntimeTest, AUnaryStreamIsSharedRatherThanConsumed) {
  // A port that did not say `stream` carries one value, so every reader of it
  // sees that one value: there are no turns to take.
  const Outcome outcome = RunFlow(R"(
flow shared {
  in  one:  string required
  out out:  string stream

  strformat("a:%s", one) -> out
  strformat("b:%s", one) -> out
}
)",
                              "shared", {{"one", {"\"x\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  Values seen = outcome.outputs.at("out");
  std::sort(seen.begin(), seen.end());
  EXPECT_EQ(seen, Values({"\"a:x\"", "\"b:x\""}));
}

TEST(FlowRuntimeTest, ASecondValueOnAUnaryStreamIsAnError) {
  // The case the old behaviour could not report at all: a port promised one value
  // and got two. Reading the first and ignoring the rest is what a flow author
  // never finds out about, so it is named instead.
  const Outcome outcome = RunFlow(R"(
flow promised {
  in  words: string stream
  out out:   string

  held = node()
  words -> held
  let only = held | first 1
  strformat("%s", only) -> out
}
)",
                              "promised", {{"words", {"\"a\"", "\"b\""}}});
  // `first 1` reduces, so `only` is unary and correct: the reduction is the flow
  // saying which value it means.
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"a\""}));
}

TEST(FlowRuntimeTest, AdvanceBindsTheNextValueOfTheSameStream) {
  // The guarantee `advance` exists for: the first use of the name sees the first
  // value, the second use the second, and so on. It holds however the flow is
  // scheduled -- each binding is the *k*th value of its stream by construction,
  // not because one step ran before another.
  const Outcome outcome = RunFlow(R"(
flow paced {
  in  words: string stream required
  out out:   string stream

  let word = words
  strformat("1:%s", word) -> out
  advance word
  strformat("2:%s", word) -> out
  advance word
  strformat("3:%s", word) -> out
}
)",
                              "paced",
                              {{"words", {"\"a\"", "\"b\"", "\"c\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  Values seen = outcome.outputs.at("out");
  std::sort(seen.begin(), seen.end());
  EXPECT_EQ(seen, Values({"\"1:a\"", "\"2:b\"", "\"3:c\""}));
}

TEST(FlowRuntimeTest, AdvancingPastTheEndBindsNothing) {
  // An empty stream binds nothing, which is what a `let` on one already does, so
  // advancing past the last value is that and not an error.
  const Outcome outcome = RunFlow(R"(
flow overrun {
  in  words: string stream required
  out out:   string stream

  let word = words
  strformat("1:%s", word) -> out
  advance word
  strformat("2:%s", word) -> out
}
)",
                              "overrun", {{"words", {"\"only\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  Values seen = outcome.outputs.at("out");
  std::sort(seen.begin(), seen.end());
  EXPECT_EQ(seen, Values({"\"1:only\"", "\"2:\""}));
}

TEST(FlowRuntimeTest, ATryBlockCatchesWhatFailsInsideIt) {
  // What a block is for: `if` blocks where it stands, and a block is how a flow
  // says which statements that blocking applies to. Bound to a name it reads as a
  // status, exactly as a call does.
  const Outcome bad = RunFlow(R"(
flow blocked {
  in  q:   string required
  out out: string stream

  s = try {
    if q == "bad" {
      fail invalid_argument "no good"
    }
  }
  strformat("ok=%s", s.ok) -> out
}
)",
                              "blocked", {{"q", {"\"bad\""}}});
  ASSERT_TRUE(bad.status.ok()) << bad.status;
  EXPECT_EQ(bad.outputs.at("out"), Values({"\"ok=false\""}));

  const Outcome good = RunFlow(R"(
flow blocked {
  in  q:   string required
  out out: string stream

  s = try {
    if q == "bad" {
      fail invalid_argument "no good"
    }
  }
  strformat("ok=%s", s.ok) -> out
}
)",
                               "blocked", {{"q", {"\"fine\""}}});
  ASSERT_TRUE(good.status.ok()) << good.status;
  EXPECT_EQ(good.outputs.at("out"), Values({"\"ok=true\""}));
}

TEST(FlowRuntimeTest, ABlockWithoutTryEndsTheFlowLikeACall) {
  // Without `try` a failure inside a block is nobody's to handle, so it ends the
  // flow with that status -- the same rule a call follows.
  const Outcome outcome = RunFlow(R"(
flow strictly {
  out out: string stream

  {
    if true {
      fail permission_denied "not yours"
    }
  }
}
)",
                              "strictly");
  EXPECT_EQ(outcome.status.code(), absl::StatusCode::kPermissionDenied);
  EXPECT_EQ(outcome.status.message(), "not yours");
}

TEST(FlowRuntimeTest, MatchPullsFieldsOutAndDropsWhatDoesNotFit) {
  // As a stage it is a `where` and a `map` at once, which is what makes reading a
  // log worth writing: the lines that fit become records, and the ones that do
  // not are gone.
  const Outcome outcome = RunFlow(R"(
flow parsed {
  in  lines: string stream required
  out names: string stream
  out ages:  string stream

  lines | match "name={name} age={age:int}" | map it.name -> names
  lines | match "name={name} age={age:int}" | map it.age  -> ages
}
)",
                              "parsed",
                              {{"lines",
                                {"\"name=Alice   age=27\"", "\"nothing here\"",
                                 "\"name=Bo age=3\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("names"), Values({"\"Alice\"", "\"Bo\""}));
  // `:int` is read as one, so the ages are numbers rather than their text.
  EXPECT_EQ(outcome.outputs.at("ages"), Values({"27", "3"}));
}

TEST(FlowRuntimeTest, MatchAsAFunctionAnswersNullWhenItDoesNotFit) {
  // For one value the answer is null rather than nothing, so `if not obj` is how
  // a flow asks. The fields are there when it does fit.
  const Outcome found = RunFlow(R"(
flow one {
  in  line: string required
  out out:  string

  let who = match("name={name} age={age:int}", line)
  strformat("%s is %d", who.name, who.age) -> out
}
)",
                                "one", {{"line", {"\"name=Alice age=27\""}}});
  ASSERT_TRUE(found.status.ok()) << found.status;
  EXPECT_EQ(found.outputs.at("out"), Values({"\"Alice is 27\""}));

  const Outcome missed = RunFlow(R"(
flow one {
  in  line: string required
  out out:  string

  let who = match("name={name}", line)
  if not who {
    "no name in it" -> out
  }
}
)",
                                 "one", {{"line", {"\"nothing here\""}}});
  ASSERT_TRUE(missed.status.ok()) << missed.status;
  EXPECT_EQ(missed.outputs.at("out"), Values({"\"no name in it\""}));
}

TEST(FlowRuntimeTest, APositionalPatternIsReadByIndex) {
  const Outcome outcome = RunFlow(R"(
flow split {
  in  lines: string stream required
  out out:   string stream

  lines | match "{}:{}" | map it[1] -> out
}
)",
                              "split",
                              {{"lines", {"\"left:right\"", "\"a:b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"right\"", "\"b\""}));
}

TEST(FlowRuntimeTest, APatternThatCannotBeReadIsRefusedBeforeItRuns) {
  // A pattern is a literal almost every time, so a typo in one is the flow's own
  // mistake. The resolver reads it where it is written, which is why this never
  // reaches the runtime at all: the flow is refused with the pattern language's
  // own complaint, and the runtime keeps its own guard for a pattern that was
  // computed rather than written.
  const Outcome outcome = RunFlow(R"(
flow broken {
  in  lines: string stream required
  out out:   string stream

  lines | match "name={name" -> out
}
)",
                              "broken", {{"lines", {"\"name=Alice\""}}});
  EXPECT_EQ(outcome.status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_NE(outcome.status.message().find("with no '}'"), std::string::npos)
      << outcome.status.message();
}

TEST(FlowRuntimeTest, ALetTakesAValueApartByFieldOrByPosition) {
  // `let name, age = user` and `let first, second = pair` are the same statement
  // written twice, and which one is meant is a question about the value: its
  // field where it has one, its position where it is a list. `Lookup` answers by
  // the value's own kind, so this has to ask both ways rather than choose once.
  const Outcome outcome = RunFlow(R"(
flow taken {
  in  users: json stream required
  in  pair:  json required
  out named: string stream
  out placed: string stream

  let name, age = users
  strformat("%s is %s", name, age) -> named

  let first, second = pair
  strformat("%s then %s", first, second) -> placed
}
)",
                              "taken",
                              {{"users", {"{\"name\": \"Alice\", \"age\": 27}"}},
                               {"pair", {"[\"one\", \"two\"]"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("named"), Values({"\"Alice is 27\""}));
  EXPECT_EQ(outcome.outputs.at("placed"), Values({"\"one then two\""}));
}

TEST(FlowRuntimeTest, DestructuringReadsWhatMatchProduces) {
  // The two halves of this stage together: a pattern names the fields, and a
  // `let` takes them apart under those names.
  const Outcome outcome = RunFlow(R"(
flow parsed {
  in  line: string required
  out out:  string

  let name, age = match("name={name} age={age:int}", line)
  strformat("%s/%d", name, age) -> out
}
)",
                              "parsed", {{"line", {"\"name=Bo age=3\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"Bo/3\""}));
}

TEST(FlowRuntimeTest, IfTakesOneBranch) {
  const Outcome outcome = RunFlow(R"(
flow branch {
  in  count: integer
  out said:  string
  if count > 1 {
    "many" -> said
  } else {
    "one" -> said
  }
}
)",
                              "branch", {{"count", {"5"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("said"), Values({"\"many\""}));
}

TEST(FlowRuntimeTest, RepeatCarriesAValueAndStops) {
  const Outcome outcome = RunFlow(R"(
flow counting {
  out steps: integer stream
  repeat total = 0 max 8 {
    total -> steps
    total <- total + 1
    until total >= 2
  }
}
)",
                              "counting");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("steps"), Values({"0", "1", "2"}));
}

TEST(FlowRuntimeTest, TryAndWaitRecoverFromAFailure) {
  const Outcome outcome = RunFlow(R"(
flow recover {
  out said: string
  risky = try run boom()
  check = wait risky
  if not check.ok {
    check.code -> said
  }
}
)",
                              "recover");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("said"), Values({"\"NOT_FOUND\""}));
}

TEST(FlowRuntimeTest, AFailureWithoutTryEndsTheFlow) {
  const Outcome outcome = RunFlow(R"(
flow strict {
  out said: string
  risky = run boom()
  "unreached" -> said
}
)",
                              "strict");
  EXPECT_EQ(outcome.status.code(), absl::StatusCode::kNotFound);
}

TEST(FlowRuntimeTest, FailEndsTheFlowWithTheCodeItNames) {
  // In an `if`, because a `fail` at the top of a body races every other statement
  // and the language now refuses one. This flow used to be written without the
  // branch and passed because `fail` happened to win that race.
  const Outcome outcome = RunFlow(R"(
flow refuse {
  in  who: string
  out said: string
  if not (who == "friend") {
    fail permission_denied "not for you"
  }
  "welcome" -> said
}
)",
                              "refuse");
  EXPECT_EQ(outcome.status.code(), absl::StatusCode::kPermissionDenied);
  EXPECT_EQ(outcome.status.message(), "not for you");
}

TEST(FlowRuntimeTest, AHeaderIsAStreamOfOneValue) {
  const Outcome outcome = RunFlow(R"(
flow tenanted {
  header "x-tenant" as tenant default "none"
  out said: string
  tenant -> said
}
)",
                              "tenanted", {}, {{"x-tenant", "acme"}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("said"), Values({"\"acme\""}));
}

TEST(FlowRuntimeTest, AHeaderFallsBackToItsDefault) {
  const Outcome outcome = RunFlow(R"(
flow tenanted {
  header "x-tenant" as tenant default "none"
  out said: string
  tenant -> said
}
)",
                              "tenanted");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("said"), Values({"\"none\""}));
}

TEST(FlowRuntimeTest, OneFlowCallsAnother) {
  const Outcome outcome = RunFlow(R"(
flow inner {
  in  word: string
  out said: string
  word | strformat "[%s]" -> said
}

flow outer {
  in  word: string
  out said: string
  step = run inner(word: word)
  step.said -> said
}
)",
                              "outer", {{"word", {"\"x\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("said"), Values({"\"[x]\""}));
}

TEST(FlowRuntimeTest, SeveralWritersShareOneOutput) {
  const Outcome outcome = RunFlow(R"(
flow both {
  in  words: string stream
  out all:   string stream
  words | first 1 -> all
  words | last 1 -> all
}
)",
                              "both", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  Values got = outcome.outputs.at("all");
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, Values({"\"a\"", "\"b\""}));
}

TEST(FlowRuntimeTest, ThenOrdersTwoStreamsIntoOne) {
  const Outcome outcome = RunFlow(R"(
flow ordered {
  in  first:  string stream
  in  second: string stream
  out all:    string stream
  first then second -> all
}
)",
                              "ordered",
                              {{"first", {"\"1\""}}, {"second", {"\"2\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("all"), Values({"\"1\"", "\"2\""}));
}

TEST(FlowRuntimeTest, SkipCountTakesValuesOffForEveryReader) {
  const Outcome outcome = RunFlow(R"(
flow headless {
  in  rows: string stream
  out kept: string stream
  out seen: integer
  skip 1 rows
  rows -> kept
  rows | count -> seen
}
)",
                              "headless",
                              {{"rows", {"\"h\"", "\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("kept"), Values({"\"a\"", "\"b\""}));
  EXPECT_EQ(outcome.outputs.at("seen"), Values({"2"}));
}

TEST(FlowRuntimeTest, CompileReportsTheFirstErrorWithItsPlace) {
  const absl::StatusOr<std::shared_ptr<CompiledProgram>> program =
      CompiledProgram::Compile("flow bad {\n  nowhere -> out\n}\n", "bad.flow");
  ASSERT_FALSE(program.ok());
  EXPECT_TRUE(absl::StrContains(program.status().message(), "bad.flow:2:"))
      << program.status().message();
}

// --- one value ---------------------------------------------------------------
//
// Everything else here is a stream, and that is the right default. Some of what
// moves through a flow is one value, though, and these are about it having a
// name that can be compared, branched on and cut up.

TEST(FlowRuntimeTest, LetBindsOneValueThatReadsAsAValue) {
  const Outcome outcome = RunFlow(R"(
flow gate {
  in  codes:  number stream
  out verdict: string
  let code = codes
  if code >= 200 and code < 300 {
    "ok" -> verdict
  } else {
    "not ok" -> verdict
  }
}
)",
                              "gate", {{"codes", {"204", "500"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  // The *first* value: `let` is one value, whatever the stream went on to say.
  EXPECT_EQ(outcome.outputs.at("verdict"), Values({"\"ok\""}));
}

TEST(FlowRuntimeTest, OneValueIsReadableInSeveralPlacesAtOnce) {
  // The stream behind it is read once and replayed, which is what the analysis
  // does for anything two things read -- so a value can be used as freely as a
  // variable in any other language.
  const Outcome outcome = RunFlow(R"(
flow twice {
  in  words: string stream
  out shout: string
  out size:  number
  out same:  bool
  let word = words
  word | map upper(it) -> shout
  strformat("%d", len(word)) | map number(it) -> size
  word == word -> same
}
)",
                              "twice", {{"words", {"\"hi\"", "\"ignored\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("shout"), Values({"\"HI\""}));
  EXPECT_EQ(outcome.outputs.at("size"), Values({"2"}));
  EXPECT_EQ(outcome.outputs.at("same"), Values({"true"}));
}

TEST(FlowRuntimeTest, OneValueOfAnEmptyStreamIsNothing) {
  // Not a failure: a stream that produced nothing is a perfectly ordinary thing
  // for a flow to meet, and `if not x` is how it asks.
  const Outcome outcome = RunFlow(R"(
flow absent {
  in  words: string stream
  out said:  string
  let word = words
  if not word {
    "nothing arrived" -> said
  } else {
    word -> said
  }
}
)",
                              "absent", {{"words", {}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("said"), Values({"\"nothing arrived\""}));
}

TEST(FlowRuntimeTest, ChunkCutsOneValueIntoSizedPieces) {
  const Outcome outcome = RunFlow(R"(
flow pieces {
  in  whole: string stream
  out parts: string stream
  out count: integer
  let body = whole
  body | chunk 4 -> parts
  body | chunk 4 | count -> count
}
)",
                              "pieces", {{"whole", {"\"abcdefghij\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("parts"),
            Values({"\"abcd\"", "\"efgh\"", "\"ij\""}));
  EXPECT_EQ(outcome.outputs.at("count"), Values({"3"}));
}

TEST(FlowRuntimeTest, ChunkDoesNotCutACharacterInHalf) {
  // Half a code point is not a piece of text anybody can use, so a piece is at
  // most the size asked for and stops at a character boundary.
  const Outcome outcome = RunFlow(R"(
flow careful {
  in  whole: string stream
  out parts: string stream
}
)",
                              "careful", {{"whole", {}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;

  const Outcome cut = RunFlow(R"(
flow careful {
  in  whole: string stream
  out parts: string stream
  let body = whole
  body | chunk 3 -> parts
}
)",
                          "careful", {{"whole", {"\"ééé\""}}});
  ASSERT_TRUE(cut.status.ok()) << cut.status;
  // Each `é` is two bytes, so three bytes takes one whole one and stops.
  EXPECT_EQ(cut.outputs.at("parts"),
            Values({"\"\\u00e9\"", "\"\\u00e9\"", "\"\\u00e9\""}));
}

TEST(FlowRuntimeTest, ChunkLeavesAloneWhatItCannotCut) {
  const Outcome outcome = RunFlow(R"(
flow whole {
  in  records: json stream
  out parts:   json stream
  records | chunk 4 -> parts
}
)",
                              "whole", {{"records", {"{\"a\": 1}", "7"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("parts"), Values({"{\"a\": 1}", "7"}));
}

// --- zip ---------------------------------------------------------------------
//
// The interesting cases are all about *ending*: two streams almost never run out
// together, and what a joint iteration does at the ragged end is the whole of
// what makes it usable or not.

TEST(FlowRuntimeTest, ZipReadsStreamsInStep) {
  const Outcome outcome = RunFlow(R"(
flow together {
  in  left:   string stream
  in  right:  string stream
  out joined: string stream
  zip(left, right) | map strformat("%s%s", it[0], it[1]) -> joined
}
)",
                              "together",
                              {{"left", {"\"a\"", "\"b\"", "\"c\""}},
                               {"right", {"\"1\"", "\"2\"", "\"3\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("joined"),
            Values({"\"a1\"", "\"b2\"", "\"c3\""}));
}

TEST(FlowRuntimeTest, ZipPadsAStreamThatEndsWellWithNulls) {
  // The short one contributes a null from then on, so the long one is read to
  // its end rather than truncated to the shorter -- which is what lets a stream
  // of values be zipped against a stream of the few annotations somebody made.
  const Outcome outcome = RunFlow(R"(
flow ragged {
  in  left:   string stream
  in  right:  string stream
  out joined: json stream
  out count:  integer
  zip(left, right) -> joined
  zip(left, right) | count -> count
}
)",
                              "ragged",
                              {{"left", {"\"a\"", "\"b\"", "\"c\""}},
                               {"right", {"\"1\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("joined"),
            Values({"[\"a\", \"1\"]", "[\"b\", null]", "[\"c\", null]"}));
  // It ends when *every* source has, and the round in which the last one ended
  // produced nothing but the nulls that say so.
  EXPECT_EQ(outcome.outputs.at("count"), Values({"3"}));

  // The other way round reads the same, so which argument is shorter is not
  // something an author has to think about.
  const Outcome mirrored = RunFlow(R"(
flow ragged {
  in  left:   string stream
  in  right:  string stream
  out joined: json stream
  zip(left, right) -> joined
}
)",
                               "ragged",
                               {{"left", {"\"a\""}},
                                {"right", {"\"1\"", "\"2\""}}});
  ASSERT_TRUE(mirrored.status.ok()) << mirrored.status;
  EXPECT_EQ(mirrored.outputs.at("joined"),
            Values({"[\"a\", \"1\"]", "[null, \"2\"]"}));
}

TEST(FlowRuntimeTest, ZipEndsWithTheStatusOfASourceThatEndedBadly) {
  // A tuple missing a value for a reason nobody has been told about is worse
  // than no tuple at all, so a source that aborts ends the whole iteration with
  // its status rather than being padded with nulls like one that finished.
  const Outcome outcome = RunFlow(R"(
flow risky {
  in  left:   string stream
  out joined: json stream
  bad = run boom()
  zip(left, bad.out) -> joined
}
)",
                              "risky", {{"left", {"\"a\"", "\"b\""}}});
  ASSERT_FALSE(outcome.status.ok());
  EXPECT_EQ(outcome.status.code(), absl::StatusCode::kNotFound);
}

TEST(FlowRuntimeTest, ZipIsAStreamLikeAnyOther) {
  // Everything a flow can do to a stream it can do to one of these: choose
  // among the tuples, drop from the front, reduce them, and wait for the whole
  // thing to finish.
  const Outcome outcome = RunFlow(R"(
flow ordinary {
  in  left:  string stream
  in  right: string stream
  out first_two: json stream
  out dropped:   json stream
  out total:     integer
  out drained:   string

  pairs = node()
  zip(left, right) -> pairs
  pairs | first 2 -> first_two
  zip(left, right) | drop 2 -> dropped
  zip(left, right) | count -> total
  done = drain pairs
  done.code -> drained
}
)",
                              "ordinary",
                              {{"left", {"\"a\"", "\"b\"", "\"c\""}},
                               {"right", {"\"1\"", "\"2\"", "\"3\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("first_two"),
            Values({"[\"a\", \"1\"]", "[\"b\", \"2\"]"}));
  EXPECT_EQ(outcome.outputs.at("dropped"), Values({"[\"c\", \"3\"]"}));
  EXPECT_EQ(outcome.outputs.at("total"), Values({"3"}));
  EXPECT_EQ(outcome.outputs.at("drained"), Values({"\"OK\""}));
}

TEST(FlowRuntimeTest, AForLoopMayTakeATupleApartByName) {
  // What makes `zip` worth having: the alternative is one name and `it[0]`
  // everywhere, and a tuple whose parts have names reads like the two streams
  // it came from.
  const Outcome outcome = RunFlow(R"(
flow named {
  in  urls:   string stream
  in  titles: string stream
  out lines:  string stream
  for url, title in zip(urls, titles) {
    strformat("%s = %s", url, title) -> lines
  }
}
)",
                              "named",
                              {{"urls", {"\"/a\"", "\"/b\""}},
                               {"titles", {"\"A\"", "\"B\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("lines"),
            Values({"\"/a = A\"", "\"/b = B\""}));
}

TEST(FlowRuntimeTest, OneNameStillTakesTheWholeValue) {
  const Outcome outcome = RunFlow(R"(
flow whole {
  in  words: string stream
  out seen:  json stream
  for pair in zip(words, words) {
    pair -> seen
  }
}
)",
                              "whole", {{"words", {"\"a\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("seen"), Values({"[\"a\", \"a\"]"}));
}

TEST(FlowRuntimeTest, ZipOfOneStreamIsThatStreamInTuples) {
  const Outcome outcome = RunFlow(R"(
flow alone {
  in  words: string stream
  out only:  json stream
  zip(words) -> only
}
)",
                              "alone", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("only"), Values({"[\"a\"]", "[\"b\"]"}));
}

TEST(FlowRuntimeTest, ZipTakesAValueAsAStreamOfOne) {
  // The same rule a pipeline's source follows, so `zip(items, "tag")` does not
  // need the tag wrapped in anything.
  const Outcome outcome = RunFlow(R"(
flow tagged {
  in  words:  string stream
  out joined: json stream
  zip(words, "tag") -> joined
}
)",
                              "tagged", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("joined"),
            Values({"[\"a\", \"tag\"]", "[\"b\", null]"}));
}

}  // namespace
}  // namespace a11::flow
