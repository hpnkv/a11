// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Remote calls reusing one connection, which a caller can silently
 * break.
 *
 * A caller binds its ports to the wire stream, and `bind_stream` is not
 * symmetric
 * between the two directions -- getting it wrong on an *output* is accepted,
 * works
 * for exactly one call, and then breaks the connection.
 *
 * An **input** on the calling side must be bound: that is how what the caller
 * writes reaches the peer. An **output** must not be. The session already
 * routes inbound fragments to the node by id, and a bound output node tees
 * what it receives straight back to the peer -- so the caller echoes each
 * reply, and the connection is corrupted for every later call on it. Binding
 * both was what made `ServerSuite` in cpp/bench/bench_main.cc lose 1-3% of its
 * calls, and made those losses look like a lost wake-up: the reply was written
 * and delivered, and the read never returned.
 *
 * The failure is worth a test because of how it presents. It is not an error at
 * the point of the mistake; the first call succeeds, and what fails is a later
 * call on the same connection, as either a read that never returns or
 * `FAILED_PRECONDITION: The opposite side has aborted the stream`. Over
 * `InProcessWireStream` it is close to deterministic -- 48 of 64 connections
 * before the fix -- which is why these run over one rather than a socket: no
 * HTTP
 * framing and no libuv loop between the mistake and the symptom.
 *
 * `A11_WEDGE_CONNECTIONS`, `A11_WEDGE_CALLS` and `A11_WEDGE_BARE_SESSION` vary
 * the shape; the defaults are what reproduced it.
 */

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/concurrency/executor.h"
#include "a11/data/types.h"
#include "a11/net/in_process_wire_stream.h"
#include "a11/nodes/async_node.h"
#include "a11/service/service.h"
#include "a11/service/session.h"

namespace a11::service {
namespace {

/// Long enough not to mistake a loaded scheduler for a wedge, short enough
/// that a wedge is reported rather than waited on.
constexpr absl::Duration kCallDeadline = absl::Seconds(15);

// Each call stage gets a deadline shorter than the driver's, so a wedge is
// reported as the stage that wedged.
/// Each call stage gets a deadline shorter than the driver's, so a wedge is
/// reported as the stage that wedged. With both the same, the driver's own
/// await
/// expires first and every failure reads DEADLINE_EXCEEDED with nothing to say
/// which of call / put-input / read-output / wait was stuck.
constexpr absl::Duration kStageDeadline = absl::Seconds(3);

actions::ActionSchema EchoSchema() {
  return actions::ActionSchema{
      .name = "echo",
      .inputs = {{"input",
                  actions::ActionPortSchema{
                      .name = "input", .type = "application/octet-stream"}}},
      .outputs = {{"output",
                   actions::ActionPortSchema{
                       .name = "output", .type = "application/octet-stream"}}},
  };
}

actions::ActionHandler EchoHandler() {
  return [](std::shared_ptr<actions::Action> action) {
    return a11::SubmitTask([action = std::move(action)]() -> absl::Status {
      absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> input =
          action->GetInput("input");
      if (!input.ok()) {
        return input.status();
      }
      absl::StatusOr<std::optional<data::NodeFragment>> fragment =
          (*input)->NextFragment().Await();
      if (!fragment.ok()) {
        return fragment.status();
      }
      if (!fragment->has_value()) {
        return absl::FailedPreconditionError("echo input ended early");
      }
      absl::StatusOr<const data::Chunk*> chunk = (*fragment)->GetChunk();
      if (!chunk.ok()) {
        return chunk.status();
      }
      absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> output =
          action->GetOutput("output");
      if (!output.ok()) {
        return output.status();
      }
      return (*output)->PutChunk(**chunk, std::nullopt, true).Await().status();
    });
  };
}

/**
 * @brief A client session and a server session joined by an in-process pair.
 */
struct Peers {
  std::shared_ptr<Session> client;
  std::shared_ptr<Session> server;
  /// Held so the service outlives the calls it is serving; null in bare mode.
  std::shared_ptr<Service> service;
  net::InProcessWireStream::Pair pair;
};

absl::StatusOr<Peers> ConnectedPeers(size_t index) {
  auto registry = std::make_shared<actions::ActionRegistry>();
  ABSL_RETURN_IF_ERROR(
      registry->Register(EchoSchema().name, EchoSchema(), EchoHandler()));

  // A11_WEDGE_BARE_SESSION=1 accepts on a bare Session instead, which is how
  // this test was first written.
  const char* bare = std::getenv("A11_WEDGE_BARE_SESSION");
  const bool use_bare_session = bare != nullptr && *bare == '1';
  std::shared_ptr<Session> server;
  std::shared_ptr<Service> service;
  if (use_bare_session) {
    ABSL_ASSIGN_OR_RETURN(
        server, Session::Create(absl::StrCat("server-", index), {}, {}, {}, {},
                                nullptr, registry));
  } else {
    ABSL_ASSIGN_OR_RETURN(service, Service::Create(registry));
  }
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<Session> client,
                        Session::Create(absl::StrCat("client-", index)));
  ABSL_ASSIGN_OR_RETURN(net::InProcessWireStream::Pair pair,
                        net::InProcessWireStream::CreatePair());
  ABSL_ASSIGN_OR_RETURN(a11::Task started,
                        client->AddStream(pair.first, StreamMode::kStart));
  if (use_bare_session) {
    ABSL_ASSIGN_OR_RETURN(a11::Task accepted,
                          server->AddStream(pair.second, StreamMode::kAccept));
    ABSL_RETURN_IF_ERROR(accepted.Await(absl::Now() + kCallDeadline).status());
  } else {
    ABSL_ASSIGN_OR_RETURN(server, service->StartStreamHandler(pair.second));
  }
  ABSL_RETURN_IF_ERROR(started.Await(absl::Now() + kCallDeadline).status());
  return Peers{.client = std::move(client),
               .server = std::move(server),
               .service = std::move(service),
               .pair = std::move(pair)};
}

/**
 * @brief One echo call on a connected pair, exactly as the bench drives it.
 *
 * The output uses the default infinite timeout to reproduce the
 * read the bench loses, and a finite one would paper over the fault by
 * re-driving
 * the pump when its timer fires.
 */
/// Labels a stage's failure, so a timeout names what wedged rather than the
/// harness. Without this every failure reads DEADLINE_EXCEEDED.
absl::Status Stage(std::string_view name, absl::Status status) {
  if (status.ok()) {
    return status;
  }
  return {status.code(), absl::StrCat("stage=", name, " ", status.message())};
}

absl::Status OneEchoCall(const Peers& peers, int round) {
  // An empty action id, so each call gets a generated one. A literal makes
  // every
  // call share an instance id and their port nodes collide in the node map.
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<actions::Action> call,
      actions::Action::Create(EchoSchema(), /*action_id=*/""));
  ABSL_RETURN_IF_ERROR(call->BindNodeMap(peers.client->GetNodeMap()));
  ABSL_RETURN_IF_ERROR(call->BindSession(peers.client));
  ABSL_RETURN_IF_ERROR(call->BindStream(peers.pair.first));
  ABSL_RETURN_IF_ERROR(
      Stage(absl::StrCat("call/round", round),
            call->Call().Await(absl::Now() + kStageDeadline).status()));

  // Omit bind_stream to exercise dispatch_stream alone.
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<nodes::AsyncNode> input,
                        call->GetInput("input"));
  ABSL_RETURN_IF_ERROR(
      Stage(absl::StrCat("put-input/round", round),
            input
                ->PutChunk(
                    data::Chunk{.metadata =
                                    data::ChunkMetadata{
                                        .mimetype = "application/octet-stream"},
                                .data = "ping"},
                    std::nullopt, true)
                .Await(absl::Now() + kStageDeadline)
                .status()));
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<nodes::AsyncNode> output,
                        call->GetOutput("output"));
  const absl::StatusOr<std::optional<data::Chunk>> read =
      output->NextChunk().Await(absl::Now() + kStageDeadline);
  ABSL_RETURN_IF_ERROR(
      Stage(absl::StrCat("read-output/round", round), read.status()));
  const std::optional<data::Chunk>& reply = *read;
  if (!reply.has_value()) {
    return absl::DataLossError("the reply ended before a chunk arrived");
  }
  return Stage(absl::StrCat("wait/round", round),
               call->Wait().Await(absl::Now() + kStageDeadline).status());
}

TEST(RemoteCallWedgeTest, OneCallOverAnInProcessPairCompletes) {
  absl::StatusOr<Peers> peers = ConnectedPeers(0);
  ASSERT_TRUE(peers.ok()) << peers.status();
  EXPECT_TRUE(OneEchoCall(*peers, 0).ok());
  (void)peers->pair.first->Abort(absl::CancelledError("test over"));
  (void)peers->pair.second->Abort(absl::CancelledError("test over"));
}

/**
 * @brief Many connections, each running a sequence of calls at the same time.
 *
 * One connection per client with several calls each is the bench's shape, and
 * the fraction it loses grows with the client count -- so this drives enough
 * of both to have a chance of catching it.
 */
TEST(RemoteCallWedgeTest, ConcurrentCallsAcrossConnectionsAllComplete) {
  // Overridable so the threshold can be swept: a failure that appears sharply
  // at a particular count is a resource cap, while one that fades in gradually
  // is a race.
  const auto knob = [](const char* name, int fallback) {
    const char* value = std::getenv(name);
    int parsed = 0;
    return value != nullptr && absl::SimpleAtoi(value, &parsed) ? parsed
                                                                : fallback;
  };
  const auto kConnections =
      static_cast<size_t>(knob("A11_WEDGE_CONNECTIONS", 64));
  const int kCallsEach = knob("A11_WEDGE_CALLS", 8);

  std::vector<Peers> connections;
  connections.reserve(kConnections);
  for (size_t index = 0; index < kConnections; ++index) {
    absl::StatusOr<Peers> peers = ConnectedPeers(index);
    ASSERT_TRUE(peers.ok()) << "connection " << index << ": " << peers.status();
    connections.push_back(std::move(*peers));
  }

  std::vector<a11::Task> drivers;
  drivers.reserve(kConnections);
  for (const Peers& peers : connections) {
    drivers.push_back(a11::SubmitTask(
        [&peers, kCallsEach]() -> absl::Status {
          for (int round = 0; round < kCallsEach; ++round) {
            ABSL_RETURN_IF_ERROR(OneEchoCall(peers, round));
          }
          return absl::OkStatus();
        },
        thread::TreeOptions{.stack_size = 512 * 1024}));
  }

  std::vector<std::string> failures;
  for (a11::Task& driver : drivers) {
    const absl::Status result =
        driver.Await(absl::Now() + kCallDeadline).status();
    if (!result.ok()) {
      failures.push_back(result.ToString());
    }
  }
  for (Peers& peers : connections) {
    (void)peers.pair.first->Abort(absl::CancelledError("test over"));
    (void)peers.pair.second->Abort(absl::CancelledError("test over"));
  }

  EXPECT_TRUE(failures.empty()) << failures.size() << " of " << kConnections
                                << " connections failed; first: "
                                << (failures.empty() ? "" : failures[0]);
}

}  // namespace
}  // namespace a11::service
