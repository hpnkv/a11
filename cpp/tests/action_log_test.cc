// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Action::Log: what it writes, where it goes, and when it refuses.
 *
 * The log port is the one output nobody declares, drains or closes, so most of
 * what is worth pinning here is about the absences: an action that never logs
 * costs no node, an action that logs into a void still finishes, and a handler
 * that narrates itself cannot fail because of it.
 */

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/actions/action.h"
#include "a11/actions/log.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/net/in_process_wire_stream.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/service/session.h"
#include "thread/boost_primitives.h"

namespace a11::actions {
namespace {

ActionSchema QuietSchema() {
  return ActionSchema{
      .name = "quiet",
      .outputs = {{"output",
                   ActionPortSchema{.name = "output", .type = "text/plain"}}},
  };
}

/// Collects every log the process sink is handed, for the length of a test.
///
/// The sink is process-wide, so a test that installs one has to put the
/// previous one back; doing that in a scope guard rather than in each test is
/// what keeps a failing assertion from leaking a sink into those that follow.
class SinkCapture {
 public:
  SinkCapture() {
    SetActionLogSink([this](const LogRecord& record) {
      thread::MutexLock lock(&mu_);
      records_.push_back(Copied{
          .action_name = std::string(record.action_name),
          .level = record.level,
          .channel = std::string(record.channel),
          .file = std::string(record.file),
          .lineno = record.lineno,
          .internal = record.internal,
          .mimetype = std::string(record.mimetype),
          .data = std::string(record.data),
          .timestamp = record.timestamp,
      });
    });
  }

  ~SinkCapture() { SetActionLogSink(nullptr); }

  SinkCapture(const SinkCapture&) = delete;
  SinkCapture& operator=(const SinkCapture&) = delete;

  struct Copied {
    std::string action_name;
    LogLevel level = kDefaultLogLevel;
    std::string channel;
    std::string file;
    std::optional<int> lineno;
    bool internal = false;
    std::string mimetype;
    std::string data;
    absl::Time timestamp = absl::InfinitePast();
  };

  std::vector<Copied> records() const {
    thread::MutexLock lock(&mu_);
    return records_;
  }

 private:
  mutable thread::Mutex mu_;
  std::vector<Copied> records_;
};

/// Runs `body` inside a handler and waits for the action to finish.
using LogBody = std::function<absl::Status(std::shared_ptr<Action>)>;

absl::Status RunWith(const std::shared_ptr<Action>& action, LogBody body) {
  // `body` is copied into the submitted task rather than captured by reference.
  // The handler returns as soon as it has submitted, so a reference to the
  // handler closure's own member outlives nothing: the fibre reads it after the
  // call that owns it has returned. It survived on macOS and read freed memory
  // on Linux, which is what a dangling reference is entitled to do.
  auto bound = action->BindHandler([body = std::move(body)](
                                       std::shared_ptr<Action> running) {
    return a11::SubmitTask([running = std::move(running),
                            body]() -> absl::Status { return body(running); });
  });
  if (!bound.ok()) {
    return bound;
  }
  if (const absl::Status started = action->Run().status(); !started.ok()) {
    return started;
  }
  return action->Wait(absl::Seconds(5)).Await().status();
}

TEST(ActionLogTest, TheLogPortCannotBeDeclaredInASchema) {
  ActionSchema schema = QuietSchema();
  schema.outputs.emplace(std::string(kActionLogOutput),
                         ActionPortSchema{.name = std::string(kActionLogOutput),
                                          .type = "text/plain"});
  const absl::Status status = schema.Validate();
  EXPECT_TRUE(absl::IsInvalidArgument(status)) << status;
  EXPECT_NE(status.message().find("is reserved"), std::string_view::npos)
      << status.message();
}

TEST(ActionLogTest, TheLogPortIsInNoSchemaAndNoActionMessage) {
  auto action = *Action::Create(QuietSchema(), "hidden");
  EXPECT_FALSE(action->GetSchema().outputs.contains(kActionLogOutput));
  for (const data::Port& port : action->GetActionMessage().outputs) {
    EXPECT_NE(port.name, kActionLogOutput);
  }
  // Reachable by name all the same, exactly as the status port is.
  EXPECT_TRUE(action->ContainsPort(kActionLogOutput));
}

TEST(ActionLogTest, OnlyARunningActionMayLog) {
  auto action = *Action::Create(QuietSchema(), "before-run");
  const absl::Status before = action->Log("too early");
  EXPECT_TRUE(absl::IsFailedPrecondition(before)) << before;

  // The calling side of a Call is the other illegal case: the port belongs to
  // whichever peer is executing the action.
  auto caller = *Action::Create(QuietSchema(), "caller");
  ASSERT_TRUE(caller->Cancel().ok());
  const absl::Status cancelled = caller->Log("also not");
  EXPECT_TRUE(absl::IsFailedPrecondition(cancelled)) << cancelled;
}

TEST(ActionLogTest, AStringIsTextWhereAPutOfOneIsBytes) {
  SinkCapture capture;
  auto action = *Action::Create(QuietSchema(), "text-by-default");
  ASSERT_TRUE(RunWith(action, [](const std::shared_ptr<Action>& running) {
                return running->Log(std::string("a string"));
              }).ok());

  // The contrast the API is for: the same value through the node API is bytes,
  // because in C++ a std::string is a sequence of them.
  const absl::StatusOr<data::Chunk> put =
      data::GlobalSerializationRegistry().ToChunk<std::string>(
          std::string("a string"));
  ASSERT_TRUE(put.ok()) << put.status();
  EXPECT_EQ(put->GetMimetype(), data::kBytesMimetype);

  const std::vector<SinkCapture::Copied> records = capture.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].mimetype, data::kTextMimetype);
  EXPECT_EQ(records[0].data, "a string");
}

TEST(ActionLogTest, EveryStringLikeSpellingIsText) {
  SinkCapture capture;
  auto action = *Action::Create(QuietSchema(), "spellings");
  const char* pointer = "pointer";
  ASSERT_TRUE(
      RunWith(action, [pointer](const std::shared_ptr<Action>& running) {
        if (const absl::Status literal = running->Log("literal");
            !literal.ok()) {
          return literal;
        }
        if (const absl::Status view = running->Log(std::string_view("view"));
            !view.ok()) {
          return view;
        }
        return running->Log(pointer);
      }).ok());
  const std::vector<SinkCapture::Copied> records = capture.records();
  ASSERT_EQ(records.size(), 3u);
  for (const SinkCapture::Copied& record : records) {
    EXPECT_EQ(record.mimetype, data::kTextMimetype) << record.data;
  }
  EXPECT_EQ(records[0].data, "literal");
  EXPECT_EQ(records[1].data, "view");
  EXPECT_EQ(records[2].data, "pointer");
}

TEST(ActionLogTest, AnExplicitMimetypeWins) {
  SinkCapture capture;
  auto action = *Action::Create(QuietSchema(), "as-bytes");
  ASSERT_TRUE(RunWith(action, [](const std::shared_ptr<Action>& running) {
                return running->Log(
                    std::string("\xff\xfe"),
                    LogOptions{.mimetype = data::kBytesMimetype});
              }).ok());
  const std::vector<SinkCapture::Copied> records = capture.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].mimetype, data::kBytesMimetype);
}

TEST(ActionLogTest, LogfFormatsLikeStrFormat) {
  SinkCapture capture;
  auto action = *Action::Create(QuietSchema(), "formatted");
  ASSERT_TRUE(RunWith(action, [](const std::shared_ptr<Action>& running) {
                if (const absl::Status plain =
                        running->Logf("read %d of %d pages", 3, 12);
                    !plain.ok()) {
                  return plain;
                }
                return running->LogfWith(
                    LogOptions{.level = "warning", .channel = "fetch"},
                    "retrying %s", "https://example.invalid");
              }).ok());
  const std::vector<SinkCapture::Copied> records = capture.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].data, "read 3 of 12 pages");
  EXPECT_EQ(records[0].level, LogLevel::kInfo);
  EXPECT_EQ(records[1].data, "retrying https://example.invalid");
  EXPECT_EQ(records[1].level, LogLevel::kWarning);
  EXPECT_EQ(records[1].channel, "fetch");
}

TEST(ActionLogTest, EveryLogCarriesATimestamp) {
  SinkCapture capture;
  const absl::Time before = absl::Now();
  auto action = *Action::Create(QuietSchema(), "stamped");
  ASSERT_TRUE(RunWith(action, [](const std::shared_ptr<Action>& running) {
                return running->Log("stamped");
              }).ok());
  const std::vector<SinkCapture::Copied> records = capture.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_GE(records[0].timestamp, before);
  EXPECT_LE(records[0].timestamp, absl::Now());
}

TEST(ActionLogTest, AnExplicitLevelBeatsOneInTheMetadataMap) {
  SinkCapture capture;
  auto action = *Action::Create(QuietSchema(), "precedence");
  const data::ByteMap extra = {{std::string(kLogLevelAttribute), "info"},
                               {"request", "42"}};
  ASSERT_TRUE(RunWith(action, [&extra](const std::shared_ptr<Action>& running) {
                return running->Log(
                    "escalated",
                    LogOptions{.level = "error", .metadata = &extra});
              }).ok());
  const std::vector<SinkCapture::Copied> records = capture.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].level, LogLevel::kError);
}

TEST(ActionLogTest, AnUnknownLevelIsRefusedBeforeAnythingIsWritten) {
  SinkCapture capture;
  auto action = *Action::Create(QuietSchema(), "bad-level");
  absl::Status logged = absl::OkStatus();
  ASSERT_TRUE(
      RunWith(action, [&logged](const std::shared_ptr<Action>& running) {
        logged = running->Log("noisy", LogOptions{.level = "verbose"});
        return absl::OkStatus();
      }).ok());
  EXPECT_TRUE(absl::IsInvalidArgument(logged)) << logged;
  EXPECT_TRUE(capture.records().empty());
}

TEST(ActionLogTest, AStatusChunkIsRefused) {
  // A status chunk on an ordinary node is a lifecycle marker, and a peer that
  // receives one aborts the node: logging one would tear the log port down.
  SinkCapture capture;
  auto action = *Action::Create(QuietSchema(), "status-log");
  absl::Status logged = absl::OkStatus();
  ASSERT_TRUE(
      RunWith(action, [&logged](const std::shared_ptr<Action>& running) {
        absl::StatusOr<data::Chunk> chunk =
            StatusToChunk(absl::InternalError("nope"));
        if (!chunk.ok()) {
          return chunk.status();
        }
        logged = running->Log(*std::move(chunk));
        return absl::OkStatus();
      }).ok());
  EXPECT_TRUE(absl::IsInvalidArgument(logged)) << logged;
  EXPECT_TRUE(capture.records().empty());
}

TEST(ActionLogTest, AnUnclaimedLocalLogMaterialisesNoNode) {
  // Nothing local reads an unclaimed log port, so writing to it would buffer
  // every line for the length of the run and then discard them.
  SinkCapture capture;
  auto node_map = *nodes::NodeMap::Create();
  auto action = *Action::Create(QuietSchema(), "no-node", {}, node_map);
  ASSERT_TRUE(RunWith(action, [](const std::shared_ptr<Action>& running) {
                return running->Log("into the sink");
              }).ok());
  EXPECT_EQ(capture.records().size(), 1u);

  const absl::StatusOr<std::string> id =
      Action::MakeNodeId("no-node", kActionLogOutput);
  ASSERT_TRUE(id.ok()) << id.status();
  const absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> existing =
      node_map->GetIfExists(*id);
  ASSERT_TRUE(existing.ok()) << existing.status();
  EXPECT_EQ(*existing, nullptr);
}

TEST(ActionLogTest, AClaimedLogPortCarriesTheChunksAndClosesItself) {
  SinkCapture capture;
  auto action = *Action::Create(QuietSchema(), "claimed");
  absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> logs = action->GetLogNode();
  ASSERT_TRUE(logs.ok()) << logs.status();

  ASSERT_TRUE(RunWith(action, [](const std::shared_ptr<Action>& running) {
                if (const absl::Status first =
                        running->Log("first", LogOptions{.channel = "work"});
                    !first.ok()) {
                  return first;
                }
                return running->Log("second", LogOptions{.level = "warning",
                                                         .lineno = 7,
                                                         .internal = true});
              }).ok());

  // A claimed port owns presentation, so the sink is not also told.
  EXPECT_TRUE(capture.records().empty());

  std::vector<data::Chunk> seen;
  while (true) {
    const absl::StatusOr<std::optional<data::Chunk>> chunk =
        (*logs)->NextChunk().Await(absl::Now() + absl::Seconds(5));
    ASSERT_TRUE(chunk.ok()) << chunk.status();
    if (!chunk->has_value()) {
      break;  // Closed with the action's other outputs: nobody had to do it.
    }
    if (data::IsStatusChunk(**chunk)) {
      continue;
    }
    seen.push_back(**chunk);
  }
  ASSERT_EQ(seen.size(), 2u);

  const LogRecord first = LogRecordFromChunk(seen[0]);
  EXPECT_EQ(first.data, "first");
  EXPECT_EQ(first.level, kDefaultLogLevel);
  EXPECT_EQ(first.channel, "work");
  EXPECT_FALSE(first.internal);
  EXPECT_EQ(first.mimetype, data::kTextMimetype);
  EXPECT_NE(first.timestamp, absl::InfinitePast());

  const LogRecord second = LogRecordFromChunk(seen[1]);
  EXPECT_EQ(second.data, "second");
  EXPECT_EQ(second.level, LogLevel::kWarning);
  EXPECT_TRUE(second.internal);
  EXPECT_EQ(second.lineno, 7);
}

TEST(ActionLogTest, AChattyActionNobodyDrainsStillFinishes) {
  SinkCapture capture;
  auto action = *Action::Create(QuietSchema(), "chatty");
  ASSERT_TRUE(action->GetLogNode().ok());  // Claimed, then never read.
  const absl::Status ran =
      RunWith(action, [](const std::shared_ptr<Action>& running) {
        for (int index = 0; index < 256; ++index) {
          if (const absl::Status logged = running->Logf("line %d", index);
              !logged.ok()) {
            return logged;
          }
        }
        return absl::OkStatus();
      });
  EXPECT_TRUE(ran.ok()) << ran;
}

TEST(ActionLogTest, ALogAfterTheActionFinishedIsReportedAndNotAnError) {
  SinkCapture capture;
  auto action = *Action::Create(QuietSchema(), "late");
  std::shared_ptr<Action> escaped;
  ASSERT_TRUE(RunWith(action, [&escaped](std::shared_ptr<Action> running) {
                escaped = std::move(running);
                return absl::OkStatus();
              }).ok());
  ASSERT_NE(escaped, nullptr);
  // The handler is gone and the outputs are closed. Logging now cannot reach
  // the port, but it is still not the late logger's failure.
  const absl::Status late = escaped->Log("after the fact");
  EXPECT_TRUE(late.ok()) << late;
}

TEST(ActionLogTest, AFailingActionStillReportsItsLogs) {
  SinkCapture capture;
  auto action = *Action::Create(QuietSchema(), "doomed");
  const absl::Status ran =
      RunWith(action, [](const std::shared_ptr<Action>& running) {
        if (const absl::Status logged = running->Log("about to fail");
            !logged.ok()) {
          return logged;
        }
        return absl::InternalError("as promised");
      });
  EXPECT_TRUE(absl::IsInternal(ran)) << ran;
  const std::vector<SinkCapture::Copied> records = capture.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, "about to fail");
  EXPECT_EQ(records[0].action_name, "quiet");
}

TEST(ActionLogTest, LevelNamesRoundTripAndAcceptTheCommonAliases) {
  for (const LogLevel level :
       {LogLevel::kDebug, LogLevel::kInfo, LogLevel::kWarning, LogLevel::kError,
        LogLevel::kCritical}) {
    const absl::StatusOr<LogLevel> parsed = ParseLogLevel(LogLevelName(level));
    ASSERT_TRUE(parsed.ok()) << parsed.status();
    EXPECT_EQ(*parsed, level);
  }
  EXPECT_EQ(*ParseLogLevel("warn"), LogLevel::kWarning);
  EXPECT_EQ(*ParseLogLevel("FATAL"), LogLevel::kCritical);
  EXPECT_EQ(*ParseLogLevel(""), kDefaultLogLevel);
  EXPECT_TRUE(absl::IsInvalidArgument(ParseLogLevel("chatty").status()));
  // Never fatal: reporting a log must not end the process.
  EXPECT_NE(LogLevelToSeverity(LogLevel::kCritical), absl::LogSeverity::kFatal);
}

TEST(ActionLogTest, ADispatchedActionsLogsReachTheCaller) {
  // The path with the most moving parts: the receiver logs, the write tees to
  // the peer, and the caller reads the mirror of a node whose id it never sent,
  // because both sides derive it from the same action id.
  auto registry = std::make_shared<ActionRegistry>();
  ASSERT_TRUE(
      registry
          ->Register("quiet", QuietSchema(),
                     [](std::shared_ptr<Action> action) {
                       return a11::SubmitTask(
                           [action = std::move(action)]() -> absl::Status {
                             return action->Log("from the far end",
                                                LogOptions{.level = "warning"});
                           });
                     })
          .ok());

  service::SessionOptions options;
  options.no_stream_timeout = absl::InfiniteDuration();
  auto client = *service::Session::Create("client", {}, {}, {}, options);
  auto server = *service::Session::Create("server", {}, {}, {}, options,
                                          nullptr, registry);
  auto pair = *net::InProcessWireStream::CreatePair();
  ASSERT_TRUE(
      client->AddStream(pair.first, service::StreamMode::kStart)->Await().ok());
  ASSERT_TRUE(server->AddStream(pair.second, service::StreamMode::kAccept)
                  ->Await()
                  .ok());

  auto caller_registry = std::make_shared<ActionRegistry>();
  ASSERT_TRUE(caller_registry->Register("quiet", QuietSchema()).ok());
  auto call = *caller_registry->MakeAction("quiet", "remote-log", nullptr,
                                           pair.first, client);
  // Claimed before the call, which is what a caller that means to read the logs
  // does; bind_stream is false, or the caller would tee every reply back at the
  // peer and corrupt the connection.
  absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> logs = call->GetLogNode();
  ASSERT_TRUE(logs.ok()) << logs.status();

  ASSERT_TRUE(call->Call().Await().ok());
  ASSERT_TRUE(call->WaitForDispatch(absl::Seconds(5)).Await().ok());
  const absl::StatusOr<std::shared_ptr<Action>> completion =
      call->Wait(absl::Seconds(5)).Await();
  ASSERT_TRUE(completion.ok()) << completion.status();

  const absl::StatusOr<std::optional<data::Chunk>> received =
      (*logs)->NextChunk().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(received.ok()) << received.status();
  ASSERT_TRUE(received->has_value());
  const LogRecord record = LogRecordFromChunk(**received);
  EXPECT_EQ(record.data, "from the far end");
  EXPECT_EQ(record.level, LogLevel::kWarning);
}

TEST(ActionLogTest, ADispatchedActionThatNeverLogsStillEndsItsLogStream) {
  // The hazard behind this one: a graceful close only tees a marker to a stream
  // something attached, and nothing attaches the receiver's log port unless it
  // logged. A caller reading logs from a silent action would then wait forever.
  auto registry = std::make_shared<ActionRegistry>();
  ASSERT_TRUE(
      registry
          ->Register("quiet", QuietSchema(),
                     [](std::shared_ptr<Action> action) {
                       return a11::SubmitTask(
                           [action = std::move(action)]() -> absl::Status {
                             return absl::OkStatus();  // Says nothing at all.
                           });
                     })
          .ok());

  service::SessionOptions options;
  options.no_stream_timeout = absl::InfiniteDuration();
  auto client = *service::Session::Create("client", {}, {}, {}, options);
  auto server = *service::Session::Create("server", {}, {}, {}, options,
                                          nullptr, registry);
  auto pair = *net::InProcessWireStream::CreatePair();
  ASSERT_TRUE(
      client->AddStream(pair.first, service::StreamMode::kStart)->Await().ok());
  ASSERT_TRUE(server->AddStream(pair.second, service::StreamMode::kAccept)
                  ->Await()
                  .ok());

  auto caller_registry = std::make_shared<ActionRegistry>();
  ASSERT_TRUE(caller_registry->Register("quiet", QuietSchema()).ok());
  auto call = *caller_registry->MakeAction("quiet", "silent-log", nullptr,
                                           pair.first, client);
  absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> logs = call->GetLogNode();
  ASSERT_TRUE(logs.ok()) << logs.status();

  ASSERT_TRUE(call->Call().Await().ok());
  ASSERT_TRUE(call->WaitForDispatch(absl::Seconds(5)).Await().ok());
  ASSERT_TRUE(call->Wait(absl::Seconds(5)).Await().ok());

  // A bounded read, so a stream that never ends fails here rather than hanging
  // the suite.
  const absl::StatusOr<std::optional<data::Chunk>> received =
      (*logs)->NextChunk().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(received.ok()) << received.status();
  EXPECT_FALSE(received->has_value()) << "expected end of stream";
}

}  // namespace
}  // namespace a11::actions
