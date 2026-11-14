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

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <absl/time/clock.h>
#include <gtest/gtest.h>

#include "a11/concurrency/executor.h"
#include "thread/boost_primitives.h"
#include "thread/fiber.h"

namespace a11 {
namespace {

TEST(FutureTest, CompletesAcrossFiberAndExternalThread) {
  Future<int> value = Submit<int>([]() -> absl::StatusOr<int> { return 42; });
  const absl::StatusOr<int> result =
      value.Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(*result, 42);
}

TEST(FutureTest, PropagatesStatus) {
  Future<int> value = Submit<int>(
      []() -> absl::StatusOr<int> { return absl::DataLossError("bad data"); });
  EXPECT_EQ(value.Await(absl::Now() + absl::Seconds(5)).status().code(),
            absl::StatusCode::kDataLoss);
}

TEST(FutureTest, PendingWaitersWakeOnCompletionAndAbandonment) {
  for (const bool abandon : {false, true}) {
    auto producer = std::make_unique<Promise<int>>();
    Future<int> pending = producer->future();
    std::atomic<int> entered{0};
    std::optional<absl::StatusOr<int>> thread_result;
    const absl::Time deadline = absl::Now() + absl::Seconds(5);
    std::thread external([&] {
      ++entered;
      thread_result = pending.Await(deadline);
    });
    Future<int> fiber =
        Submit<int>([pending, &entered, deadline]() -> absl::StatusOr<int> {
          ++entered;
          return pending.Await(deadline);
        });

    while (entered.load() != 2 && absl::Now() < deadline) {
      thread::SleepFor(absl::Milliseconds(1));
    }
    if (abandon) {
      producer.reset();
    } else {
      EXPECT_TRUE(producer->SetValue(42).ok());
    }
    const absl::StatusOr<int> fiber_result = fiber.Await(deadline);
    external.join();
    ASSERT_TRUE(thread_result.has_value());
    if (abandon) {
      EXPECT_EQ(fiber_result.status().code(), absl::StatusCode::kCancelled);
      EXPECT_EQ(thread_result->status().code(), absl::StatusCode::kCancelled);
    } else {
      ASSERT_TRUE(fiber_result.ok()) << fiber_result.status();
      ASSERT_TRUE(thread_result->ok()) << thread_result->status();
      EXPECT_EQ(*fiber_result, 42);
      EXPECT_EQ(**thread_result, 42);
    }
  }
}

TEST(FutureTest, CancellingFiberWaitDoesNotCompleteProducer) {
  Promise<Unit> producer;
  Future<Unit> pending = producer.future();
  std::atomic<bool> entered{false};
  Task waiter = Submit<Unit>([pending, &entered]() -> absl::StatusOr<Unit> {
    entered = true;
    return pending.Await(absl::Now() + absl::Seconds(5));
  });
  const absl::Time deadline = absl::Now() + absl::Seconds(5);
  while (!entered && absl::Now() < deadline) {
    thread::SleepFor(absl::Milliseconds(1));
  }
  EXPECT_TRUE(entered);
  ASSERT_TRUE(waiter.Cancel().ok());
  EXPECT_EQ(waiter.Await(deadline).status().code(),
            absl::StatusCode::kCancelled);
  EXPECT_FALSE(pending.IsReady());
  EXPECT_TRUE(producer.SetValue(Unit{}).ok());
  EXPECT_TRUE(pending.Await(deadline).ok());
}

TEST(FutureTest, CancelSafelyOwnsRootFiberUntilCompletion) {
  std::atomic<bool> started = false;
  Future<int> value = Submit<int>([&started]() -> absl::StatusOr<int> {
    started = true;
    while (!thread::Cancelled()) {
      thread::SleepFor(absl::Milliseconds(1));
    }
    return absl::CancelledError("observed cancellation");
  });
  const absl::Time start_limit = absl::Now() + absl::Seconds(5);
  while (!started && absl::Now() < start_limit) {
    thread::SleepFor(absl::Milliseconds(1));
  }
  ASSERT_TRUE(started);
  ASSERT_TRUE(value.Cancel().ok());
  EXPECT_EQ(value.Await(absl::Now() + absl::Seconds(5)).status().code(),
            absl::StatusCode::kCancelled);
  // Cancellation after completion is idempotent.
  EXPECT_TRUE(value.Cancel().ok());
}

TEST(FutureTest, CallableCapturesAreReleasedBeforeCompletionObservation) {
  for (const bool fail : {false, true}) {
    auto payload = std::make_shared<int>(42);
    const std::weak_ptr<int> weak = payload;
    std::atomic<bool> cleaned = false;
    Promise<Unit> gate;
    auto ready = gate.future();
    auto task = Submit<int>(
        [payload = std::move(payload), fail, ready]() -> absl::StatusOr<int> {
          ABSL_RETURN_IF_ERROR(ready.Await().status());
          if (fail) {
            return absl::DataLossError("original failure");
          }
          return *payload;
        });
    task.OnReady([&](const auto&) { cleaned = weak.expired(); });
    ASSERT_TRUE(gate.SetValue(Unit{}).ok());
    const auto result = task.Await(absl::Now() + absl::Seconds(5));
    EXPECT_EQ(result.ok(), !fail);
    EXPECT_TRUE(weak.expired());
    const auto limit = absl::Now() + absl::Seconds(5);
    while (!cleaned && absl::Now() < limit) {
      thread::SleepFor(absl::Milliseconds(1));
    }
    EXPECT_TRUE(cleaned);
  }
}

TEST(FutureTest, ConcurrentMigratingFiberStacksHaveIndependentLifetime) {
  constexpr size_t kTaskCount = 512;
  std::vector<Future<size_t>> tasks;
  tasks.reserve(kTaskCount);
  for (size_t index = 0; index < kTaskCount; ++index) {
    tasks.push_back(
        Submit<size_t>([index]() -> absl::StatusOr<size_t> { return index; }));
  }
  for (size_t index = 0; index < tasks.size(); ++index) {
    absl::StatusOr<size_t> result =
        tasks[index].Await(absl::Now() + absl::Seconds(5));
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(*result, index);
  }
}

}  // namespace
}  // namespace a11
