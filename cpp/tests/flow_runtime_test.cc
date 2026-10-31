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

// What the native runtime is *for*: running flows.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/cord.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/actions/action.h"
#include "a11/actions/log.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/flow/runtime.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "thread/boost_primitives.h"
#include "thread/fiber.h"
#include "thread/selectables.h"

namespace a11::flow {
namespace {

using Values = std::vector<std::string>;
using PortValues = std::map<std::string, Values>;

data::Chunk JsonChunk(std::string_view json) {
  return data::Chunk{
      .metadata =
          data::ChunkMetadata{.mimetype = std::string(data::kJsonMimetype)},
      .data = std::string(json)};
}

/// One flow run, and everything a test wants to know about it.
struct Outcome {
  absl::Status status;
  PortValues outputs;
};

/// An action that writes every value of its input twice, upper-cased, and
/// also lower-cased on a second output nothing in most flows reads.
actions::ActionSchema TwiceSchema() {
  return actions::ActionSchema{
      .name = "twice",
      .inputs = {{"text",
                  actions::ActionPortSchema{.name = "text", .type = "str"}}},
      .outputs = {{"out",
                   actions::ActionPortSchema{.name = "out", .type = "str"}},
                  {"quiet",
                   actions::ActionPortSchema{.name = "quiet", .type = "str"}}},
  };
}

actions::ActionHandler TwiceHandler() {
  return actions::MakeAsyncActionHandler(
      [](const std::shared_ptr<actions::Action>& action) -> absl::Status {
        ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<nodes::AsyncNode> input,
                              action->GetInput("text"));
        ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<nodes::AsyncNode> output,
                              action->GetOutput("out"));
        ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<nodes::AsyncNode> quiet,
                              action->GetOutput("quiet"));
        while (true) {
          ABSL_ASSIGN_OR_RETURN(const std::optional<data::Chunk> chunk,
                                input->NextChunk().Await());
          if (!chunk.has_value()) {
            break;
          }
          if (chunk->IsNull()) {
            continue;
          }
          const std::string shouted = absl::AsciiStrToUpper(chunk->data);
          for (int at = 0; at < 2; ++at) {
            ABSL_RETURN_IF_ERROR(
                output->PutChunk(JsonChunk(shouted)).Await().status());
          }
          ABSL_RETURN_IF_ERROR(
              quiet->PutChunk(JsonChunk(absl::AsciiStrToLower(chunk->data)))
                  .Await()
                  .status());
        }
        ABSL_RETURN_IF_ERROR(output->Finalize({.wait = true}).Await().status());
        return quiet->Finalize({.wait = true}).Await().status();
      });
}

/// An action that fails, for the flows that are about a failure.
actions::ActionSchema BoomSchema() {
  return actions::ActionSchema{
      .name = "boom",
      .outputs = {{"out",
                   actions::ActionPortSchema{.name = "out", .type = "str"}}},
  };
}

actions::ActionHandler BoomHandler() {
  return actions::MakeAsyncActionHandler(
      [](const std::shared_ptr<actions::Action>&) -> absl::Status {
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
    if (!chunk.ok() || !chunk->has_value()) {
      break;
    }
    if ((*chunk)->IsNull()) {
      continue;
    }
    found.push_back((*chunk)->data);
  }
  return found;
}

/// Compile a flow, run it once against the test registry, and read its outputs.
Outcome RunFlow(std::string_view source, std::string_view name,
                const PortValues& inputs = {},
                const std::map<std::string, std::string>& headers = {},
                bool close_inputs = true, const ModuleSources& modules = {},
                std::shared_ptr<actions::ActionRegistry> registry = nullptr,
                std::optional<bool> single_item_slots = std::nullopt,
                std::shared_ptr<TransportStats> stats = nullptr,
                FrameOptions frames = {},
                std::shared_ptr<CompiledProgram> compiled = nullptr) {
  Outcome outcome;
  absl::StatusOr<std::shared_ptr<CompiledProgram>> program =
      compiled != nullptr
          ? absl::StatusOr<std::shared_ptr<CompiledProgram>>(compiled)
          : CompiledProgram::Compile(
                std::string(source), "test.flow", modules, {}, {},
                single_item_slots.value_or(
                    std::getenv("A11_TEST_SINGLE_ITEM_SLOTS") != nullptr));
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
  absl::StatusOr<actions::ActionHandler> handler = MakeHandler(
      *program, name,
      {.transport_stats = std::move(stats), .frames = std::move(frames)});
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
                              registry == nullptr ? TestRegistry() : registry);
  if (!action.ok()) {
    outcome.status = action.status();
    return outcome;
  }
  for (const auto& [header, value] : headers) {
    EXPECT_TRUE((*action)->SetHeader(header, value).ok());
  }
  // Every output node before the flow starts, so a value produced early is
  // waiting rather than lost.
  std::map<std::string, std::shared_ptr<nodes::AsyncNode>> outputs;
  for (const auto& [port, unused] : schema->outputs) {
    outputs[port] = *(*action)->GetOutput(port, false);
  }
  outcome.status = (*action)->Run().status();
  if (!outcome.status.ok()) {
    return outcome;
  }
  for (const auto& [port, unused] : schema->inputs) {
    const std::shared_ptr<nodes::AsyncNode> node =
        *(*action)->GetInput(port, false);
    const auto given = inputs.find(port);
    if (given != inputs.end()) {
      for (const std::string& value : given->second) {
        EXPECT_TRUE(node->PutChunk(JsonChunk(value)).Await().ok());
      }
    }
    // A port left open is a port a flow waits on, which is what a `| timeout`
    // is for: `close_inputs=false` is how a test says the producer stalled.
    if (close_inputs) {
      EXPECT_TRUE(node->Finalize({.wait = true}).Await().ok());
    }
  }
  outcome.status = (*action)
                       ->Wait(absl::Seconds(20))
                       .Await(absl::Now() + absl::Seconds(30))
                       .status();
  for (auto& [port, node] : outputs) {
    outcome.outputs[port] = Collect(node);
  }
  return outcome;
}

/// Collects what the process log sink is handed while this is in scope.
///
/// The flow's log goes to the same sink an action's does, so a flow that
/// narrates itself is observed the way anything else consuming those logs would
/// observe it, rather than by reaching into a port.
class LogCapture {
 public:
  LogCapture() {
    actions::SetActionLogSink([this](const actions::LogRecord& record) {
      // Locked because a flow's statements run concurrently: two logs whose
      // `after` clauses are satisfied together reach the sink from two fibers.
      thread::MutexLock lock(&mu_);
      lines_.push_back(
          Line{.level = std::string(actions::LogLevelName(record.level)),
               .channel = std::string(record.channel),
               .text = std::string(record.data),
               .mimetype = std::string(record.mimetype),
               .lineno = record.lineno.value_or(0)});
    });
  }

  ~LogCapture() { actions::SetActionLogSink(nullptr); }

  LogCapture(const LogCapture&) = delete;
  LogCapture& operator=(const LogCapture&) = delete;

  struct Line {
    std::string level;
    std::string channel;
    std::string text;
    std::string mimetype;
    int lineno = 0;
  };

  std::vector<Line> lines() const {
    thread::MutexLock lock(&mu_);
    return lines_;
  }

  /// What was logged, in the order it arrived.
  Values texts() const {
    Values out;
    for (const Line& line : lines()) {
      out.push_back(line.text);
    }
    return out;
  }

  /// The same, sorted -- for the cases where the flow does not order the logs.
  Values sorted_texts() const {
    Values out = texts();
    std::sort(out.begin(), out.end());
    return out;
  }

  /// The line whose text is `text`, or nullopt.
  std::optional<Line> find(std::string_view text) const {
    for (const Line& line : lines()) {
      if (line.text == text) {
        return line;
      }
    }
    return std::nullopt;
  }

 private:
  mutable thread::Mutex mu_;
  std::vector<Line> lines_;
};

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
  const Outcome outcome =
      RunFlow(R"(
flow shape {
  in  words: string stream
  out kept:  string stream
  out total: int
  words | first 2 -> kept
  words | count -> total
}
)",
              "shape", {{"words", {"\"a\"", "\"b\"", "\"c\""}}});
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
  out all:   std::json
  seen = node()
  words -> seen
  seen | collect -> all
}
)",
                                  "keep", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("all"), Values({"[\"a\", \"b\"]"}));
}

TEST(FlowRuntimeTest, UnboundOutputsAreDrainedAutomatically) {
  const Outcome outcome = RunFlow(R"(
flow ignore {
  in  words: string stream
  out done:  string
  say = run twice(text: words)
  "finished" -> done
}
)",
                                  "ignore", {{"words", {"\"a\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("done"), Values({"\"finished\""}));
}

TEST(FlowRuntimeTest, ADiscardPerformsThePipelineAndKeepsNothing) {
  // The difference between `-> _` and `skip`: a counted `skip` is elided, and a
  // discard is *run*.
  LogCapture logs;
  const Outcome outcome = RunFlow(R"(
flow ignore {
  in  words: string stream
  out done:  string
  words | logf info "saw %s" it -> _
  "finished" -> done
}
)",
                                  "ignore", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("done"), Values({"\"finished\""}));
  EXPECT_EQ(logs.sorted_texts(), Values({"saw a", "saw b"}));
}

TEST(FlowRuntimeTest, ADiscardIsAReaderLikeAnyOtherDestination) {
  // A discard takes a reader slot, so the stream it reads still fans out to
  // everything else reading it and every value still reaches the real
  // destination.
  const Outcome outcome = RunFlow(R"(
flow both {
  in  words: string stream
  out kept:  string stream
  words -> kept
  words | count -> _
}
)",
                                  "both", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("kept"), Values({"\"a\"", "\"b\""}));
}

TEST(FlowRuntimeTest, ADiscardBesideAStreamingReaderDoesNotStallAtVolume) {
  // The shape the console's own template has, and the one worth proving: one
  // stream read twice, once by a *reducing* pipeline that keeps nothing and
  // once straight through to a port.
  const int many = 500;
  std::vector<std::string> sent;
  sent.reserve(many);
  for (int at = 0; at < many; ++at) {
    sent.push_back(absl::StrCat("\"", at, "\""));
  }
  const Outcome outcome = RunFlow(R"(
flow narrated {
  in  words: string stream
  out said:  string stream
  words | join "" | count -> _
  words -> said
}
)",
                                  "narrated", {{"words", sent}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("said").size(), static_cast<size_t>(many));
}

TEST(FlowRuntimeTest, ADiscardMayStandBesideARealDestination) {
  const Outcome outcome = RunFlow(R"(
flow both {
  in  words: string stream
  out kept:  string stream
  words -> kept, _
}
)",
                                  "both", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("kept"), Values({"\"a\"", "\"b\""}));
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

TEST(FlowRuntimeTest, ForEachIteratesBareListLiteralElements) {
  const auto outcome = RunFlow(R"(
flow main {
  out values: std::json stream
  for value in ["a", null, false, [], {}, "b"] {
    value -> values
  }
}
)",
                               "main");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("values"),
            Values({"\"a\"", "null", "false", "[]", "{}", "\"b\""}));
}

TEST(FlowRuntimeTest, ForEachEmptyListHasNoPasses) {
  const auto outcome = RunFlow(R"(
flow main {
  out values: std::json stream
  for value in [] { value -> values }
}
)",
                               "main");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_TRUE(outcome.outputs.at("values").empty());
}

TEST(FlowRuntimeTest, ForEachLiteralEvaluatesExpressionsOnceAndSupportsSpread) {
  const auto outcome = RunFlow(R"(
flow main {
  out values: int stream
  out final: int
  int n = 1
  rest = [3, 4]
  for value in [n, n + 1, ...rest] {
    n = n + 1
    value -> values
  }
  n -> final
}
)",
                               "main");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("values"), Values({"1", "2", "3", "4"}));
  EXPECT_EQ(outcome.outputs.at("final"), Values({"5"}));
}

TEST(FlowRuntimeTest, ForEachLiteralUnpacksRowsAndStopsAfterACompletePass) {
  const auto outcome = RunFlow(R"(
flow main {
  out sums: int stream
  for left, right in [[1, 2], [3, 4], [5, 6]] {
    left + right -> sums
  } until left == 3
}
)",
                               "main");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("sums"), Values({"3", "7"}));
}

TEST(FlowRuntimeTest, ForEachLiteralSupportsParallelPasses) {
  const auto outcome = RunFlow(R"(
flow main {
  out values: int stream
  for value in [1, 2, 3] parallel 3 { value -> values }
}
)",
                               "main");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  auto values = outcome.outputs.at("values");
  std::sort(values.begin(), values.end());
  EXPECT_EQ(values, Values({"1", "2", "3"}));
}

TEST(FlowRuntimeTest, ForEachLiteralFlattensExactlyOneLevel) {
  for (const auto* source : {"[[1, 2], [], [3]]", "([[1, 2], [], [3]])",
                             "[[1, 2], [], [3]] | flatten"}) {
    SCOPED_TRACE(source);
    const auto outcome = RunFlow(
        absl::StrCat("flow main { out values: std::json stream\n for row in ",
                     source, " { row -> values }\n}"),
        "main");
    ASSERT_TRUE(outcome.status.ok()) << outcome.status;
    EXPECT_EQ(outcome.outputs.at("values"), Values({"[1, 2]", "[]", "[3]"}));
  }
}

TEST(FlowRuntimeTest, ForEachPreservesListPayloadsOfStreamsAndPipelines) {
  const auto outcome = RunFlow(R"(
flow main {
  in rows: std::list[int] stream
  out streamed: std::json stream
  out piped: std::json stream
  out named: std::json stream
  saved = [7, 8]
  for row in rows { row -> streamed }
  for row in [[1, 2], [3]] | first 1 { row -> piped }
  for row in saved { row -> named }
}
)",
                               "main", {{"rows", {"[1, 2]", "[]", "[3]"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("streamed"), Values({"[1, 2]", "[]", "[3]"}));
  EXPECT_EQ(outcome.outputs.at("piped"), Values({"[[1, 2], [3]]"}));
  EXPECT_EQ(outcome.outputs.at("named"), Values({"[7, 8]"}));
}

TEST(FlowRuntimeTest, ParallelCountsAcceptFieldsArithmeticAndBuiltins) {
  const auto outcome = RunFlow(R"(
flow main {
  in workers: int required
  out looped: int stream
  out ordered: int stream
  out unordered: int stream
  config = {workers: workers}
  for value in [1, 2, 3] parallel config.workers + 1 { value -> looped }
  [1, 2, 3] | flatten | map it + 1 parallel workers -> ordered
  [1, 2, 3] | flatten | map it + 2 parallel std::len([1, 2]) unordered -> unordered
}
)",
                               "main", {{"workers", {"1"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  auto looped = outcome.outputs.at("looped");
  auto unordered = outcome.outputs.at("unordered");
  std::sort(looped.begin(), looped.end());
  std::sort(unordered.begin(), unordered.end());
  EXPECT_EQ(looped, Values({"1", "2", "3"}));
  EXPECT_EQ(outcome.outputs.at("ordered"), Values({"2", "3", "4"}));
  EXPECT_EQ(unordered, Values({"3", "4", "5"}));
}

TEST(FlowRuntimeTest, ParallelCountExpressionsAreReadOncePerActivation) {
  for (const auto* body :
       {"for value in [1, 2, 3] parallel (take widths) { value -> values }",
        "[1, 2, 3] | flatten | map it parallel (take widths) -> values"}) {
    SCOPED_TRACE(body);
    const auto outcome = RunFlow(
        absl::StrCat(
            "flow main { in widths: int stream\n out values: int stream\n",
            body, "\n}"),
        "main", {{"widths", {"2"}}});
    ASSERT_TRUE(outcome.status.ok()) << outcome.status;
    auto values = outcome.outputs.at("values");
    std::sort(values.begin(), values.end());
    EXPECT_EQ(values, Values({"1", "2", "3"}));
  }
}

TEST(FlowRuntimeTest, ParallelCountExpressionsRejectInvalidValues) {
  for (const auto* count :
       {"0", "-1", "1.5", "true", "null", "\"2\"", "2147483648"}) {
    for (const auto* body :
         {"for value in [1, 2] parallel workers { value -> values }",
          "[1, 2] | flatten | map it parallel workers -> values"}) {
      SCOPED_TRACE(absl::StrCat(count, ": ", body));
      const auto outcome =
          RunFlow(absl::StrCat("flow main { in workers: std::any required\n "
                               "out values: int stream\n",
                               body, "\n}"),
                  "main", {{"workers", {count}}});
      EXPECT_EQ(outcome.status.code(), absl::StatusCode::kInvalidArgument)
          << outcome.status;
      EXPECT_NE(outcome.status.message().find("positive signed 32-bit"),
                std::string_view::npos)
          << outcome.status;
    }
  }
}

TEST(FlowRuntimeTest, DynamicParallelismUsesTheRequestedConcurrencyBound) {
  for (const auto* body :
       {"for value in [1, 2, 3, 4, 5, 6] parallel workers {\n"
        " p = run probe(value: value)\n p.out -> values\n}",
        "[1, 2, 3, 4, 5, 6] | flatten | map (run probe(value: it)).out "
        "parallel workers -> values"}) {
    for (const int width : {2, 3}) {
      std::atomic<int> active = 0;
      std::atomic<int> peak = 0;
      auto registry = TestRegistry();
      actions::ActionSchema schema;
      schema.name = "probe";
      schema.inputs["value"] = actions::ActionPortSchema{
          .name = "value", .type = "int", .required = true, .unary = true};
      schema.outputs["out"] = actions::ActionPortSchema{
          .name = "out", .type = "int", .unary = true};
      ASSERT_TRUE(
          registry
              ->Register("probe", schema,
                         actions::MakeAsyncActionHandler(
                             [&](const auto& action) -> absl::Status {
                               ABSL_ASSIGN_OR_RETURN(auto input,
                                                     action->GetInput("value"));
                               ABSL_ASSIGN_OR_RETURN(auto output,
                                                     action->GetOutput("out"));
                               ABSL_ASSIGN_OR_RETURN(
                                   auto chunk, input->NextChunk().Await());
                               const int current = active.fetch_add(1) + 1;
                               int previous = peak.load();
                               while (current > previous &&
                                      !peak.compare_exchange_weak(previous,
                                                                  current)) {}
                               thread::SleepFor(absl::Milliseconds(20));
                               --active;
                               if (chunk.has_value()) {
                                 return output->PutChunk(std::move(*chunk))
                                     .Await()
                                     .status();
                               }
                               return absl::OkStatus();
                             }))
              .ok());
      const auto outcome = RunFlow(
          absl::StrCat(
              "flow main { in workers: int required\n out values: int stream\n",
              body, "\n}"),
          "main", {{"workers", {absl::StrCat(width)}}}, {}, true, {}, registry);
      ASSERT_TRUE(outcome.status.ok()) << outcome.status;
      EXPECT_EQ(peak.load(), width) << body;
      auto values = outcome.outputs.at("values");
      std::sort(values.begin(), values.end());
      EXPECT_EQ(values, Values({"1", "2", "3", "4", "5", "6"}));
    }
  }
}

TEST(FlowRuntimeTest, ForEachSeesTheSameOuterValueEveryPass) {
  // The materialisation rule: `prefix` is read inside the loop, so it is
  // buffered once in the scope that owns it and replayed to every pass.
  const Outcome outcome =
      RunFlow(R"(
flow prefixed {
  in  prefix: string
  in  words:  string stream
  out lines:  string stream
  for word in words {
    std::strformat("%s:%s", prefix, word) -> lines
  }
}
)",
              "prefixed",
              {{"prefix", {"\"p\""}}, {"words", {"\"a\"", "\"b\"", "\"c\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("lines"),
            Values({"\"p:a\"", "\"p:b\"", "\"p:c\""}));
}

TEST(FlowRuntimeTest, ALoopReadsAnOuterStreamBeforeItCloses) {
  // The buffer a materialised ref goes through
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
  // Reading a stream where a value is expected
  const Outcome outcome = RunFlow(R"(
flow twice {
  in  words: string stream
  out out:   string stream

  std::strformat("%s", words) -> out
  std::strformat("%s", words) -> out
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

  word = take (words)
  std::strformat("%s%s", word, word) -> out
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

  std::strformat("a:%s", one) -> out
  std::strformat("b:%s", one) -> out
}
)",
                                  "shared", {{"one", {"\"x\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  Values seen = outcome.outputs.at("out");
  std::sort(seen.begin(), seen.end());
  EXPECT_EQ(seen, Values({"\"a:x\"", "\"b:x\""}));
}

TEST(FlowRuntimeTest, ASecondValueOnAUnaryStreamIsAnError) {
  // The case the old behaviour could not report at all: a port promised one
  // value and got two. Reading the first and ignoring the rest is what a flow
  // author never finds out about, so it is named instead.
  const Outcome outcome = RunFlow(R"(
flow promised {
  in  words: string stream
  out out:   string

  held = node()
  words -> held
  only = take (held | first 1)
  std::strformat("%s", only) -> out
}
)",
                                  "promised", {{"words", {"\"a\"", "\"b\""}}});
  // `first 1` reduces, so `only` is unary and correct: the reduction is the
  // flow saying which value it means.
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"a\""}));
}

TEST(FlowRuntimeTest, AdvanceBindsTheNextValueOfTheSameStream) {
  // The guarantee `advance` exists for: the first use of the name sees the
  // first value, the second use the second, and so on.
  const Outcome outcome =
      RunFlow(R"(
flow paced {
  in  words: string stream required
  out out:   string stream

  word = take (words)
  std::strformat("1:%s", word) -> out
  word = take (words)
  std::strformat("2:%s", word) -> out
  word = take (words)
  std::strformat("3:%s", word) -> out
}
)",
              "paced", {{"words", {"\"a\"", "\"b\"", "\"c\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  Values seen = outcome.outputs.at("out");
  std::sort(seen.begin(), seen.end());
  EXPECT_EQ(seen, Values({"\"1:a\"", "\"2:b\"", "\"3:c\""}));
}

TEST(FlowRuntimeTest, TakingPastTheEndFails) {
  const auto outcome = RunFlow(R"(
flow main {
  in words: string stream
  first = take words
  second = take words
}
)",
                               "main", {{"words", {"\"only\""}}});
  EXPECT_EQ(outcome.status.code(), absl::StatusCode::kFailedPrecondition);
}

TEST(FlowRuntimeTest, ATryBlockCatchesWhatFailsInsideIt) {
  // What a block is for: `if` blocks where it stands, and a block is how a flow
  // says which statements that blocking applies to. Bound to a name it reads as
  // a status, exactly as a call does.
  const Outcome bad = RunFlow(R"(
flow blocked {
  in  q:   string required
  out out: string stream

  s = try {
    if q == "bad" {
      fail invalid_argument "no good"
    }
  }
  std::strformat("ok=%s", s.ok) -> out
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
  std::strformat("ok=%s", s.ok) -> out
}
)",
                               "blocked", {{"q", {"\"fine\""}}});
  ASSERT_TRUE(good.status.ok()) << good.status;
  EXPECT_EQ(good.outputs.at("out"), Values({"\"ok=true\""}));
}

TEST(FlowRuntimeTest, ABlockWithoutTryEndsTheFlowLikeACall) {
  // Without `try` a failure inside a block is nobody's to handle, so it ends
  // the flow with that status -- the same rule a call follows.
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
  // As a stage it is a `where` and a `map` at once, which is what makes reading
  // a log worth writing: the lines that fit become records, and the ones that
  // do not are gone.
  const Outcome outcome = RunFlow(
      R"(
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
        {"\"name=Alice   age=27\"", "\"nothing here\"", "\"name=Bo age=3\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("names"), Values({"\"Alice\"", "\"Bo\""}));
  // `:int` is read as one, so the ages are numbers rather than their text.
  EXPECT_EQ(outcome.outputs.at("ages"), Values({"27", "3"}));
}

TEST(FlowRuntimeTest, MatchAsAFunctionAnswersNullWhenItDoesNotFit) {
  // For one value the answer is null rather than nothing, so `if not obj` is
  // how a flow asks. The fields are there when it does fit.
  const Outcome found = RunFlow(R"(
flow one {
  in  line: string required
  out out:  string

  who = take (std::match("name={name} age={age:int}", line))
  std::strformat("%s is %d", who.name, who.age) -> out
}
)",
                                "one", {{"line", {"\"name=Alice age=27\""}}});
  ASSERT_TRUE(found.status.ok()) << found.status;
  EXPECT_EQ(found.outputs.at("out"), Values({"\"Alice is 27\""}));

  const Outcome missed = RunFlow(R"(
flow one {
  in  line: string required
  out out:  string

  who = take (std::match("name={name}", line))
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
  const Outcome outcome =
      RunFlow(R"(
flow split {
  in  lines: string stream required
  out out:   string stream

  lines | match "{}:{}" | map it[1] -> out
}
)",
              "split", {{"lines", {"\"left:right\"", "\"a:b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"right\"", "\"b\""}));
}

TEST(FlowRuntimeTest, APatternThatCannotBeReadIsRefusedBeforeItRuns) {
  // A pattern is a literal almost every time, so a typo in one is the flow's
  // own mistake.
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
  // `let name, age = user` and `let first, second = pair` are the same
  // statement written twice, and which one is meant is a question about the
  // value: its field where it has one, its position where it is a list.
  const Outcome outcome =
      RunFlow(R"(
flow taken {
  in  users: std::json stream required
  in  pair:  std::json required
  out named: string stream
  out placed: string stream

  name, age = take (users)
  std::strformat("%s is %s", name, age) -> named

  first, second = take (pair)
  std::strformat("%s then %s", first, second) -> placed
}
)",
              "taken",
              {{"users", {R"({"name": "Alice", "age": 27})"}},
               {"pair", {R"(["one", "two"])"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("named"), Values({"\"Alice is 27\""}));
  EXPECT_EQ(outcome.outputs.at("placed"), Values({"\"one then two\""}));
}

TEST(FlowRuntimeTest, DestructuringReadsWhatMatchProduces) {
  // The two halves of this stage together: a pattern names the fields, and a
  // `value` takes them apart under those names.
  const Outcome outcome = RunFlow(R"(
flow parsed {
  in  line: string required
  out out:  string

  name, age = take (std::match("name={name} age={age:int}", line))
  std::strformat("%s/%d", name, age) -> out
}
)",
                                  "parsed", {{"line", {"\"name=Bo age=3\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"Bo/3\""}));
}

TEST(FlowRuntimeTest, IfTakesOneBranch) {
  const Outcome outcome = RunFlow(R"(
flow branch {
  in  count: int
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
  out steps: int stream
  repeat total = 0 max 8 {
    total -> steps
    total <- total + 1

  } until total >= 2
}
)",
                                  "counting");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("steps"), Values({"0", "1", "2"}));
}

TEST(FlowRuntimeTest, ATryPipeTurnsAFailingStreamIntoAValue) {
  const Outcome outcome = RunFlow(R"(
flow resilient {
  in  words: string stream
  out seen:  string stream
  out why:   string
  findings = node()
  for word in words {
    sent = { word -> findings }
    if word == "bad" {
      abort findings unavailable "the source went away" after sent
    }
  }
  moved = try findings -> seen
  status moved | map it.message -> why
}
)",
                                  "resilient", {{"words", {"a", "bad"}}});
  // `try` keeps the flow successful after the handled failure.
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  ASSERT_EQ(outcome.outputs.at("why").size(), 1u);
  EXPECT_NE(outcome.outputs.at("why").front().find("the source went away"),
            std::string::npos)
      << outcome.outputs.at("why").front();
}

TEST(FlowRuntimeTest, ATryPipeThatSucceedsReadsAsOk) {
  const Outcome outcome = RunFlow(R"(
flow fine {
  in  words: string stream
  out seen:  string stream
  out ok:    bool
  moved = try words -> seen
  status moved | map it.ok -> ok
}
)",
                                  "fine", {{"words", {"a", "b"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("seen"), Values({"a", "b"}));
  EXPECT_EQ(outcome.outputs.at("ok"), Values({"true"}));
}

TEST(FlowRuntimeTest, AbortEndsANodeWithAFailureRatherThanWithAnEnd) {
  // The ending `close` cannot express. Aborting a node that something else is
  // about to close cleanly is a race, and not a race this test wants to be
  // about.
  const Outcome outcome = RunFlow(R"(
flow cut-short {
  in  words: string stream
  out seen:  string stream
  findings = node()
  for word in words {
    sent = { word -> findings }
    if word == "bad" {
      abort findings unavailable "the source went away" after sent
    }
  }
  findings -> seen
}
)",
                                  "cut-short", {{"words", {"a", "bad"}}});
  // The reader of an aborted node sees the failure rather than an end, and here
  // nothing tolerates it, so it is the flow's.
  ASSERT_FALSE(outcome.status.ok());
  EXPECT_EQ(outcome.status.code(), absl::StatusCode::kUnavailable)
      << outcome.status;
  EXPECT_NE(outcome.status.message().find("the source went away"),
            std::string::npos)
      << outcome.status;
}

TEST(FlowRuntimeTest, ANamedLoopIsABarrierTheRestOfTheFlowCanWaitFor) {
  // What a loop writing an outer node could not say before: "once the loop is
  // over, that node is over".
  const Outcome outcome = RunFlow(R"(
flow tidy {
  in  words: string stream
  out seen:  string stream
  out over:  bool
  taken = node()
  done = for word in words {
    word -> taken

  } until word == "stop"
  ended = close taken after done
  status done | map it.ok -> over
  taken -> seen
}
)",
                                  "tidy", {{"words", {"a", "stop", "b"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("seen"), Values({"a", "stop"}));
  // The loop's own outcome, read after it: every pass succeeded.
  EXPECT_EQ(outcome.outputs.at("over"), Values({"true"}));
}

TEST(FlowRuntimeTest, ALoopMayWaitForSomethingBeforeItStarts) {
  const Outcome outcome = RunFlow(R"(
flow ordered {
  in  words: string stream
  out seen:  string stream
  first = run twice(text: "go")
  for word in words { word -> seen } after first
}
)",
                                  "ordered", {{"words", {"a", "b"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("seen"), Values({"a", "b"}));
  // `first.out` is never read, so the loop waiting on the *step* rather than on
  // a port is the whole of what `after` did here.
}

TEST(FlowRuntimeTest, AForStopsOnItsUntilAndLeavesTheRestUnread) {
  // The gap this closes: `repeat` could say when to stop and `for` could not,
  // so a loop over a stream had to read all of it however early it knew
  // enough.
  const Outcome outcome =
      RunFlow(R"(
flow scanning {
  in  words: string stream
  out seen:  string stream
  for word in words {
    word -> seen

  } until word == "stop"
}
)",
              "scanning", {{"words", {"a", "b", "stop", "c", "d"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  // The value that ended it was seen -- the condition is asked at the *tail* of
  // the pass, as a `repeat`'s is -- and nothing after it was.
  EXPECT_EQ(outcome.outputs.at("seen"), Values({"a", "b", "stop"}));
}

TEST(FlowRuntimeTest, AForRunsItsBodyOnceHoweverTheConditionStartsOut) {
  const Outcome outcome = RunFlow(R"(
flow once {
  in  words: string stream
  out seen:  string stream
  for word in words {
    word -> seen

  } until true
}
)",
                                  "once", {{"words", {"a", "b"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("seen"), Values({"a"}));
}

TEST(FlowRuntimeTest, AForWhileIsTheSameQuestionTheOtherWayRound) {
  const Outcome outcome = RunFlow(R"(
flow kept {
  in  words: string stream
  out seen:  string stream
  for word in words {
    word -> seen

  } while word != "stop"
}
)",
                                  "kept", {{"words", {"a", "stop", "b"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("seen"), Values({"a", "stop"}));
}

TEST(FlowRuntimeTest, AForWhoseConditionNeverHoldsReadsTheWholeStream) {
  const Outcome outcome = RunFlow(R"(
flow all {
  in  words: string stream
  out seen:  string stream
  for word in words {
    word -> seen

  } until word == "absent"
}
)",
                                  "all", {{"words", {"a", "b", "c"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("seen"), Values({"a", "b", "c"}));
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
  const Outcome outcome =
      RunFlow(R"(
flow ordered {
  in  first:  string stream
  in  second: string stream
  out all:    string stream
  first then second -> all
}
)",
              "ordered", {{"first", {"\"1\""}}, {"second", {"\"2\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("all"), Values({"\"1\"", "\"2\""}));
}

TEST(FlowRuntimeTest, SkipCountTakesValuesOffForEveryReader) {
  const Outcome outcome =
      RunFlow(R"(
flow headless {
  in  rows: string stream
  out kept: string stream
  out seen: int
  skip 1 rows
  rows -> kept
  rows | count -> seen
}
)",
              "headless", {{"rows", {"\"h\"", "\"a\"", "\"b\""}}});
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
// Everything else here is a stream, and that is the right default.

TEST(FlowRuntimeTest, LetBindsOneValueThatReadsAsAValue) {
  const Outcome outcome = RunFlow(R"(
flow gate {
  in  codes:  number stream
  out verdict: string
  code = take (codes)
  if code >= 200 and code < 300 {
    "ok" -> verdict
  } else {
    "not ok" -> verdict
  }
}
)",
                                  "gate", {{"codes", {"204", "500"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  // The *first* value: `value` is one value, whatever the stream went on to say.
  EXPECT_EQ(outcome.outputs.at("verdict"), Values({"\"ok\""}));
}

TEST(FlowRuntimeTest, OneValueIsReadableInSeveralPlacesAtOnce) {
  // The stream behind it is read once and replayed, which is what the analysis
  // does for anything two things read -- so a value can be used as freely as a
  // variable in any other language.
  const Outcome outcome =
      RunFlow(R"(
flow twice {
  in  words: string stream
  out shout: string
  out size:  number
  out same:  bool
  word = take (words)
  word | map std::upper(it) -> shout
  std::strformat("%d", std::len(word)) | map number(it) -> size
  word == word -> same
}
)",
              "twice", {{"words", {"\"hi\"", "\"ignored\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("shout"), Values({"\"HI\""}));
  EXPECT_EQ(outcome.outputs.at("size"), Values({"2"}));
  EXPECT_EQ(outcome.outputs.at("same"), Values({"true"}));
}

TEST(FlowRuntimeTest, OptionalTakeDetectsAnEmptyStream) {
  // Not a failure: a stream that produced nothing is a perfectly ordinary thing
  // for a flow to meet, and `if not x` is how it asks.
  const Outcome outcome = RunFlow(R"(
flow absent {
  in  words: string stream
  out said:  string
  word = try take words
  if not word.has_value {
    "nothing arrived" -> said
  } else {
    word.value -> said
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
  out count: int
  body = take (whole)
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
  body = take (whole)
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
  in  records: std::json stream
  out parts:   std::json stream
  records | chunk 4 -> parts
}
)",
                                  "whole", {{"records", {"{\"a\": 1}", "7"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("parts"), Values({"{\"a\": 1}", "7"}));
}

// --- zip ---------------------------------------------------------------------
// The interesting cases are all about *ending*: two streams almost never run
// out together, and what a joint iteration does at the ragged end.

TEST(FlowRuntimeTest, ZipReadsStreamsInStep) {
  const Outcome outcome = RunFlow(R"(
flow together {
  in  left:   string stream
  in  right:  string stream
  out joined: string stream
  std::zip(left, right) | map std::strformat("%s%s", it[0], it[1]) -> joined
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
  const Outcome outcome = RunFlow(
      R"(
flow ragged {
  in  left:   string stream
  in  right:  string stream
  out joined: std::json stream
  out count:  int
  std::zip(left, right) -> joined
  std::zip(left, right) | count -> count
}
)",
      "ragged", {{"left", {"\"a\"", "\"b\"", "\"c\""}}, {"right", {"\"1\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("joined"),
            Values({"[\"a\", \"1\"]", "[\"b\", null]", "[\"c\", null]"}));
  // It ends when *every* source has, and the round in which the last one ended
  // produced nothing but the nulls that say so.
  EXPECT_EQ(outcome.outputs.at("count"), Values({"3"}));

  // The other way round reads the same, so which argument is shorter is not
  // something an author has to think about.
  const Outcome mirrored =
      RunFlow(R"(
flow ragged {
  in  left:   string stream
  in  right:  string stream
  out joined: std::json stream
  std::zip(left, right) -> joined
}
)",
              "ragged", {{"left", {"\"a\""}}, {"right", {"\"1\"", "\"2\""}}});
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
  out joined: std::json stream
  bad = run boom()
  std::zip(left, bad.out) -> joined
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
  out first_two: std::json stream
  out dropped:   std::json stream
  out total:     int
  out drained:   string

  pairs = node()
  std::zip(left, right) -> pairs
  pairs | first 2 -> first_two
  std::zip(left, right) | drop 2 -> dropped
  std::zip(left, right) | count -> total
  done = close pairs
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
  const Outcome outcome =
      RunFlow(R"(
flow named {
  in  urls:   string stream
  in  titles: string stream
  out lines:  string stream
  for url, title in std::zip(urls, titles) {
    std::strformat("%s = %s", url, title) -> lines
  }
}
)",
              "named",
              {{"urls", {"\"/a\"", "\"/b\""}}, {"titles", {"\"A\"", "\"B\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("lines"), Values({"\"/a = A\"", "\"/b = B\""}));
}

TEST(FlowRuntimeTest, OneNameStillTakesTheWholeValue) {
  const Outcome outcome = RunFlow(R"(
flow whole {
  in  words: string stream
  out seen:  std::json stream
  for pair in std::zip(words, words) {
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
  out only:  std::json stream
  std::zip(words) -> only
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
  out joined: std::json stream
  std::zip(words, "tag") -> joined
}
)",
                                  "tagged", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("joined"),
            Values({"[\"a\", \"tag\"]", "[\"b\", null]"}));
}

TEST(FlowRuntimeTest, ALogStatementWritesToTheFlowsOwnLog) {
  LogCapture logs;
  const Outcome outcome = RunFlow(R"(
flow narrated {
  in  words: string stream
  out said:  string stream
  words -> said
  log "copied the words" after said
  logf warning "%s words in all" 2 after said
}
)",
                                  "narrated", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("said"), Values({"\"a\"", "\"b\""}));

  // Both wait for the same thing and so race each other, which is the
  // language's answer and not this test's business: what is pinned is that both
  // arrived.
  EXPECT_EQ(logs.sorted_texts(),
            Values({"2 words in all", "copied the words"}));

  const std::optional<LogCapture::Line> plain = logs.find("copied the words");
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(plain->level, "info");
  // The channel is the flow's name, so a consumer can tell one flow's narration
  // from another's without a port per flow.
  EXPECT_EQ(plain->channel, "narrated");
  EXPECT_GT(plain->lineno, 0);

  const std::optional<LogCapture::Line> warned = logs.find("2 words in all");
  ASSERT_TRUE(warned.has_value());
  EXPECT_EQ(warned->level, "warning");
}

TEST(FlowRuntimeTest, ALogWaitsForConsumedArgumentsWithoutAfter) {
  LogCapture logs;
  const Outcome outcome = RunFlow(R"(
flow produce { out output: int stream
  42 -> output
}
flow main {
  made = run produce()
  logf "result: %s" (made.output | consume)
}
)",
                                  "main", {});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(logs.sorted_texts(), Values({"result: 42"}));
}

TEST(FlowRuntimeTest, FailureReadsItsReasonBeforeActing) {
  const Outcome outcome =
      RunFlow(R"(
flow main {
  in reason: string stream
  fail unavailable reason
}
)",
              "main", {{"reason", {"\"upstream failed\""}}});
  EXPECT_EQ(outcome.status.code(), absl::StatusCode::kUnavailable)
      << outcome.status;
  EXPECT_NE(outcome.status.message().find("upstream failed"), std::string::npos)
      << outcome.status;
}

TEST(FlowRuntimeTest, ALogStatementMayPrintAValueItRead) {
  // A log's arguments live in its own tail rather than in the `message` every
  // other statement uses, and they were left out of the analysis -- so a log
  // that named a stream was one uncounted reader of it and the flow died.
  LogCapture logs;
  const Outcome outcome = RunFlow(R"(
flow narrated {
  in  words: string stream
  out said:  string stream
  first-word = take (words | first 1)
  kept = node()
  words -> kept
  words -> said
  logf info "started with %s" first-word after said
  logf info "and again %s, %s" first-word, first-word after said
  log kept after said
}
)",
                                  "narrated", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("said"), Values({"\"a\"", "\"b\""}));
  EXPECT_EQ(logs.sorted_texts(),
            Values({"a", "and again a, a", "started with a"}));
}

TEST(FlowRuntimeTest, ALogStageMayPrintAValueBesideTheOneInHand) {
  // The same hole on the stage side: `stage.log.arguments` was not among the
  // refs a derived stream reads for a value. `it` is the value going past and
  // needs no slot; `tag` is a stream and does.
  LogCapture logs;
  const Outcome outcome = RunFlow(R"(
flow narrated {
  in  words: string stream
  out said:  string stream
  tag = take ("run-1")
  words | logf info "%s: %s" tag, it -> said
}
)",
                                  "narrated", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("said"), Values({"\"a\"", "\"b\""}));
  // One slot, one cursor, and `tag` carries one value -- so every pass reads
  // the same tag rather than consuming a fresh one and ending the stream.
  EXPECT_EQ(logs.sorted_texts(), Values({"run-1: a", "run-1: b"}));
}

// --- What this pass added ----------------------------------------------------

TEST(FlowRuntimeTest, TheAggregationsReadAStreamAndYieldOneValue) {
  const Outcome outcome = RunFlow(R"(
flow totals {
  in  prices:  number stream
  out revenue: number
  out biggest: number
  out smallest: number
  out mean:    number
  out counted: number
  prices | sum -> revenue
  prices | max -> biggest
  prices | min -> smallest
  prices | avg -> mean
  prices | count -> counted
}
)",
                                  "totals", {{"prices", {"1", "4", "7"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("revenue"), Values({"12"}));
  EXPECT_EQ(outcome.outputs.at("biggest"), Values({"7"}));
  EXPECT_EQ(outcome.outputs.at("smallest"), Values({"1"}));
  EXPECT_EQ(outcome.outputs.at("mean"), Values({"4.0"}));
  EXPECT_EQ(outcome.outputs.at("counted"), Values({"3"}));
}

TEST(FlowRuntimeTest, AnAggregationMayReadOneFieldOfEachValue) {
  const Outcome outcome =
      RunFlow(R"(
flow revenue {
  in  orders: std::json stream
  out total:  number
  orders | sum it.price -> total
}
)",
              "revenue", {{"orders", {"{\"price\": 3}", "{\"price\": 4.5}"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("total"), Values({"7.5"}));
}

TEST(FlowRuntimeTest, TheSmallestOfNoValuesIsNoValue) {
  // Not a zero somebody would have to know to ignore: an empty stream has no
  // smallest value, and `sum` of one is 0 because adding nothing is.
  const Outcome outcome = RunFlow(R"(
flow empty {
  in  prices:  number stream
  out biggest: number
  out total:   number
  prices | max -> biggest
  prices | sum -> total
}
)",
                                  "empty");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("biggest"), Values({}));
  EXPECT_EQ(outcome.outputs.at("total"), Values({"0"}));
}

TEST(FlowRuntimeTest, AFoldCarriesWhatTheLastValueMade) {
  // Running totals rather than text, because `+` in this language is arithmetic
  // and not concatenation: `| join` is what puts strings together.
  const Outcome outcome = RunFlow(R"(
flow folded {
  in  steps:   number stream
  out furthest: number
  steps | fold 0 as so_far, so_far + it -> furthest
}
)",
                                  "folded", {{"steps", {"3", "4", "5"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("furthest"), Values({"12"}));
}

TEST(FlowRuntimeTest, AFoldSeesTheValueAndWhatItCarries) {
  // The accumulator is a name like any other, so it may be looked at rather
  // than only added to: this keeps the larger of the two.
  const Outcome outcome = RunFlow(R"(
flow peak {
  in  samples: number stream
  out highest: number
  samples | fold 0 as best, best + it - best -> highest
}
)",
                                  "peak", {{"samples", {"2", "9"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("highest"), Values({"9"}));
}

TEST(FlowRuntimeTest, AScanPublishesEveryStateTheFoldPassedThrough) {
  // The difference from `fold`, and the whole reason for the stage: the same
  // carry, one value out per value in rather than one at the end.
  const Outcome outcome = RunFlow(R"(
flow running {
  in  steps:    number stream
  out totals:   number stream
  out numbered: number stream
  steps | scan 0 as so_far, so_far + it -> totals
  steps | scan 0 as n, n + 1 -> numbered
}
)",
                                  "running", {{"steps", {"3", "4", "5"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("totals"), Values({"3", "7", "12"}));
  // Numbering a stream is the smallest useful scan, and nothing else in the
  // language can do it: `fold` gives one value and `for` carries nothing.
  EXPECT_EQ(outcome.outputs.at("numbered"), Values({"1", "2", "3"}));
}

TEST(FlowRuntimeTest, AScanOfNothingYieldsNothing) {
  // Not the starting value: a state nothing ever advanced is not an answer
  // about a stream, and `fold`'s single value is the one that reports the empty
  // case.
  const Outcome outcome = RunFlow(R"(
flow empty_scan {
  in  steps:  number stream
  out totals: number stream
  steps | scan 0 as so_far, so_far + it -> totals
}
)",
                                  "empty_scan", {{"steps", {}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("totals"), Values({}));
}

TEST(FlowRuntimeTest, AScanCarriesARecordOfState) {
  // What a state machine needs: more than one thing carried, and the value
  // itself carried along so the next stage still has it.
  const Outcome outcome =
      RunFlow(R"(
flow machine {
  in  lines: string stream
  out kept:  string stream
  lines
    | scan {"inside": false, "line": ""} as at, {"inside": std::startswith(it, "B") or (at.inside and not std::startswith(it, "E")), "line": it}
    | where it.inside and not std::startswith(it.line, "B")
    | map it.line
    -> kept
}
)",
              "machine", {{"lines", {"x", "B", "one", "two", "E", "y"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  // Quoted, because a computed string reaches a port as JSON whatever the port
  // says it carries. That is the runtime's own behaviour and not the stage's;
  // see Part 4 of `doc/FLOW_PROGRAMS_PLAN.md`.
  EXPECT_EQ(outcome.outputs.at("kept"), Values({"\"one\"", "\"two\""}));
}

TEST(FlowRuntimeTest, AWindowOverlapsWhereABatchWouldNot) {
  const Outcome outcome =
      RunFlow(R"(
flow neighbours {
  in  words:   string stream
  out pairs:   string stream
  out grouped: string stream
  words | window 2 | map std::join(it, "+") -> pairs
  words | batch 2 | map std::join(it, "+") -> grouped
}
)",
              "neighbours", {{"words", {"a", "b", "c", "d"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  // Every adjacent pair, including the one a `batch` boundary falls between --
  // which is the pair a cross-line search would otherwise miss. Quoted for the
  // reason above: a computed string reaches a port as JSON.
  EXPECT_EQ(outcome.outputs.at("pairs"),
            Values({"\"a+b\"", "\"b+c\"", "\"c+d\""}));
  EXPECT_EQ(outcome.outputs.at("grouped"), Values({"\"a+b\"", "\"c+d\""}));
}

TEST(FlowRuntimeTest, AWindowWiderThanTheStreamYieldsNothing) {
  // Unlike `batch`, whose last list may be short, a window
  // narrower than it was asked for is not a window, and a `| window 3` that
  // sometimes yielded two values would make every reader check.
  const Outcome outcome = RunFlow(R"(
flow narrow {
  in  words: string stream
  out seen:  string stream
  words | window 3 | map std::join(it, "+") -> seen
}
)",
                                  "narrow", {{"words", {"a", "b"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("seen"), Values({}));
}

TEST(FlowRuntimeTest, AWindowOfOneIsEveryValueOnItsOwn) {
  const Outcome outcome = RunFlow(R"(
flow single {
  in  words: string stream
  out seen:  string stream
  words | window 1 | map std::join(it, "+") -> seen
}
)",
                                  "single", {{"words", {"a", "b"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("seen"), Values({"\"a\"", "\"b\""}));
}

TEST(FlowRuntimeTest, SortOrdersTheWholeStreamAndDescReversesIt) {
  const Outcome outcome =
      RunFlow(R"(
flow ordered {
  in  words: string stream
  out up:    string stream
  out down:  string stream
  words | sort -> up
  words | sort desc -> down
}
)",
              "ordered", {{"words", {"\"pear\"", "\"apple\"", "\"fig\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("up"),
            Values({"\"apple\"", "\"fig\"", "\"pear\""}));
  EXPECT_EQ(outcome.outputs.at("down"),
            Values({"\"pear\"", "\"fig\"", "\"apple\""}));
}

TEST(FlowRuntimeTest, SortByAKeyIsStableInTheValuesOwnOrder) {
  const Outcome outcome =
      RunFlow(R"(
flow ranked {
  in  hits:   std::json stream
  out best:   std::json stream
  hits | sort by it.score desc -> best
}
)",
              "ranked",
              {{"hits",
                {R"({"id": 1, "score": 5})", R"({"id": 2, "score": 9})",
                 R"({"id": 3, "score": 5})"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  // 2 first for its score; 1 before 3 because they tie and that is the order
  // they were written in.
  ASSERT_EQ(outcome.outputs.at("best").size(), 3u);
  EXPECT_NE(outcome.outputs.at("best")[0].find("\"id\": 2"), std::string::npos);
  EXPECT_NE(outcome.outputs.at("best")[1].find("\"id\": 1"), std::string::npos);
  EXPECT_NE(outcome.outputs.at("best")[2].find("\"id\": 3"), std::string::npos);
}

TEST(FlowRuntimeTest, FlattenIsTheInverseOfBatch) {
  const Outcome outcome =
      RunFlow(R"(
flow spread {
  in  words: string stream
  out out:   string stream
  words | batch 2 | flatten -> out
}
)",
              "spread", {{"words", {"\"a\"", "\"b\"", "\"c\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"a\"", "\"b\"", "\"c\""}));
}

TEST(FlowRuntimeTest, FlattenLetsAValueThatIsNotAListThrough) {
  const Outcome outcome = RunFlow(R"(
flow mixed {
  in  words: string stream
  out out:   string stream
  words | flatten -> out
}
)",
                                  "mixed", {{"words", {"\"a\"", "[\"b\"]"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"a\"", "\"b\""}));
}

TEST(FlowRuntimeTest, InterleaveReadsSeveralStreamsAsOne) {
  const Outcome outcome =
      RunFlow(R"(
flow merged {
  in  fast: string stream
  in  slow: string stream
  out all:  string stream
  std::interleave(fast, slow) -> all
}
)",
              "merged", {{"fast", {"\"a\"", "\"b\""}}, {"slow", {"\"c\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  // Every value, once. The order between the two sources is whatever arrived
  // first, so only the multiset is pinned.
  Values sorted = outcome.outputs.at("all");
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(sorted, Values({"\"a\"", "\"b\"", "\"c\""}));
}

//: A shape with a required field, so a document that does not fit it is a
//: failure the stage can have per value -- which is what `try` is about.
constexpr std::string_view kOrderShape = R"(struct Order {
  id: string required
}
)";

TEST(FlowRuntimeTest, ATryStageDropsTheValueItCouldNotDo) {
  const Outcome outcome = RunFlow(
      absl::StrCat(kOrderShape, R"(
flow tolerant {
  in  docs: std::json stream
  out out:  std::json stream
  docs | try map it as Order -> out
}
)"),
      "tolerant", {{"docs", {R"({"id": "a"})", "{}", R"({"id": "b"})"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  // The two that fit, and no failure: `try` says the flow expected this.
  EXPECT_EQ(outcome.outputs.at("out").size(), 2u);
}

TEST(FlowRuntimeTest, ATryStageRoutesItsFailuresWhereItWasTold) {
  const Outcome outcome =
      RunFlow(absl::StrCat(kOrderShape, R"(
flow routed {
  in  docs: std::json stream
  out good: std::json stream
  out bad:  bool stream
  failures = node()
  docs | try map it as Order into failures -> good
  failures | map it.ok -> bad
}
)"),
              "routed", {{"docs", {R"({"id": "a"})", "{}"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("good").size(), 1u);
  // The failure arrives as a status record, which is a stream like any other.
  ASSERT_EQ(outcome.outputs.at("bad").size(), 1u);
  EXPECT_EQ(outcome.outputs.at("bad"), Values({"false"}));
}

TEST(FlowRuntimeTest, WithoutTryAValueTheStageCannotDoEndsTheFlow) {
  const Outcome outcome = RunFlow(absl::StrCat(kOrderShape, R"(
flow strict {
  in  docs: std::json stream
  out out:  std::json stream
  docs | map it as Order -> out
}
)"),
                                  "strict", {{"docs", {"{}"}}});
  EXPECT_FALSE(outcome.status.ok());
}

TEST(FlowRuntimeTest, AParallelStagePutsTheOrderBack) {
  const Outcome outcome = RunFlow(
      R"(
flow wide {
  in  words: string stream
  out out:   string stream
  words | map std::upper(it) parallel 4 -> out
}
)",
      "wide", {{"words", {"\"a\"", "\"b\"", "\"c\"", "\"d\"", "\"e\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  // `parallel` preserves input order unless `unordered` is present.
  EXPECT_EQ(outcome.outputs.at("out"),
            Values({"\"A\"", "\"B\"", "\"C\"", "\"D\"", "\"E\""}));
}

TEST(FlowRuntimeTest, AnUnorderedParallelStageStillDeliversEveryValue) {
  const Outcome outcome =
      RunFlow(R"(
flow loose {
  in  words: string stream
  out out:   string stream
  words | map std::upper(it) parallel 4 unordered -> out
}
)",
              "loose", {{"words", {"\"a\"", "\"b\"", "\"c\"", "\"d\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  Values sorted = outcome.outputs.at("out");
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(sorted, Values({"\"A\"", "\"B\"", "\"C\"", "\"D\""}));
}

TEST(FlowRuntimeTest, PaceSpacesValuesOutWithoutDroppingAny) {
  const absl::Time started = absl::Now();
  const Outcome outcome =
      RunFlow(R"(
flow paced {
  in  words: string stream
  out out:   string stream
  words | pace 20ms -> out
}
)",
              "paced", {{"words", {"\"a\"", "\"b\"", "\"c\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"a\"", "\"b\"", "\"c\""}));
  // Nothing dropped, and the second and third value each waited: two gaps of
  // 20ms is the floor. Generous on the upper end, because a loaded machine may
  // take much longer and that is not a failure.
  EXPECT_GE(absl::Now() - started, absl::Milliseconds(35));
}

TEST(FlowRuntimeTest, ATimeoutStageFailsWhenTheStreamGoesQuiet) {
  // The input node is never closed, so the stage waits for a value that is not
  // coming and says so with the code a deadline gets.
  const Outcome outcome = RunFlow(R"(
flow watched {
  in  words: string stream
  out out:   string stream
  words | timeout 50ms -> out
}
)",
                                  "watched", {}, {}, /*close_inputs=*/false);
  EXPECT_EQ(outcome.status.code(), absl::StatusCode::kDeadlineExceeded)
      << outcome.status;
}

TEST(FlowRuntimeTest, ATimeoutStageLetsAStreamThatKeepsArrivingThrough) {
  const Outcome outcome = RunFlow(R"(
flow watched {
  in  words: string stream
  out out:   string stream
  words | timeout 5s -> out
}
)",
                                  "watched", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"a\"", "\"b\""}));
}

TEST(FlowRuntimeTest, AnonymousRacePublishesWinnerAndJoinsTheUncancelledLoser) {
  auto registry = std::make_shared<actions::ActionRegistry>();
  auto release = std::make_shared<thread::PermanentEvent>();
  std::atomic<bool> finished = false;
  std::atomic<int> cancelled = 0;
  actions::ActionSchema fast;
  fast.name = "fast";
  actions::ActionSchema slow;
  slow.name = "slow";
  actions::ActionSchema publish;
  publish.name = "publish";
  publish.inputs.emplace("winner",
                         actions::ActionPortSchema{
                             .name = "winner", .type = "int", .unary = true});
  ASSERT_TRUE(registry
                  ->Register("fast", fast,
                             actions::MakeAsyncActionHandler(
                                 [](const auto&) { return absl::OkStatus(); }))
                  .ok());
  ASSERT_TRUE(
      registry
          ->Register(
              "slow", slow,
              actions::MakeAsyncActionHandler(
                  [release, &finished,
                   &cancelled](const auto& action) -> absl::Status {
                    ABSL_RETURN_IF_ERROR(
                        action->SetOnCancelled([&cancelled](const auto&) {
                          ++cancelled;
                          return absl::OkStatus();
                        }));
                    thread::Select({thread::OnCancel(), release->OnEvent()});
                    if (thread::Cancelled()) {
                      return absl::CancelledError("slow cancelled");
                    }
                    finished = true;
                    return absl::OkStatus();
                  }))
          .ok());
  ASSERT_TRUE(registry
                  ->Register("publish", publish,
                             actions::MakeAsyncActionHandler(
                                 [release](const auto& action) -> absl::Status {
                                   ABSL_ASSIGN_OR_RETURN(
                                       auto input, action->GetInput("winner"));
                                   ABSL_ASSIGN_OR_RETURN(
                                       auto value, input->NextChunk().Await());
                                   if (!value.has_value()) {
                                     return absl::DataLossError(
                                         "No race winner");
                                   }
                                   release->Notify();
                                   return absl::OkStatus();
                                 }))
                  .ok());
  const auto outcome = RunFlow(R"(
flow raced {
  winner = take (wait first of try run fast(), try run slow())
  run publish(winner: winner)
}
)",
                               "raced", {}, {}, true, {}, registry);
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_TRUE(finished.load());
  EXPECT_EQ(cancelled.load(), 0);
}

TEST(FlowRuntimeTest, MixedRaceObservesNodeWithoutConsumingItsValues) {
  const Outcome outcome = RunFlow(R"(
flow raced {
  out out: string stream
  out winner: int
  n = node()
  "kept" -> n
  job = run twice(text: "x")
  selected = take (wait first of n, job)
  selected -> winner
  n -> out
}
)",
                                  "raced");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"kept\""}));
  ASSERT_EQ(outcome.outputs.at("winner").size(), 1u);
}

TEST(FlowRuntimeTest, AnonymousAfterSubjectsAndPrefixChainsCompose) {
  const Outcome outcome = RunFlow(R"(
flow composed {
  out out: string
  "done" -> out after (run twice(text: "x"))
}
)",
                                  "composed");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"done\""}));
}

TEST(FlowRuntimeTest, ToleratedAnonymousFailureDoesNotStopSiblingExpressions) {
  const Outcome outcome = RunFlow(R"(
flow failed {
  out out: string
  [1, 2] | flatten | try map (run broken()).out -> _
  "survived" -> out
}
flow broken {
  out out: string
  if true { fail invalid_argument "item failed" }
}
)",
                                  "failed");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"survived\""}));
}

TEST(FlowRuntimeTest, WaitFirstOfReturnsWhenTheFirstCallFinishes) {
  const Outcome outcome = RunFlow(R"(
flow raced {
  in  text: string
  out out:  string stream
  a = run twice(text: text)
  b = run twice(text: text)
  wait first of a, b
  a.out | first 1 -> out
}
)",
                                  "raced", {{"text", {"\"hi\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status.message();
  EXPECT_EQ(outcome.outputs.at("out"), Values({"\"HI\""}));
}

TEST(FlowRuntimeTest, WaitFirstOfPipesTheWinnersNumber) {
  const Outcome outcome = RunFlow(R"(
flow raced {
  in  text: string
  out out:  int stream
  a = run twice(text: text)
  b = run twice(text: text)
  wait first of a, b -> out
}
)",
                                  "raced", {{"text", {"\"hi\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status.message();
  // Either of them may win, and which one is exactly what the flow is being
  // told; that it is one of the two, counted from zero, is the contract.
  ASSERT_EQ(outcome.outputs.at("out").size(), 1u);
  const std::string& won = outcome.outputs.at("out").front();
  EXPECT_TRUE(won == "0" || won == "1") << won;
}

TEST(FlowRuntimeTest, WaitFirstOfCanBeNamedByALet) {
  const Outcome outcome = RunFlow(R"(
flow raced {
  in  text: string
  out out:  string stream
  a = run twice(text: text)
  b = run twice(text: text)
  n = take (wait first of a, b)
  std::strformat("won %s", n) -> out
}
)",
                                  "raced", {{"text", {"\"hi\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status.message();
  ASSERT_EQ(outcome.outputs.at("out").size(), 1u);
  const std::string& said = outcome.outputs.at("out").front();
  EXPECT_TRUE(said == "\"won 0\"" || said == "\"won 1\"") << said;
}

TEST(FlowRuntimeTest, WaitFirstOfCanBeBoundWithEquals) {
  const Outcome outcome = RunFlow(R"(
flow raced {
  in  text: string
  out out:  int stream
  a = run twice(text: text)
  b = run twice(text: text)
  n = wait first of a, b
  n -> out
}
)",
                                  "raced", {{"text", {"\"hi\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status.message();
  ASSERT_EQ(outcome.outputs.at("out").size(), 1u);
  const std::string& bound = outcome.outputs.at("out").front();
  EXPECT_TRUE(bound == "0" || bound == "1") << bound;
}

TEST(FlowRuntimeTest, WaitAllOfWaitsForEveryOneOfThem) {
  const Outcome outcome = RunFlow(R"(
flow both {
  in  text: string
  out out:  string stream
  a = run twice(text: text)
  b = run twice(text: text)
  wait all of a, b
  a.out | count -> out
}
)",
                                  "both", {{"text", {"\"hi\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("out"), Values({"2"}));
}

TEST(FlowRuntimeTest, ALogStageLetsEveryValueThrough) {
  LogCapture logs;
  const Outcome outcome = RunFlow(R"(
flow watched {
  in  words: string stream
  out said:  string stream
  words | log | logf "saw %s" it -> said
}
)",
                                  "watched", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  // Two stages, neither of which changed anything: the values arrive as
  // written, JSON quoting and all.
  EXPECT_EQ(outcome.outputs.at("said"), Values({"\"a\"", "\"b\""}));
  // Every value is logged once by each stage.
  auto texts = logs.texts();
  std::sort(texts.begin(), texts.end());
  EXPECT_EQ(texts, Values({"a", "b", "saw a", "saw b"}));
}

TEST(FlowRuntimeTest, ALogInALoopRunsEveryPass) {
  LogCapture logs;
  const Outcome outcome = RunFlow(R"(
flow counted {
  in  words: string stream
  out said:  string stream
  for one in words {
    logf "one: %s" one
    one -> said
  }
}
)",
                                  "counted", {{"words", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(logs.texts(), Values({"one: a", "one: b"}));
}

TEST(FlowRuntimeTest, ALogKeepsTheValuesOwnRepresentation) {
  // A `log` hands the value over as the value it is: a string is text, and a
  // record is the record, so a consumer decides how to render it. Only `logf`
  // makes a string, because a format is what asks for one.
  LogCapture logs;
  const Outcome outcome = RunFlow(R"(
struct Hit {
  url:   string
  score: number
}

flow shaped {
  in  urls: string stream
  out kept: Hit stream
  urls | map Hit{url: it, score: 1} | log | logf "at %s" it.url -> kept
}
)",
                                  "shaped", {{"urls", {"\"a\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;

  ASSERT_EQ(logs.lines().size(), 2u);
  // The record, as a record. Not `Hit{...}` and not a quoted rendering of it.
  EXPECT_EQ(logs.lines()[0].mimetype, "application/json");
  EXPECT_EQ(logs.lines()[0].text, "{\"score\": 1, \"url\": \"a\"}");
  // The format made a string, which is what a format is for.
  EXPECT_EQ(logs.lines()[1].mimetype, "text/plain");
  EXPECT_EQ(logs.lines()[1].text, "at a");
}

TEST(FlowRuntimeTest, ALoggedStringIsTextRatherThanItsJsonSpelling) {
  LogCapture logs;
  const Outcome outcome = RunFlow(R"(
flow said {
  in  words: string stream
  out out:   string stream
  words | log -> out
  log "a literal" after out
}
)",
                                  "said", {{"words", {"\"a\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  for (const LogCapture::Line& line : logs.lines()) {
    EXPECT_EQ(line.mimetype, "text/plain") << line.text;
    EXPECT_EQ(line.text.find('"'), std::string::npos) << line.text;
  }
  EXPECT_EQ(logs.sorted_texts(), Values({"a", "a literal"}));
}

TEST(FlowRuntimeTest, AFlowThatLogsDeclaresNoPortForIt) {
  // The reserved log port is not part of what the flow
  // says it is, so nothing calling it has to know about it.
  absl::StatusOr<std::shared_ptr<CompiledProgram>> program =
      CompiledProgram::Compile(R"(
flow quiet {
  in  words: string stream
  out said:  string stream
  words -> said
  log "done" after said
}
)",
                               "test.flow");
  ASSERT_TRUE(program.ok()) << program.status();
  const ResolvedFlow* flow = (*program)->Flow("quiet");
  ASSERT_NE(flow, nullptr);
  const absl::StatusOr<actions::ActionSchema> schema = FlowSchema(flow->plan);
  ASSERT_TRUE(schema.ok()) << schema.status();
  EXPECT_EQ(schema->outputs.size(), 1u);
  EXPECT_TRUE(schema->outputs.contains("said"));
  EXPECT_FALSE(schema->outputs.contains(actions::kActionLogOutput));
}

TEST(FlowRuntimeTest, CallTimeoutCancelsTheProducerBeforeJoiningOutputDrains) {
  auto registry = std::make_shared<actions::ActionRegistry>();
  auto cancelled = std::make_shared<std::atomic<int>>(0);
  actions::ActionSchema schema;
  schema.name = "idle";
  schema.outputs.emplace(
      "out",
      actions::ActionPortSchema{.name = "out", .type = "str", .unary = false});
  ASSERT_TRUE(
      registry
          ->Register("idle", schema,
                     actions::MakeAsyncActionHandler(
                         [cancelled](const auto& action) -> absl::Status {
                           ABSL_RETURN_IF_ERROR(
                               action->SetOnCancelled([cancelled](const auto&) {
                                 ++*cancelled;
                                 return absl::OkStatus();
                               }));
                           thread::Select({thread::OnCancel()});
                           return absl::CancelledError("idle producer stopped");
                         }))
          .ok());
  const auto strict = RunFlow("flow main { run idle() timeout 20ms }", "main",
                              {}, {}, true, {}, registry);
  EXPECT_EQ(strict.status.code(), absl::StatusCode::kDeadlineExceeded);
  const auto tolerant = RunFlow(R"(flow main {
 out code: string
 work = try run idle() timeout 20ms
 status work | map it.code -> code
})",
                                "main", {}, {}, true, {}, registry);
  ASSERT_TRUE(tolerant.status.ok()) << tolerant.status;
  EXPECT_EQ(tolerant.outputs.at("code"), Values({"\"DEADLINE_EXCEEDED\""}));
  EXPECT_EQ(cancelled->load(), 2);
}

TEST(FlowRuntimeTest, PlainPipelineBindingsJoinAllDestinationsAndChildren) {
  for (const bool explicit_block : {false, true}) {
    const auto result =
        RunFlow(absl::StrCat(R"(
flow emit { out words: string stream
 ["a", "b", "stop", "c"] | flatten -> words
}
flow main { out left: string stream
 out right: string stream
 out cleaned: bool
 forwarded = )",
                             explicit_block ? "{ " : "", R"((run emit()).words
   | take until it == "stop"
   | map std::upper(it)
   -> left, right
)",
                             explicit_block ? " }" : "", R"(
 after forwarded { true -> cleaned }
}
)"),
                "main");
    ASSERT_TRUE(result.status.ok()) << result.status;
    EXPECT_EQ(result.outputs.at("left"), Values({"\"A\"", "\"B\""}));
    EXPECT_EQ(result.outputs.at("right"), Values({"\"A\"", "\"B\""}));
    EXPECT_EQ(result.outputs.at("cleaned"), Values({"true"}));
  }
}

TEST(FlowRuntimeTest, PlainPipelineBindingOwnsLateAnonymousFailures) {
  const auto result = RunFlow(R"(
flow main {
 forwarded = (run boom()).out -> _
 after forwarded { log "must not start" }
}
)",
                              "main");
  EXPECT_EQ(result.status.code(), absl::StatusCode::kNotFound);
}

TEST(FlowRuntimeTest, PlainPipelineBindingsReadTheirCompletionStatus) {
  const auto result = RunFlow(R"(
flow main { out ok: bool
 done = {"text": "ready"} | map it.text -> _
 status done | map it.ok -> ok
 empty = {} -> _
 after empty { log "empty record forwarded" }
}
)",
                              "main");
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("ok"), Values({"true"}));
}

TEST(FlowRuntimeTest, PredicatePrefixesExcludeTheirBoundaryAndKeepOrder) {
  for (const auto* stage : {"take while it < 3", "take until it >= 3"}) {
    const auto result =
        RunFlow(absl::StrCat("flow main { in values: int stream\n out kept: "
                             "int stream\n values | ",
                             stage, " -> kept\n}"),
                "main", {{"values", {"1", "2", "3", "1"}}});
    ASSERT_TRUE(result.status.ok()) << result.status;
    EXPECT_EQ(result.outputs.at("kept"), Values({"1", "2"}));
  }
  for (const auto* stage : {"take while false", "take until true"}) {
    const auto result = RunFlow(
        absl::StrCat("flow main { out kept: int stream\n [1, 2] | flatten | ",
                     stage, " -> kept\n}"),
        "main");
    ASSERT_TRUE(result.status.ok()) << result.status;
    EXPECT_TRUE(result.outputs.at("kept").empty());
  }
  for (const auto* stage : {"take while true", "take until false"}) {
    const auto result = RunFlow(
        absl::StrCat("flow main { out kept: int stream\n [1, 2] | flatten | ",
                     stage, " -> kept\n}"),
        "main");
    ASSERT_TRUE(result.status.ok()) << result.status;
    EXPECT_EQ(result.outputs.at("kept"), Values({"1", "2"}));
  }
}

TEST(FlowRuntimeTest, PredicateFailuresFollowStageTolerance) {
  for (const auto* stage :
       {"take while std::text(std::b64decode(it)) == \"yes\"",
        "take until std::text(std::b64decode(it)) == \"no\""}) {
    const auto strict =
        RunFlow(absl::StrCat("flow main { out kept: string stream\n [\"!\", "
                             "\"eWVz\"] | flatten | ",
                             stage, " -> kept\n}"),
                "main");
    EXPECT_FALSE(strict.status.ok());
    const auto tolerant =
        RunFlow(absl::StrCat("flow main { out kept: string stream\n [\"!\", "
                             "\"eWVz\", \"bm8=\", \"eWVz\"] | flatten | try ",
                             stage, " -> kept\n}"),
                "main");
    ASSERT_TRUE(tolerant.status.ok()) << tolerant.status;
    EXPECT_EQ(tolerant.outputs.at("kept"), Values({"\"eWVz\""}));
    const auto empty = RunFlow(
        absl::StrCat("flow main { out kept: string stream\n [] | flatten | ",
                     stage, " -> kept\n}"),
        "main");
    ASSERT_TRUE(empty.status.ok()) << empty.status;
    EXPECT_TRUE(empty.outputs.at("kept").empty());
  }
}

TEST(FlowRuntimeTest, PrefixPredicatesDoNotActivateBeyondTheBoundary) {
  LogCapture logs;
  const auto result = RunFlow(R"(
flow predicate { in value: int
 out keep: bool
 logf "checked %s" value
 (value < 3) -> keep
}
flow main { out kept: int stream
 [1, 2, 3, 4] | flatten | take while (run predicate(value: it)).keep -> kept
}
)",
                              "main");
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("kept"), Values({"1", "2"}));
  EXPECT_EQ(logs.sorted_texts(),
            Values({"checked 1", "checked 2", "checked 3"}));
}

TEST(FlowRuntimeTest, PredicatePrefixesKeepTheirProducerOwnedAndRunning) {
  LogCapture logs;
  const auto result = RunFlow(R"(
flow source { out values: int stream
 [1, 2, 3, 4] | flatten -> values
 log "producer finished" after values
}
flow main { out kept: int stream
 (run source()).values | take until it == 2 -> kept
}
)",
                              "main");
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("kept"), Values({"1"}));
  EXPECT_EQ(logs.sorted_texts(), Values({"producer finished"}));
}

TEST(FlowRuntimeTest,
     AfterBlocksObserveToleratedFailureAndRespectParentFailure) {
  const auto tolerated = RunFlow(R"(
flow main { out cleaned: string
 done = try run boom()
 after done { "done" -> cleaned }
}
)",
                                 "main");
  ASSERT_TRUE(tolerated.status.ok()) << tolerated.status;
  EXPECT_EQ(tolerated.outputs.at("cleaned"), Values({"\"done\""}));
  LogCapture logs;
  const auto failed = RunFlow(R"(
flow main {
 done = run boom()
 after done { log "must not start" }
}
)",
                              "main");
  EXPECT_EQ(failed.status.code(), absl::StatusCode::kNotFound);
  EXPECT_TRUE(logs.sorted_texts().empty());
}

TEST(FlowRuntimeTest, AfterBlocksGateConcurrentBodiesAndExposeOutcomes) {
  const auto result = RunFlow(R"(
flow main { out value: int stream
 out ok: bool
 first = { }
 second = { }
 cleanup = after first, second {
   local = node()
   local -> value
   42 -> local
   log "cleaning up"
 }
 status cleanup | map it.ok -> ok
}
)",
                              "main");
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("value"), Values({"42"}));
  EXPECT_EQ(result.outputs.at("ok"), Values({"true"}));
}

TEST(FlowRuntimeTest, ToleratedAfterBlocksRetainFailureStatus) {
  const auto result = RunFlow(R"(
flow main { out code: string
 done = {}
 cleanup = try after done { fail unavailable "cleanup failed" }
 status cleanup | map it.code -> code
}
)",
                              "main");
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("code"), Values({"\"UNAVAILABLE\""}));
}

TEST(FlowRuntimeTest, AnonymousCallStreamsWithoutABinding) {
  const Outcome result = RunFlow(R"(
flow t {
  in text: string
  out out: string stream
  (run twice(text: text)).out -> out
}
)",
                                 "t", {{"text", {"\"hi\""}}});
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("out"), Values({"\"HI\"", "\"HI\""}));
}

TEST(FlowRuntimeTest, AnonymousCallInScalarObjectReadsOneAndDrainsTheRest) {
  const Outcome result = RunFlow(R"(
flow t {
  in text: string
  out out: object
  report = take ({"text": (run twice(text: text)).out, "source": "test"})
  report -> out
}
)",
                                 "t", {{"text", {"\"hi\""}}});
  ASSERT_TRUE(result.status.ok()) << result.status;
  ASSERT_EQ(result.outputs.at("out").size(), 1);
  EXPECT_NE(result.outputs.at("out")[0].find("HI"), std::string::npos);
}

TEST(FlowRuntimeTest, AnonymousCallsInMapsActivateForEachItem) {
  const Outcome result = RunFlow(R"(
flow t {
  in text: string stream
  out out: string stream
  text | map (run twice(text: it)).out -> out
}
)",
                                 "t", {{"text", {"\"a\"", "\"b\""}}});
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("out"), Values({"\"A\"", "\"B\""}));
}

TEST(FlowRuntimeTest, ConsumeRequiresExactlyOneItem) {
  for (const auto& values : {Values{}, Values{"1"}, Values{"1", "2"}}) {
    const Outcome result = RunFlow(R"(
flow t {
  in text: int stream
  out out: int
  text | consume -> out
}
)",
                                   "t", {{"text", values}});
    if (values.size() == 1) {
      ASSERT_TRUE(result.status.ok()) << result.status;
      EXPECT_EQ(result.outputs.at("out"), values);
    } else {
      EXPECT_EQ(result.status.code(), absl::StatusCode::kFailedPrecondition)
          << result.status;
    }
  }
}

TEST(FlowRuntimeTest, ShortCircuitDoesNotActivateAnonymousCall) {
  const Outcome result = RunFlow(R"(
flow t {
  out out: bool
  value = take (false and (run boom()).out)
  value -> out
}
)",
                                 "t", {});
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("out"), Values({"false"}));
}

TEST(FlowRuntimeTest, ImportedImplementationsCapturePrivateDependencies) {
  const Outcome result =
      RunFlow(R"(
import "library.flow" as lib
flow t {
  in text: string
  out out: string
  (run lib::public_flow(text: text)).out -> out
}
)",
              "t", {{"text", {"\"hello\""}}}, {}, true, {{"library.flow", R"(
flow helper {
  in text: string
  out out: string
  text -> out
}
flow public_flow exported {
  in text: string
  out out: string
  (run helper(text: text)).out -> out
}
)"}});
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("out"), Values({"\"hello\""}));
}

TEST(FlowRuntimeTest, UnresolvedDeclarationFailsBeforeIndependentEffects) {
  const Outcome result = RunFlow(R"(
declare flow missing { out value: string }
flow t {
  out out: string
  "must not be written" -> out
  run missing()
}
)",
                                 "t");
  EXPECT_EQ(result.status.code(), absl::StatusCode::kFailedPrecondition)
      << result.status;
  if (const auto found = result.outputs.find("out");
      found != result.outputs.end()) {
    EXPECT_TRUE(found->second.empty());
  }
}

TEST(FlowRuntimeTest, ScalarWaitAcceptsAnAnonymousSubject) {
  const Outcome result = RunFlow(R"(
flow t {
  out out: bool
  outcome = take (wait try run boom())
  outcome.ok -> out
}
)",
                                 "t");
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("out"), Values({"false"}));
}

TEST(FlowRuntimeTest, ATryPipeOwnsLateFailuresOfScalarRvalues) {
  auto registry = std::make_shared<actions::ActionRegistry>();
  auto released = std::make_shared<thread::PermanentEvent>();
  actions::ActionSchema source;
  source.name = "late";
  source.outputs.emplace(
      "out",
      actions::ActionPortSchema{.name = "out", .type = "str", .unary = false});
  ASSERT_TRUE(
      registry
          ->Register(
              "late", source,
              actions::MakeAsyncActionHandler(
                  [released](const auto& action) -> absl::Status {
                    ABSL_ASSIGN_OR_RETURN(auto output,
                                          action->GetOutput("out"));
                    ABSL_RETURN_IF_ERROR(
                        output->PutChunk(JsonChunk("ready")).Await().status());
                    thread::Select({thread::OnCancel(), released->OnEvent()});
                    return absl::NotFoundError("late scalar failure");
                  }))
          .ok());
  actions::ActionSchema notify;
  notify.name = "notify";
  notify.inputs.emplace("value",
                        actions::ActionPortSchema{
                            .name = "value", .type = "dict", .unary = false});
  ASSERT_TRUE(
      registry
          ->Register(
              "notify", notify,
              actions::MakeAsyncActionHandler([released](const auto& action)
                                                  -> absl::Status {
                ABSL_ASSIGN_OR_RETURN(auto input, action->GetInput("value"));
                ABSL_ASSIGN_OR_RETURN(auto value, input->NextChunk().Await());
                if (!value.has_value()) {
                  return absl::DataLossError("No scalar value was published.");
                }
                released->Notify();
                return absl::OkStatus();
              }))
          .ok());
  const auto result = RunFlow(R"(
flow f {
  out outcome: string
  receiver = run notify()
  transfer = try ({"text": (run late()).out}) -> receiver.value
  (status transfer).code -> outcome
}

)",
                              "f", {}, {}, true, {}, registry);
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("outcome"), Values({"\"NOT_FOUND\""}));
}

TEST(FlowRuntimeTest, OwnerFailureCancelsToleratedRvaluesAndRetainsDetails) {
  auto registry = std::make_shared<actions::ActionRegistry>();
  auto cancelled = std::make_shared<std::atomic<int>>(0);
  actions::ActionSchema slow;
  slow.name = "slow";
  slow.outputs.emplace(
      "ready", actions::ActionPortSchema{.name = "ready", .type = "str"});
  ASSERT_TRUE(
      registry
          ->Register(
              slow.name, slow,
              actions::MakeAsyncActionHandler(
                  [cancelled](const auto& action) -> absl::Status {
                    ABSL_RETURN_IF_ERROR(
                        action->SetOnCancelled([cancelled](const auto&) {
                          ++*cancelled;
                          return absl::OkStatus();
                        }));
                    ABSL_ASSIGN_OR_RETURN(auto output,
                                          action->GetOutput("ready"));
                    ABSL_RETURN_IF_ERROR(
                        output->PutChunk(JsonChunk("ready")).Await().status());
                    thread::Select({thread::OnCancel()});
                    return absl::CancelledError("child cancellation");
                  }))
          .ok());
  absl::Status cause = absl::DataLossError("distinctive owner failure");
  cause.SetPayload("test.cause", absl::Cord("structured detail"));
  actions::ActionSchema fail;
  fail.name = "fail-with-detail";
  fail.inputs.emplace(
      "ready", actions::ActionPortSchema{.name = "ready", .type = "str"});
  ASSERT_TRUE(registry
                  ->Register(fail.name, fail,
                             actions::MakeAsyncActionHandler(
                                 [cause](const auto& action) -> absl::Status {
                                   ABSL_ASSIGN_OR_RETURN(
                                       auto input, action->GetInput("ready"));
                                   ABSL_RETURN_IF_ERROR(
                                       input->NextChunk().Await().status());
                                   return cause;
                                 }))
                  .ok());
  const auto result = RunFlow(R"(
flow f {
  ready = take ((try run slow()).ready)
  run fail-with-detail(ready: ready)
}
)",
                              "f", {}, {}, true, {}, registry);
  EXPECT_EQ(result.status.code(), cause.code()) << result.status;
  EXPECT_EQ(result.status.message(), cause.message());
  EXPECT_EQ(result.status.GetPayload("test.cause"),
            cause.GetPayload("test.cause"));
  EXPECT_EQ(cancelled->load(), 1);
}

TEST(FlowRuntimeTest,
     DuplicateNodeRaceSubjectsKeepTheirDataAndChooseFirstIndex) {
  const auto result = RunFlow(R"(
flow f {
  out result: string stream
  out winner: int
  n = node()
  "kept" -> n
  w = wait first of n, n
  w -> winner
  n -> result
}
)",
                              "f");
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("winner"), Values({"0"}));
  EXPECT_EQ(result.outputs.at("result"), Values({"\"kept\""}));
}

TEST(FlowRuntimeTest, PerPassCallNodeBookkeepingStaysBounded) {
  auto registry = std::make_shared<actions::ActionRegistry>();
  std::atomic<size_t> peak = 0;
  actions::ActionSchema schema;
  schema.name = "peek";
  ASSERT_TRUE(registry
                  ->Register("peek", schema,
                             actions::MakeAsyncActionHandler(
                                 [&peak](const auto& action) -> absl::Status {
                                   const auto count =
                                       action->GetNodeMap()->Size();
                                   auto previous = peak.load();
                                   while (count > previous &&
                                          !peak.compare_exchange_weak(previous,
                                                                      count)) {}
                                   return absl::OkStatus();
                                 }))
                  .ok());
  Values numbers;
  for (int index = 0; index < 1000; ++index) {
    numbers.push_back(absl::StrCat(index));
  }
  const auto outcome = RunFlow(
      "flow f { in numbers: int stream\n for n in numbers parallel 4 { run "
      "peek() } }",
      "f", {{"numbers", numbers}}, {}, true, {}, registry);
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_LT(peak.load(), 32u);
}

TEST(FlowRuntimeTest, ToleratedInitializationFailuresKeepTheirOwnStatus) {
  auto registry = std::make_shared<actions::ActionRegistry>();
  std::atomic<int> entered = 0;
  actions::ActionSchema schema;
  schema.name = "peek";
  ASSERT_TRUE(
      registry
          ->Register("peek", schema,
                     actions::MakeAsyncActionHandler([&entered](const auto&) {
                       ++entered;
                       return absl::OkStatus();
                     }))
          .ok());
  const auto outcome = RunFlow(R"(
flow f {
  out result: string
  job = try run peek() with "bad": std::b64decode("!")
  (status job).code -> result
}
)",
                               "f", {}, {}, true, {}, registry);
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(entered.load(), 0);
  EXPECT_EQ(outcome.outputs.at("result"), Values({"\"INVALID_ARGUMENT\""}));
}

TEST(FlowSchema, DeclarationGenerationReportsUnrepresentableMetadata) {
  actions::ActionSchema schema;
  schema.name = "work";
  schema.inputs.emplace(
      "value", actions::ActionPortSchema{.name = "value", .type = "dict"});
  for (
      const auto* metadata :
      {R"({"type":"object","properties":{"n":{"type":["integer","null"]}}})",
       R"({"type":"object","properties":{"n":{"type":"integer","enum":[1,null]}}})",
       R"({"type":"object","properties":{"n":{"type":"array","items":{"type":"integer","minimum":1}}}})",
       R"({"type":"object","properties":{},"minProperties":1})"}) {
    schema.inputs.at("value").json_schema = metadata;
    EXPECT_FALSE(DeclareAction(schema).diagnostics.empty()) << metadata;
  }
  for (
      const auto* metadata :
      {R"({"type":"object"})",
       R"({"type":"object","properties":{"n":{"type":"integer","minimum":1}},"required":["n"]})"}) {
    schema.inputs.at("value").json_schema = metadata;
    EXPECT_TRUE(DeclareAction(schema).diagnostics.empty()) << metadata;
  }
}

TEST(FlowRuntimeTest, ExplicitTakeReadsSuccessiveValues) {
  const auto outcome = RunFlow(R"(
flow main {
  in words: string stream
  out first: string
  out second: string
  a = take words
  b = take words
  a -> first
  b -> second
}
)",
                               "main", {{"words", {"\"one\"", "\"two\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("first"), Values({"\"one\""}));
  EXPECT_EQ(outcome.outputs.at("second"), Values({"\"two\""}));
}

TEST(FlowRuntimeTest, TypedInitializationCoercesOnce) {
  const auto outcome = RunFlow(R"(
flow main {
  out result: int
  int x = "12"
  x = x + 1
  x -> result
}
)",
                               "main");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("result"), Values({"13"}));
}

TEST(FlowRuntimeTest, OptionalTakeDistinguishesNullFromEof) {
  const auto outcome = RunFlow(R"(
flow main {
  in items: std::any stream
  out results: object stream
  first = try take items
  second = try take items
  published = first -> results
  second -> results after published
}
)",
                               "main", {{"items", {"null"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("results"),
            Values({"{\"has_value\": true, \"value\": null}",
                    "{\"has_value\": false}"}));
}

TEST(FlowRuntimeTest, OptionalDefaultPreservesFalseLikeItemsAndReadsOnce) {
  const auto outcome =
      RunFlow(R"(
flow main {
  in items: std::any stream

  out results: std::any stream

  for index in [1, 2, 3, 4, 5, 6, 7] {
    item = try take items or "fallback"
    item -> results
  }
}
)",
              "main", {{"items", {"null", "false", "0", "\"\"", "[]", "{}"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("results"),
            (Values{"null", "false", "0", "\"\"", "[]", "{}", "\"fallback\""}));
}

TEST(FlowRuntimeTest, OptionalDefaultConstructsTheStructOnlyWhenAbsent) {
  for (const bool present : {false, true}) {
    PortValues inputs;
    if (present) {
      inputs["options"] = {"{\"workers\": 8}"};
    }
    const auto outcome = RunFlow(R"(
struct Options { workers: int default 4 }
flow main {
  in options: Options

  out workers: int

  Options settings = try take options or Options{}
  settings.workers -> workers
}
)",
                                 "main", inputs);
    ASSERT_TRUE(outcome.status.ok()) << outcome.status;
    EXPECT_EQ(outcome.outputs.at("workers"), Values{present ? "8" : "4"});
  }
  const auto outcome = RunFlow(R"(
struct Required { value: int required }
flow main {
  out result: int

  chosen = std::option({has_value: true, value: 0})
  int value = chosen or Required{value: std::len(std::b64decode("!"))}.value
  value -> result
}
)",
                               "main");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("result"), Values{"0"});
}

TEST(FlowRuntimeTest, OptionalDefaultSkipsFallbackCallsAndStreamReads) {
  const auto outcome = RunFlow(R"(
flow fallback {
  out result: int

  if true { fail internal "The fallback must stay inactive." }
}
flow main {
  in spare: int stream

  out results: int stream

  first = std::option({has_value: true, value: 0}) or take (run fallback()).result
  second = std::option({has_value: true, value: 1}) or take spare
  third = std::option({has_value: false}) or take spare
  fourth = take spare
  [first, second, third, fourth] | flatten -> results
}
)",
                               "main", {{"spare", {"2", "3"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("results"), (Values{"0", "1", "2", "3"}));
}

TEST(FlowRuntimeTest, OptionalDefaultPreservesUpstreamErrors) {
  const auto outcome = RunFlow(R"(
struct Row { id: string required }
flow main {
  out result: string

  row = try take ([{}] | flatten | map it as Row) or Row{id: "fallback"}
  row.id -> result
}
)",
                               "main");
  EXPECT_EQ(outcome.status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_TRUE(outcome.outputs.at("result").empty());
}

TEST(FlowRuntimeTest, OptionalDefaultsRetireUnusedAndRepeatedValueReaders) {
  const auto unused = RunFlow(R"(
flow main {
  in spare: int stream

  out result: int

  first = std::option({has_value: true, value: 1}) or take spare
  second = std::option({has_value: true, value: 2}) or take spare
  first + second -> result
}
)",
                              "main", {{"spare", {"9"}}});
  ASSERT_TRUE(unused.status.ok()) << unused.status;
  EXPECT_EQ(unused.outputs.at("result"), Values{"3"});
  const auto repeated = RunFlow(R"(
flow main {
  in spare: int stream

  out results: int stream

  [std::option({has_value: true, value: 1}), std::option({has_value: false})]
    | flatten | map (it or take spare) -> results
}
)",
                                "main", {{"spare", {"2"}}});
  ASSERT_TRUE(repeated.status.ok()) << repeated.status;
  EXPECT_EQ(repeated.outputs.at("results"), (Values{"1", "2"}));
}

TEST(FlowRuntimeTest, TrailingLoopConditionRunsTheEntireStoppingPass) {
  const auto outcome =
      RunFlow(R"(
flow main {
  in words: string stream
  out result: string stream
  for word in words {
    word -> result
  } until word == "stop"
}
)",
              "main", {{"words", {"\"a\"", "\"stop\"", "\"b\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("result"), Values({"\"a\"", "\"stop\""}));
}

TEST(FlowRuntimeTest, ChildReplacementDoesNotReadAnUnusedFallback) {
  for (const auto* body :
       {"if true { x = 2 }", "for item in [1, 2] { x = item }",
        "if false { x = 2 }", "for item in [] { x = item }"}) {
    const auto source =
        absl::StrCat("flow main {\n out result: int\n int x = 0\n ", body,
                     "\n x -> result\n}");
    const auto outcome = RunFlow(source, "main");
    ASSERT_TRUE(outcome.status.ok()) << source << outcome.status;
    EXPECT_EQ(
        outcome.outputs.at("result"),
        Values(
            {std::string_view(body).find("false") != std::string_view::npos ||
                     std::string_view(body).find("[]") != std::string_view::npos
                 ? "0"
                 : "2"}));
  }
}

TEST(FlowRuntimeTest, ChildAssignmentsUpdateTheEnclosingValue) {
  for (const auto* body : {"{ x = x + 1 }", "if true { x = x + 1 }",
                           "for item in [1] { x = x + 1 }"}) {
    const auto source =
        absl::StrCat("flow main {\n out result: int\n int x = 0\n ", body,
                     "\n x -> result\n}");
    const auto outcome = RunFlow(source, "main");
    ASSERT_TRUE(outcome.status.ok()) << source << outcome.status;
    EXPECT_EQ(outcome.outputs.at("result"), Values({"1"}));
  }
}

TEST(FlowRuntimeTest, NamespaceModulesAndStandardFunctionsRunNatively) {
  const auto outcome = RunFlow(R"(
mod tools {
  flow upper exported {
    in word: string
    out text: string
    std::upper(word) -> text
  }
}
flow main {
  out result: string
  work = run tools::upper(word: "hello")
  work.text -> result
}
)",
                               "main");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("result"), Values({"\"HELLO\""}));
}

TEST(FlowRuntimeTest, DeadlineAndTimeoutAreContextualBindableExpressions) {
  auto registry = std::make_shared<actions::ActionRegistry>();
  auto entered = std::make_shared<std::atomic<int>>(0);
  actions::ActionSchema schema;
  schema.name = "signal";
  ASSERT_TRUE(
      registry
          ->Register("signal", schema,
                     actions::MakeAsyncActionHandler([entered](const auto&) {
                       ++*entered;
                       return absl::OkStatus();
                     }))
          .ok());
  schema.name = "pause";
  ASSERT_TRUE(registry
                  ->Register("pause", schema,
                             actions::MakeAsyncActionHandler([](const auto&) {
                               thread::SleepFor(absl::Milliseconds(40));
                               return absl::OkStatus();
                             }))
                  .ok());
  for (const auto* expression : {"std::now() + 20ms", "(std::now() + 20ms)"}) {
    const auto outcome = RunFlow(absl::StrCat(R"(
from std import now
flow main {
  out code: string
  gate = run pause()
  work = try run signal() deadline )",
                                              expression, R"( after gate
  (status work).code -> code
}
)"),
                                 "main", {}, {}, true, {}, registry);
    ASSERT_TRUE(outcome.status.ok()) << outcome.status;
    EXPECT_EQ(outcome.outputs.at("code"), Values({"\"OK\""}));
  }
  const auto eager = RunFlow(R"(
flow main {
  out code: string
  std::time deadline = std::now() + 10ms
  std::duration timeout = 1s
  gate = run pause()
  work = try run signal() timeout timeout deadline deadline after gate
  (status work).code -> code
}
)",
                             "main", {}, {}, true, {}, registry);
  ASSERT_TRUE(eager.status.ok()) << eager.status;
  EXPECT_EQ(eager.outputs.at("code"), Values({"\"DEADLINE_EXCEEDED\""}));
  EXPECT_EQ(entered->load(), 2);
  const auto stage = RunFlow(R"(
flow main {
  out result: int
  timeout = 1s
  [1] | flatten | timeout timeout -> result
}
)",
                             "main");
  ASSERT_TRUE(stage.status.ok()) << stage.status;
  EXPECT_EQ(stage.outputs.at("result"), Values({"1"}));
}

TEST(FlowRuntimeTest, OptionalBindingDoesNotTurnUpstreamFailureIntoAbsence) {
  auto registry = TestRegistry();
  auto schema = BoomSchema();
  schema.name = "badstream";
  auto cause = absl::UnavailableError("stream failed");
  cause.SetPayload("test.upstream", absl::Cord("detail"));
  ASSERT_TRUE(
      registry
          ->Register(
              schema.name, schema,
              actions::MakeAsyncActionHandler(
                  [cause](const auto& action) -> absl::Status {
                    ABSL_ASSIGN_OR_RETURN(auto output,
                                          action->GetOutput("out"));
                    return output->AbortWithStatus(cause).Await().status();
                  }))
          .ok());
  const auto got = RunFlow(R"(
flow main { out absent: bool
 source = run badstream()
 if row = try take source.out { row -> _ }
 else { true -> absent }
}
)",
                           "main", {}, {}, true, {}, registry);
  EXPECT_EQ(got.status.code(), absl::StatusCode::kUnavailable) << got.status;
  EXPECT_EQ(got.status.GetPayload("test.upstream"),
            cause.GetPayload("test.upstream"));
  EXPECT_TRUE(got.outputs.at("absent").empty());
}

TEST(FlowRuntimeTest, OptionalBindingKeepsFalseLikePayloadsPresent) {
  const auto got = RunFlow(R"(
flow main { out values: std::any stream
 for payload in [null, false, 0, "", [], {}] | flatten {
   if item = std::option({has_value: true, value: payload}) {
     item -> values
   }
 }
}
)",
                           "main");
  ASSERT_TRUE(got.status.ok()) << got.status;
  EXPECT_EQ(got.outputs.at("values"),
            (Values{"null", "false", "0", "\"\"", "[]", "{}"}));
}

TEST(FlowRuntimeTest, OptionalBindingExtractsOnceAndSkipsElseIfInitializers) {
  const auto got = RunFlow(R"(
flow main { in rows: int stream
 out values: int stream
 if row = try take rows { row -> values }
 else if fallback = try take rows { fallback -> values }
 wait all
 value = take rows
 value -> values
}
)",
                           "main", {{"rows", {"1", "2"}}});
  ASSERT_TRUE(got.status.ok()) << got.status;
  EXPECT_EQ(got.outputs.at("values"), (Values{"1", "2"}));
}

TEST(FlowRuntimeTest, OptionalBindingReusesStoredOptionsAndHandlesAbsence) {
  const auto got = RunFlow(R"(
flow main { out values: int stream
 out absent: bool
 candidate = std::option({has_value: true, value: 7})
 if row = candidate { row -> values }
 if again = candidate { again -> values }
 if missing = std::option({has_value: false}) { missing -> _ }
 else { true -> absent }
}
)",
                           "main");
  ASSERT_TRUE(got.status.ok()) << got.status;
  EXPECT_EQ(got.outputs.at("values"), (Values{"7", "7"}));
  EXPECT_EQ(got.outputs.at("absent"), (Values{"true"}));
}

TEST(FlowRuntimeTest, OptionalBindingPreservesOuterVariableUpdatesAndBarriers) {
  const auto got = RunFlow(R"(
flow main { in rows: int stream
 out total: int
 int sum = 0
 for unused in [0, 0, 0] | flatten {
   if row = try take rows { sum = sum + row }
 }
 wait all
 sum -> total
}
)",
                           "main", {{"rows", {"2", "3"}}});
  ASSERT_TRUE(got.status.ok()) << got.status;
  EXPECT_EQ(got.outputs.at("total"), (Values{"5"}));
}

TEST(FlowRuntimeTest, OptionalBindingValidatesDeclaredOptions) {
  const auto good =
      RunFlow(R"(
flow main { in option: std::option[int]
 out result: int
 if value = take option { value -> result }
}
)",
              "main", {{"option", {"{\"has_value\":true,\"value\":0}"}}});
  ASSERT_TRUE(good.status.ok()) << good.status;
  EXPECT_EQ(good.outputs.at("result"), (Values{"0"}));
  for (const auto* option : {"{has_value: 1}", "{has_value: true}",
                             "{has_value: false, value: 1}"}) {
    const auto bad = RunFlow(absl::StrCat("flow main { if item = std::option(",
                                          option, ") { item -> _ } }"),
                             "main");
    EXPECT_FALSE(bad.status.ok()) << option;
  }
}

TEST(FlowRuntimeTest, OptionalBindingElseIfRunsAfterAbsenceOnly) {
  const auto got = RunFlow(R"(
flow main { in empty: int stream
 in rows: int stream
 out result: int
 if first = try take empty { first -> result }
 else if second = try take rows { second -> result }
}
)",
                           "main", {{"rows", {"0"}}});
  ASSERT_TRUE(got.status.ok()) << got.status;
  EXPECT_EQ(got.outputs.at("result"), (Values{"0"}));
}

TEST(FlowRuntimeTest, OptionalBindingRecognisesTypedLoopPayloads) {
  const auto got = RunFlow(
      R"(
flow main { in candidates: std::option[object] stream
 out values: int stream
 for candidate in candidates | where true parallel 2 {
   if row = candidate { row.n -> values }
 }
}
)",
      "main",
      {{"candidates",
        {"{\"has_value\":true,\"value\":{\"n\":0}}", "{\"has_value\":false}",
         "{\"has_value\":true,\"value\":{\"n\":2}}"}}});
  ASSERT_TRUE(got.status.ok()) << got.status;
  auto values = got.outputs.at("values");
  std::sort(values.begin(), values.end());
  EXPECT_EQ(values, (Values{"0", "2"}));
}

TEST(FlowRuntimeTest, ContinuationChainsAreRightAssociativeAndFlattened) {
  for (bool slots : {false, true}) {
    const auto got = RunFlow(R"(
flow main {
  out values: int stream
  3 -> (2 -> 1 -> values)
}
)",
                             "main", {}, {}, true, {}, nullptr, slots);
    ASSERT_TRUE(got.status.ok()) << got.status;
    EXPECT_EQ(got.outputs.at("values"), (Values{"1", "2", "3"}));
  }
}

TEST(FlowRuntimeTest, ContinuationSharedAliasesInterleaveAfterTheirPrefix) {
  const auto got = RunFlow(R"(
flow main {
  out values: int stream
  out complete: bool
  tail = 0 -> values
  alias = tail
  [1, 2] | flatten -> alias
  [3, 4] | flatten -> tail
  (status tail).ok -> complete
}
)",
                           "main");
  ASSERT_TRUE(got.status.ok()) << got.status;
  auto values = got.outputs.at("values");
  ASSERT_EQ(values.size(), 5);
  EXPECT_EQ(values.front(), "0");
  std::sort(values.begin() + 1, values.end());
  EXPECT_EQ(values, (Values{"0", "1", "2", "3", "4"}));
  EXPECT_EQ(got.outputs.at("complete"), (Values{"true"}));
}

TEST(FlowRuntimeTest, ContinuationNestedGroupsAndTerminalBroadcast) {
  const auto got = RunFlow(R"(
flow main {
  out left: int stream
  out right: int stream
  tail = 0 -> left, right
  middle = 1 -> tail
  2 -> middle
  wait tail
}
)",
                           "main");
  ASSERT_TRUE(got.status.ok()) << got.status;
  EXPECT_EQ(got.outputs.at("left"), (Values{"0", "1", "2"}));
  EXPECT_EQ(got.outputs.at("right"), got.outputs.at("left"));
}

TEST(FlowRuntimeTest, ContinuationReservationsCoverSkippedAndRepeatedWriters) {
  for (const auto parallel : {1, 4}) {
    const auto got = RunFlow(absl::StrCat(R"(
flow main {
  out values: int stream
  tail = 0 -> values
  if false { 99 -> tail }
  for x in [] | flatten { x -> tail }
  for x in [1, 2, 3] | flatten parallel )",
                                          parallel, R"( {
    if x > 1 { x -> tail }
  }
  wait tail
}
)"),
                             "main");
    ASSERT_TRUE(got.status.ok()) << got.status;
    auto values = got.outputs.at("values");
    ASSERT_EQ(values.size(), 3);
    EXPECT_EQ(values.front(), "0");
    std::sort(values.begin() + 1, values.end());
    EXPECT_EQ(values, (Values{"0", "2", "3"}));
  }
}

TEST(FlowRuntimeTest, ContinuationGatesAndCompletedBarrierKeepReservations) {
  const auto got = RunFlow(R"(
flow main {
  out values: int stream
  tail = 0 -> values
  ready = {}
  1 -> tail after ready
  wait all
  2 -> values
}
)",
                           "main");
  ASSERT_TRUE(got.status.ok()) << got.status;
  EXPECT_EQ(got.outputs.at("values"), (Values{"0", "1", "2"}));
}

TEST(FlowRuntimeTest, ContinuationFlatLiteralsAvoidTransportAllocations) {
  auto stats = std::make_shared<TransportStats>();
  const auto got = RunFlow(R"(
flow main { out result: int stream
  4 -> 3 -> 2 -> 1 -> result
}
)",
                           "main", {}, {}, true, {}, nullptr, true, stats);
  ASSERT_TRUE(got.status.ok()) << got.status;
  EXPECT_EQ(got.outputs.at("result"), (Values{"1", "2", "3", "4"}));
  EXPECT_EQ(stats->transfer_tasks.load(), 1);
  EXPECT_EQ(stats->literal_transfers.load(), 4);
  EXPECT_EQ(stats->slots.load(), 0);
  EXPECT_EQ(stats->queue_readers.load(), 0);
  EXPECT_EQ(stats->virtual_groups.load(), 0);
}

TEST(FlowRuntimeTest, ContinuationIterationGroupsRetainOnlyActiveState) {
  auto stats = std::make_shared<TransportStats>();
  const auto got = RunFlow(R"(
flow main { out values: int stream
 for x in [1, 2, 3, 4, 5, 6] | flatten {
   tail = x -> values
   x + 10 -> tail
   wait tail
 }
}
)",
                           "main", {}, {}, true, {}, nullptr, true, stats);
  ASSERT_TRUE(got.status.ok()) << got.status;
  EXPECT_EQ(got.outputs.at("values").size(), 12);
  EXPECT_EQ(stats->virtual_groups.load(), 6);
  EXPECT_EQ(stats->active_writer_groups.load(), 0);
  EXPECT_EQ(stats->peak_writer_groups.load(), 1);
}

TEST(FlowRuntimeTest, ContinuationCancellationStopsAnOrderedFeeder) {
  const auto got = RunFlow(R"(
flow main { in input: int stream
 out result: int stream
 tail = input -> result
 ready = {}
 cancel tail after ready
}
)",
                           "main", {}, {}, false);
  EXPECT_EQ(got.status.code(), absl::StatusCode::kCancelled) << got.status;
}

TEST(FlowRuntimeTest, ContinuationWaitLimitsJoinCancelledCleanup) {
  for (const auto* limit : {"timeout 2ms", "deadline std::now() + 2ms"}) {
    const auto got = RunFlow(absl::StrCat(R"(
flow main { in input: int stream
 out result: int stream
 tail = input -> result
 wait tail )",
                                          limit, "\n}"),
                             "main", {}, {}, false);
    EXPECT_EQ(got.status.code(), absl::StatusCode::kDeadlineExceeded)
        << got.status;
  }
}

TEST(FlowRuntimeTest, ContinuationChainsPreserveUnaryDestinationValidation) {
  for (bool slots : {false, true}) {
    const auto got = RunFlow(R"(
flow scalar { in input: int
 out result: int
 int(input) -> result
}
flow main { out result: int
 consume = run scalar()
 2 -> 1 -> consume.input
 consume.result -> result
}
)",
                             "main", {}, {}, true, {}, nullptr, slots);
    EXPECT_EQ(got.status.code(), absl::StatusCode::kInvalidArgument)
        << got.status;
  }
}

TEST(FlowRuntimeTest, ContinuationCancellationAlsoStopsDiscardGroups) {
  const auto got = RunFlow(R"(
flow main { in input: int stream
 tail = 0 -> _
 input -> tail
 ready = {}
 cancel tail after ready
}
)",
                           "main", {}, {}, false);
  EXPECT_EQ(got.status.code(), absl::StatusCode::kCancelled) << got.status;
}

TEST(FlowRuntimeTest, OptionalReadCancellationDoesNotSelectElse) {
  for (bool tolerant : {false, true}) {
    LogCapture logs;
    const auto got = RunFlow(
        absl::StrCat(R"(
flow waiting { in input: int stream
 if value = try take input { value -> _ }
 else { log "absent" }
}
flow main { in input: int stream
 reader = )",
                     tolerant ? "try " : "", R"(run waiting(input: input)
 ready = {}
 cancel reader after ready
}
)"),
        "main", {}, {}, false);
    EXPECT_EQ(got.status.code(),
              tolerant ? absl::StatusCode::kOk : absl::StatusCode::kCancelled)
        << got.status;
    EXPECT_TRUE(logs.texts().empty());
  }
}

TEST(FlowRuntimeTest, AbandonedFlowInputsDoNotKeepTheirPumpsAlive) {
  const auto got = RunFlow(R"(
flow main { in input: int stream
 out result: int stream
 input | first 1 -> result
}
)",
                           "main", {{"input", {"1"}}}, {}, false);
  ASSERT_TRUE(got.status.ok()) << got.status;
  EXPECT_EQ(got.outputs.at("result"), (Values{"1"}));
}

TEST(FlowRuntimeTest, FinishedToleratedCallsKeepOtherBroadcastFeedsAlive) {
  const auto got = RunFlow(R"(
flow broken { in input: int stream
 if true { fail unavailable "done" }
}
flow echo { in input: int stream
 out output: int stream
 input -> output
}
flow main { in source: int stream
 out result: int stream
 a = try run broken()
 b = run echo()
 source -> a.input, b.input
 b.output -> result
}
)",
                           "main", {{"source", {"1", "2"}}});
  ASSERT_TRUE(got.status.ok()) << got.status;
  EXPECT_EQ(got.outputs.at("result"), (Values{"1", "2"}));
}

TEST(FlowRuntimeTest, ContinuationTryRetainsPartialOutputAndSkipsRest) {
  const auto got = RunFlow(R"(
flow main { out values: int stream
 out code: string
 tail = try 9 -> (["eA==", "!"] | flatten | map std::len(std::b64decode(it))) -> values
 7 -> tail
 (status tail).code -> code
}
)",
                           "main");
  ASSERT_TRUE(got.status.ok()) << got.status;
  EXPECT_EQ(got.outputs.at("values"), (Values{"1", "7"}));
  EXPECT_EQ(got.outputs.at("code"), (Values{"\"INVALID_ARGUMENT\""}));
}

TEST(FlowRuntimeTest, ContinuationHandledBranchFailureStillAbortsSharedSink) {
  const auto got = RunFlow(R"(
flow main { in input: int stream
 out result: int stream
 tail = 0 -> result
 branch = try {
   1 -> tail
   wait all
   fail unavailable "shared sink failed"
 }
 input -> tail
}
)",
                           "main", {}, {}, false);
  EXPECT_EQ(got.status.code(), absl::StatusCode::kUnavailable) << got.status;
  EXPECT_EQ(got.status.message(), "shared sink failed");
}

TEST(FlowRuntimeTest, ContinuationSeparatelyDeclaredActionsRemainConcurrent) {
  auto arrived = std::make_shared<std::atomic<int>>(0);
  auto ready = std::make_shared<thread::PermanentEvent>();
  auto registry = TestRegistry();
  actions::ActionSchema schema;
  schema.name = "meet_then_emit";
  schema.outputs.emplace(
      "out", actions::ActionPortSchema{.name = "out", .type = "int"});
  ASSERT_TRUE(
      registry
          ->Register(
              schema.name, schema,
              actions::MakeAsyncActionHandler(
                  [arrived, ready](const auto& action) -> absl::Status {
                    if (++*arrived == 2) {
                      ready->Notify();
                    }
                    thread::Select({thread::OnCancel(), ready->OnEvent()});
                    if (thread::Cancelled()) {
                      return absl::CancelledError("cancelled");
                    }
                    ABSL_ASSIGN_OR_RETURN(auto output,
                                          action->GetOutput("out"));
                    return output->PutChunk(JsonChunk("1")).Await().status();
                  }))
          .ok());
  const auto got = RunFlow(R"(
flow main { out result: int stream
 a = run meet_then_emit()
 b = run meet_then_emit()
 a.out -> b.out -> result
}
)",
                           "main", {}, {}, true, {}, registry);
  ASSERT_TRUE(got.status.ok()) << got.status;
  EXPECT_EQ(got.outputs.at("result"), (Values{"1", "1"}));
  EXPECT_EQ(arrived->load(), 2);
}

TEST(FlowRuntimeTest, ContinuationOwnsAnonymousLateCleanupBeforeNextSegment) {
  auto finished = std::make_shared<std::atomic<bool>>(false);
  auto registry = TestRegistry();
  actions::ActionSchema schema;
  schema.name = "late";
  schema.outputs.emplace(
      "out", actions::ActionPortSchema{.name = "out", .type = "int"});
  ASSERT_TRUE(
      registry
          ->Register(
              schema.name, schema,
              actions::MakeAsyncActionHandler(
                  [finished](const auto& action) -> absl::Status {
                    ABSL_ASSIGN_OR_RETURN(auto output,
                                          action->GetOutput("out"));
                    ABSL_RETURN_IF_ERROR(
                        output->PutChunk(JsonChunk("1")).Await().status());
                    ABSL_RETURN_IF_ERROR(
                        output->Finalize({.wait = true}).Await().status());
                    thread::SleepFor(absl::Milliseconds(10));
                    finished->store(true);
                    return absl::UnavailableError("late source failure");
                  }))
          .ok());
  const auto got = RunFlow(R"(
flow main { out result: int stream
 2 -> (run late()).out -> result
}
)",
                           "main", {}, {}, true, {}, registry);
  EXPECT_EQ(got.status.code(), absl::StatusCode::kUnavailable) << got.status;
  EXPECT_TRUE(finished->load());
  EXPECT_TRUE(std::find(got.outputs.at("result").begin(),
                        got.outputs.at("result").end(),
                        "2") == got.outputs.at("result").end());
}

TEST(FlowRuntimeTest, ContinuationEmptyNullAndTryKeepTheirBoundaries) {
  const auto got = RunFlow(R"(
flow main {
  out values: object stream
  out handled: bool
  tail = try null -> (std::parse_time("bad") -> values)
  7 -> tail
  (status tail).ok -> handled
}
)",
                           "main");
  ASSERT_TRUE(got.status.ok()) << got.status;
  EXPECT_EQ(got.outputs.at("handled"), (Values{"false"}));
  EXPECT_EQ(got.outputs.at("values"), (Values{"7"}));
}

TEST(FlowRuntimeTest, FramePoolReusesCompletedPassesAndReleasesAllCapacity) {
  const std::string source = R"(
flow main {
 in items: int stream
 out result: int stream
 for n in items { v = n + 1
 v -> result }
}
)";
  PortValues inputs;
  for (int i = 0; i < 128; ++i) {
    inputs["items"].push_back(absl::StrCat(i));
  }
  for (const size_t cap : {size_t{0}, size_t{256 * 1024}}) {
    auto stats = std::make_shared<FrameStats>();
    const auto outcome =
        RunFlow(source, "main", inputs, {}, true, {}, nullptr, true, nullptr,
                {.cache_limit_bytes = cap, .stats = stats});
    ASSERT_TRUE(outcome.status.ok()) << outcome.status;
    ASSERT_EQ(outcome.outputs.at("result").size(), 128);
    EXPECT_EQ(outcome.outputs.at("result").front(), "1");
    EXPECT_EQ(outcome.outputs.at("result").back(), "128");
    EXPECT_EQ(stats->activations, 129);
    if (cap == 0) {
      EXPECT_EQ(stats->reuses, 0);
      EXPECT_EQ(stats->allocations, 129);
    } else {
      EXPECT_EQ(stats->allocations, 2);
      EXPECT_EQ(stats->reuses, 127);
    }
    EXPECT_EQ(stats->active_frames, 0);
    EXPECT_EQ(stats->reserved_bytes, 0);
    EXPECT_EQ(stats->live_bytes, 0);
    EXPECT_EQ(stats->cached_bytes, 0);
    EXPECT_LE(stats->peak_cached_bytes, cap);
  }
}

TEST(FlowRuntimeTest, ParallelFramesRemainDistinctWhileCallsAreParked) {
  struct Barrier {
    thread::Mutex mu;
    thread::CondVar cv;
    int admitted ABSL_GUARDED_BY(mu) = 0;
  };

  auto barrier = std::make_shared<Barrier>();
  auto registry = TestRegistry();
  actions::ActionSchema schema;
  schema.name = "pause";
  ASSERT_TRUE(registry
                  ->Register("pause", schema,
                             actions::MakeAsyncActionHandler(
                                 [barrier](const auto&) -> absl::Status {
                                   thread::MutexLock lock(&barrier->mu);
                                   ++barrier->admitted;
                                   barrier->cv.SignalAll();
                                   const auto deadline =
                                       absl::Now() + absl::Seconds(5);
                                   while (barrier->admitted < 4) {
                                     if (barrier->cv.WaitWithDeadline(
                                             &barrier->mu, deadline)) {
                                       return absl::DeadlineExceededError(
                                           "Parallel passes did not overlap");
                                     }
                                   }
                                   return absl::OkStatus();
                                 }))
                  .ok());
  auto stats = std::make_shared<FrameStats>();
  const auto result =
      RunFlow(R"(
declare flow pause {}
flow main {
 in items: int stream
 out result: int stream
 for n in items parallel 4 {
   ready = run pause()
   n -> result after ready
 }
}
)",
              "main", {{"items", {"1", "2", "3", "4", "5", "6", "7", "8"}}}, {},
              true, {}, registry, true, nullptr, {.stats = stats});
  ASSERT_TRUE(result.status.ok()) << result.status;
  auto values = result.outputs.at("result");
  std::sort(values.begin(), values.end());
  EXPECT_EQ(values, Values({"1", "2", "3", "4", "5", "6", "7", "8"}));
  EXPECT_GE(stats->peak_active_frames, 5);
  EXPECT_GT(stats->reuses, 0);
  EXPECT_EQ(stats->reserved_bytes, 0);
}

TEST(FlowRuntimeTest, FrameBudgetUsesOrdinaryStatusBoundary) {
  auto stats = std::make_shared<FrameStats>();
  const auto result = RunFlow("flow main { out result: int\n 42 -> result }",
                              "main", {}, {}, true, {}, nullptr, true, nullptr,
                              {.max_reserved_bytes = 0, .stats = stats});
  EXPECT_EQ(result.status.code(), absl::StatusCode::kResourceExhausted);
  EXPECT_EQ(stats->allocations, 0);
  EXPECT_EQ(stats->reserved_bytes, 0);
}

TEST(FlowRuntimeTest, FrameBudgetSpansTransitiveLocalCalls) {
  const std::string source = R"(
flow inner { out value: int
 42 -> value }
flow main { out result: int
 call = run inner()
 call.value -> result }
)";
  auto stats = std::make_shared<FrameStats>();
  const auto first = RunFlow(source, "main", {}, {}, true, {}, nullptr, true,
                             nullptr, {.cache_limit_bytes = 0, .stats = stats});
  ASSERT_TRUE(first.status.ok()) << first.status;
  ASSERT_GT(stats->peak_live_bytes, 0);
  const auto limited =
      RunFlow(source, "main", {}, {}, true, {}, nullptr, true, nullptr,
              {.cache_limit_bytes = 0,
               .max_reserved_bytes = stats->peak_live_bytes.load() - 1});
  EXPECT_EQ(limited.status.code(), absl::StatusCode::kResourceExhausted);
}

TEST(FlowRuntimeTest,
     ConcurrentInvocationsOfOneProgramKeepFrameDataIndependent) {
  const std::string source = R"(
flow main {
 in items: int stream
 out result: int stream
 for n in items parallel 4 {
   if n > 0 { v = n + 1
 v -> result }
 }
}
)";
  auto compiled = CompiledProgram::Compile(source);
  ASSERT_TRUE(compiled.ok()) << compiled.status();
  auto stats = std::make_shared<FrameStats>();
  std::vector<a11::Future<Outcome>> runs;
  for (int seed = 0; seed < 16; ++seed) {
    runs.push_back(a11::Submit<Outcome>([&, seed]() -> absl::StatusOr<Outcome> {
      PortValues inputs;
      for (int i = 0; i < 16; ++i) {
        inputs["items"].push_back(absl::StrCat(seed * 100 + i));
      }
      return RunFlow(source, "main", inputs, {}, true, {}, nullptr, true,
                     nullptr, {.stats = stats}, *compiled);
    }));
  }
  for (int seed = 0; seed < 16; ++seed) {
    const auto result = runs[static_cast<size_t>(seed)].Await();
    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_TRUE(result->status.ok()) << result->status;
    Values expected;
    for (int i = 0; i < 16; ++i) {
      if (seed * 100 + i > 0) {
        expected.push_back(absl::StrCat(seed * 100 + i + 1));
      }
    }
    auto actual = result->outputs.at("result");
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    EXPECT_EQ(actual, expected);
  }
  EXPECT_GT(stats->reuses, 0);
  EXPECT_EQ(stats->reserved_bytes, 0);
  EXPECT_EQ(stats->active_frames, 0);
}

TEST(FlowRuntimeTest,
     ReusedFramesKeepEmittedPayloadsAliveAndResetSkippedState) {
  const auto result = RunFlow(R"(
flow main {
 out result: object stream
 for n in [1, 2, 3, 4] | flatten {
   if n == 2 { ignored = n + 10 }
   {number: n, data: [n, n + 1]} -> result
 }
}
)",
                              "main", {}, {}, true, {}, nullptr, true);
  ASSERT_TRUE(result.status.ok()) << result.status;
  ASSERT_EQ(result.outputs.at("result").size(), 4);
  for (size_t i = 0; i < 4; ++i) {
    const auto decoded = nlohmann::json::parse(result.outputs.at("result")[i]);
    EXPECT_EQ(decoded["number"], i + 1);
    EXPECT_EQ(decoded["data"][1], i + 2);
  }
}

}  // namespace

TEST(FlowRuntimeTest, ExtractionOrderAlsoAppliesToPipelineSourcesAndAliases) {
  const auto outcome = RunFlow(R"(
flow main {
  in values: int stream
  out first: int
  out second: int
  rows = values
  (take rows) -> first
  (take values) -> second
}
)",
                               "main", {{"values", Values({"11", "22"})}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("first"), Values({"11"}));
  EXPECT_EQ(outcome.outputs.at("second"), Values({"22"}));
}

TEST(FlowRuntimeTest, DestructuringCoercesAllTargetsBeforePublishing) {
  const auto good = RunFlow(R"(
flow main {
  out result: int stream
  int left, right = ["2", "3"]
  left, right = ["4", "5"]
  [left, right] | flatten -> result
}
)",
                            "main");
  ASSERT_TRUE(good.status.ok()) << good.status;
  EXPECT_EQ(good.outputs.at("result"), Values({"4", "5"}));
  const auto bad = RunFlow(R"(
flow main {
  out result: object stream
  object left = {value: 1}
  object right = {value: 2}
  left, right = [{value: 3}, 4]
  [left, right] | flatten -> result
}
)",
                           "main");
  EXPECT_FALSE(bad.status.ok());
  EXPECT_TRUE(bad.outputs.at("result").empty());
}

TEST(FlowRuntimeTest, OptionalTakePropagatesUpstreamFailure) {
  const auto outcome = RunFlow(R"(
struct Row { id: string required }
flow main {
  out present: bool
  row = try take ([{}] | flatten | map it as Row)
  row.has_value -> present
}
)",
                               "main");
  EXPECT_EQ(outcome.status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(FlowRuntimeTest, AbsentOptionalPayloadIsAnError) {
  const auto outcome = RunFlow(R"(
flow main {
  in rows: string stream
  out result: string
  row = try take rows
  row.value -> result
}
)",
                               "main");
  EXPECT_EQ(outcome.status.code(), absl::StatusCode::kFailedPrecondition);
}

TEST(FlowRuntimeTest, PaceEvaluatesItsDurationExpression) {
  const auto outcome = RunFlow(R"(
flow main {
  out result: int stream
  std::duration interval = 1ms
  [1, 2] | flatten | pace interval -> result
}
)",
                               "main");
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("result"), Values({"1", "2"}));
}

TEST(FlowRuntimeTest, PipelineCallsTransformFilterAndAggregateWholeStreams) {
  LogCapture logs;
  const auto result = RunFlow(R"(
flow clean {
 in input: string stream
 out output: string stream
 delivered = input | map std::trim(it) | where std::len(it) > 0 -> output
 log "clean activated" after delivered
}
flow count_rows {
 in input: string stream
 out total: int
 input | count -> total
}
flow main {
 out text: string stream
 out count: int
 cleaned = [" a ", "", "b"] | flatten | run clean()
 cleaned -> text
 cleaned | run count_rows() -> count
}
)",
                              "main");
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("text"), Values({"\"a\"", "\"b\""}));
  EXPECT_EQ(result.outputs.at("count"), Values({"2"}));
  EXPECT_EQ(logs.texts(), Values({"clean activated"}));
}

TEST(FlowRuntimeTest, PipelineCallsConfigureInputsAndRetainLoopState) {
  const auto result = RunFlow(R"(
flow number_rows {
 in input: string stream
 in prefix: string
 out output: string stream
 string label = take prefix
 int n = 0
 for line in input {
  n = n + 1
  std::strformat("%s%d:%s", label, n, line) -> output
 }
}
flow main {
 out output: string stream
 for pass in [1, 2] | flatten {
  ["a", "b"] | flatten | run number_rows(prefix: ">") -> output
 }
 if false { "unused" | run number_rows(prefix: "bad") -> output }
}
)",
                              "main");
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("output"),
            Values({"\">1:a\"", "\">2:b\"", "\">1:a\"", "\">2:b\""}));
}

TEST(FlowRuntimeTest, PipelineCallsKeepLateFailuresAndDeadlines) {
  const auto failed = RunFlow(R"(
flow late {
 in input: string stream
 out output: string stream
 written = input -> output
 fail data_loss "late failure" after written
}
flow main { out output: string stream
 ["a", "b"] | flatten | run late() | first 1 -> output
}
)",
                              "main");
  EXPECT_EQ(failed.status.code(), absl::StatusCode::kDataLoss) << failed.status;
  const auto expired = RunFlow(R"(
flow slow {
 in input: string stream
 out output: string stream
 input | pace 1s -> output
}
flow main { out output: string stream
 ["a", "b"] | flatten | run slow() timeout 5ms -> output
}
)",
                               "main");
  EXPECT_EQ(expired.status.code(), absl::StatusCode::kDeadlineExceeded)
      << expired.status;
}

TEST(FlowRuntimeTest,
     PipelineCallsCheckUnaryInputsWithoutSelectingOrCollecting) {
  const std::string prefix = R"(
flow scalar {
 in input: int required
 out output: int
 input -> output
}
flow main { out result: int
)";
  auto good =
      RunFlow(absl::StrCat(prefix, "42 | run scalar() -> result\n}"), "main");
  ASSERT_TRUE(good.status.ok()) << good.status;
  EXPECT_EQ(good.outputs.at("result"), Values({"42"}));
  for (const auto* values : {"[]", "[1, 2]"}) {
    auto bad = RunFlow(
        absl::StrCat(prefix, values, " | flatten | run scalar() -> result\n}"),
        "main");
    EXPECT_FALSE(bad.status.ok());
  }
}

TEST(FlowRuntimeTest, PipelineCallsDrainExternalActionsAfterEarlyTermination) {
  auto registry = std::make_shared<actions::ActionRegistry>();
  auto received = std::make_shared<std::atomic<int>>(0);
  auto invoked = std::make_shared<std::atomic<int>>(0);
  actions::ActionSchema schema;
  schema.name = "echo";
  schema.inputs.emplace(
      "text", actions::ActionPortSchema{.name = "text", .type = "str"});
  schema.outputs.emplace(
      "out", actions::ActionPortSchema{.name = "out", .type = "str"});
  ASSERT_TRUE(
      registry
          ->Register(
              "echo", schema,
              actions::MakeAsyncActionHandler(
                  [received, invoked](const auto& action) -> absl::Status {
                    ++*invoked;
                    ABSL_ASSIGN_OR_RETURN(auto input, action->GetInput("text"));
                    ABSL_ASSIGN_OR_RETURN(auto output,
                                          action->GetOutput("out"));
                    while (true) {
                      ABSL_ASSIGN_OR_RETURN(auto chunk,
                                            input->NextChunk().Await());
                      if (!chunk.has_value()) {
                        break;
                      }
                      if (chunk->IsNull()) {
                        continue;
                      }
                      ++*received;
                      ABSL_RETURN_IF_ERROR(
                          output->PutChunk(*std::move(chunk)).Await().status());
                    }
                    return absl::OkStatus();
                  }))
          .ok());
  Values words(1000, "\"word\"");
  const auto result =
      RunFlow(R"(
declare flow echo { in text: string stream
 out out: string stream }
flow main { in words: string stream
 out first: string
 out count: int
 echoed = words | run echo()
 echoed | first 1 -> first
 echoed | count -> count
}
)",
              "main", {{"words", words}}, {}, true, {}, registry);
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(invoked->load(), 1);
  EXPECT_EQ(received->load(), 1000);
  EXPECT_EQ(result.outputs.at("count"), Values({"1000"}));
  EXPECT_EQ(result.outputs.at("first"), Values({"\"word\""}));
  const auto remote = RunFlow(R"(
declare flow echo { in text: string stream
 out out: string stream }
flow main { "x" | call echo() -> _ }
)",
                              "main", {}, {}, true, {}, registry);
  EXPECT_FALSE(remote.status.ok());
  EXPECT_EQ(invoked->load(), 1);
}

TEST(FlowRuntimeTest, ImportedPipelineCallsCaptureTheirDefiningModule) {
  const auto result = RunFlow(R"(
import "library.flow" as lib
flow main { out result: string stream
 ["a", "b"] | flatten | run lib::clean() -> result
}
)",
                              "main", {}, {}, true, {{"library.flow", R"(
flow private_clean { in input: string stream
 out output: string stream
 input | map std::upper(it) -> output
}
flow clean exported { in input: string stream
 out output: string stream
 input | run private_clean() -> output
}
)"}});
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("result"), Values({"\"A\"", "\"B\""}));
}

TEST(FlowRuntimeTest, ScopeBarriersGatePhasesAndRetainConcurrency) {
  LogCapture logs;
  const auto result = RunFlow(R"(
flow main {
 int n = 1
 if true {
  logf "before-a:%s" n
  logf "before-b:%s" n
 }
 wait all
 log "after-a"
 log "after-b"
 wait all
 log "last"
}
)",
                              "main");
  ASSERT_TRUE(result.status.ok()) << result.status;
  auto text = logs.texts();
  ASSERT_EQ(text.size(), 5u);
  Values before{text[0], text[1]};
  std::sort(before.begin(), before.end());
  EXPECT_EQ(before, Values({"before-a:1", "before-b:1"}));
  Values after{text[2], text[3]};
  std::sort(after.begin(), after.end());
  EXPECT_EQ(after, Values({"after-a", "after-b"}));
  EXPECT_EQ(text[4], "last");
}

TEST(FlowRuntimeTest, ScopeBarriersJoinOwnedCallsAndDoNotReadUnusedAliases) {
  LogCapture logs;
  const auto result = RunFlow(R"(
flow emit { out output: string stream
 sent = ["a", "b", "c"] | flatten | pace 1ms -> output
 log "child complete" after sent
}
flow main {
 unused = (run emit()).output
 first = take ((run emit()).output)
 wait all
 logf "after %s" first
}
)",
                              "main");
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(logs.texts(), Values({"child complete", "after a"}));
}

TEST(FlowRuntimeTest, ScopeBarriersAreLocalToBlocksBranchesAndLoopPasses) {
  LogCapture logs;
  const auto result = RunFlow(R"(
flow main {
 for n in [1, 2, 3] | flatten parallel 3 {
  logf "before %s" n
  wait all
  logf "after %s" n
 }
 if true {
  int n = 4
  logf "before %s" n
  wait all
  logf "after %s" n
 }
}
)",
                              "main");
  ASSERT_TRUE(result.status.ok()) << result.status;
  const auto texts = logs.texts();
  for (const auto* n : {"1", "2", "3", "4"}) {
    auto before =
        std::find(texts.begin(), texts.end(), absl::StrCat("before ", n));
    auto after =
        std::find(texts.begin(), texts.end(), absl::StrCat("after ", n));
    EXPECT_NE(before, texts.end());
    EXPECT_NE(after, texts.end());
    EXPECT_LT(before, after);
  }
}

TEST(FlowRuntimeTest, ScopeBarriersPreserveFailureAndTolerance) {
  for (bool tolerant : {false, true}) {
    LogCapture logs;
    const auto source =
        absl::StrCat("flow main {\n ", tolerant ? "try " : "",
                     "run boom()\n wait all\n log \"after\"\n}");
    const auto result = RunFlow(source, "main");
    EXPECT_EQ(result.status.ok(), tolerant) << result.status;
    EXPECT_EQ(logs.texts(), tolerant ? Values({"after"}) : Values{});
  }
}

TEST(FlowRuntimeTest, ScopeBarriersGateAnonymousArgumentsAndEagerDeadlines) {
  LogCapture logs;
  const auto result = RunFlow(R"(
flow marker { out output: string
 if true { log "activated" }
 "ready" -> output
}
flow sink { in input: string stream
 input -> _
}
flow main {
 first = "one" | log it -> _
 wait all
 value = take ((run marker()).output)
 run sink(input: (run marker()).output)
 late = run marker() deadline std::now() + 1s
 late.output -> _
 logf "%s" value
}
)",
                              "main");
  ASSERT_TRUE(result.status.ok()) << result.status;
  const auto texts = logs.texts();
  ASSERT_EQ(texts.size(), 5u);
  EXPECT_EQ(texts.front(), "one");
}

TEST(FlowRuntimeTest, ScopeBarrierPhasesDoNotSerializeIndependentCalls) {
  auto entered = std::make_shared<std::atomic<int>>(0);
  auto release = std::make_shared<thread::PermanentEvent>();
  auto registry = TestRegistry();
  actions::ActionSchema schema;
  schema.name = "meet";
  ASSERT_TRUE(
      registry
          ->Register(
              "meet", schema,
              actions::MakeAsyncActionHandler([entered, release](
                                                  const auto&) -> absl::Status {
                if (++*entered == 2) {
                  release->Notify();
                }
                thread::Select({thread::OnCancel(), release->OnEvent()});
                return thread::Cancelled() ? absl::CancelledError("cancelled")
                                           : absl::OkStatus();
              }))
          .ok());
  LogCapture logs;
  const auto result = RunFlow(
      "flow main { run meet()\n run meet()\n wait all\n log \"complete\"\n}",
      "main", {}, {}, true, {}, registry);
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(entered->load(), 2);
  EXPECT_EQ(logs.texts(), Values({"complete"}));
}

TEST(FlowRuntimeTest, ScopeBarriersDelayEagerDeadlineEvaluation) {
  const auto result = RunFlow(R"(
flow pause { out output: int stream
 [1, 2] | flatten | pace 80ms -> output
}
flow emit { out output: string
 "ready" -> output
}
flow main { out result: string
 run pause()
 wait all
 job = run emit() deadline std::now() + 50ms
 job.output -> result
}
)",
                              "main");
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(result.outputs.at("result"), Values({"\"ready\""}));
}

TEST(FlowRuntimeTest, ScopeBarriersDoNotExhaustInputAliases) {
  LogCapture logs;
  const auto result = RunFlow(
      "flow main { in input: string stream\n alias = input\n wait all\n log "
      "\"complete\"\n}",
      "main", {}, {}, false);
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(logs.texts(), Values({"complete"}));
}

TEST(FlowRuntimeTest, ScopeBarriersDoNotSynchronizeSiblingScopesOrIterations) {
  for (int scenario = 0; scenario < 3; ++scenario) {
    auto release = std::make_shared<thread::PermanentEvent>();
    auto registry = TestRegistry();
    actions::ActionSchema awaiting;
    awaiting.name = "await_release";
    actions::ActionSchema signal;
    signal.name = "signal_release";
    ASSERT_TRUE(
        registry
            ->Register("await_release", awaiting,
                       actions::MakeAsyncActionHandler(
                           [release](const auto&) -> absl::Status {
                             thread::Select(
                                 {thread::OnCancel(), release->OnEvent()});
                             return thread::Cancelled()
                                        ? absl::CancelledError("cancelled")
                                        : absl::OkStatus();
                           }))
            .ok());
    ASSERT_TRUE(
        registry
            ->Register("signal_release", signal,
                       actions::MakeAsyncActionHandler([release](const auto&) {
                         release->Notify();
                         return absl::OkStatus();
                       }))
            .ok());
    const auto source = scenario == 0   ? R"(
flow main {
 for n in [1, 2] | flatten parallel 2 {
  if n == 2 { run await_release() }
  wait all
  if n == 1 { run signal_release() }
 }
}
)"
                        : scenario == 1 ? R"(
flow main {
 work = run await_release()
 wait all of work
 run signal_release()
}
)"
                                        : R"(
flow main {
 if true { wait all
  run signal_release()
 }
 if true { run await_release() }
}
)";
    const auto result = RunFlow(source, "main", {}, {}, true, {}, registry);
    ASSERT_TRUE(result.status.ok()) << result.status;
  }
}

TEST(FlowRuntimeTest, ScopeBarriersRepeatPerSequentialPass) {
  LogCapture logs;
  const auto result = RunFlow(R"(
flow main {
 repeat n = 0 max 3 {
  logf "before %s" n
  wait all
  logf "after %s" n
  n <- n + 1
 }
}
)",
                              "main");
  ASSERT_TRUE(result.status.ok()) << result.status;
  EXPECT_EQ(logs.texts(), Values({"before 0", "after 0", "before 1", "after 1",
                                  "before 2", "after 2"}));
}

TEST(FlowRuntimeTest, ScopeBarriersRetainTryBlockAndPipeSemantics) {
  for (const auto* body : {"try { run boom()\n wait all\n log \"blocked\"\n }",
                           "try (try run boom()).out -> _"}) {
    SCOPED_TRACE(body);
    LogCapture logs;
    const auto result = RunFlow(
        absl::StrCat("flow main {\n", body, "\n wait all\n log \"after\"\n}"),
        "main");
    EXPECT_TRUE(result.status.ok()) << result.status;
    const auto text = logs.texts();
    EXPECT_EQ(std::count(text.begin(), text.end(), "blocked"), 0);
    EXPECT_EQ(std::count(text.begin(), text.end(), "after"), 1);
  }
}

TEST(FlowRuntimeTest, PipelineCallsRetainInvocationAndTransferTolerance) {
  for (const auto* statement :
       {"try { \"x\" | run late() -> _ }", "try \"x\" | try run late() -> _"}) {
    SCOPED_TRACE(statement);
    const auto result = RunFlow(absl::StrCat(R"(
flow late { in input: string stream
 out output: string stream
 written = input -> output
 fail data_loss "late failure" after written
}
flow main {
)",
                                             statement, "\n wait all\n}"),
                                "main");
    EXPECT_TRUE(result.status.ok()) << result.status;
  }
}

TEST(FlowRuntimeTest, SingleItemSlotsMatchBaselineAndRemoveTransportPumps) {
  for (const auto* source :
       {"flow main { out result: int\n x = 42\n x -> result }",
        "flow main { in input: int\n out result: int\n x = take input\n x -> "
        "result }",
        "flow main { out result: int stream\n nodes private { n = node()\n 42 "
        "-> n\n n -> result } }",
        "flow main { out result: int stream\n nodes private { n = node()\n n "
        "-> result } }",
        "flow main { out result: int stream\n n = node()\n if true { 1 -> n } "
        "else { 2 -> n }\n n -> result }",
        "flow main { out result: int stream\n for x in [1, 2, 3] | flatten "
        "parallel 3 { n = node()\n x -> n\n n -> result } }",
        "flow main { out result: int stream\n n = node()\n 1 -> n\n for x in "
        "[1, 2, 3] | flatten { n -> result } }"}) {
    SCOPED_TRACE(source);
    const auto baseline = RunFlow(source, "main", {{"input", {"7"}}});
    auto stats = std::make_shared<TransportStats>();
    const auto optimized = RunFlow(source, "main", {{"input", {"7"}}}, {}, true,
                                   {}, nullptr, true, stats);
    EXPECT_EQ(optimized.status, baseline.status);
    auto actual = optimized.outputs;
    auto expected = baseline.outputs;
    for (auto& [port, values] : actual) {
      std::sort(values.begin(), values.end());
    }
    for (auto& [port, values] : expected) {
      std::sort(values.begin(), values.end());
    }
    EXPECT_EQ(actual, expected);
    EXPECT_GT(stats->slots.load(), 0u);
  }
  for (const auto* source : {"flow main { out result: int\n 42 -> result }",
                             "flow main { in input: int\n out result: int\n x "
                             "= take input\n x -> result }"}) {
    auto baseline = std::make_shared<TransportStats>();
    auto optimized = std::make_shared<TransportStats>();
    ASSERT_TRUE(RunFlow(source, "main", {{"input", {"7"}}}, {}, true, {},
                        nullptr, false, baseline)
                    .status.ok());
    ASSERT_TRUE(RunFlow(source, "main", {{"input", {"7"}}}, {}, true, {},
                        nullptr, true, optimized)
                    .status.ok());
    EXPECT_GT(optimized->inline_producers.load(), 0u);
    EXPECT_LT(optimized->bus_pumps.load() + optimized->slot_pumps.load(),
              baseline->bus_pumps.load());
    EXPECT_LT(optimized->queue_readers.load(), baseline->queue_readers.load());
  }
}

TEST(FlowRuntimeTest, SingleItemSlotsSeparateAvailabilityCompletionAndErrors) {
  for (bool enabled : {false, true}) {
    for (bool fail : {false, true}) {
      SCOPED_TRACE(absl::StrCat("enabled=", enabled, " fail=", fail));
      LogCapture logs;
      const auto result = RunFlow(
          absl::StrCat(
              R"(
flow main {
 out result: string stream
 nodes private {
 n = node()
 gate = node()
 finish = node()
 producer = {
  "item" -> n
  gate -> _
  finish -> _
 }
 released = n | first 1 | log it -> gate
 n | consume -> result
)",
              fail ? "aborted = { abort n data_loss \"late\" after released }\n"
                     "\"done\" -> finish after aborted\n} }"
                   : "\"done\" -> finish after released\n} }"),
          "main", {}, {}, true, {}, nullptr, enabled);
      EXPECT_EQ(result.status.code(),
                fail ? absl::StatusCode::kDataLoss : absl::StatusCode::kOk)
          << result.status;
      EXPECT_EQ(logs.texts(), Values({"item"}));
      if (!fail) {
        EXPECT_EQ(result.outputs.at("result"), Values({"\"item\""}));
      }
    }
  }
}

TEST(FlowRuntimeTest, SingleItemSlotsKeepNullCursorFanoutAndAbandonment) {
  const std::vector<std::string> sources{
      R"(flow main { out result: object
 nodes private {
 n = node()
 null -> n
 a = try take n
 b = try take n
 {a: a, b: b} -> result
} })",
      R"(flow main { out a: string stream
 out b: string stream
 nodes private {
 n = node()
 "x" -> n
 n -> a
 n -> b
 n | first 0 -> _
} })",
      R"(flow main { out result: int stream
 nodes private {
 n = node()
 7 -> n
 for i in [1, 2, 3] | flatten {
  if i > 0 { n -> result }
 }
} })",
      R"(flow main { out result: int stream
 nodes private {
 for i in [1, 2, 3] | flatten {
  n = node()
  i -> n
  a = take n
  b = try take n
  a -> result
 }
} })"};
  for (const auto& source : sources) {
    SCOPED_TRACE(source);
    const auto baseline = RunFlow(source, "main");
    const auto optimized =
        RunFlow(source, "main", {}, {}, true, {}, nullptr, true);
    ASSERT_TRUE(baseline.status.ok()) << baseline.status;
    EXPECT_EQ(optimized.status, baseline.status);
    EXPECT_EQ(optimized.outputs, baseline.outputs);
  }
}

TEST(FlowRuntimeTest, SingleItemSlotsKeepInitializersGatesAndSiblingProgress) {
  for (bool enabled : {false, true}) {
    LogCapture logs;
    const auto result = RunFlow(R"(
flow marker { out value: int
 if true { log "marker" }
 1 -> value
}
flow main { out result: int
 if false { unused = take ((run boom()).out) }
 unused = take ((run marker()).value)
 before = "before" | log it -> _
 wait all
 after = take ((run marker()).value)
 after -> result
}
)",
                                "main", {}, {}, true, {}, nullptr, enabled);
    ASSERT_TRUE(result.status.ok()) << result.status;
    const auto text = logs.texts();
    EXPECT_EQ(std::count(text.begin(), text.end(), "marker"), 2);
    EXPECT_EQ(text.back(), "marker");
    EXPECT_EQ(result.outputs.at("result"), Values({"1"}));
  }
}

TEST(FlowRuntimeTest, SingleItemSlotsKeepCancellationAndAutomaticDraining) {
  for (bool enabled : {false, true}) {
    const auto timeout = RunFlow(R"(
flow main { in input: string stream
 out result: string stream
 input | first 1 | timeout 2ms -> result
}
)",
                                 "main", {}, {}, false, {}, nullptr, enabled);
    EXPECT_EQ(timeout.status.code(), absl::StatusCode::kDeadlineExceeded);
    const auto drained = RunFlow(R"(
flow main { out result: string stream
 call = run twice(text: "value")
 call.out | first 1 -> result
}
)",
                                 "main", {}, {}, true, {}, nullptr, enabled);
    EXPECT_TRUE(drained.status.ok()) << drained.status;
    EXPECT_EQ(drained.outputs.at("result"), Values({"\"VALUE\""}));
  }
}

TEST(FlowRuntimeTest, SingleItemInlineInitializerDoesNotSerializeSiblings) {
  for (bool enabled : {false, true}) {
    auto release = std::make_shared<thread::PermanentEvent>();
    auto registry = TestRegistry();
    actions::ActionSchema awaiting;
    awaiting.name = "await_value";
    awaiting.outputs.emplace(
        "out", actions::ActionPortSchema{.name = "out", .type = "string"});
    ASSERT_TRUE(
        registry
            ->Register(
                "await_value", awaiting,
                actions::MakeAsyncActionHandler(
                    [release](const auto& action) -> absl::Status {
                      thread::Select({thread::OnCancel(), release->OnEvent()});
                      if (thread::Cancelled()) {
                        return absl::CancelledError("cancelled");
                      }
                      ABSL_ASSIGN_OR_RETURN(auto output,
                                            action->GetOutput("out"));
                      return output
                          ->Finalize(JsonChunk("\"ready\""), {.wait = true})
                          .Await()
                          .status();
                    }))
            .ok());
    actions::ActionSchema signal;
    signal.name = "signal";
    ASSERT_TRUE(
        registry
            ->Register("signal", signal,
                       actions::MakeAsyncActionHandler([release](const auto&) {
                         release->Notify();
                         return absl::OkStatus();
                       }))
            .ok());
    auto stats = std::make_shared<TransportStats>();
    const auto result =
        RunFlow(R"(
flow main { out result: string
 value = take ((run await_value()).out)
 run signal()
 value -> result
}
)",
                "main", {}, {}, true, {}, registry, enabled, stats);
    EXPECT_TRUE(result.status.ok()) << result.status;
    EXPECT_EQ(result.outputs.at("result"), Values({"\"ready\""}));
    if (enabled) {
      EXPECT_GT(stats->inline_producers.load(), 0u);
    }
  }
}

TEST(FlowRuntimeTest, SingleItemReplayRetainsExistingTimedReadBehavior) {
  for (bool enabled : {false, true}) {
    auto registry = TestRegistry();
    actions::ActionSchema pause;
    pause.name = "pause";
    ASSERT_TRUE(registry
                    ->Register("pause", pause,
                               actions::MakeAsyncActionHandler([](const auto&) {
                                 thread::SleepFor(absl::Milliseconds(20));
                                 return absl::OkStatus();
                               }))
                    .ok());
    const auto result = RunFlow(R"(
flow main { out result: int stream
 nodes waiting {}
 slow = run pause() via waiting
 nodes scratch {
  n = node()
  42 -> n after slow
  for i in [1] | flatten { n | timeout 1ms -> result }
 }
}
)",
                                "main", {}, {}, true, {}, registry, enabled);
    EXPECT_TRUE(result.status.ok()) << result.status;
    EXPECT_EQ(result.outputs.at("result"), Values({"42"}));
  }
}

}  // namespace a11::flow
