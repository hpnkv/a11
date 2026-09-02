// Copyright 2026 The A11 Authors.

#include <atomic>
#include <chrono>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/log/initialize.h>
#include <absl/strings/match.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "thread/concurrency.h"
#include "thread/fiber.h"
#include "thread/internal/stack_walk.h"
#include "thread/introspect.h"

namespace thread {
namespace {

const FiberSnapshot* absl_nullable FindById(
    const std::vector<FiberSnapshot>& snapshot, std::uint64_t id) {
  for (const FiberSnapshot& fiber : snapshot) {
    if (fiber.id == id) {
      return &fiber;
    }
  }
  return nullptr;
}

// Waits for `predicate` over repeated snapshots. Fiber scheduling decides when
// a fiber actually reaches its wait, so a single snapshot can be too early.
template <typename Predicate>
std::vector<FiberSnapshot> SnapshotUntil(Predicate predicate) {
  const absl::Time deadline = absl::Now() + absl::Seconds(5);
  std::vector<FiberSnapshot> snapshot;
  while (absl::Now() < deadline) {
    snapshot = SnapshotFibers();
    if (predicate(snapshot)) {
      return snapshot;
    }
    thread::SleepFor(absl::Milliseconds(2));
  }
  return snapshot;
}

bool StackMentions(const FiberSnapshot& fiber, std::string_view symbol) {
  for (void* pc : fiber.stack) {
    if (absl::StrContains(internal::DescribeProgramCounter(pc), symbol)) {
      return true;
    }
  }
  return false;
}

TEST(StackWalkTest, RejectsFramesItCannotTrust) {
  void* frames[8];
  alignas(16) std::byte fake_stack[256] = {};
  const auto* lo = static_cast<const void*>(fake_stack);
  const auto* hi = static_cast<const void*>(fake_stack + sizeof(fake_stack));

  EXPECT_EQ(internal::WalkFramePointers(nullptr, lo, hi, frames, 8), 0u);

  // Outside the bounds.
  void* outside = frames;
  EXPECT_EQ(internal::WalkFramePointers(outside, lo, hi, frames, 8), 0u);

  // Zeroed frame record: no next frame, no return address.
  void* inside = fake_stack;
  EXPECT_EQ(internal::WalkFramePointers(inside, lo, hi, frames, 8), 0u);

  // A non-increasing chain stops after the frame it can read.
  auto* record = reinterpret_cast<void**>(fake_stack + 128);
  record[0] = fake_stack;                     // Points backwards.
  record[1] = reinterpret_cast<void*>(0x42);  // Plausible return address.
  EXPECT_EQ(internal::WalkFramePointers(record, lo, hi, frames, 8), 1u);

  EXPECT_EQ(internal::WalkFramePointers(record, lo, hi, frames, 0), 0u);
}

TEST(FiberIntrospectTest, ParkedSelectShowsTheCallersFrames) {
  if (!internal::FramePointerWalkSupported()) {
    GTEST_SKIP() << "no frame-record layout for this architecture";
  }
  Channel<int> channel(0);
  std::atomic<std::uint64_t> waiter_id{0};
  PermanentEvent entered;

  auto waiter = NewTree({.name = "parked-reader"}, [&] {
    waiter_id.store(CurrentFiberId(), std::memory_order_release);
    entered.Notify();
    int value = 0;
    bool ok = false;
    Select({channel.reader()->OnRead(&value, &ok)});
  });
  ASSERT_EQ(Select({entered.OnEvent()}), 0);

  const std::uint64_t id = waiter_id.load(std::memory_order_acquire);
  ASSERT_NE(id, 0u);
  const std::vector<FiberSnapshot> snapshot =
      SnapshotUntil([id](const std::vector<FiberSnapshot>& fibers) {
        const FiberSnapshot* fiber = FindById(fibers, id);
        return fiber != nullptr && fiber->kind == WaitKind::kSelect &&
               !fiber->stack.empty();
      });

  const FiberSnapshot* fiber = FindById(snapshot, id);
  ASSERT_NE(fiber, nullptr);
  EXPECT_EQ(fiber->kind, WaitKind::kSelect);
  EXPECT_EQ(fiber->name, "parked-reader");
  EXPECT_FALSE(fiber->selectables.empty());
  EXPECT_FALSE(fiber->trace_raced);
  ASSERT_FALSE(fiber->stack.empty());
  EXPECT_GE(fiber->waited, absl::ZeroDuration());
  // The walk reaches the bottom of a stack no OS thread is running: the frames
  // between here and the fiber's entry are what `bt` cannot see. Which
  // intermediate frames survive depends on inlining, so the assertion is that
  // the entry was reached at all.
  EXPECT_GE(fiber->stack.size(), 2u) << FormatFiberReport({.max_frames = 20});
  EXPECT_TRUE(StackMentions(*fiber, "worker_context") ||
              StackMentions(*fiber, "fiber_entry") ||
              StackMentions(*fiber, "Fiber::Start"))
      << FormatFiberReport({.max_frames = 20});

  channel.writer()->Write(1);
  waiter->Join();
}

FiberSnapshot MakeWaiter(std::uint64_t id, std::uint64_t blocked_on) {
  FiberSnapshot fiber;
  fiber.id = id;
  fiber.kind = WaitKind::kMutex;
  fiber.blocking_fiber_id = blocked_on;
  return fiber;
}

TEST(FindWaitCyclesTest, FindsCyclesAndIgnoresChains) {
  EXPECT_TRUE(FindWaitCycles({MakeWaiter(1, 2), MakeWaiter(2, 0)}).empty());

  // A fiber recorded as blocked on itself is a lost race, not a cycle.
  EXPECT_TRUE(FindWaitCycles({MakeWaiter(1, 1)}).empty());

  const std::vector<std::vector<std::uint64_t>> pair =
      FindWaitCycles({MakeWaiter(1, 2), MakeWaiter(2, 1)});
  ASSERT_EQ(pair.size(), 1u);
  EXPECT_EQ(pair.front().size(), 2u);

  const std::vector<std::vector<std::uint64_t>> triple = FindWaitCycles(
      {MakeWaiter(1, 2), MakeWaiter(2, 3), MakeWaiter(3, 1), MakeWaiter(4, 1)});
  ASSERT_EQ(triple.size(), 1u);
  EXPECT_EQ(triple.front().size(), 3u);

  const std::vector<std::vector<std::uint64_t>> two_cycles = FindWaitCycles(
      {MakeWaiter(1, 2), MakeWaiter(2, 1), MakeWaiter(3, 4), MakeWaiter(4, 3)});
  EXPECT_EQ(two_cycles.size(), 2u);
}

TEST(FiberIntrospectTest, MutexWaitNamesItsHolder) {
  Mutex contended;
  PermanentEvent held;
  PermanentEvent release;
  std::atomic<std::uint64_t> holder_id{0};
  std::atomic<std::uint64_t> blocked_id{0};

  auto holder = NewTree({.name = "mutex-holder"}, [&] {
    holder_id.store(CurrentFiberId(), std::memory_order_release);
    MutexLock lock(&contended);
    held.Notify();
    ASSERT_EQ(Select({release.OnEvent()}), 0);
  });
  ASSERT_EQ(Select({held.OnEvent()}), 0);

  auto blocked = NewTree({.name = "mutex-waiter"}, [&] {
    blocked_id.store(CurrentFiberId(), std::memory_order_release);
    MutexLock lock(&contended);
  });

  const std::uint64_t waiter = blocked_id.load(std::memory_order_acquire);
  const std::vector<FiberSnapshot> snapshot =
      SnapshotUntil([&](const std::vector<FiberSnapshot>& fibers) {
        const std::uint64_t id = blocked_id.load(std::memory_order_acquire);
        const FiberSnapshot* fiber = id == 0 ? nullptr : FindById(fibers, id);
        return fiber != nullptr && fiber->kind == WaitKind::kMutex &&
               fiber->blocking_fiber_id != 0;
      });

  const FiberSnapshot* fiber = FindById(
      snapshot,
      waiter != 0 ? waiter : blocked_id.load(std::memory_order_acquire));
  ASSERT_NE(fiber, nullptr);
  EXPECT_EQ(fiber->kind, WaitKind::kMutex);
  EXPECT_EQ(fiber->blocking_fiber_id, holder_id.load(std::memory_order_acquire))
      << FormatFiberReport({.max_frames = 8});
  EXPECT_EQ(fiber->wait_object, &contended);

  release.Notify();
  holder->Join();
  blocked->Join();
}

TEST(FiberIntrospectTest, NamesParentsAndCreationSitesSurvive) {
  PermanentEvent entered;
  PermanentEvent finish;
  std::atomic<std::uint64_t> child_id{0};
  std::atomic<std::uint64_t> parent_id{0};

  auto root = NewTree({.name = "named-root"}, [&] {
    parent_id.store(CurrentFiberId(), std::memory_order_release);
    auto child = std::make_unique<Fiber>([&] {
      SetCurrentFiberName("renamed-child");
      child_id.store(CurrentFiberId(), std::memory_order_release);
      entered.Notify();
      Select({finish.OnEvent()});
    });
    ASSERT_EQ(Select({entered.OnEvent()}), 0);
    child->Join();
  });
  ASSERT_EQ(Select({entered.OnEvent()}), 0);

  const std::uint64_t child = child_id.load(std::memory_order_acquire);
  const std::vector<FiberSnapshot> snapshot =
      SnapshotUntil([child](const std::vector<FiberSnapshot>& fibers) {
        const FiberSnapshot* fiber = FindById(fibers, child);
        return fiber != nullptr && fiber->kind == WaitKind::kSelect;
      });

  const FiberSnapshot* fiber = FindById(snapshot, child);
  ASSERT_NE(fiber, nullptr);
  EXPECT_EQ(fiber->name, "renamed-child");
  EXPECT_EQ(fiber->parent_id, parent_id.load(std::memory_order_acquire));
  EXPECT_NE(fiber->creation_pc, nullptr);
  EXPECT_NE(fiber->stack_lo, nullptr);
  EXPECT_LT(fiber->stack_lo, fiber->stack_hi);

  const FiberSnapshot* root_fiber =
      FindById(snapshot, parent_id.load(std::memory_order_acquire));
  ASSERT_NE(root_fiber, nullptr);
  EXPECT_EQ(root_fiber->name, "named-root");

  finish.Notify();
  root->Join();
}

TEST(FiberIntrospectTest, ReportsJoinAndSleepWaits) {
  PermanentEvent finish;
  std::atomic<std::uint64_t> joiner_id{0};
  std::atomic<std::uint64_t> sleeper_id{0};
  PermanentEvent both_entered;
  std::atomic<int> entered{0};

  auto joiner = NewTree({.name = "joiner"}, [&] {
    joiner_id.store(CurrentFiberId(), std::memory_order_release);
    auto child = std::make_unique<Fiber>([&] {
      if (entered.fetch_add(1) == 1) {
        both_entered.Notify();
      }
      Select({finish.OnEvent()});
    });
    child->Join();
  });
  auto sleeper = NewTree({.name = "sleeper"}, [&] {
    sleeper_id.store(CurrentFiberId(), std::memory_order_release);
    if (entered.fetch_add(1) == 1) {
      both_entered.Notify();
    }
    thread::SleepFor(absl::Milliseconds(1500));
  });

  ASSERT_EQ(Select({both_entered.OnEvent()}), 0);
  const std::uint64_t join_id = joiner_id.load(std::memory_order_acquire);
  const std::uint64_t sleep_id = sleeper_id.load(std::memory_order_acquire);

  const std::vector<FiberSnapshot> snapshot = SnapshotUntil(
      [join_id, sleep_id](const std::vector<FiberSnapshot>& fibers) {
        const FiberSnapshot* joining = FindById(fibers, join_id);
        const FiberSnapshot* sleeping = FindById(fibers, sleep_id);
        return joining != nullptr && joining->kind == WaitKind::kJoin &&
               sleeping != nullptr && sleeping->kind == WaitKind::kSleep;
      });

  const FiberSnapshot* joining = FindById(snapshot, join_id);
  ASSERT_NE(joining, nullptr);
  EXPECT_EQ(joining->kind, WaitKind::kJoin);
  EXPECT_NE(joining->blocking_fiber_id, 0u);
  EXPECT_EQ(joining->deadline, absl::InfiniteFuture());

  const FiberSnapshot* sleeping = FindById(snapshot, sleep_id);
  ASSERT_NE(sleeping, nullptr);
  EXPECT_EQ(sleeping->kind, WaitKind::kSleep);
  EXPECT_LT(sleeping->deadline, absl::InfiniteFuture());

  finish.Notify();
  joiner->Join();
  sleeper->Join();
}

TEST(FiberIntrospectTest, SnapshotIsSafeWhileFibersChurn) {
  std::atomic<bool> stop{false};
  auto churn = NewTree({.name = "churn"}, [&] {
    while (!stop.load(std::memory_order_acquire)) {
      auto child = std::make_unique<Fiber>(
          [] { thread::SleepFor(absl::Microseconds(50)); });
      child->Join();
    }
  });

  for (int round = 0; round < 200; ++round) {
    const std::vector<FiberSnapshot> snapshot = SnapshotFibers();
    for (const FiberSnapshot& fiber : snapshot) {
      // A raced trace reports no frames; frames and a race are exclusive.
      EXPECT_TRUE(!fiber.trace_raced || fiber.stack.empty()) << fiber.id;
      if (fiber.kind == WaitKind::kRunning) {
        EXPECT_TRUE(fiber.stack.empty()) << fiber.id;
      }
    }
  }
  stop.store(true, std::memory_order_release);
  churn->Join();
}

TEST(FiberIntrospectTest, ReportNamesTheWaitAndTheFiber) {
  PermanentEvent finish;
  PermanentEvent entered;
  auto waiter = NewTree({.name = "reported-fiber"}, [&] {
    entered.Notify();
    Select({finish.OnEvent()});
  });
  ASSERT_EQ(Select({entered.OnEvent()}), 0);
  SnapshotUntil([](const std::vector<FiberSnapshot>& fibers) {
    for (const FiberSnapshot& fiber : fibers) {
      if (fiber.name == "reported-fiber" && fiber.kind == WaitKind::kSelect) {
        return true;
      }
    }
    return false;
  });

  const std::string report = FormatFiberReport({.max_frames = 8});
  EXPECT_TRUE(absl::StrContains(report, "A11 fiber dump")) << report;
  EXPECT_TRUE(absl::StrContains(report, "reported-fiber")) << report;
  EXPECT_TRUE(absl::StrContains(report, "select")) << report;
  EXPECT_TRUE(absl::StrContains(report, "census:")) << report;

  finish.Notify();
  waiter->Join();
}

TEST(FiberIntrospectTest, ProgressCounterAdvancesWithCompletedWaits) {
  const std::uint64_t before = TotalCompletedWaits();
  auto worker = NewTree({}, [] {
    for (int round = 0; round < 8; ++round) {
      thread::SleepFor(absl::Microseconds(100));
    }
  });
  worker->Join();
  EXPECT_GT(TotalCompletedWaits(), before);
}

TEST(FiberIntrospectTest, AnOsThreadsWaitIsReportedAndThenPlaceholderAgain) {
  // An OS thread outside a fiber carries a `kThreadPlaceholder` record. Its
  // scheduler park appears as a wait in the report.
  std::atomic<std::uint64_t> outsider_id{0};
  std::atomic<bool> slept{false};
  std::atomic<bool> stop{false};
  std::thread outsider([&] {
    // Give this thread its placeholder record.
    outsider_id.store(Fiber::Current()->Diagnostics().id,
                      std::memory_order_release);
    thread::SleepFor(absl::Milliseconds(600));
    slept.store(true, std::memory_order_release);
    while (!stop.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  const absl::Time appeared = absl::Now() + absl::Seconds(5);
  while (outsider_id.load(std::memory_order_acquire) == 0 &&
         absl::Now() < appeared) {
    thread::SleepFor(absl::Milliseconds(2));
  }
  const std::uint64_t id = outsider_id.load(std::memory_order_acquire);
  ASSERT_NE(id, 0u);

  const std::vector<FiberSnapshot> waiting =
      SnapshotUntil([id](const std::vector<FiberSnapshot>& fibers) {
        const FiberSnapshot* one = FindById(fibers, id);
        return one != nullptr && one->kind == WaitKind::kSleep;
      });
  const FiberSnapshot* sleeping = FindById(waiting, id);
  ASSERT_NE(sleeping, nullptr);
  EXPECT_EQ(sleeping->kind, WaitKind::kSleep);
  EXPECT_LT(sleeping->deadline, absl::InfiniteFuture());

  // Leaving the wait restores the idle thread marker.
  const std::vector<FiberSnapshot> idle =
      SnapshotUntil([id, &slept](const std::vector<FiberSnapshot>& fibers) {
        const FiberSnapshot* one = FindById(fibers, id);
        return slept.load(std::memory_order_acquire) && one != nullptr &&
               one->kind == WaitKind::kThreadPlaceholder;
      });
  const FiberSnapshot* placeholder = FindById(idle, id);
  ASSERT_NE(placeholder, nullptr);
  EXPECT_EQ(placeholder->kind, WaitKind::kThreadPlaceholder);

  stop.store(true, std::memory_order_release);
  outsider.join();
}

}  // namespace
}  // namespace thread

int main(int argc, char** argv) {
  absl::InitializeLog();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
