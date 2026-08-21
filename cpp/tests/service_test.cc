// Copyright 2026 The A11 Authors.

#include "a11/service/service.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/future.h"
#include "a11/net/in_process_wire_stream.h"
#include "a11/service/serving.h"
#include "a11/service/session.h"

namespace a11::service {
namespace {

using ::a11::actions::ActionPortSchema;
using ::a11::actions::ActionRegistry;
using ::a11::actions::ActionSchema;

ActionSchema EchoSchema() {
  ActionSchema schema;
  schema.name = "echo";
  schema.description = "Echo the input back.";
  schema.inputs.emplace("text",
                        ActionPortSchema{.name = "text", .type = "text/plain"});
  schema.outputs.emplace("out",
                         ActionPortSchema{.name = "out", .type = "text/plain"});
  return schema;
}

ActionSchema PeerOnlySchema() {
  ActionSchema schema;
  schema.name = "peer_only";
  schema.description = "A tool only one peer announced.";
  schema.outputs.emplace("out",
                         ActionPortSchema{.name = "out", .type = "text/plain"});
  return schema;
}

std::shared_ptr<ActionRegistry> EchoRegistry() {
  auto registry = std::make_shared<ActionRegistry>();
  (void)registry->Register("echo", EchoSchema());
  return registry;
}

/// A connected in-process pair, as any transport would hand over.
net::InProcessWireStream::Pair Pair() {
  return *net::InProcessWireStream::CreatePair();
}

TEST(ServiceTest, CreatesAnEmptyRegistryWhenGivenNone) {
  const auto service = Service::Create();
  ASSERT_TRUE(service.ok()) << service.status();
  EXPECT_NE((*service)->GetActionRegistry(), nullptr);
  EXPECT_TRUE((*service)->accepting());
  EXPECT_EQ((*service)->SessionCount(), 0u);
}

TEST(ServiceTest, ServesAStreamThatCameFromNowhereInParticular) {
  const auto service = Service::Create(EchoRegistry());
  ASSERT_TRUE(service.ok()) << service.status();

  auto [server, client] = Pair();
  const auto session = (*service)->StartStreamHandler(server);
  ASSERT_TRUE(session.ok()) << session.status();
  EXPECT_EQ((*service)->SessionCount(), 1u);
  // The session is findable both ways round, which is what lets a second
  // transport be attached to the same peer.
  EXPECT_TRUE((*service)->GetSession((*session)->GetId()).ok());
  EXPECT_TRUE((*service)->GetSessionForStream(server->GetId()).ok());
  EXPECT_EQ((*service)->SessionIds().size(), 1u);

  (void)(*service)->Abort(absl::CancelledError("done"));
}

TEST(ServiceTest, TheConnectionHookRunsBeforeTheSessionStartsPumping) {
  std::vector<std::string> prepared;
  auto registry = EchoRegistry();
  const auto service = Service::Create(
      /*action_registry=*/nullptr,
      [&prepared, registry](
          const std::shared_ptr<Session>& session,
          const std::shared_ptr<net::WireStream>& /*stream*/) {
        // Specialising the connection: the service itself has an empty registry.
        (void)session->SetActionRegistry(registry);
        prepared.push_back(session->GetId());
        return a11::ReadyTask();
      });
  ASSERT_TRUE(service.ok()) << service.status();

  auto [server, client] = Pair();
  const auto session = (*service)->StartStreamHandler(server);
  ASSERT_TRUE(session.ok()) << session.status();
  ASSERT_EQ(prepared.size(), 1u);
  EXPECT_EQ(prepared.front(), (*session)->GetId());
  // The hook's registry is the one the session dispatches against.
  EXPECT_TRUE((*session)->GetActionRegistry()->IsRegistered("echo"));

  (void)(*service)->Abort(absl::CancelledError("done"));
}

TEST(ServiceTest, ARejectingHookRefusesTheConnectionAndLeavesNothingBehind) {
  const auto service = Service::Create(
      EchoRegistry(), [](const std::shared_ptr<Session>&,
                         const std::shared_ptr<net::WireStream>&) {
        return a11::FailedTask(absl::PermissionDeniedError("not for you"));
      });
  ASSERT_TRUE(service.ok()) << service.status();

  auto [server, client] = Pair();
  const auto session = (*service)->StartStreamHandler(server);
  ASSERT_FALSE(session.ok());
  EXPECT_EQ(session.status().code(), absl::StatusCode::kPermissionDenied);
  // Nothing registered, so a later drain cannot wait on a connection that was
  // never admitted.
  EXPECT_EQ((*service)->SessionCount(), 0u);
}

TEST(ServiceTest, StopAcceptingRefusesNewConnections) {
  const auto service = Service::Create(EchoRegistry());
  ASSERT_TRUE(service.ok()) << service.status();
  ASSERT_TRUE((*service)->StopAccepting().ok());
  EXPECT_FALSE((*service)->accepting());

  auto [server, client] = Pair();
  const auto session = (*service)->StartStreamHandler(server);
  ASSERT_FALSE(session.ok());
  EXPECT_EQ(session.status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST(ServiceTest, DrainReturnsImmediatelyWithNothingInFlight) {
  const auto service = Service::Create(EchoRegistry());
  ASSERT_TRUE(service.ok()) << service.status();
  ASSERT_TRUE((*service)->StopAccepting().ok());
  EXPECT_TRUE((*service)->Drain(absl::Seconds(5)).Await().ok());
}

TEST(ServiceTest, DrainReportsWhatItCouldNotWaitOut) {
  const auto service = Service::Create(EchoRegistry());
  ASSERT_TRUE(service.ok()) << service.status();
  auto [server, client] = Pair();
  ASSERT_TRUE((*service)->StartStreamHandler(server).ok());
  ASSERT_TRUE((*service)->StopAccepting().ok());

  // The peer is still there, so the session does not finish; draining must give
  // up rather than hang, and say so.
  const absl::Status drained =
      (*service)->Drain(absl::Milliseconds(200)).Await().status();
  EXPECT_EQ(drained.code(), absl::StatusCode::kDeadlineExceeded);

  (void)(*service)->Abort(absl::CancelledError("done"));
}

TEST(ServiceTest, AbortEndsEverySession) {
  const auto service = Service::Create(EchoRegistry());
  ASSERT_TRUE(service.ok()) << service.status();
  auto [first_server, first_client] = Pair();
  auto [second_server, second_client] = Pair();
  ASSERT_TRUE((*service)->StartStreamHandler(first_server).ok());
  ASSERT_TRUE((*service)->StartStreamHandler(second_server).ok());
  EXPECT_EQ((*service)->SessionCount(), 2u);

  ASSERT_TRUE((*service)->Abort(absl::CancelledError("shutting down")).ok());
  // Aborting also closes the door, so nothing new arrives mid-shutdown.
  EXPECT_FALSE((*service)->accepting());
  EXPECT_TRUE((*service)->Drain(absl::Seconds(10)).Await().ok());
  EXPECT_EQ((*service)->SessionCount(), 0u);
}

TEST(ServiceTest, EachConnectionGetsItsOwnRegistryCopyWhenAsked) {
  ServiceOptions options;
  options.copy_registry_per_connection = true;
  const auto service =
      Service::Create(EchoRegistry(), /*on_connection=*/{}, options);
  ASSERT_TRUE(service.ok()) << service.status();

  auto [first_server, first_client] = Pair();
  auto [second_server, second_client] = Pair();
  const auto first = (*service)->StartStreamHandler(first_server);
  const auto second = (*service)->StartStreamHandler(second_server);
  ASSERT_TRUE(first.ok() && second.ok());

  // Two peers, two registries: an action registered for one must not become
  // callable on the other's session.
  EXPECT_NE((*first)->GetActionRegistry(), (*second)->GetActionRegistry());
  EXPECT_NE((*first)->GetActionRegistry(), (*service)->GetActionRegistry());
  ASSERT_TRUE((*first)
                  ->GetActionRegistry()
                  ->Register("peer_only", PeerOnlySchema())
                  .ok());
  EXPECT_FALSE((*second)->GetActionRegistry()->IsRegistered("peer_only"));
  EXPECT_FALSE((*service)->GetActionRegistry()->IsRegistered("peer_only"));

  (void)(*service)->Abort(absl::CancelledError("done"));
}

TEST(ServiceTest, ReplacingTheRegistryDoesNotDisturbLiveSessions) {
  const auto service = Service::Create(EchoRegistry());
  ASSERT_TRUE(service.ok()) << service.status();
  auto [server, client] = Pair();
  const auto session = (*service)->StartStreamHandler(server);
  ASSERT_TRUE(session.ok()) << session.status();

  auto replacement = EchoRegistry();
  ASSERT_TRUE((*service)->SetActionRegistry(replacement).ok());
  EXPECT_EQ((*service)->GetActionRegistry(), replacement);
  // Not copying per connection, so the live session follows the swap -- and it
  // is still the same, unaborted session.
  EXPECT_EQ((*session)->GetActionRegistry(), replacement);
  EXPECT_FALSE((*session)->IsDone());

  (void)(*service)->Abort(absl::CancelledError("done"));
}

TEST(ServiceTest, RejectsANullStreamAndAnUnknownSession) {
  const auto service = Service::Create(EchoRegistry());
  ASSERT_TRUE(service.ok()) << service.status();
  EXPECT_EQ((*service)->StartStreamHandler(nullptr).status().code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ((*service)->GetSession("nope").status().code(),
            absl::StatusCode::kNotFound);
  EXPECT_EQ((*service)->GetSessionForStream("nope").status().code(),
            absl::StatusCode::kNotFound);
}

TEST(ServiceTest, DestructionWithALiveSessionDoesNotBlock) {
  // A destructor that drained would have to wait for a peer that is still
  // there, and at process shutdown there is no scheduler left to wait on --
  // which presents as a process that ignores SIGTERM.
  auto [server, client] = Pair();
  const absl::Time started = absl::Now();
  {
    const auto service = Service::Create(EchoRegistry());
    ASSERT_TRUE(service.ok()) << service.status();
    ASSERT_TRUE((*service)->StartStreamHandler(server).ok());
  }
  EXPECT_LT(absl::Now() - started, absl::Seconds(5));
}

TEST(ServiceTest, AcceptIntoNoOpsOnceItsServiceIsGone) {
  std::function<a11::Task(std::shared_ptr<net::InProcessWireStream>)> accept;
  {
    const auto service = Service::Create(EchoRegistry());
    ASSERT_TRUE(service.ok()) << service.status();
    accept = AcceptInto<net::InProcessWireStream>(*service);
  }
  // A server that outlives its service must refuse rather than dereference it.
  auto pair = Pair();
  const absl::Status status = accept(pair.first).Await().status();
  EXPECT_EQ(status.code(), absl::StatusCode::kFailedPrecondition);
}

}  // namespace
}  // namespace a11::service
