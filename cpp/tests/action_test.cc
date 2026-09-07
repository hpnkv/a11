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

#include "a11/actions/action.h"
#include "a11/actions/authorization.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/in_process_wire_stream.h"
#include "a11/nodes/async_node.h"
#include "a11/service/session.h"
#include "thread/fiber.h"

namespace a11::actions {
namespace {

ActionSchema EchoSchema() {
  ActionSchema schema{
      .name = "echo",
      .inputs = {{"input",
                  ActionPortSchema{.name = "input",
                                   .type = "application/octet-stream"}}},
      .outputs = {{"output",
                   ActionPortSchema{.name = "output",
                                    .type = "application/octet-stream"}}},
  };
  return schema;
}

ActionHandler EchoHandler() {
  return [](std::shared_ptr<Action> action) {
    return a11::SubmitTask([action = std::move(action)]() -> absl::Status {
      absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> input =
          action->GetInput("input");
      if (!input.ok()) {
        return input.status();
      }
      absl::StatusOr<std::optional<data::Chunk>> chunk =
          (*input)->NextChunk().Await();
      if (!chunk.ok()) {
        return chunk.status();
      }
      if (!chunk->has_value()) {
        return absl::FailedPreconditionError("echo input ended early");
      }
      absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> output =
          action->GetOutput("output");
      if (!output.ok()) {
        return output.status();
      }
      return (*output)
          ->PutChunk(std::move(**chunk), std::nullopt, true)
          .Await()
          .status();
    });
  };
}

ActionSchema EmptySchema(std::string name) {
  return ActionSchema{.name = std::move(name)};
}

std::shared_ptr<service::Session> SessionWithOneActionSlot() {
  service::SessionOptions options;
  options.max_concurrent_root_actions = 1;
  options.no_stream_timeout = absl::InfiniteDuration();
  return *service::Session::Create("limited", {}, {}, {}, options);
}

TEST(ActionLimiterTest, TryAcquireReportsImmediateCapacity) {
  auto limiter = *ActionLimiter::Create(1);
  EXPECT_TRUE(limiter->TryAcquire());
  EXPECT_FALSE(limiter->TryAcquire());
  limiter->Release();
  EXPECT_TRUE(limiter->TryAcquire());
  limiter->Release();
}

TEST(ActionTest, NestedActionsDoNotForwardAuthorizationReferences) {
  auto parent = *Action::Create(EmptySchema("parent"));
  ASSERT_TRUE(parent->SetHeader("x-a11-auth", "proof").ok());
  ASSERT_TRUE(parent->SetHeader("x-a11-auth-ref", "connection-local").ok());
  ASSERT_TRUE(parent->SetHeader("x-a11-trace", "trace").ok());

  auto child = *parent->MakeNested(EmptySchema("child"));
  EXPECT_EQ(*child->GetHeader("x-a11-auth"),
            std::optional<data::Bytes>("proof"));
  EXPECT_EQ(*child->GetHeader("x-a11-trace"),
            std::optional<data::Bytes>("trace"));
  EXPECT_EQ(*child->GetHeader("x-a11-auth-ref"), std::nullopt);
}

TEST(ActionTest, UncontendedSessionHandlerStartsWithoutAFiber) {
  (void)thread::Fiber::Current();
  auto session = SessionWithOneActionSlot();
  auto completion = std::make_shared<a11::Promise<a11::Unit>>();
  std::atomic<int> starts{0};
  auto action = *Action::Create(
      EmptySchema("hold"), "uncontended",
      [completion, &starts](std::shared_ptr<Action>) {
        ++starts;
        return completion->future();
      },
      nullptr, nullptr, session);

  const size_t created = thread::internal::CreatedFiberCountForTesting();
  ASSERT_TRUE(action->Run().ok());
  EXPECT_EQ(starts, 1);
  EXPECT_EQ(thread::internal::CreatedFiberCountForTesting(), created);

  ASSERT_TRUE(completion->SetValue(a11::Unit{}).ok());
  EXPECT_TRUE(action->Wait(absl::Seconds(5)).Await().ok());
}

TEST(ActionTest, SaturatedSessionLimiterRetainsTheFiberPath) {
  (void)thread::Fiber::Current();
  auto session = SessionWithOneActionSlot();
  auto first_completion = std::make_shared<a11::Promise<a11::Unit>>();
  auto second_completion = std::make_shared<a11::Promise<a11::Unit>>();
  std::atomic<int> starts{0};
  auto first = *Action::Create(
      EmptySchema("hold"), "first",
      [first_completion, &starts](std::shared_ptr<Action>) {
        ++starts;
        return first_completion->future();
      },
      nullptr, nullptr, session);
  auto second = *Action::Create(
      EmptySchema("hold"), "second",
      [second_completion, &starts](std::shared_ptr<Action>) {
        ++starts;
        return second_completion->future();
      },
      nullptr, nullptr, session);

  ASSERT_TRUE(first->Run().ok());
  const size_t created = thread::internal::CreatedFiberCountForTesting();
  ASSERT_TRUE(second->Run().ok());
  EXPECT_EQ(starts, 1);
  EXPECT_GT(thread::internal::CreatedFiberCountForTesting(), created);

  ASSERT_TRUE(first_completion->SetValue(a11::Unit{}).ok());
  for (int attempt = 0; attempt < 100 && starts != 2; ++attempt) {
    absl::SleepFor(absl::Milliseconds(1));
  }
  ASSERT_EQ(starts, 2);
  ASSERT_TRUE(second_completion->SetValue(a11::Unit{}).ok());
  EXPECT_TRUE(first->Wait(absl::Seconds(5)).Await().ok());
  EXPECT_TRUE(second->Wait(absl::Seconds(5)).Await().ok());
}

TEST(ActionTest, CancellationReleasesFastPathLimiterCapacity) {
  auto session = SessionWithOneActionSlot();
  auto first_completion = std::make_shared<a11::Promise<a11::Unit>>();
  auto second_completion = std::make_shared<a11::Promise<a11::Unit>>();
  ASSERT_TRUE(first_completion->SetCancellationCallback([] {}).ok());
  std::atomic<int> starts{0};
  auto first = *Action::Create(
      EmptySchema("hold"), "cancelled",
      [first_completion, &starts](std::shared_ptr<Action>) {
        ++starts;
        return first_completion->future();
      },
      nullptr, nullptr, session);
  auto second = *Action::Create(
      EmptySchema("hold"), "after-cancel",
      [second_completion, &starts](std::shared_ptr<Action>) {
        ++starts;
        return second_completion->future();
      },
      nullptr, nullptr, session);

  ASSERT_TRUE(first->Run().ok());
  ASSERT_TRUE(first->Cancel().ok());
  ASSERT_TRUE(second->Run().ok());
  EXPECT_EQ(starts, 2);

  ASSERT_TRUE(first_completion->SetValue(a11::Unit{}).ok());
  ASSERT_TRUE(second_completion->SetValue(a11::Unit{}).ok());
  EXPECT_EQ(first->Wait(absl::Seconds(5)).Await().status().code(),
            absl::StatusCode::kCancelled);
  EXPECT_TRUE(second->Wait(absl::Seconds(5)).Await().ok());
}

TEST(ActionTest, InputAutofillRetainsTheFiberPath) {
  (void)thread::Fiber::Current();
  auto completion = std::make_shared<a11::Promise<a11::Unit>>();
  ActionSchema schema = EmptySchema("autofill");
  schema.inputs.emplace("input",
                        ActionPortSchema{.name = "input",
                                         .type = "application/octet-stream",
                                         .autofills = {std::nullopt}});
  auto action = *Action::Create(
      std::move(schema), "autofill",
      [completion](std::shared_ptr<Action>) { return completion->future(); });

  const size_t created = thread::internal::CreatedFiberCountForTesting();
  ASSERT_TRUE(action->Run().ok());
  EXPECT_GT(thread::internal::CreatedFiberCountForTesting(), created);
  ASSERT_TRUE(completion->SetValue(a11::Unit{}).ok());
  EXPECT_TRUE(action->Wait(absl::Seconds(5)).Await().ok());
}

TEST(ActionTest, LocalRunStreamsDataAndPublishesStatus) {
  auto action = *Action::Create(EchoSchema(), "local", EchoHandler());
  auto input = *action->GetInput("input", false);
  ASSERT_EQ(*input->PutChunk(data::Chunk{.data = "hello"}, std::nullopt, true)
                 .Await(),
            0);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(absl::Seconds(5)).Await().ok());

  auto output = *action->GetOutput("output", false);
  auto value = output->NextChunk().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(value.ok()) << value.status();
  ASSERT_TRUE(value->has_value());
  EXPECT_EQ(value->value().data, "hello");
  EXPECT_TRUE(action->GetStatus().ok());

  auto status_node =
      *action->GetOutput(std::string(kActionStatusOutput), false);
  auto status_chunk =
      status_node->NextChunk().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(status_chunk.ok()) << status_chunk.status();
  ASSERT_TRUE(status_chunk->has_value());
  auto decoded = StatusFromChunk(**status_chunk);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_TRUE(decoded->ok());
}

TEST(ActionTest, RejectsAPortNameUsedInBothDirections) {
  // A port's node id is derived from the action id and the port name alone, so
  // an input and an output sharing a name are the *same node*: feeding the
  // input would establish an end the output then cannot write past.
  actions::ActionSchema schema;
  schema.name = "both-ways";
  schema.inputs.emplace(
      "body", actions::ActionPortSchema{.name = "body", .type = "text/plain"});
  schema.outputs.emplace(
      "body", actions::ActionPortSchema{.name = "body", .type = "text/plain"});
  const absl::Status status = schema.Validate();
  EXPECT_TRUE(absl::IsInvalidArgument(status)) << status;
  EXPECT_NE(status.message().find("both an input and an output"),
            std::string_view::npos)
      << status.message();

  // Either direction alone is fine.
  schema.outputs.erase("body");
  schema.outputs.emplace(
      "text", actions::ActionPortSchema{.name = "text", .type = "text/plain"});
  EXPECT_TRUE(schema.Validate().ok());
}

TEST(ActionTest, HandlerFailureAbortsUnfinishedOutput) {
  ActionHandler failing = [](std::shared_ptr<Action> action) {
    return a11::SubmitTask([action = std::move(action)]() -> absl::Status {
      auto output = action->GetOutput("output", false);
      if (!output.ok()) {
        return output.status();
      }
      return absl::DataLossError("handler failed");
    });
  };
  auto action = *Action::Create(EchoSchema(), "failure", failing);
  ASSERT_TRUE(action->Run().ok());
  EXPECT_EQ(action->Wait(absl::Seconds(5)).Await().status().code(),
            absl::StatusCode::kDataLoss);
  EXPECT_EQ(action->GetStatus().code(), absl::StatusCode::kDataLoss);
  auto output = *action->GetOutput("output", false);
  EXPECT_EQ(output->NextChunk().Await().status().code(),
            absl::StatusCode::kDataLoss);
}

TEST(ActionTest, AuthorizationEnvelopeAndActionHelpersAreNative) {
  const AuthorizationEnvelope envelope{
      .version = kAuthorizationVersion,
      .chain = {"header.payload.signature"}};
  const auto encoded = EncodeAuthorization(envelope);
  ASSERT_TRUE(encoded.ok()) << encoded.status();
  const auto decoded = DecodeAuthorization(*encoded);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(*decoded, envelope);

  const auto text = AuthorizationToText(envelope);
  ASSERT_TRUE(text.ok()) << text.status();
  EXPECT_EQ(*AuthorizationFromText(*text), envelope);

  auto action = *Action::Create(EchoSchema());
  ASSERT_TRUE(SetAuthorization(action, envelope).ok());
  const auto attached = GetAuthorization(*action);
  ASSERT_TRUE(attached.ok() && attached->has_value());
  EXPECT_EQ(**attached, envelope);

  const std::string reference(16, 'r');
  ASSERT_TRUE(SetAuthorizationReference(action, reference).ok());
  EXPECT_FALSE((*GetAuthorization(*action)).has_value());
  EXPECT_EQ(**GetAuthorizationReference(*action), reference);
  EXPECT_FALSE(action->GetHeader(kAuthorizationHeader)->has_value());

  auto session = *service::Session::Create();
  auto pair = *net::InProcessWireStream::CreatePair();
  ASSERT_TRUE(action->BindSession(session).ok());
  ASSERT_TRUE(action->BindStream(pair.first).ok());
  auto verified = std::make_shared<VerifiedAuthorization>();
  verified->envelope = envelope;
  verified->subject = "user-1";
  verified->subject_kind = "user";
  verified->actors = {"user-1"};
  verified->audience = "agent";
  verified->expires_at = absl::Now() + absl::Minutes(1);
  AuthorizationContextStore contexts;
  const auto installed = contexts.Install(action, verified);
  ASSERT_TRUE(installed.ok()) << installed.status();
  EXPECT_TRUE(installed->is_default);

  auto next = *Action::Create(EchoSchema(), "next", {}, nullptr, pair.first,
                              session);
  const auto resolved = contexts.Resolve(next);
  ASSERT_TRUE(resolved.ok()) << resolved.status();
  EXPECT_EQ((*resolved)->subject, "user-1");
  auto child = *next->MakeNested(EchoSchema());
  EXPECT_EQ(child->GetVerifiedAuthorization()->subject, "user-1");

  auto registry = std::make_shared<ActionRegistry>();
  const auto installed_authorizer = InstallAuthorizer(
      registry, [verified](std::string_view raw)
                    -> absl::StatusOr<VerifiedAuthorization> {
        ABSL_RETURN_IF_ERROR(DecodeAuthorization(raw).status());
        return *verified;
      });
  ASSERT_TRUE(installed_authorizer.ok()) << installed_authorizer.status();
  auto authorizing = *registry->MakeAction(
      kAuthorizeAction, "authorize", session->GetNodeMap(), pair.first,
      session);
  ASSERT_TRUE(SetAuthorization(authorizing, envelope).ok());
  ASSERT_TRUE(authorizing->Run().ok());
  ASSERT_TRUE(authorizing->Wait(absl::Seconds(5)).Await().ok());
  auto result = *authorizing->GetOutput("output", false);
  auto response = result->NextChunk().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(response.ok() && response->has_value());
  EXPECT_NE(response->value().data.find("context_id"), std::string::npos);

  auto protected_action =
      *Action::Create(EchoSchema(), "protected", {}, session->GetNodeMap(),
                      pair.first, session);
  const auto protected_authorization =
      (*installed_authorizer)->Resolve(protected_action);
  ASSERT_TRUE(protected_authorization.ok())
      << protected_authorization.status();
  EXPECT_EQ((*protected_authorization)->subject, "user-1");
}

TEST(ActionTest, RemoteCallHasSymmetricInputOutputAndStatus) {
  auto registry = std::make_shared<ActionRegistry>();
  ASSERT_TRUE(registry->Register("echo", EchoSchema(), EchoHandler()).ok());
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
  ASSERT_TRUE(caller_registry->Register("echo", EchoSchema()).ok());
  auto call = *caller_registry->MakeAction("echo", "remote", nullptr,
                                           pair.first, client);
  ASSERT_TRUE(call->Call().Await().ok());
  auto input = *call->GetInput("input");
  ASSERT_TRUE(
      input->PutChunk(data::Chunk{.data = "over-wire"}, std::nullopt, true)
          .Await()
          .ok());
  ASSERT_TRUE(call->WaitForDispatch(absl::Seconds(5)).Await().ok());
  auto completion = call->Wait(absl::Seconds(5)).Await();
  ASSERT_TRUE(completion.ok()) << completion.status();
  auto output = *call->GetOutput("output", false);
  auto received = output->NextChunk().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(received.ok()) << received.status();
  ASSERT_TRUE(received->has_value());
  EXPECT_EQ(received->value().data, "over-wire");
}

}  // namespace
}  // namespace a11::actions
