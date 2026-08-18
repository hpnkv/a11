// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief What happens when a callable a caller handed A11 throws.
 *
 * A11 is compiled `-fno-exceptions` and raises nothing of its own, but the
 * callables it is handed -- a WireStream's `on_message`, a registered codec, an
 * action handler -- belong to callers who may be built with exceptions. Each is
 * wrapped where A11 adopts it (see a11/exception_guard.h), and this file is the
 * check that the wrapping is actually in place: without it the throw would
 * unwind through a frame that has no cleanup information and terminate the
 * process.
 *
 * This test file is compiled *with* exceptions, which is what lets it write the
 * throwing callback in the first place -- exactly the position a caller is in.
 */

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <gtest/gtest.h>

#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "a11/actions/action.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/future.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/net/in_process_wire_stream.h"

namespace a11 {
namespace {

data::WireMessage OneFragment() {
  return data::WireMessage{
      .node_fragments = {data::NodeFragment{.id = "n",
                                            .data = data::Chunk{.data = "x"}}}};
}

TEST(ExceptionBoundaryTest, AThrowingOnMessageAbortsTheStreamNotTheProcess) {
  auto pair = net::InProcessWireStream::CreatePair();
  ASSERT_TRUE(pair.ok()) << pair.status();
  auto [client, server] = *pair;

  ASSERT_TRUE(server
                  ->Accept(
                      [](std::optional<data::WireMessage>) -> a11::Task {
                        throw std::runtime_error("from on_message");
                      },
                      []() -> a11::Task { return a11::ReadyTask(); })
                  .Await()
                  .status()
                  .ok());
  ASSERT_TRUE(client
                  ->Start([](std::optional<data::WireMessage>)
                              -> a11::Task { return a11::ReadyTask(); },
                          []() -> a11::Task { return a11::ReadyTask(); })
                  .Await()
                  .status()
                  .ok());

  // Send reports OK: a delivery failure reaches the application through the
  // stream's lifecycle, not through the send that happened to trigger it.
  EXPECT_TRUE(client->Send(OneFragment()).ok());

  // And it reaches it on the receiving endpoint's own fibre, so poll rather
  // than assume the abort has already happened.
  absl::Status status;
  const absl::Time limit = absl::Now() + absl::Seconds(5);
  do {
    status = server->GetStatus();
  } while (status.ok() && absl::Now() < limit);
  EXPECT_FALSE(status.ok());
  EXPECT_NE(status.message().find("from on_message"), std::string::npos)
      << status;
  EXPECT_NE(status.message().find("on_message"), std::string::npos) << status;
}

TEST(ExceptionBoundaryTest, AThrowingCodecBecomesAnErrorStatus) {
  data::SerializationRegistry registry;
  struct Thrower {
    int value = 0;
  };
  ASSERT_TRUE(registry
                  .RegisterSerializer<Thrower>(
                      "thrower", "application/x-test",
                      [](const Thrower&) -> absl::StatusOr<data::Chunk> {
                        throw std::runtime_error("from serializer");
                      })
                  .ok());
  ASSERT_TRUE(registry
                  .RegisterDeserializer<Thrower>(
                      "thrower", "application/x-test",
                      [](const data::Chunk&) -> absl::StatusOr<Thrower> {
                        throw std::runtime_error("from deserializer");
                      })
                  .ok());

  const absl::StatusOr<data::Chunk> encoded =
      registry.ToChunk(Thrower{}, "application/x-test");
  ASSERT_FALSE(encoded.ok());
  EXPECT_NE(encoded.status().message().find("from serializer"),
            std::string::npos)
      << encoded.status();

  const absl::StatusOr<Thrower> decoded = registry.FromChunk<Thrower>(
      data::Chunk{.metadata =
                      data::ChunkMetadata{
                          .mimetype = "application/x-test;type=thrower"},
                  .data = ""});
  ASSERT_FALSE(decoded.ok());
  EXPECT_NE(decoded.status().message().find("from deserializer"),
            std::string::npos)
      << decoded.status();
}

TEST(ExceptionBoundaryTest, AThrowingActionHandlerFailsTheAction) {
  const actions::ActionSchema schema{.name = "thrower"};
  absl::StatusOr<std::shared_ptr<actions::Action>> action =
      actions::Action::Create(
          schema, "thrower",
          [](std::shared_ptr<actions::Action>) -> a11::Task {
            throw std::runtime_error("from handler");
          });
  ASSERT_TRUE(action.ok()) << action.status();
  ASSERT_TRUE((*action)->Run().ok());
  const absl::Status status =
      (*action)->Wait(absl::Seconds(5)).Await().status();
  EXPECT_FALSE(status.ok());
  EXPECT_NE(status.message().find("from handler"), std::string::npos) << status;
}

}  // namespace
}  // namespace a11
