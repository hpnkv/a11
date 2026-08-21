// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief
 *   Cancelling a task around the moment its fiber finishes.
 *
 * `Submit` needs the work fiber's *pointer* to stay valid, because `Cancel()` walks
 * the fiber tree and locks each node and so cannot be handed a fiber that might
 * delete itself. It used to buy that with a second fiber per Submit whose only job
 * was to block in `Join()` -- ~15% of every fiber a server workload created.
 *
 * `thread::ReapWhenFinished` replaces it: the pool joins and destroys the fiber
 * from `ReapFinishedFibers()` as workers come round. The guarantee that makes it
 * safe is an ordering -- join, then let the owner clear its pointer under the
 * owner's lock, then destroy -- so a `Cancel()` racing completion either acts on a
 * finished-but-live fiber, which is harmless, or finds the handle already cleared.
 *
 * These tests aim at that window. A finished task is cancelled while it may not yet
 * have been reaped, from one fiber and from many, because the failure being
 * guarded against is a use-after-free that a single well-timed call would not
 * reliably produce.
 */

#include <atomic>
#include <cstddef>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/executor.h"
#include "thread/fiber.h"

namespace a11 {
namespace {

constexpr absl::Duration kDeadline = absl::Seconds(10);

TEST(FibreReapingTest, CancellingAFinishedTaskIsSafe) {
  // The window this is about: the work is over, so the fiber is finished, but
  // nothing has necessarily reaped it yet, so the owner's pointer may still be
  // live. Cancelling here used to be held safe by the joiner fiber.
  for (int attempt = 0; attempt < 500; ++attempt) {
    std::atomic<bool> ran{false};
    Task task = SubmitTask([&ran]() -> absl::Status {
      ran.store(true, std::memory_order_relaxed);
      return absl::OkStatus();
    });
    ASSERT_TRUE(task.Await(absl::Now() + kDeadline).ok())
        << "attempt " << attempt;
    EXPECT_TRUE(ran.load(std::memory_order_relaxed));
    // After completion, and deliberately without waiting for a reap.
    (void)task.Cancel();
  }
}

TEST(FibreReapingTest, CancellingWithoutAwaitingIsSafe) {
  // The same window from the other side: never awaited, so the cancel can land
  // before the work starts, while it runs, or after it has finished.
  for (int attempt = 0; attempt < 500; ++attempt) {
    Task task = SubmitTask([]() -> absl::Status { return absl::OkStatus(); });
    (void)task.Cancel();
  }
}

TEST(FibreReapingTest, ManyTasksCancelledConcurrentlyAreAllReaped) {
  // Concurrency, because the ordering being tested is between a worker running
  // ReapFinishedFibers() and a caller holding the owner's lock in Cancel(). One
  // fiber doing this in sequence would rarely produce that overlap.
  constexpr size_t kFibres = 64;
  constexpr int kRounds = 40;
  std::atomic<int> completed{0};

  std::vector<Task> drivers;
  drivers.reserve(kFibres);
  for (size_t index = 0; index < kFibres; ++index) {
    drivers.push_back(SubmitTask([&completed]() -> absl::Status {
      for (int round = 0; round < kRounds; ++round) {
        Task inner = SubmitTask([&completed]() -> absl::Status {
          completed.fetch_add(1, std::memory_order_relaxed);
          return absl::OkStatus();
        });
        ABSL_RETURN_IF_ERROR(inner.Await(absl::Now() + kDeadline).status());
        (void)inner.Cancel();
      }
      return absl::OkStatus();
    }));
  }
  for (Task& driver : drivers) {
    EXPECT_TRUE(driver.Await(absl::Now() + kDeadline).ok());
  }
  EXPECT_EQ(completed.load(std::memory_order_relaxed),
            static_cast<int>(kFibres) * kRounds);
}

TEST(FibreReapingTest, CancellingBeforeTheWorkRunsStillCancels) {
  // The reaper must not have cost the thing the pointer was kept for. A task
  // cancelled before it is scheduled must still report cancellation rather than
  // running to completion.
  std::atomic<bool> ran{false};
  Task task = SubmitTask([&ran]() -> absl::Status {
    ran.store(true, std::memory_order_relaxed);
    return absl::OkStatus();
  });
  (void)task.Cancel();
  const absl::Status result = task.Await(absl::Now() + kDeadline).status();
  // Either it was stopped before it started, or it had already finished -- both
  // are legitimate outcomes of a race against a running pool. What must not
  // happen is a crash or a hang, and `ran` tells which way it went.
  EXPECT_TRUE(result.ok() || absl::IsCancelled(result)) << result;
}

}  // namespace
}  // namespace a11
