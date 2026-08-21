// Copyright 2026 The A11 Authors.

#include "sdk/flow/actions/flow_actions.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/future.h"
#include "a11/data/msgpack.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/json_codec.h"
#include "a11/nodes/async_node.h"
#include "a11/uuid.h"
#include "sdk/flow/actions/fs_actions.h"
#include "sdk/flow/actions/options.h"
#include "sdk/flow/actions/policy.h"
#include "sdk/flow/actions/process_actions.h"
#include "sdk/flow/actions/sandbox.h"
#include "sdk/flow/actions/stop.h"
#include "sdk/flow/actions/system_actions.h"
#include "sdk/flow/actions/time_actions.h"

namespace a11::sdk::flow {
namespace {

using ::a11::actions::Action;
using ::a11::actions::ActionRegistry;
using ::a11::nodes::AsyncNode;

namespace fs = std::filesystem;

constexpr absl::Duration kPatience = absl::Seconds(10);

// --- fixtures ----------------------------------------------------------------

/// A directory of its own, removed however the test ends.
class Workspace {
 public:
  Workspace() {
    root_ =
        fs::temp_directory_path() / absl::StrCat("a11-flow-", a11::NewUuid());
    std::error_code error;
    fs::create_directories(root_, error);
    // Canonical, because the actions report canonical paths and are right to:
    // on a Mac /var is a symlink to /private/var, so a test that compared
    // against the spelling it passed in would be asserting that symlinks are
    // not resolved -- which is the property the sandbox depends on.
    root_ = fs::weakly_canonical(root_, error);
  }

  ~Workspace() {
    std::error_code error;
    fs::remove_all(root_, error);
  }

  [[nodiscard]] const fs::path& root() const { return root_; }

  [[nodiscard]] std::string path(std::string_view name) const {
    return (root_ / name).string();
  }

  // Not [[nodiscard]]: writing the file is the point, and the path it returns
  // is a convenience for the callers that want it.
  std::string Write(std::string_view name, std::string_view contents) const {
    const fs::path at = root_ / name;
    std::ofstream out(at, std::ios::binary);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.close();
    return at.string();
  }

  [[nodiscard]] std::string Read(std::string_view name) const {
    std::ifstream in(root_ / name, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
  }

 private:
  fs::path root_;
};

CapabilitiesPtr WritableIn(const fs::path& root) {
  CapabilitiesBuilder capabilities = WorkspaceCapabilities({root.string()});
  return capabilities;
}

/// Writes a unary input, the way a caller would.
absl::Status PutInput(const std::shared_ptr<Action>& action,
                      std::string_view port, const nlohmann::json& value) {
  absl::StatusOr<std::shared_ptr<AsyncNode>> node =
      action->GetInput(std::string(port));
  if (!node.ok()) {
    return node.status();
  }
  data::Chunk chunk;
  chunk.metadata = data::ChunkMetadata{.mimetype = "application/json"};
  chunk.data = value.dump();
  return (*node)
      ->PutChunk(std::move(chunk), std::nullopt, /*final=*/true)
      .Await()
      .status();
}

/// Reads every value on an output port as raw payloads.
std::vector<std::string> ReadAll(const std::shared_ptr<Action>& action,
                                 std::string_view port) {
  std::vector<std::string> values;
  absl::StatusOr<std::shared_ptr<AsyncNode>> node =
      action->GetOutput(std::string(port));
  if (!node.ok()) {
    return values;
  }
  while (true) {
    absl::StatusOr<std::optional<data::Chunk>> chunk =
        (*node)->NextChunk(kPatience).Await();
    if (!chunk.ok() || !chunk->has_value()) {
      break;
    }
    if ((*chunk)->IsNull()) {
      continue;
    }
    values.push_back((*chunk)->data);
  }
  return values;
}

std::optional<nlohmann::json> ReadOne(const std::shared_ptr<Action>& action,
                                      std::string_view port) {
  const std::vector<std::string> values = ReadAll(action, port);
  if (values.empty()) {
    return std::nullopt;
  }
  return nlohmann::json::parse(values.front(), nullptr, false);
}

std::string Concat(const std::vector<std::string>& pieces) {
  std::string joined;
  for (const std::string& piece : pieces) {
    joined += piece;
  }
  return joined;
}

// --- options -----------------------------------------------------------------

TEST(FlowOptionsTest, AbsentOptionsTakeEveryFallback) {
  absl::StatusOr<Options> options = Options::Parse(nullptr);
  ASSERT_TRUE(options.ok());
  EXPECT_EQ(options->Int("count", 7).value_or(0), 7);
  EXPECT_TRUE(options->Bool("catch_up", true).value_or(false));
}

TEST(FlowOptionsTest, SaysWhichKeyAndWhatArrived) {
  const nlohmann::json value{{"chunk_bytes", "big"}};
  absl::StatusOr<Options> options = Options::Parse(&value);
  ASSERT_TRUE(options.ok());
  const absl::StatusOr<std::int64_t> read = options->Int("chunk_bytes", 1);
  ASSERT_FALSE(read.ok());
  EXPECT_EQ(read.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_NE(read.status().message().find("options.chunk_bytes"),
            std::string::npos);
  EXPECT_NE(read.status().message().find("a string"), std::string::npos);
}

TEST(FlowOptionsTest, ReadsDurationsTheWayFlowWritesThem) {
  const nlohmann::json value{{"a", "1m30s"}, {"b", 45}, {"c", "250ms"}};
  absl::StatusOr<Options> options = Options::Parse(&value);
  ASSERT_TRUE(options.ok());
  EXPECT_EQ(options->Duration("a", absl::ZeroDuration()).value(),
            absl::Seconds(90));
  EXPECT_EQ(options->Duration("b", absl::ZeroDuration()).value(),
            absl::Seconds(45));
  EXPECT_EQ(options->Duration("c", absl::ZeroDuration()).value(),
            absl::Milliseconds(250));
}

TEST(FlowOptionsTest, RefusesANegativeDurationRatherThanMeaningForever) {
  const nlohmann::json value{{"timeout", -5}};
  absl::StatusOr<Options> options = Options::Parse(&value);
  ASSERT_TRUE(options.ok());
  EXPECT_FALSE(options->Duration("timeout", absl::ZeroDuration()).ok());
}

TEST(FlowOptionsTest, ReadsTheDeadlineHeaderInBothUnits) {
  const absl::StatusOr<absl::Time> milliseconds = ParseDeadlineHeader("1500");
  ASSERT_TRUE(milliseconds.ok());
  EXPECT_EQ(*milliseconds, absl::UnixEpoch() + absl::Milliseconds(1500));
  const absl::StatusOr<absl::Time> nanoseconds =
      ParseDeadlineHeader("1500000000ns");
  ASSERT_TRUE(nanoseconds.ok());
  EXPECT_EQ(*nanoseconds, absl::UnixEpoch() + absl::Milliseconds(1500));
  EXPECT_FALSE(ParseDeadlineHeader("soon").ok());
}

// --- the deadline watcher ----------------------------------------------------
//
// The watcher is a libuv timer on A11's one loop rather than a fibre parked in a
// timed Select, so what these check is the two things that swap could get wrong:
// a deadline that passes with nobody asking must still stop the action, and a
// run that finishes first must not have a timer fire on it afterwards and
// rewrite its reason.

/// An action with `x-a11-deadline` set @p after from now. `read_file`'s schema
/// stands in for any action's: StopSignal reads the header and nothing else.
std::shared_ptr<Action> MakeActionDueIn(absl::Duration after) {
  absl::StatusOr<std::shared_ptr<Action>> created =
      Action::Create(ReadFileSchema(), a11::NewUuid(),
                     ReadFileHandler(WritableIn(fs::temp_directory_path())));
  if (!created.ok()) {
    return nullptr;
  }
  const std::int64_t due =
      absl::ToInt64Milliseconds((absl::Now() + after) - absl::UnixEpoch());
  if (!(*created)
           ->SetHeader(absl::StrCat(actions::kActionHeaderPrefix, "deadline"),
                       absl::StrCat(due))
           .ok()) {
    return nullptr;
  }
  return *created;
}

TEST(StopSignalTest, ADeadlineNobodyAsksAboutStillStopsTheAction) {
  const std::shared_ptr<Action> action =
      MakeActionDueIn(absl::Milliseconds(40));
  ASSERT_NE(action, nullptr);
  absl::StatusOr<std::shared_ptr<StopSignal>> signal =
      StopSignal::Create(action);
  ASSERT_TRUE(signal.ok()) << signal.status();
  ASSERT_TRUE((*signal)->has_deadline());
  EXPECT_FALSE((*signal)->stopped());

  // Waiting on OnStop() rather than sleeping and looking: a deadline that only
  // fires when something asks is the bug this watcher exists to prevent, and
  // polling would not tell the two apart.
  EXPECT_TRUE((*signal)->WaitFor(kPatience));
  EXPECT_EQ((*signal)->reason(), StopReason::kDeadline);
  EXPECT_TRUE(absl::IsDeadlineExceeded((*signal)->Check()));
  // A source asked to finish has finished.
  EXPECT_TRUE((*signal)->ExitStatus().ok());
}

TEST(StopSignalTest, ARunThatFinishesFirstIsNotStoppedByItsOwnDeadline) {
  const std::shared_ptr<Action> action =
      MakeActionDueIn(absl::Milliseconds(30));
  ASSERT_NE(action, nullptr);
  absl::StatusOr<std::shared_ptr<StopSignal>> signal =
      StopSignal::Create(action);
  ASSERT_TRUE(signal.ok()) << signal.status();

  // What a handler does on its way out. Join() disarms the timer and waits for
  // the loop to have closed it, so nothing can fire afterwards.
  (*signal)->Join();
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  EXPECT_FALSE((*signal)->stopped());
  EXPECT_EQ((*signal)->reason(), StopReason::kRunning);
  EXPECT_TRUE((*signal)->Check().ok());
}

TEST(StopSignalTest, RefusesADeadlineThatHasAlreadyPassed) {
  const std::shared_ptr<Action> action =
      MakeActionDueIn(-absl::Milliseconds(10));
  ASSERT_NE(action, nullptr);
  const absl::StatusOr<std::shared_ptr<StopSignal>> signal =
      StopSignal::Create(action);
  EXPECT_TRUE(absl::IsDeadlineExceeded(signal.status())) << signal.status();
}

TEST(StopSignalTest, ManyDeadlinesShareTheOneLoop) {
  // Fifty of these used to be fifty sleeping fibres, each with a stack. The
  // point of the test is that arming and disarming that many is not a special
  // case: half are joined before they fire and half are left to fire.
  std::vector<std::shared_ptr<StopSignal>> signals;
  for (int index = 0; index < 50; ++index) {
    const std::shared_ptr<Action> action =
        MakeActionDueIn(absl::Milliseconds(index % 2 == 0 ? 30 : 5000));
    ASSERT_NE(action, nullptr);
    absl::StatusOr<std::shared_ptr<StopSignal>> signal =
        StopSignal::Create(action);
    ASSERT_TRUE(signal.ok()) << signal.status();
    signals.push_back(*std::move(signal));
  }
  for (std::size_t index = 1; index < signals.size(); index += 2) {
    signals[index]->Join();
  }
  for (std::size_t index = 0; index < signals.size(); index += 2) {
    EXPECT_TRUE(signals[index]->WaitFor(kPatience));
    EXPECT_EQ(signals[index]->reason(), StopReason::kDeadline);
  }
  for (std::size_t index = 1; index < signals.size(); index += 2) {
    EXPECT_EQ(signals[index]->reason(), StopReason::kRunning);
  }
}

// --- policy ------------------------------------------------------------------

TEST(FlowPolicyTest, RefusesAPathOutsideEveryRoot) {
  Workspace workspace;
  FilesystemPolicy policy;
  policy.roots = {workspace.root().string()};
  EXPECT_TRUE(ResolvePath(policy, workspace.path("inside"), false).ok());
  const absl::StatusOr<fs::path> outside =
      ResolvePath(policy, "/etc/passwd", false);
  ASSERT_FALSE(outside.ok());
  EXPECT_EQ(outside.status().code(), absl::StatusCode::kPermissionDenied);
}

TEST(FlowPolicyTest, ResolvesDotDotBeforeCheckingContainment) {
  Workspace workspace;
  FilesystemPolicy policy;
  policy.roots = {workspace.root().string()};
  EXPECT_FALSE(
      ResolvePath(policy, workspace.path("../../etc/passwd"), false).ok());
}

TEST(FlowPolicyTest, ResolvesASymlinkOutOfARootAndRefusesIt) {
  Workspace workspace;
  FilesystemPolicy policy;
  policy.roots = {workspace.root().string()};
  std::error_code error;
  fs::create_symlink("/etc", workspace.root() / "escape", error);
  if (error) {
    GTEST_SKIP() << "this filesystem does not do symlinks";
  }
  EXPECT_FALSE(
      ResolvePath(policy, workspace.path("escape/passwd"), false).ok());
}

TEST(FlowPolicyTest, ARootPrefixIsNotAContainingRoot) {
  FilesystemPolicy policy;
  policy.roots = {"/srv/data"};
  // `/srv/data-2` starts with `/srv/data` and is not inside it, which a string
  // prefix check would get wrong.
  EXPECT_FALSE(ResolvePath(policy, "/srv/data-2/secret", false).ok());
}

TEST(FlowPolicyTest, RefusesWritesUnderAReadOnlyPolicy) {
  Workspace workspace;
  const CapabilitiesBuilder capabilities =
      ReadOnlyCapabilities({workspace.root().string()});
  EXPECT_TRUE(
      ResolvePath(capabilities->filesystem, workspace.path("x"), false).ok());
  EXPECT_FALSE(
      ResolvePath(capabilities->filesystem, workspace.path("x"), true).ok());
}

TEST(FlowPolicyTest, RefusesTheMetadataEndpointByDefault) {
  NetworkPolicy policy;
  policy.enabled = true;
  policy.any_host = true;
  EXPECT_FALSE(CheckHost(policy, "169.254.169.254").ok());
  EXPECT_FALSE(CheckHost(policy, "127.0.0.1").ok());
  EXPECT_FALSE(CheckHost(policy, "10.1.2.3").ok());
  EXPECT_TRUE(CheckHost(policy, "example.com").ok());
}

TEST(FlowPolicyTest, MatchesAHostPatternOneLabelAtATime) {
  NetworkPolicy policy;
  policy.enabled = true;
  policy.hosts = {"*.example.com"};
  EXPECT_TRUE(CheckHost(policy, "api.example.com").ok());
  EXPECT_FALSE(CheckHost(policy, "example.com").ok());
  EXPECT_FALSE(CheckHost(policy, "api.example.com.evil.test").ok());
}

TEST(FlowPolicyTest, SystemCapabilitiesStillRefuseLinkLocal) {
  const CapabilitiesBuilder capabilities = SystemCapabilities();
  EXPECT_FALSE(CheckHost(capabilities->network, "169.254.169.254").ok());
}

// --- registration ------------------------------------------------------------

TEST(FlowRegistrationTest, SchemasValidate) {
  for (const actions::ActionSchema& schema :
       {TickerSchema(), SleepSchema(), ReadFileSchema(), WriteFileSchema(),
        ListDirectorySchema(), StatPathSchema(), MakeDirectorySchema(),
        RemovePathSchema(), MovePathSchema(), CopyPathSchema(),
        MakeTempSchema()}) {
    EXPECT_TRUE(schema.Validate().ok()) << schema.name;
  }
}

TEST(FlowRegistrationTest, NoActionDeclaresAPortCalledStatus) {
  // Flow reads `x.status` as the outcome of the step called `x`, whatever ports
  // it declares, so a port of that name would be unreachable from a flow.
  for (const actions::ActionSchema& schema :
       {TickerSchema(), SleepSchema(), ReadFileSchema(), WriteFileSchema(),
        ListDirectorySchema(), StatPathSchema(), MakeDirectorySchema(),
        RemovePathSchema(), MovePathSchema(), CopyPathSchema(),
        MakeTempSchema()}) {
    EXPECT_FALSE(schema.outputs.contains("status")) << schema.name;
  }
}

TEST(FlowRegistrationTest, NoActionSharesAPortNameAcrossDirections) {
  // A port name is one node whichever way it faces, so an input and an output
  // of the same name are the same stream.
  for (const actions::ActionSchema& schema :
       {TickerSchema(), SleepSchema(), ReadFileSchema(), WriteFileSchema(),
        ListDirectorySchema(), StatPathSchema(), MakeDirectorySchema(),
        RemovePathSchema(), MovePathSchema(), CopyPathSchema(),
        MakeTempSchema()}) {
    for (const auto& [name, port] : schema.inputs) {
      EXPECT_FALSE(schema.outputs.contains(name))
          << schema.name << " declares '" << name << "' in both directions";
    }
  }
}

TEST(FlowRegistrationTest, ReadOnlyPolicyRegistersNoWriters) {
  Workspace workspace;
  ActionRegistry registry;
  ASSERT_TRUE(RegisterFlowActions(
                  registry, ReadOnlyCapabilities({workspace.root().string()}))
                  .ok());
  EXPECT_TRUE(registry.IsRegistered(kReadFileAction));
  EXPECT_TRUE(registry.IsRegistered(kListDirectoryAction));
  EXPECT_TRUE(registry.IsRegistered(kTickerAction));
  EXPECT_FALSE(registry.IsRegistered(kWriteFileAction));
  EXPECT_FALSE(registry.IsRegistered(kRemovePathAction));
}

TEST(FlowRegistrationTest, WorkspacePolicyRegistersBothHalves) {
  Workspace workspace;
  ActionRegistry registry;
  ASSERT_TRUE(RegisterFlowActions(
                  registry, WorkspaceCapabilities({workspace.root().string()}))
                  .ok());
  EXPECT_TRUE(registry.IsRegistered(kWriteFileAction));
  EXPECT_TRUE(registry.IsRegistered(kMakeTempAction));
}

TEST(FlowRegistrationTest, ReportsAWritablePolicyWithNoRoots) {
  ActionRegistry registry;
  const CapabilitiesBuilder contradictory = std::make_shared<Capabilities>();
  contradictory->filesystem.writable = true;
  EXPECT_FALSE(RegisterFlowActions(registry, contradictory).ok());
}

TEST(FlowRegistrationTest, ATimeOnlyHostGetsAClockAndNothingElse) {
  ActionRegistry registry;
  ASSERT_TRUE(RegisterUnprivilegedFlowActions(registry).ok());
  EXPECT_TRUE(registry.IsRegistered(kTickerAction));
  EXPECT_TRUE(registry.IsRegistered(kSleepAction));
  EXPECT_FALSE(registry.IsRegistered(kReadFileAction));
}

// --- read_file ---------------------------------------------------------------

std::shared_ptr<Action> MakeReadFile(const Workspace& workspace,
                                     const std::string& path,
                                     const nlohmann::json& options = {}) {
  absl::StatusOr<std::shared_ptr<Action>> created =
      Action::Create(ReadFileSchema(), a11::NewUuid(),
                     ReadFileHandler(WritableIn(workspace.root())));
  if (!created.ok()) {
    return nullptr;
  }
  if (!PutInput(*created, "path", path).ok()) {
    return nullptr;
  }
  if (!PutInput(*created, "options",
                options.is_null() ? nlohmann::json::object() : options)
           .ok()) {
    return nullptr;
  }
  return *created;
}

TEST(ReadFileTest, WritesInfoBeforeAnyContent) {
  Workspace workspace;
  const std::string path = workspace.Write("hello.txt", "hello, world");
  const std::shared_ptr<Action> action = MakeReadFile(workspace, path);
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());

  const std::optional<nlohmann::json> info = ReadOne(action, "info");
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->value("kind", std::string()), "file");
  EXPECT_EQ(info->value("size", 0), 12);
  EXPECT_TRUE(info->value("exists", false));
  EXPECT_EQ(Concat(ReadAll(action, "bytes")), "hello, world");
}

TEST(ReadFileTest, ChunksTheContentAndPutsItBackTogether) {
  Workspace workspace;
  const std::string body(10000, 'x');
  const std::string path = workspace.Write("big.bin", body);
  const std::shared_ptr<Action> action =
      MakeReadFile(workspace, path, {{"chunk_bytes", 1024}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());

  const std::vector<std::string> chunks = ReadAll(action, "bytes");
  EXPECT_EQ(chunks.size(), 10u);  // 9 full, one of 784
  EXPECT_EQ(Concat(chunks), body);
}

TEST(ReadFileTest, SplitsLinesAndKeepsALastOneWithoutANewline) {
  Workspace workspace;
  const std::string path = workspace.Write("lines.txt", "one\ntwo\r\nthree");
  const std::shared_ptr<Action> action = MakeReadFile(workspace, path);
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());

  const std::vector<std::string> lines = ReadAll(action, "lines");
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(nlohmann::json::parse(lines[0], nullptr, false), "one");
  EXPECT_EQ(nlohmann::json::parse(lines[1], nullptr, false), "two");
  EXPECT_EQ(nlohmann::json::parse(lines[2], nullptr, false), "three");
}

TEST(ReadFileTest, ReadsAWindowWithOffsetAndLength) {
  Workspace workspace;
  const std::string path = workspace.Write("window.txt", "0123456789");
  const std::shared_ptr<Action> action =
      MakeReadFile(workspace, path, {{"offset", 3}, {"length", 4}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  EXPECT_EQ(Concat(ReadAll(action, "bytes")), "3456");
}

TEST(ReadFileTest, RefusesADirectoryWithAUsefulCode) {
  Workspace workspace;
  const std::shared_ptr<Action> action =
      MakeReadFile(workspace, workspace.root().string());
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  const absl::Status status = action->Wait(kPatience).Await().status();
  EXPECT_EQ(status.code(), absl::StatusCode::kFailedPrecondition);
}

TEST(ReadFileTest, ReportsAMissingFileAsNotFound) {
  Workspace workspace;
  const std::shared_ptr<Action> action =
      MakeReadFile(workspace, workspace.path("absent.txt"));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  const absl::Status status = action->Wait(kPatience).Await().status();
  EXPECT_EQ(status.code(), absl::StatusCode::kNotFound);
}

TEST(ReadFileTest, RefusesToReadPastTheByteLimit) {
  Workspace workspace;
  const std::string path = workspace.Write("big.bin", std::string(5000, 'y'));
  const std::shared_ptr<Action> action =
      MakeReadFile(workspace, path, {{"max_bytes", 100}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  const absl::Status status = action->Wait(kPatience).Await().status();
  EXPECT_EQ(status.code(), absl::StatusCode::kResourceExhausted);
}

TEST(ReadFileTest, OmittingAPortClosesItWithoutWaitingForAReader) {
  Workspace workspace;
  const std::string path = workspace.Write("hello.txt", "hello");
  const std::shared_ptr<Action> action = MakeReadFile(
      workspace, path, {{"omit", nlohmann::json::array({"bytes", "lines"})}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  // Nothing was written to them, and nothing had to be drained for the run to
  // finish -- which is the whole point of `omit`.
  EXPECT_TRUE(ReadAll(action, "bytes").empty());
  const std::optional<nlohmann::json> text = ReadOne(action, "text");
  ASSERT_TRUE(text.has_value());
  EXPECT_EQ(*text, "hello");
}

TEST(ReadFileTest, ReportsAMisspelledOmission) {
  Workspace workspace;
  const std::string path = workspace.Write("hello.txt", "hello");
  const std::shared_ptr<Action> action =
      MakeReadFile(workspace, path, {{"omit", "bites"}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  const absl::Status status = action->Wait(kPatience).Await().status();
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

// --- write_file --------------------------------------------------------------

std::shared_ptr<Action> MakeWriteFile(const Workspace& workspace,
                                      const std::string& path,
                                      const std::vector<std::string>& content,
                                      const nlohmann::json& options = {}) {
  absl::StatusOr<std::shared_ptr<Action>> created =
      Action::Create(WriteFileSchema(), a11::NewUuid(),
                     WriteFileHandler(WritableIn(workspace.root())));
  if (!created.ok()) {
    return nullptr;
  }
  const std::shared_ptr<Action>& action = *created;
  if (!PutInput(action, "path", path).ok()) {
    return nullptr;
  }
  if (!PutInput(action, "options",
                options.is_null() ? nlohmann::json::object() : options)
           .ok()) {
    return nullptr;
  }
  absl::StatusOr<std::shared_ptr<AsyncNode>> node = action->GetInput("content");
  if (!node.ok()) {
    return nullptr;
  }
  for (const std::string& piece : content) {
    data::Chunk chunk;
    chunk.metadata =
        data::ChunkMetadata{.mimetype = "application/octet-stream"};
    chunk.data = piece;
    if (!(*node)
             ->PutChunk(std::move(chunk), std::nullopt, false)
             .Await()
             .ok()) {
      return nullptr;
    }
  }
  if (!(*node)->Finalize({.wait = true, .close = false}).Await().ok()) {
    return nullptr;
  }
  return action;
}

TEST(WriteFileTest, WritesAStreamAndReportsWhatItWrote) {
  Workspace workspace;
  const std::string path = workspace.path("out.txt");
  const std::shared_ptr<Action> action =
      MakeWriteFile(workspace, path, {"one ", "two ", "three"});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());

  EXPECT_EQ(workspace.Read("out.txt"), "one two three");
  const std::optional<nlohmann::json> written =
      ReadOne(action, "bytes_written");
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(*written, 13);
  const std::optional<nlohmann::json> resolved = ReadOne(action, "resolved");
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->get<std::string>(), path);
}

TEST(WriteFileTest, LeavesNoTemporaryBehind) {
  Workspace workspace;
  const std::shared_ptr<Action> action =
      MakeWriteFile(workspace, workspace.path("out.txt"), {"body"});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());

  int entries = 0;
  std::error_code error;
  for (const fs::directory_entry& entry :
       fs::directory_iterator(workspace.root(), error)) {
    (void)entry;
    ++entries;
  }
  EXPECT_EQ(entries, 1);  // the file, and no `.out.txt.a11-...` beside it
}

TEST(WriteFileTest, AppendsWhenAskedTo) {
  Workspace workspace;
  workspace.Write("log.txt", "first\n");
  const std::shared_ptr<Action> action = MakeWriteFile(
      workspace, workspace.path("log.txt"), {"second\n"}, {{"append", true}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  EXPECT_EQ(workspace.Read("log.txt"), "first\nsecond\n");
}

TEST(WriteFileTest, RefusesToClaimAnAtomicAppend) {
  Workspace workspace;
  const std::shared_ptr<Action> action =
      MakeWriteFile(workspace, workspace.path("log.txt"), {"x"},
                    {{"append", true}, {"atomic", true}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  const absl::Status status = action->Wait(kPatience).Await().status();
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(WriteFileTest, RefusesAPathOutsideTheWorkspace) {
  Workspace workspace;
  const std::shared_ptr<Action> action =
      MakeWriteFile(workspace, "/tmp/a11-should-not-exist", {"x"});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  const absl::Status status = action->Wait(kPatience).Await().status();
  EXPECT_EQ(status.code(), absl::StatusCode::kPermissionDenied);
  EXPECT_FALSE(fs::exists("/tmp/a11-should-not-exist"));
}

TEST(WriteFileTest, ReadIntoWriteMovesAFileWithoutHoldingIt) {
  Workspace workspace;
  const std::string body(200000, 'z');
  workspace.Write("from.bin", body);

  const std::shared_ptr<Action> reader =
      MakeReadFile(workspace, workspace.path("from.bin"),
                   {{"chunk_bytes", 4096},
                    {"omit", nlohmann::json::array({"text", "lines"})}});
  ASSERT_NE(reader, nullptr);

  absl::StatusOr<std::shared_ptr<Action>> created =
      Action::Create(WriteFileSchema(), a11::NewUuid(),
                     WriteFileHandler(WritableIn(workspace.root())));
  ASSERT_TRUE(created.ok());
  const std::shared_ptr<Action>& writer = *created;
  ASSERT_TRUE(PutInput(writer, "path", workspace.path("to.bin")).ok());
  ASSERT_TRUE(PutInput(writer, "options", nlohmann::json::object()).ok());

  // The join a flow writes as `reader.bytes -> writer.content`, done by hand:
  // one chunk in flight at a time, and the writer's pace is the reader's.
  ASSERT_TRUE(reader->Run().ok());
  absl::StatusOr<std::shared_ptr<AsyncNode>> source =
      reader->GetOutput("bytes");
  ASSERT_TRUE(source.ok());
  absl::StatusOr<std::shared_ptr<AsyncNode>> sink = writer->GetInput("content");
  ASSERT_TRUE(sink.ok());
  ASSERT_TRUE(writer->Run().ok());
  while (true) {
    absl::StatusOr<std::optional<data::Chunk>> chunk =
        (*source)->NextChunk(kPatience).Await();
    ASSERT_TRUE(chunk.ok());
    if (!chunk->has_value() || (*chunk)->IsNull()) {
      break;
    }
    ASSERT_TRUE((*sink)
                    ->PutChunk(*std::move(*chunk), std::nullopt, false)
                    .Await()
                    .ok());
  }
  ASSERT_TRUE((*sink)->Finalize({.wait = true, .close = false}).Await().ok());
  ASSERT_TRUE(writer->Wait(kPatience).Await().ok());
  EXPECT_EQ(workspace.Read("to.bin"), body);
}

// --- list_directory ----------------------------------------------------------

std::shared_ptr<Action> MakeList(const Workspace& workspace,
                                 const std::string& path,
                                 const nlohmann::json& options = {}) {
  absl::StatusOr<std::shared_ptr<Action>> created =
      Action::Create(ListDirectorySchema(), a11::NewUuid(),
                     ListDirectoryHandler(WritableIn(workspace.root())));
  if (!created.ok()) {
    return nullptr;
  }
  if (!PutInput(*created, "path", path).ok()) {
    return nullptr;
  }
  if (!PutInput(*created, "options",
                options.is_null() ? nlohmann::json::object() : options)
           .ok()) {
    return nullptr;
  }
  return *created;
}

TEST(ListDirectoryTest, ReportsEntriesAndACount) {
  Workspace workspace;
  workspace.Write("a.txt", "a");
  workspace.Write("b.txt", "b");
  workspace.Write(".hidden", "h");

  const std::shared_ptr<Action> action =
      MakeList(workspace, workspace.root().string());
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());

  EXPECT_EQ(ReadAll(action, "entries").size(), 2u);  // the dot file is hidden
  const std::optional<nlohmann::json> count = ReadOne(action, "count");
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(*count, 2);
  const std::optional<nlohmann::json> truncated = ReadOne(action, "truncated");
  ASSERT_TRUE(truncated.has_value());
  EXPECT_FALSE(truncated->get<bool>());
}

TEST(ListDirectoryTest, MatchesNamesAgainstAPattern) {
  Workspace workspace;
  workspace.Write("one.txt", "1");
  workspace.Write("two.csv", "2");
  workspace.Write("three.txt", "3");

  const std::shared_ptr<Action> action =
      MakeList(workspace, workspace.root().string(), {{"match", "*.txt"}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  EXPECT_EQ(ReadAll(action, "entries").size(), 2u);
}

TEST(ListDirectoryTest, SaysWhenALimitCutTheListingShort) {
  Workspace workspace;
  for (int i = 0; i < 10; ++i) {
    workspace.Write(absl::StrCat("f", i, ".txt"), "x");
  }
  const std::shared_ptr<Action> action =
      MakeList(workspace, workspace.root().string(), {{"max_entries", 4}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());

  EXPECT_EQ(ReadAll(action, "entries").size(), 4u);
  const std::optional<nlohmann::json> truncated = ReadOne(action, "truncated");
  ASSERT_TRUE(truncated.has_value());
  EXPECT_TRUE(truncated->get<bool>());
}

TEST(ListDirectoryTest, WalksRecursivelyWhenAskedTo) {
  Workspace workspace;
  std::error_code error;
  fs::create_directories(workspace.root() / "nested" / "deeper", error);
  workspace.Write("top.txt", "t");
  std::ofstream((workspace.root() / "nested" / "mid.txt")) << "m";
  std::ofstream((workspace.root() / "nested" / "deeper" / "low.txt")) << "l";

  const std::shared_ptr<Action> action =
      MakeList(workspace, workspace.root().string(),
               {{"recursive", true}, {"kinds", "file"}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  EXPECT_EQ(ReadAll(action, "entries").size(), 3u);
}

// --- stat_path / directories -------------------------------------------------

TEST(StatPathTest, AMissingPathIsAnAnswerRatherThanAFailure) {
  Workspace workspace;
  absl::StatusOr<std::shared_ptr<Action>> created =
      Action::Create(StatPathSchema(), a11::NewUuid(),
                     StatPathHandler(WritableIn(workspace.root())));
  ASSERT_TRUE(created.ok());
  ASSERT_TRUE(PutInput(*created, "path", workspace.path("nope")).ok());
  ASSERT_TRUE(PutInput(*created, "options", nlohmann::json::object()).ok());
  ASSERT_TRUE((*created)->Run().ok());
  ASSERT_TRUE((*created)->Wait(kPatience).Await().ok());

  const std::optional<nlohmann::json> exists = ReadOne(*created, "exists");
  ASSERT_TRUE(exists.has_value());
  EXPECT_FALSE(exists->get<bool>());
}

TEST(RemovePathTest, RefusesADirectoryUnlessRecursiveIsAsked) {
  Workspace workspace;
  std::error_code error;
  fs::create_directories(workspace.root() / "tree" / "inner", error);

  absl::StatusOr<std::shared_ptr<Action>> created =
      Action::Create(RemovePathSchema(), a11::NewUuid(),
                     RemovePathHandler(WritableIn(workspace.root())));
  ASSERT_TRUE(created.ok());
  ASSERT_TRUE(PutInput(*created, "path", workspace.path("tree")).ok());
  ASSERT_TRUE(PutInput(*created, "options", nlohmann::json::object()).ok());
  ASSERT_TRUE((*created)->Run().ok());
  const absl::Status status = (*created)->Wait(kPatience).Await().status();
  EXPECT_EQ(status.code(), absl::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(fs::exists(workspace.root() / "tree"));
}

TEST(MakeTempTest, MakesScratchSpaceInsideTheFirstRoot) {
  Workspace workspace;
  absl::StatusOr<std::shared_ptr<Action>> created =
      Action::Create(MakeTempSchema(), a11::NewUuid(),
                     MakeTempHandler(WritableIn(workspace.root())));
  ASSERT_TRUE(created.ok());
  ASSERT_TRUE(PutInput(*created, "options", nlohmann::json::object()).ok());
  ASSERT_TRUE((*created)->Run().ok());
  ASSERT_TRUE((*created)->Wait(kPatience).Await().ok());

  const std::optional<nlohmann::json> path = ReadOne(*created, "path");
  ASSERT_TRUE(path.has_value());
  const fs::path made(path->get<std::string>());
  EXPECT_TRUE(fs::is_directory(made));
  // Not in the system temporary directory: the host named a root, and that is
  // where scratch space belongs.
  EXPECT_EQ(made.parent_path(), workspace.root());
}

// --- ticker ------------------------------------------------------------------

std::shared_ptr<Action> MakeTicker(const nlohmann::json& options) {
  absl::StatusOr<std::shared_ptr<Action>> created =
      Action::Create(TickerSchema(), a11::NewUuid(), TickerHandler());
  if (!created.ok()) {
    return nullptr;
  }
  if (!PutInput(*created, "options", options).ok()) {
    return nullptr;
  }
  return *created;
}

TEST(TickerTest, DeliversTheCountItWasAskedFor) {
  const std::shared_ptr<Action> action =
      MakeTicker({{"every", "10ms"}, {"count", 3}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());

  const std::vector<std::string> ticks = ReadAll(action, "ticks");
  ASSERT_EQ(ticks.size(), 3u);
  const nlohmann::json first = nlohmann::json::parse(ticks[0], nullptr, false);
  EXPECT_EQ(first.value("number", 0), 1);
  const std::optional<nlohmann::json> count = ReadOne(action, "count");
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(*count, 3);
}

TEST(TickerTest, TicksAtRoughlyTheIntervalItWasGiven) {
  const absl::Time started = absl::Now();
  const std::shared_ptr<Action> action =
      MakeTicker({{"every", "25ms"}, {"count", 4}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  ASSERT_EQ(ReadAll(action, "ticks").size(), 4u);
  // Four ticks at 25ms is 100ms of waiting, and the first one is not immediate.
  EXPECT_GE(absl::Now() - started, absl::Milliseconds(90));
}

TEST(TickerTest, StopsGracefullyRatherThanFailing) {
  const std::shared_ptr<Action> action =
      MakeTicker({{"every", "20ms"}});  // unbounded
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());

  absl::StatusOr<std::shared_ptr<AsyncNode>> control =
      action->GetInput(std::string(kControlPort));
  ASSERT_TRUE(control.ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  data::Chunk chunk;
  chunk.metadata = data::ChunkMetadata{.mimetype = "application/json"};
  chunk.data = nlohmann::json{{"command", "stop"}}.dump();
  ASSERT_TRUE(
      (*control)->PutChunk(std::move(chunk), std::nullopt, true).Await().ok());

  // A source asked to finish has finished: the stream ends, and the run is a
  // success rather than a cancellation.
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  EXPECT_FALSE(ReadAll(action, "ticks").empty());
}

TEST(TickerTest, EndsOnItsOwnDurationBound) {
  const std::shared_ptr<Action> action =
      MakeTicker({{"every", "10ms"}, {"for", "60ms"}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  const std::size_t ticks = ReadAll(action, "ticks").size();
  EXPECT_GE(ticks, 3u);
  EXPECT_LE(ticks, 7u);
}

TEST(TickerTest, DeliversOneImmediatelyWhenAsked) {
  const std::shared_ptr<Action> action =
      MakeTicker({{"every", "10s"}, {"count", 1}, {"immediate", true}});
  ASSERT_NE(action, nullptr);
  const absl::Time started = absl::Now();
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  EXPECT_EQ(ReadAll(action, "ticks").size(), 1u);
  EXPECT_LT(absl::Now() - started, absl::Seconds(5));
}

TEST(TickerTest, RefusesAnIntervalTooShortToBeAClock) {
  const std::shared_ptr<Action> action = MakeTicker({{"every", "1ns"}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  const absl::Status status = action->Wait(kPatience).Await().status();
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

// --- spawn_process -----------------------------------------------------------

CapabilitiesPtr CanRun(std::vector<std::string> programs) {
  CapabilitiesBuilder capabilities = SystemCapabilities();
  capabilities->process.any_program = programs.empty();
  capabilities->process.programs = std::move(programs);
  return capabilities;
}

std::shared_ptr<Action> MakeSpawn(const CapabilitiesPtr& capabilities,
                                  const std::string& program,
                                  const nlohmann::json& arguments = {},
                                  const nlohmann::json& options = {},
                                  const std::vector<std::string>& input = {}) {
  absl::StatusOr<std::shared_ptr<Action>> created = Action::Create(
      SpawnProcessSchema(), a11::NewUuid(), SpawnProcessHandler(capabilities));
  if (!created.ok()) {
    return nullptr;
  }
  const std::shared_ptr<Action>& action = *created;
  if (!PutInput(action, "program", program).ok()) {
    return nullptr;
  }
  if (!PutInput(action, "arguments",
                arguments.is_null() ? nlohmann::json::array() : arguments)
           .ok()) {
    return nullptr;
  }
  if (!PutInput(action, "options",
                options.is_null() ? nlohmann::json::object() : options)
           .ok()) {
    return nullptr;
  }
  absl::StatusOr<std::shared_ptr<AsyncNode>> stdin_node =
      action->GetInput("stdin");
  if (!stdin_node.ok()) {
    return nullptr;
  }
  for (const std::string& piece : input) {
    data::Chunk chunk;
    chunk.metadata =
        data::ChunkMetadata{.mimetype = "application/octet-stream"};
    chunk.data = piece;
    if (!(*stdin_node)
             ->PutChunk(std::move(chunk), std::nullopt, false)
             .Await()
             .ok()) {
      return nullptr;
    }
  }
  if (!(*stdin_node)->Finalize({.wait = true, .close = false}).Await().ok()) {
    return nullptr;
  }
  return action;
}

TEST(SpawnProcessTest, KeepsStandardOutputAndStandardErrorApart) {
  const std::shared_ptr<Action> action =
      MakeSpawn(CanRun({}), "sh",
                nlohmann::json::array({"-c", "echo to-out; echo to-err 1>&2"}));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());

  EXPECT_EQ(Concat(ReadAll(action, "stdout")), "to-out\n");
  EXPECT_EQ(Concat(ReadAll(action, "stderr")), "to-err\n");
  const std::vector<std::string> out_lines = ReadAll(action, "stdout_lines");
  ASSERT_EQ(out_lines.size(), 1u);
  EXPECT_EQ(nlohmann::json::parse(out_lines[0], nullptr, false), "to-out");
}

TEST(SpawnProcessTest, WritesThePidBeforeAnyOutput) {
  const std::shared_ptr<Action> action =
      MakeSpawn(CanRun({}), "sh", nlohmann::json::array({"-c", "echo hi"}));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  const std::optional<nlohmann::json> pid = ReadOne(action, "pid");
  ASSERT_TRUE(pid.has_value());
  EXPECT_GT(pid->get<std::int64_t>(), 0);
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
}

TEST(SpawnProcessTest, ANonZeroExitIsAnAnswerRatherThanAFailure) {
  const std::shared_ptr<Action> action =
      MakeSpawn(CanRun({}), "sh", nlohmann::json::array({"-c", "exit 3"}));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  // The program ran and this is what it said, exactly as a 404 is a response.
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  const std::optional<nlohmann::json> code = ReadOne(action, "exit_code");
  ASSERT_TRUE(code.has_value());
  EXPECT_EQ(*code, 3);
}

TEST(SpawnProcessTest, FailsWhenTheProgramIsNotThere) {
  const std::shared_ptr<Action> action =
      MakeSpawn(CanRun({}), "a11-no-such-program-exists");
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  // not_found rather than an exit code of 127 the caller has to decode.
  const absl::Status status = action->Wait(kPatience).Await().status();
  EXPECT_EQ(status.code(), absl::StatusCode::kNotFound);
}

TEST(SpawnProcessTest, FeedsStandardInputFromAStream) {
  const std::shared_ptr<Action> action =
      MakeSpawn(CanRun({}), "cat", {}, {}, {"one ", "two ", "three"});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  EXPECT_EQ(Concat(ReadAll(action, "stdout")), "one two three");
}

TEST(SpawnProcessTest, AProgramThatStopsReadingIsNotAnError) {
  // `head -1` of a long stream is a correct program, and the SIGPIPE it causes
  // must not take this process down with it.
  std::vector<std::string> lots;
  lots.reserve(200);
  for (int i = 0; i < 200; ++i) {
    lots.push_back(absl::StrCat("line ", i, "\n"));
  }
  const std::shared_ptr<Action> action =
      MakeSpawn(CanRun({}), "head", nlohmann::json::array({"-1"}), {}, lots);
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  EXPECT_EQ(Concat(ReadAll(action, "stdout")), "line 0\n");
}

TEST(SpawnProcessTest, RefusesAProgramThePolicyDoesNotName) {
  const std::shared_ptr<Action> action =
      MakeSpawn(CanRun({"echo"}), "rm", nlohmann::json::array({"-rf", "/"}));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  const absl::Status status = action->Wait(kPatience).Await().status();
  EXPECT_EQ(status.code(), absl::StatusCode::kPermissionDenied);
}

TEST(SpawnProcessTest, PassesArgumentsWithoutSplittingThem) {
  const std::shared_ptr<Action> action = MakeSpawn(
      CanRun({}), "echo", nlohmann::json::array({"one two", "three"}));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  // Two arguments, so one space between them from echo -- not three words.
  EXPECT_EQ(Concat(ReadAll(action, "stdout")), "one two three\n");
}

TEST(SpawnProcessTest, SetsTheEnvironmentItWasGiven) {
  const std::shared_ptr<Action> action = MakeSpawn(
      CanRun({}), "sh", nlohmann::json::array({"-c", "echo $A11_TEST"}),
      {{"environment", {{"A11_TEST", "present"}}}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  EXPECT_EQ(Concat(ReadAll(action, "stdout")), "present\n");
}

TEST(SpawnProcessTest, StopsALongRunningProcessAndSaysHow) {
  const std::shared_ptr<Action> action =
      MakeSpawn(CanRun({}), "sleep", nlohmann::json::array({"120"}),
                {{"grace", "200ms"}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(ReadOne(action, "pid").has_value());  // it has started

  absl::StatusOr<std::shared_ptr<AsyncNode>> control =
      action->GetInput(std::string(kControlPort));
  ASSERT_TRUE(control.ok());
  data::Chunk chunk;
  chunk.metadata = data::ChunkMetadata{.mimetype = "application/json"};
  chunk.data = nlohmann::json{{"command", "stop"}}.dump();
  ASSERT_TRUE(
      (*control)->PutChunk(std::move(chunk), std::nullopt, true).Await().ok());

  const absl::Status status = action->Wait(kPatience).Await().status();
  // Stopped on purpose, so `cancelled` -- and `signal` says which signal did it
  // rather than leaving the caller to decode an exit code of 143.
  EXPECT_EQ(status.code(), absl::StatusCode::kCancelled);
}

TEST(SpawnProcessTest, RunsInTheWorkingDirectoryItWasGiven) {
  Workspace workspace;
  workspace.Write("marker.txt", "here");
  CapabilitiesBuilder capabilities = SystemCapabilities();
  capabilities->filesystem.unrestricted = true;
  const std::shared_ptr<Action> action =
      MakeSpawn(capabilities, "ls", {}, {{"cwd", workspace.root().string()}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  EXPECT_EQ(Concat(ReadAll(action, "stdout")), "marker.txt\n");
}

TEST(SpawnProcessTest, RefusesAWorkingDirectoryOutsideThePolicysRoots) {
  Workspace workspace;
  CapabilitiesBuilder capabilities =
      WorkspaceCapabilities({workspace.root().string()});
  capabilities->process.enabled = true;
  capabilities->process.any_program = true;
  const std::shared_ptr<Action> action =
      MakeSpawn(capabilities, "ls", {}, {{"cwd", "/etc"}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  const absl::Status status = action->Wait(kPatience).Await().status();
  EXPECT_EQ(status.code(), absl::StatusCode::kPermissionDenied);
}

TEST(SpawnProcessTest, ReportsUsageOnceItHasFinished) {
  const std::shared_ptr<Action> action =
      MakeSpawn(CanRun({}), "sh", nlohmann::json::array({"-c", "echo x"}));
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  const std::optional<nlohmann::json> usage = ReadOne(action, "usage");
  ASSERT_TRUE(usage.has_value());
  EXPECT_TRUE(usage->contains("user_ms"));
  EXPECT_EQ(usage->value("output_bytes", -1), 2);
}

TEST(SpawnProcessTest, RefusesToRegisterWithAPolicyThatAllowsNothing) {
  ActionRegistry registry;
  const CapabilitiesBuilder capabilities = std::make_shared<Capabilities>();
  EXPECT_FALSE(RegisterProcessActions(registry, capabilities).ok());
  capabilities->process.enabled = true;
  // Enabled but naming no programs: nothing could ever run, which is a mistake
  // worth reporting where it was made.
  EXPECT_FALSE(RegisterProcessActions(registry, capabilities).ok());
}

// --- the sandbox -------------------------------------------------------------

TEST(SandboxTest, SaysWhatTheRunningSystemCanDo) {
  const SandboxAvailability& available = Availability();
  if (available.kind == SandboxKind::kNone) {
    EXPECT_FALSE(available.why_not.empty())
        << "an unavailable sandbox has to say why";
  } else {
    EXPECT_TRUE(available.why_not.empty());
  }
}

TEST(SandboxTest, RefusesToPrepareWhenRequiredAndThereAreNoRoots) {
  CapabilitiesBuilder capabilities = SystemCapabilities();
  capabilities->process.sandbox = SandboxRequest::kRequired;
  // Unrestricted, so there is nothing to confine to. A sandbox that allows
  // everything must not be reported as a sandbox.
  EXPECT_FALSE(Sandbox::Prepare(*capabilities, "/bin/ls").ok());
}

TEST(SandboxTest, PreferredCarriesOnWhereThereIsNothingToConfine) {
  CapabilitiesBuilder capabilities = SystemCapabilities();
  capabilities->process.sandbox = SandboxRequest::kPreferred;
  absl::StatusOr<std::shared_ptr<Sandbox>> prepared =
      Sandbox::Prepare(*capabilities, "/bin/ls");
  ASSERT_TRUE(prepared.ok());
  EXPECT_FALSE((*prepared)->active());
  EXPECT_FALSE((*prepared)->Describe().empty());
}

TEST(SandboxTest, PreparesConfinementForARootedPolicy) {
  Workspace workspace;
  CapabilitiesBuilder capabilities =
      WorkspaceCapabilities({workspace.root().string()});
  capabilities->process.enabled = true;
  capabilities->process.any_program = true;
  absl::StatusOr<std::shared_ptr<Sandbox>> prepared =
      Sandbox::Prepare(*capabilities, "/bin/cat");
  ASSERT_TRUE(prepared.ok());
  if (Availability().kind == SandboxKind::kNone) {
    GTEST_SKIP() << "no sandbox on this system: " << Availability().why_not;
  }
  EXPECT_TRUE((*prepared)->active());
}

/// A workspace-rooted policy that runs programs under required confinement.
CapabilitiesBuilder ConfinedIn(const fs::path& root) {
  CapabilitiesBuilder capabilities = WorkspaceCapabilities({root.string()});
  capabilities->process.enabled = true;
  capabilities->process.any_program = true;
  capabilities->process.sandbox = SandboxRequest::kRequired;
  return capabilities;
}

TEST(SandboxTest, TheKernelStopsAWriteThePolicyOnlyChecked) {
  // The point of the whole file: a spawned program makes its own syscalls, and
  // no check in policy.cc is between it and the rest of the filesystem. A write
  // rather than a read, because writes are the part both platforms confine --
  // see SandboxAvailability.
  if (!Availability().confines_writes) {
    GTEST_SKIP() << "no write confinement here: " << Availability().why_not;
  }
  Workspace workspace;
  Workspace elsewhere;
  const std::shared_ptr<Action> reaching_out =
      MakeSpawn(ConfinedIn(workspace.root()), "/usr/bin/touch",
                nlohmann::json::array({elsewhere.path("escaped.txt")}));
  ASSERT_NE(reaching_out, nullptr);
  ASSERT_TRUE(reaching_out->Run().ok());
  ASSERT_TRUE(reaching_out->Wait(kPatience).Await().ok());

  // The program ran; the kernel refused. A non-zero exit and no such file.
  const std::optional<nlohmann::json> code = ReadOne(reaching_out, "exit_code");
  ASSERT_TRUE(code.has_value());
  EXPECT_NE(*code, 0);
  EXPECT_FALSE(fs::exists(elsewhere.path("escaped.txt")));

  const std::optional<nlohmann::json> describe =
      ReadOne(reaching_out, "sandbox");
  ASSERT_TRUE(describe.has_value());
  EXPECT_FALSE(describe->get<std::string>().empty());
}

TEST(SandboxTest, AWriteInsideTheRootStillWorks) {
  if (!Availability().confines_writes) {
    GTEST_SKIP() << "no write confinement here: " << Availability().why_not;
  }
  Workspace workspace;
  const std::shared_ptr<Action> inside =
      MakeSpawn(ConfinedIn(workspace.root()), "/usr/bin/touch",
                nlohmann::json::array({workspace.path("made.txt")}));
  ASSERT_NE(inside, nullptr);
  ASSERT_TRUE(inside->Run().ok());
  ASSERT_TRUE(inside->Wait(kPatience).Await().ok());
  const std::optional<nlohmann::json> code = ReadOne(inside, "exit_code");
  ASSERT_TRUE(code.has_value());
  EXPECT_EQ(*code, 0);
  EXPECT_TRUE(fs::exists(workspace.path("made.txt")));
}

TEST(SandboxTest, StopsAReadOutsideTheRootWhereThePlatformCan) {
  // Linux confines reads; macOS as this library builds it does not, and says
  // so. The test asserts whichever of those is true here rather than asserting
  // the one that would be nicer.
  if (Availability().kind == SandboxKind::kNone) {
    GTEST_SKIP() << "no sandbox here: " << Availability().why_not;
  }
  Workspace workspace;
  const std::shared_ptr<Action> reading_out =
      MakeSpawn(ConfinedIn(workspace.root()), "/bin/cat",
                nlohmann::json::array({"/etc/passwd"}));
  ASSERT_NE(reading_out, nullptr);
  ASSERT_TRUE(reading_out->Run().ok());
  ASSERT_TRUE(reading_out->Wait(kPatience).Await().ok());
  const std::string got = Concat(ReadAll(reading_out, "stdout"));
  if (Availability().confines_reads) {
    EXPECT_TRUE(got.empty());
  } else {
    // Documented limitation, pinned by a test so that it cannot quietly become
    // an undocumented one.
    EXPECT_FALSE(got.empty())
        << "reads are reported as unconfined; if this platform started "
           "confining them, update SandboxAvailability rather than this test";
    const std::optional<nlohmann::json> describe =
        ReadOne(reading_out, "sandbox");
    ASSERT_TRUE(describe.has_value());
    EXPECT_NE(describe->get<std::string>().find("reads NOT confined"),
              std::string::npos)
        << "an unconfined read has to be visible to whoever reads the port";
  }
}

TEST(SandboxTest, ConfinementDoesNotStopTheProgramDoingItsJob) {
  // A sandbox that stops the program from working is not a useful sandbox, and
  // this is the half of the claim that is easy to break while tightening the
  // other half.
  if (Availability().kind == SandboxKind::kNone) {
    GTEST_SKIP() << "no sandbox on this system: " << Availability().why_not;
  }
  Workspace workspace;
  workspace.Write("mine.txt", "readable");
  const std::shared_ptr<Action> inside =
      MakeSpawn(ConfinedIn(workspace.root()), "/bin/cat",
                nlohmann::json::array({workspace.path("mine.txt")}));
  ASSERT_NE(inside, nullptr);
  ASSERT_TRUE(inside->Run().ok());
  ASSERT_TRUE(inside->Wait(kPatience).Await().ok());
  EXPECT_EQ(Concat(ReadAll(inside, "stdout")), "readable");
}

// --- encodings ---------------------------------------------------------------

TEST(EncodingTest, ReadsMsgpackOnAnInputPort) {
  // A producer upstream may have chosen MessagePack, and `| packb` puts it in
  // front of any port. Reading one as a text payload would hand this action a
  // string of packed bytes and call it a path.
  Workspace workspace;
  const std::string path = workspace.Write("packed.txt", "by msgpack");
  absl::StatusOr<std::shared_ptr<Action>> created =
      Action::Create(ReadFileSchema(), a11::NewUuid(),
                     ReadFileHandler(WritableIn(workspace.root())));
  ASSERT_TRUE(created.ok());
  const std::shared_ptr<Action>& action = *created;

  absl::StatusOr<std::shared_ptr<AsyncNode>> node = action->GetInput("path");
  ASSERT_TRUE(node.ok());
  absl::StatusOr<std::string> packed = a11::PackMsgpack(path, "a test path");
  ASSERT_TRUE(packed.ok());
  data::Chunk chunk;
  chunk.metadata =
      data::ChunkMetadata{.mimetype = std::string(data::kMsgpackMimetype)};
  chunk.data = *packed;
  ASSERT_TRUE(
      (*node)->PutChunk(std::move(chunk), std::nullopt, true).Await().ok());
  ASSERT_TRUE(PutInput(action, "options", nlohmann::json::object()).ok());

  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());
  EXPECT_EQ(Concat(ReadAll(action, "bytes")), "by msgpack");
}

TEST(EncodingTest, WritesMsgpackWhenAsked) {
  Workspace workspace;
  workspace.Write("one.txt", "1");
  const std::shared_ptr<Action> action =
      MakeList(workspace, workspace.root().string(), {{"encoding", "msgpack"}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(kPatience).Await().ok());

  const std::vector<std::string> entries = ReadAll(action, "entries");
  ASSERT_EQ(entries.size(), 1u);
  // Packed, and it unpacks back into the same record.
  absl::StatusOr<nlohmann::json> unpacked =
      a11::UnpackMsgpack(entries.front(), "an entry");
  ASSERT_TRUE(unpacked.ok());
  EXPECT_EQ(unpacked->value("name", std::string()), "one.txt");
}

TEST(EncodingTest, RefusesAnUnknownEncoding) {
  Workspace workspace;
  const std::shared_ptr<Action> action =
      MakeList(workspace, workspace.root().string(), {{"encoding", "cbor"}});
  ASSERT_NE(action, nullptr);
  ASSERT_TRUE(action->Run().ok());
  EXPECT_EQ(action->Wait(kPatience).Await().status().code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(EncodingTest, ANonUtf8NameIsAStatusUnderJsonAndAValueUnderMsgpack) {
  // A filename is a byte string, and nothing obliges it to be UTF-8. JSON has
  // no spelling for one that is not, so the two honest answers are to fail and
  // to offer an encoding that can hold it. Aborting -- which is what nlohmann
  // does under -fno-exceptions -- is not among them.
  Workspace workspace;
  const std::string awkward = workspace.path("bad-\xff\xfe-name.txt");
  {
    std::ofstream out(awkward, std::ios::binary);
    if (!out) {
      GTEST_SKIP() << "this filesystem will not take that name";
    }
    out << "x";
  }

  const std::shared_ptr<Action> as_json =
      MakeList(workspace, workspace.root().string(), {});
  ASSERT_NE(as_json, nullptr);
  ASSERT_TRUE(as_json->Run().ok());
  const absl::Status refused = as_json->Wait(kPatience).Await().status();
  EXPECT_EQ(refused.code(), absl::StatusCode::kInvalidArgument);
  // And it says how to get the answer, rather than only that it will not.
  EXPECT_NE(refused.message().find("msgpack"), std::string::npos);

  const std::shared_ptr<Action> as_msgpack =
      MakeList(workspace, workspace.root().string(), {{"encoding", "msgpack"}});
  ASSERT_NE(as_msgpack, nullptr);
  ASSERT_TRUE(as_msgpack->Run().ok());
  ASSERT_TRUE(as_msgpack->Wait(kPatience).Await().ok());
  EXPECT_EQ(ReadAll(as_msgpack, "entries").size(), 1u);
}

// --- env_get / randomness ----------------------------------------------------

TEST(EnvGetTest, ReadsOnlyWhatThePolicyNames) {
  const CapabilitiesBuilder capabilities = std::make_shared<Capabilities>();
  capabilities->environment.names = {"PATH"};

  const auto ask = [&capabilities](const nlohmann::json& names) {
    absl::StatusOr<std::shared_ptr<Action>> created = Action::Create(
        EnvGetSchema(), a11::NewUuid(), EnvGetHandler(capabilities));
    if (!created.ok() || !PutInput(*created, "names", names).ok() ||
        !PutInput(*created, "options", nlohmann::json::object()).ok()) {
      return std::shared_ptr<Action>();
    }
    return *created;
  };

  const std::shared_ptr<Action> allowed = ask("PATH");
  ASSERT_NE(allowed, nullptr);
  ASSERT_TRUE(allowed->Run().ok());
  ASSERT_TRUE(allowed->Wait(kPatience).Await().ok());
  const std::optional<nlohmann::json> values = ReadOne(allowed, "values");
  ASSERT_TRUE(values.has_value());
  EXPECT_TRUE(values->contains("PATH"));

  const std::shared_ptr<Action> refused = ask("AWS_SECRET_ACCESS_KEY");
  ASSERT_NE(refused, nullptr);
  ASSERT_TRUE(refused->Run().ok());
  EXPECT_EQ(refused->Wait(kPatience).Await().status().code(),
            absl::StatusCode::kPermissionDenied);
}

TEST(EnvGetTest, ThereIsNoWayToAskForTheWholeEnvironment) {
  const CapabilitiesBuilder capabilities = std::make_shared<Capabilities>();
  capabilities->environment.any_name = true;
  absl::StatusOr<std::shared_ptr<Action>> created = Action::Create(
      EnvGetSchema(), a11::NewUuid(), EnvGetHandler(capabilities));
  ASSERT_TRUE(created.ok());
  ASSERT_TRUE(PutInput(*created, "names", nlohmann::json()).ok());
  ASSERT_TRUE(PutInput(*created, "options", nlohmann::json::object()).ok());
  ASSERT_TRUE((*created)->Run().ok());
  EXPECT_EQ((*created)->Wait(kPatience).Await().status().code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(EnvGetTest, SaysWhichNamesWereNotSetAtAll) {
  const CapabilitiesBuilder capabilities = std::make_shared<Capabilities>();
  capabilities->environment.any_name = true;
  absl::StatusOr<std::shared_ptr<Action>> created = Action::Create(
      EnvGetSchema(), a11::NewUuid(), EnvGetHandler(capabilities));
  ASSERT_TRUE(created.ok());
  ASSERT_TRUE(PutInput(*created, "names",
                       nlohmann::json::array({"PATH", "A11_NOT_SET_ANYWHERE"}))
                  .ok());
  ASSERT_TRUE(PutInput(*created, "options", nlohmann::json::object()).ok());
  ASSERT_TRUE((*created)->Run().ok());
  ASSERT_TRUE((*created)->Wait(kPatience).Await().ok());
  const std::optional<nlohmann::json> missing = ReadOne(*created, "missing");
  ASSERT_TRUE(missing.has_value());
  ASSERT_EQ(missing->size(), 1u);
  EXPECT_EQ((*missing)[0], "A11_NOT_SET_ANYWHERE");
}

TEST(RandomBytesTest, DrawsTheCountAndEncodingItWasAsked) {
  absl::StatusOr<std::shared_ptr<Action>> created =
      Action::Create(RandomBytesSchema(), a11::NewUuid(), RandomBytesHandler());
  ASSERT_TRUE(created.ok());
  ASSERT_TRUE(PutInput(*created, "options",
                       nlohmann::json{{"count", 16}, {"format", "hex"}})
                  .ok());
  ASSERT_TRUE((*created)->Run().ok());
  ASSERT_TRUE((*created)->Wait(kPatience).Await().ok());

  EXPECT_EQ(Concat(ReadAll(*created, "bytes")).size(), 16u);
  const std::optional<nlohmann::json> text = ReadOne(*created, "text");
  ASSERT_TRUE(text.has_value());
  EXPECT_EQ(text->get<std::string>().size(), 32u);  // two hex digits a byte
}

TEST(NewUuidTest, MakesAsManyAsAsked) {
  absl::StatusOr<std::shared_ptr<Action>> created =
      Action::Create(NewUuidSchema(), a11::NewUuid(), NewUuidHandler());
  ASSERT_TRUE(created.ok());
  ASSERT_TRUE(PutInput(*created, "options", nlohmann::json{{"count", 5}}).ok());
  ASSERT_TRUE((*created)->Run().ok());
  ASSERT_TRUE((*created)->Wait(kPatience).Await().ok());
  EXPECT_EQ(ReadAll(*created, "ids").size(), 5u);
}

TEST(WriteStdoutTest, TakesAStreamAndCountsWhatItWrote) {
  // Writing to the real standard output, which under a test runner is a pipe
  // or the terminal; either way the byte count is what is being checked.
  absl::StatusOr<std::shared_ptr<Action>> created =
      Action::Create(WriteStderrSchema(), a11::NewUuid(), WriteStderrHandler());
  ASSERT_TRUE(created.ok());
  absl::StatusOr<std::shared_ptr<AsyncNode>> content =
      (*created)->GetInput("content");
  ASSERT_TRUE(content.ok());
  data::Chunk chunk;
  chunk.metadata = data::ChunkMetadata{.mimetype = "application/octet-stream"};
  chunk.data = "";  // nothing visible in the test output, and still a write
  ASSERT_TRUE(
      (*content)->PutChunk(std::move(chunk), std::nullopt, false).Await().ok());
  ASSERT_TRUE(
      (*content)->Finalize({.wait = true, .close = false}).Await().ok());
  ASSERT_TRUE((*created)->Run().ok());
  ASSERT_TRUE((*created)->Wait(kPatience).Await().ok());
  const std::optional<nlohmann::json> written =
      ReadOne(*created, "bytes_written");
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(*written, 0);
}

// --- sleep_for ---------------------------------------------------------------

TEST(SleepTest, WaitsAndSaysItRanItsCourse) {
  absl::StatusOr<std::shared_ptr<Action>> created =
      Action::Create(SleepSchema(), a11::NewUuid(), SleepHandler());
  ASSERT_TRUE(created.ok());
  ASSERT_TRUE(PutInput(*created, "duration", "30ms").ok());
  const absl::Time started = absl::Now();
  ASSERT_TRUE((*created)->Run().ok());
  ASSERT_TRUE((*created)->Wait(kPatience).Await().ok());
  EXPECT_GE(absl::Now() - started, absl::Milliseconds(25));

  const std::optional<nlohmann::json> woke = ReadOne(*created, "woke");
  ASSERT_TRUE(woke.has_value());
  EXPECT_FALSE(woke->value("interrupted", true));
}

}  // namespace
}  // namespace a11::sdk::flow
