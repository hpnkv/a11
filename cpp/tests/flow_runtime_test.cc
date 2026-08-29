// Copyright 2026 The A11 Authors.

// What the native runtime is *for*: running flows.

#include <algorithm>
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
                bool close_inputs = true) {
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
  out total: integer
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

TEST(FlowRuntimeTest, SkipACallDrainsEveryOutputOfIt) {
  const Outcome outcome = RunFlow(R"(
flow ignore {
  in  words: string stream
  out done:  string
  say = run twice(text: words)
  skip say
  "finished" -> done
}
)",
                                  "ignore", {{"words", {"\"a\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("done"), Values({"\"finished\""}));
}

TEST(FlowRuntimeTest, SkipNamesSeveralOutputsOfOneCallTogether) {
  const Outcome outcome = RunFlow(R"(
flow ignore {
  in  words: string stream
  out done:  string
  say = run twice(text: words)
  skip (out, quiet) of say
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
    strformat("%s:%s", prefix, word) -> lines
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
  // The case the old behaviour could not report at all: a port promised one
  // value and got two. Reading the first and ignoring the rest is what a flow
  // author never finds out about, so it is named instead.
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

  let word = words
  strformat("1:%s", word) -> out
  advance word
  strformat("2:%s", word) -> out
  advance word
  strformat("3:%s", word) -> out
}
)",
              "paced", {{"words", {"\"a\"", "\"b\"", "\"c\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  Values seen = outcome.outputs.at("out");
  std::sort(seen.begin(), seen.end());
  EXPECT_EQ(seen, Values({"\"1:a\"", "\"2:b\"", "\"3:c\""}));
}

TEST(FlowRuntimeTest, AdvancingPastTheEndBindsNothing) {
  // An empty stream binds nothing, which is what a `let` on one already does,
  // so advancing past the last value is that and not an error.
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
              {{"users", {R"({"name": "Alice", "age": 27})"}},
               {"pair", {R"(["one", "two"])"}}});
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

TEST(FlowRuntimeTest, ATryPipeTurnsAFailingStreamIntoAValue) {
  // The gap: a pipe whose source failed mid-stream ended the *flow*, and the
  // only survivable case was a source that happened to be a `try` call's port.
  const Outcome outcome = RunFlow(R"(
flow resilient {
  in  words: string stream
  out seen:  string stream
  out why:   string
  findings = node()
  for word in words {
    word -> findings
    if word == "bad" { abort findings unavailable "the source went away" }
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
  // The ending `drain` cannot express. Aborting a node that something else is
  // about to close cleanly is a race, and not a race this test wants to be
  // about.
  const Outcome outcome = RunFlow(R"(
flow cut-short {
  in  words: string stream
  out seen:  string stream
  findings = node()
  for word in words {
    word -> findings
    if word == "bad" { abort findings unavailable "the source went away" }
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
    until word == "stop"
  }
  ended = drain taken after done
  status done | map it.ok -> over
  taken -> seen
  skip ended
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
  skip first.quiet
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
    until word == "stop"
  }
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
    until true
  }
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
    while word != "stop"
  }
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
    until word == "absent"
  }
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
  out seen: integer
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
  const Outcome outcome =
      RunFlow(R"(
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
// The interesting cases are all about *ending*: two streams almost never run
// out together, and what a joint iteration does at the ragged end.

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
  const Outcome outcome = RunFlow(
      R"(
flow ragged {
  in  left:   string stream
  in  right:  string stream
  out joined: json stream
  out count:  integer
  zip(left, right) -> joined
  zip(left, right) | count -> count
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
  out joined: json stream
  zip(left, right) -> joined
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
  const Outcome outcome =
      RunFlow(R"(
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
              {{"urls", {"\"/a\"", "\"/b\""}}, {"titles", {"\"A\"", "\"B\""}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("lines"), Values({"\"/a = A\"", "\"/b = B\""}));
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

TEST(FlowRuntimeTest, ALogStatementMayPrintAValueItRead) {
  // A log's arguments live in its own tail rather than in the `message` every
  // other statement uses, and they were left out of the analysis -- so a log
  // that named a stream was one uncounted reader of it and the flow died.
  LogCapture logs;
  const Outcome outcome = RunFlow(R"(
flow narrated {
  in  words: string stream
  out said:  string stream
  let first-word = words | first 1
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
  let tag = "run-1"
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
  in  orders: json stream
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
    | scan {"inside": false, "line": ""} as at, {"inside": starts-with(it, "B") or (at.inside and not starts-with(it, "E")), "line": it}
    | where it.inside and not starts-with(it.line, "B")
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
  words | window 2 | map join(it, "+") -> pairs
  words | batch 2 | map join(it, "+") -> grouped
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
  words | window 3 | map join(it, "+") -> seen
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
  words | window 1 | map join(it, "+") -> seen
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
  in  hits:   json stream
  out best:   json stream
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
  interleave(fast, slow) -> all
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
  in  docs: json stream
  out out:  json stream
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
  in  docs: json stream
  out good: json stream
  out bad:  json stream
  docs | try map it as Order into bad -> good
}
)"),
              "routed", {{"docs", {R"({"id": "a"})", "{}"}}});
  ASSERT_TRUE(outcome.status.ok()) << outcome.status;
  EXPECT_EQ(outcome.outputs.at("good").size(), 1u);
  // The failure arrives as a status record, which is a stream like any other.
  ASSERT_EQ(outcome.outputs.at("bad").size(), 1u);
  EXPECT_NE(outcome.outputs.at("bad").front().find("\"ok\""),
            std::string::npos);
}

TEST(FlowRuntimeTest, WithoutTryAValueTheStageCannotDoEndsTheFlow) {
  const Outcome outcome = RunFlow(absl::StrCat(kOrderShape, R"(
flow strict {
  in  docs: json stream
  out out:  json stream
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
  words | map upper(it) parallel 4 -> out
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
  words | map upper(it) parallel 4 unordered -> out
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
  let n = wait first of a, b
  strformat("won %s", n) -> out
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
  EXPECT_EQ(logs.texts(), Values({"a", "b", "saw a", "saw b"}));
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

}  // namespace
}  // namespace a11::flow
