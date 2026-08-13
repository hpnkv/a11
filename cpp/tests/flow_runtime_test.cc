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
  const Outcome outcome = RunFlow(R"(
flow refuse {
  out said: string
  fail permission_denied "not for you"
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

}  // namespace
}  // namespace a11::flow
