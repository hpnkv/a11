// Copyright 2026 The A11 Authors.

#include <atomic>
#include <cstddef>
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
