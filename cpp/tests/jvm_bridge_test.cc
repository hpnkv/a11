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

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "jvm/bridge.h"
#include "jvm/values.h"

namespace a11::jvm {
namespace test {
struct MoveOnly {
  explicit MoveOnly(std::string text)
      : text(std::make_unique<std::string>(std::move(text))) {}

  MoveOnly(MoveOnly&&) = default;
  MoveOnly& operator=(MoveOnly&&) = default;
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly& operator=(const MoveOnly&) = delete;
  std::unique_ptr<std::string> text;
};

absl::StatusOr<nlohmann::json> Encode(const MoveOnly& value) {
  return nlohmann::json(*value.text);
}
}  // namespace test

namespace {
TEST(JvmBridgeTest, CompletionRetainsMoveOnlyPayloadAndAllowsSeveralObservers) {
  Promise<test::MoveOnly> promise;
  auto operation =
      std::make_shared<PendingFuture<test::MoveOnly>>(promise.future());
  ASSERT_TRUE(Subscribe(1).ok());
  ASSERT_TRUE(Subscribe(2).ok());
  operation->SubscribeResult(1);
  operation->SubscribeResult(2);
  ASSERT_TRUE(promise.SetValue(test::MoveOnly("move-only")).ok());
  EXPECT_TRUE(operation->IsReady());
  operation.reset();
  auto events = TakeEvents();
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events[0]["result"]["value"], "move-only");
  EXPECT_EQ(events[1]["result"]["value"], "move-only");
}

TEST(JvmBridgeTest, ObserversAreBoundedWithoutBlockingTheProducer) {
  auto operation = std::make_shared<PendingFuture<Unit>>(ReadyTask());
  for (int token = 1; token <= 1024; ++token) {
    ASSERT_TRUE(Subscribe(token).ok());
    operation->SubscribeResult(token);
  }
  EXPECT_EQ(Subscribe(1025).code(), absl::StatusCode::kResourceExhausted);
  for (int token = 1; token <= 1024; ++token) {
    Unsubscribe(token);
  }
  ASSERT_TRUE(Subscribe(1025).ok());
  operation->SubscribeResult(1025);
  const auto events = TakeEvents();
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0]["token"], 1025);
}

TEST(JvmBridgeTest, ReleasedHandleFailsWithoutInvalidatingAnotherOwner) {
  const auto baseline = HandleCount();
  auto value = std::make_shared<std::string>("owned");
  const auto first = Retain(value);
  const auto second = Retain(value);
  value.reset();
  Release(first);
  EXPECT_EQ(Object<std::string>(first).status().code(),
            absl::StatusCode::kFailedPrecondition);
  auto retained = Object<std::string>(second);
  ASSERT_TRUE(retained.ok());
  EXPECT_EQ(**retained, "owned");
  EXPECT_EQ(Object<int>(second).status().code(),
            absl::StatusCode::kInvalidArgument);
  Release(second);
  Release(second);
  EXPECT_EQ(HandleCount(), baseline);
  EXPECT_EQ(**retained, "owned");
}

TEST(JvmBridgeTest, FailedFuturePreservesCodeAndMessageWithoutReadingItsValue) {
  const auto failure = absl::PermissionDeniedError("denied");
  auto operation = std::make_shared<PendingFuture<Unit>>(FailedTask(failure));
  ASSERT_TRUE(Subscribe(1).ok());
  operation->SubscribeResult(1);
  const auto events = TakeEvents();
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0]["result"]["status"]["code"], 7);
  EXPECT_EQ(events[0]["result"]["status"]["message"], "denied");
  EXPECT_TRUE(events[0]["result"]["value"].is_null());
}
}  // namespace

TEST(JvmBridgeTest, MalformedJsonRemainsRecoverableInCombinedRuntime) {
  const auto result = Dispatch(
      nlohmann::json{{"op", "json.parse"}, {"args", {{"value", "{"}}}});
  EXPECT_EQ(result.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(JvmValueTest, NativeMutationsAreVisibleThroughRetainedMemberReferences) {
  const auto root_id = KeepValue(data::NodeFragment{
      .id = "node", .data = data::Chunk{}, .seq = {}, .continued = false});
  auto root = Value<data::NodeFragment>(root_id);
  const auto child_id =
      KeepView<data::Chunk>(root, ValueMember::kPayload,
                            [](data::NodeFragment& fragment) -> data::Chunk& {
                              return std::get<data::Chunk>(fragment.data);
                            });
  auto child = Value<data::Chunk>(child_id);
  std::get<data::Chunk>(root->get().data).data = "native update";
  EXPECT_EQ(child->get().data, "native update");
  child->get().data = "member update";
  EXPECT_EQ(std::get<data::Chunk>(root->get().data).data, "member update");
  Release(root_id);
  root.reset();
  EXPECT_EQ(child->get().data, "member update");
  Release(child_id);
}

TEST(JvmValueTest, ReplacedMemberReferencesFailBeforeDereferencingStorage) {
  const auto root_id = KeepValue(data::NodeFragment{
      .id = "node", .data = data::Chunk{}, .seq = {}, .continued = false});
  auto root = Value<data::NodeFragment>(root_id);
  const auto child_id =
      KeepView<data::Chunk>(root, ValueMember::kPayload,
                            [](data::NodeFragment& fragment) -> data::Chunk& {
                              return std::get<data::Chunk>(fragment.data);
                            });
  root->Invalidate(ValueMember::kPayload);
  root->get().data =
      data::NodeRef{.id = "replacement", .offset = 0, .length = {}};
  EXPECT_DEATH(Value<data::Chunk>(child_id)->get(), "invalidated");
  Release(child_id);
  Release(root_id);
}

}  // namespace a11::jvm
