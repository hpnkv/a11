// Copyright 2026 The A11 Authors.

#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "thread/concurrency.h"
#include "thread/executor.h"
#include "thread/internal/work_queue.h"

namespace thread {
namespace {

// Small enough that every test here can fill it and exercise the overflow path.
using TestQueue = internal::WorkQueue<int, 4>;

TEST(WorkQueueTest, PreservesOrderThroughAndPastTheRing) {
  TestQueue queue;
  int value = 0;
  EXPECT_FALSE(queue.Pop(value));

  // Twice the ring, so the second half arrives while the first is still in it
  // and has to come out behind it.
  for (int index = 0; index < 8; ++index) {
    queue.Push(index);
  }
  for (int index = 0; index < 8; ++index) {
    ASSERT_TRUE(queue.Pop(value)) << index;
    EXPECT_EQ(value, index);
  }
  EXPECT_FALSE(queue.Pop(value));

  // Draining the overflow must leave the ring usable rather than stuck one lap
  // behind.
  for (int round = 0; round < 100; ++round) {
    queue.Push(round);
    ASSERT_TRUE(queue.Pop(value));
    EXPECT_EQ(value, round);
  }
}

TEST(WorkQueueTest, MovesMoveOnlyPayloadsExactlyOnce) {
  internal::WorkQueue<std::unique_ptr<int>, 4> queue;
  for (int index = 0; index < 8; ++index) {
    queue.Push(std::make_unique<int>(index));
  }
  for (int index = 0; index < 8; ++index) {
    std::unique_ptr<int> taken;
    ASSERT_TRUE(queue.Pop(taken));
    ASSERT_NE(taken, nullptr);
    EXPECT_EQ(*taken, index);
  }
}

TEST(WorkQueueTest, LosesNothingUnderConcurrentPushersAndPoppers) {
  // Smaller than the traffic so the ring is full for much of the
  // run and both paths are exercised together.
  internal::WorkQueue<int, 64> queue;
  constexpr int kPushers = 4;
  constexpr int kPoppers = 3;
  constexpr int kPerPusher = 20000;

  std::atomic<bool> done{false};
  std::atomic<std::int64_t> popped_count{0};
  std::atomic<std::int64_t> popped_sum{0};
  std::vector<std::thread> threads;
  threads.reserve(kPushers + kPoppers);

  for (int popper = 0; popper < kPoppers; ++popper) {
    threads.emplace_back([&] {
      int value = 0;
      while (true) {
        if (queue.Pop(value)) {
          popped_sum.fetch_add(value, std::memory_order_relaxed);
          popped_count.fetch_add(1, std::memory_order_relaxed);
        } else if (done.load(std::memory_order_acquire)) {
          // One last look: an item may have arrived between the failed pop and
          // the flag.
          while (queue.Pop(value)) {
            popped_sum.fetch_add(value, std::memory_order_relaxed);
            popped_count.fetch_add(1, std::memory_order_relaxed);
          }
          return;
        }
      }
    });
  }
  for (int pusher = 0; pusher < kPushers; ++pusher) {
    threads.emplace_back([&, pusher] {
      for (int index = 0; index < kPerPusher; ++index) {
        queue.Push(pusher * kPerPusher + index);
      }
    });
  }

  for (size_t index = kPoppers; index < kPoppers + kPushers; ++index) {
    threads[index].join();
  }
  done.store(true, std::memory_order_release);
  for (size_t index = 0; index < kPoppers; ++index) {
    threads[index].join();
  }

  constexpr std::int64_t kTotal = std::int64_t{kPushers} * kPerPusher;
  EXPECT_EQ(popped_count.load(std::memory_order_relaxed), kTotal);
  // Every value once and only once: a duplicated or dropped item shows up here
  // even when the count happens to come out right.
  EXPECT_EQ(popped_sum.load(std::memory_order_relaxed),
            kTotal * (kTotal - 1) / 2);
}

TEST(WorkQueueTest, PreservesEachPushersOrderUnderOneConsumer) {
  internal::WorkQueue<int, 8> queue;
  constexpr int kPushers = 3;
  constexpr int kPerPusher = 5000;

  std::atomic<bool> done{false};
  std::vector<int> last(kPushers, -1);
  std::vector<int> seen(kPushers, 0);
  std::thread consumer([&] {
    int value = 0;
    while (true) {
      if (queue.Pop(value)) {
        const auto pusher = static_cast<size_t>(value / kPerPusher);
        // Within one pusher's stream the queue must not reorder: this is what a
        // Chase-Lev deque would give up, and what parts of A11 rely on.
        ASSERT_GT(value, last[pusher]);
        last[pusher] = value;
        ++seen[pusher];
      } else if (done.load(std::memory_order_acquire)) {
        while (queue.Pop(value)) {
          const auto pusher = static_cast<size_t>(value / kPerPusher);
          ASSERT_GT(value, last[pusher]);
          last[pusher] = value;
          ++seen[pusher];
        }
        return;
      }
    }
  });

  std::vector<std::thread> pushers;
  pushers.reserve(kPushers);
  for (int pusher = 0; pusher < kPushers; ++pusher) {
    pushers.emplace_back([&, pusher] {
      for (int index = 0; index < kPerPusher; ++index) {
        queue.Push(pusher * kPerPusher + index);
      }
    });
  }
  for (std::thread& pusher : pushers) {
    pusher.join();
  }
  done.store(true, std::memory_order_release);
  consumer.join();
  for (size_t pusher = 0; pusher < kPushers; ++pusher) {
    EXPECT_EQ(seen[pusher], kPerPusher) << pusher;
  }
}

TEST(ThreadMutexTest, ConditionVariableHandsOffBetweenFiberAndThread) {
  thread::Mutex mu;
  thread::CondVar cv;
  bool ready = false;
  bool observed = false;

  auto waiter = NewTree({}, [&]() {
    thread::MutexLock lock(&mu);
    while (!ready) {
      cv.Wait(&mu);
    }
    observed = true;
  });

  std::thread notifier([&]() {
    thread::MutexLock lock(&mu);
    ready = true;
    cv.SignalAll();
  });
  notifier.join();
  waiter->Join();
  EXPECT_TRUE(observed);
}

TEST(ThreadMutexTest, TimedWaitReportsTimeoutAndRetainsLock) {
  thread::Mutex mu;
  thread::CondVar cv;
  thread::MutexLock lock(&mu);
  const absl::Time start = absl::Now();
  EXPECT_TRUE(cv.WaitWithTimeout(&mu, absl::Milliseconds(2)));
  EXPECT_GE(absl::Now() - start, absl::Milliseconds(1));
}

TEST(ThreadSelectTest, SelectsReadyCaseAndTimesOutWithoutLeavingWaiters) {
  PermanentEvent event;
  EXPECT_EQ(Select({AlwaysSelectableCase(), event.OnEvent()}), 0);

  const absl::Time start = absl::Now();
  EXPECT_EQ(SelectUntil(absl::Now() + absl::Milliseconds(2), {event.OnEvent()}),
            -1);
  EXPECT_GE(absl::Now() - start, absl::Milliseconds(1));

  // Repeated registration and timeout exercises intrusive-list cleanup. The
  // event destructor also asserts that no waiter remains linked.
  for (int iteration = 0; iteration < 500; ++iteration) {
    EXPECT_EQ(SelectUntil(absl::InfinitePast(), {event.OnEvent()}), -1);
  }
  event.Notify();
  EXPECT_EQ(Select({event.OnEvent()}), 0);
}

TEST(ThreadSelectTest, NotificationWinsAConcurrentTimedSelection) {
  for (int iteration = 0; iteration < 100; ++iteration) {
    PermanentEvent event;
    std::atomic<int> selected{-2};
    auto waiter = NewTree({}, [&]() {
      selected.store(
          SelectUntil(absl::Now() + absl::Seconds(1), {event.OnEvent()}),
          std::memory_order_release);
    });
    thread::SleepFor(absl::Microseconds(10));
    event.Notify();
    waiter->Join();
    EXPECT_EQ(selected.load(std::memory_order_acquire), 0);
  }
}

TEST(ThreadSelectTest, CancellationUnregistersOtherCasesAndReleasesFibers) {
  (void)Fiber::Current();
  const size_t baseline = internal::LiveFiberCountForTesting();

  for (int iteration = 0; iteration < 100; ++iteration) {
    PermanentEvent entered;
    PermanentEvent unrelated;
    std::atomic<int> selected{-1};
    auto waiter = NewTree({}, [&]() {
      entered.Notify();
      selected.store(Select({OnCancel(), unrelated.OnEvent()}),
                     std::memory_order_release);
    });
    EXPECT_EQ(Select({entered.OnEvent()}), 0);
    thread::SleepFor(absl::Microseconds(10));
    waiter->Cancel();
    waiter->Join();
    EXPECT_EQ(selected.load(std::memory_order_acquire), 0);

    // This must not encounter the selector state destroyed by the cancelled
    // Select call. PermanentEvent's destructor also verifies list cleanup.
    unrelated.Notify();
  }
  EXPECT_EQ(internal::LiveFiberCountForTesting(), baseline);
}

TEST(ThreadSelectTest, ChannelTimeoutsLeaveNoWaiters) {
  Channel<int> channel(0);
  int value = 0;
  bool ok = false;
  for (int iteration = 0; iteration < 500; ++iteration) {
    EXPECT_EQ(SelectUntil(absl::InfinitePast(),
                          {channel.reader()->OnRead(&value, &ok)}),
              -1);
  }

  auto writer = NewTree({}, [&]() { channel.writer()->Write(42); });
  EXPECT_EQ(Select({channel.reader()->OnRead(&value, &ok)}), 0);
  writer->Join();
  EXPECT_TRUE(ok);
  EXPECT_EQ(value, 42);
  channel.writer()->Close();
}

TEST(ThreadFiberTest, CancellationPropagatesAcrossMultipleChildren) {
  Channel<int> started(2);
  std::atomic<int> cancelled_children{0};

  auto root = NewTree({}, [&]() {
    Fiber first([&]() {
      started.writer()->Write(1);
      EXPECT_EQ(Select({OnCancel()}), 0);
      cancelled_children.fetch_add(1, std::memory_order_relaxed);
    });
    Fiber second([&]() {
      started.writer()->Write(2);
      EXPECT_EQ(Select({OnCancel()}), 0);
      cancelled_children.fetch_add(1, std::memory_order_relaxed);
    });
    first.Join();
    second.Join();
  });

  int value = 0;
  ASSERT_TRUE(started.reader()->Read(&value));
  ASSERT_TRUE(started.reader()->Read(&value));
  root->Cancel();
  root->Cancel();  // Cancellation is idempotent.
  root->Join();
  EXPECT_EQ(cancelled_children.load(std::memory_order_relaxed), 2);
}

TEST(ThreadFiberTest, CancelInterruptsBlockedChannelOperation) {
  Channel<int> channel(0);
  std::atomic<bool> write_result{true};
  auto writer = NewTree({}, [&]() {
    write_result.store(channel.writer()->WriteUnlessCancelled(42),
                       std::memory_order_release);
  });
  thread::SleepFor(absl::Milliseconds(1));
  writer->Cancel();
  writer->Join();
  EXPECT_FALSE(write_result.load(std::memory_order_acquire));
  channel.writer()->Close();
}

TEST(ThreadFiberTest, JoinedAndDetachedFibersReleaseCapturedState) {
  (void)Fiber::Current();
  const size_t baseline = internal::LiveFiberCountForTesting();

  std::weak_ptr<int> joined_capture;
  {
    auto capture = std::make_shared<int>(7);
    joined_capture = capture;
    auto fiber = NewTree({}, [capture]() { EXPECT_EQ(*capture, 7); });
    capture.reset();
    fiber->Join();
  }
  EXPECT_TRUE(joined_capture.expired());
  EXPECT_EQ(internal::LiveFiberCountForTesting(), baseline);

  PermanentEvent finished;
  std::weak_ptr<int> detached_capture;
  {
    auto capture = std::make_shared<int>(9);
    detached_capture = capture;
    Detach({}, [capture, &finished]() {
      EXPECT_EQ(*capture, 9);
      finished.Notify();
    });
    capture.reset();
  }
  EXPECT_EQ(Select({finished.OnEvent()}), 0);
  // Two things have to happen after the work returns and they happen in order:
  // the closure is destroyed, releasing the capture, and then the Fiber itself
  // is reaped by whichever pool worker next comes round (see.
  for (int attempt = 0; attempt < 100 && !detached_capture.expired();
       ++attempt) {
    thread::SleepFor(absl::Microseconds(100));
  }
  EXPECT_TRUE(detached_capture.expired());
  for (int attempt = 0;
       attempt < 100 && internal::LiveFiberCountForTesting() != baseline;
       ++attempt) {
    thread::SleepFor(absl::Microseconds(100));
  }
  EXPECT_EQ(internal::LiveFiberCountForTesting(), baseline);
}

TEST(ThreadFiberTest, PlaceholderFibersRetireCleanlyOnThreadExit) {
  // A thread that touches the fiber API without being a fiber gets a per-thread
  // placeholder from Fiber::Current().
  (void)Fiber::Current();
  const size_t baseline = internal::LiveFiberCountForTesting();
  const size_t before_created = internal::CreatedFiberCountForTesting();

  constexpr int kThreads = 32;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([] {
      // Adopt the per-thread placeholder and take a fiber-aware lock, so the
      // thread has exercised Boost's thread_local scheduler before it exits.
      EXPECT_NE(Fiber::Current(), nullptr);
      thread::Mutex mu;
      thread::MutexLock lock(&mu);
    });
  }
  for (std::thread& worker : threads) {
    worker.join();
  }

  EXPECT_EQ(internal::LiveFiberCountForTesting(), baseline);
  EXPECT_GE(internal::CreatedFiberCountForTesting() - before_created,
            static_cast<size_t>(kThreads));
}

TEST(ThreadSleepTest, SleepSuspendsOnlyTheCallingFiber) {
  PermanentEvent sleeper_started;
  PermanentEvent peer_ran;
  const absl::Time start = absl::Now();

  auto sleeper = NewTree({}, [&]() {
    sleeper_started.Notify();
    thread::SleepFor(absl::Milliseconds(5));
    EXPECT_TRUE(peer_ran.HasBeenNotified());
  });
  auto peer = NewTree({}, [&]() {
    Select({sleeper_started.OnEvent()});
    peer_ran.Notify();
  });

  sleeper->Join();
  peer->Join();
  EXPECT_GE(absl::Now() - start, absl::Milliseconds(4));
}

TEST(ThreadSleepTest, NonpositiveSleepCooperativelyYields) {
  PermanentEvent peer_ran;
  auto peer = NewTree({}, [&]() { peer_ran.Notify(); });

  for (int attempt = 0; attempt < 100 && !peer_ran.HasBeenNotified();
       ++attempt) {
    thread::SleepFor(absl::ZeroDuration());
  }
  peer->Join();
  EXPECT_TRUE(peer_ran.HasBeenNotified());
}

TEST(ThreadExecutorTest, StacklessCallbacksAndTimersDoNotAllocateFibers) {
  (void)Fiber::Current();
  const size_t baseline = internal::LiveFiberCountForTesting();
  thread::Mutex mu;
  thread::CondVar cv;
  bool immediate_done = false;
  bool timer_done = false;
  std::atomic<bool> callback_had_fiber{true};
  const absl::Time start = absl::Now();

  Post([&] {
    callback_had_fiber.store(GetPerThreadFiberPtr() != nullptr,
                             std::memory_order_release);
    thread::MutexLock lock(&mu);
    immediate_done = true;
    cv.SignalAll();
  });
  PostAfter(absl::Milliseconds(5), [&] {
    thread::MutexLock lock(&mu);
    timer_done = true;
    cv.SignalAll();
  });

  thread::MutexLock lock(&mu);
  const absl::Time deadline = absl::Now() + absl::Seconds(5);
  while (!immediate_done || !timer_done) {
    ASSERT_FALSE(cv.WaitWithDeadline(&mu, deadline));
  }
  EXPECT_FALSE(callback_had_fiber.load(std::memory_order_acquire));
  EXPECT_GE(absl::Now() - start, absl::Milliseconds(4));
  EXPECT_EQ(internal::LiveFiberCountForTesting(), baseline);
}

// Each of these timers is registered while the pool is parked for a later one,
// so each has to shorten somebody's park to run on time.
TEST(ThreadExecutorTest, TimersRegisteredInDecreasingOrderRunOnTime) {
  // Let the pool go quiet, so that its workers are parked rather than looking
  // for work when the timers below are registered.
  absl::SleepFor(absl::Milliseconds(50));

  // A far deadline first, so that a worker parking afterwards arms for that and
  // not for anything sooner.
  PostAfter(absl::Seconds(30), [] {});

  thread::Mutex mu;
  thread::CondVar cv;
  // Each entry becomes the earliest timer in turn, so each has to shorten a
  // park that was armed for the one before it.
  const std::vector<absl::Duration> delays = {
      absl::Milliseconds(120), absl::Milliseconds(60), absl::Milliseconds(10)};
  for (const absl::Duration delay : delays) {
    const absl::Time due = absl::Now() + delay;
    absl::Duration lateness;
    bool fired = false;
    PostAt(due, [&] {
      thread::MutexLock lock(&mu);
      lateness = absl::Now() - due;
      fired = true;
      cv.SignalAll();
    });

    thread::MutexLock lock(&mu);
    const absl::Time give_up = absl::Now() + absl::Seconds(5);
    while (!fired) {
      ASSERT_FALSE(cv.WaitWithDeadline(&mu, give_up));
    }
    EXPECT_LT(lateness, absl::Milliseconds(25)) << "delay " << delay;
  }
}

TEST(ThreadExecutorTest, ASchedulerParkDropsAndRestoresTheHostLock) {
  // CPython uses this guard to release the GIL around a scheduler park. The
  // shared state outlives calls already inside the installed guard.
  struct Counts {
    std::atomic<int> released{0};
    std::atomic<int> acquired{0};
    std::atomic<void*> handed{nullptr};
  };
  const auto counts = std::make_shared<Counts>();
  SetSchedulerParkGuard(SchedulerParkGuard{
      .release =
          [counts]() -> void* {
            counts->released.fetch_add(1, std::memory_order_relaxed);
            return reinterpret_cast<void*>(0x5eed);
          },
      .acquire =
          [counts](void* held) {
            counts->handed.store(held, std::memory_order_relaxed);
            counts->acquired.fetch_add(1, std::memory_order_relaxed);
          },
  });

  PermanentEvent finish;
  auto blocked = NewTree({.name = "parks-its-thread"}, [&] {
    Select({finish.OnEvent()});
  });

  const absl::Time deadline = absl::Now() + absl::Seconds(5);
  while (counts->released.load(std::memory_order_relaxed) == 0 &&
         absl::Now() < deadline) {
    thread::SleepFor(absl::Milliseconds(5));
  }
  finish.Notify();
  blocked->Join();
  SetSchedulerParkGuard(SchedulerParkGuard{});

  EXPECT_GT(counts->released.load(std::memory_order_relaxed), 0);
  // Active parks may leave more releases than acquires at this instant.
  EXPECT_GT(counts->acquired.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(counts->handed.load(std::memory_order_relaxed),
            reinterpret_cast<void*>(0x5eed));
}

TEST(ThreadExecutorTest, ParksRunWithNoGuardInstalled) {
  SetSchedulerParkGuard(SchedulerParkGuard{});
  PermanentEvent finish;
  auto blocked = NewTree({}, [&] { Select({finish.OnEvent()}); });
  thread::SleepFor(absl::Milliseconds(80));
  finish.Notify();
  blocked->Join();
}

TEST(ThreadChannelTest, PreservesFifoAndCloseWakesReader) {
  Channel<int> channel(2);
  channel.writer()->Write(10);
  channel.writer()->Write(20);

  int value = 0;
  ASSERT_TRUE(channel.reader()->Read(&value));
  EXPECT_EQ(value, 10);
  ASSERT_TRUE(channel.reader()->Read(&value));
  EXPECT_EQ(value, 20);

  std::atomic<bool> read_ok{true};
  auto reader = NewTree({}, [&]() {
    int ignored = 0;
    read_ok.store(channel.reader()->Read(&ignored), std::memory_order_release);
  });
  thread::SleepFor(absl::Milliseconds(1));
  channel.writer()->Close();
  reader->Join();
  EXPECT_FALSE(read_ok.load(std::memory_order_acquire));
}

TEST(ThreadChannelTest, ConcurrentProducersAndConsumerDoNotDeadlock) {
  constexpr int kProducerCount = 4;
  constexpr int kValuesPerProducer = 250;
  Channel<int> channel(8);
  std::atomic<int> sum{0};

  auto consumer = NewTree({}, [&]() {
    for (int count = 0; count < kProducerCount * kValuesPerProducer; ++count) {
      int value = 0;
      ASSERT_TRUE(channel.reader()->Read(&value));
      sum.fetch_add(value, std::memory_order_relaxed);
    }
  });

  std::vector<std::unique_ptr<Fiber>> producers;
  producers.reserve(kProducerCount);
  for (int producer = 0; producer < kProducerCount; ++producer) {
    producers.push_back(NewTree({}, [&channel]() {
      for (int value = 0; value < kValuesPerProducer; ++value) {
        channel.writer()->Write(1);
      }
    }));
  }
  for (auto& producer : producers) {
    producer->Join();
  }
  consumer->Join();
  channel.writer()->Close();
  EXPECT_EQ(sum.load(std::memory_order_relaxed),
            kProducerCount * kValuesPerProducer);
}

TEST(ThreadChannelTest, LosingWriteCaseDoesNotMovePayload) {
  Channel<std::unique_ptr<int>> channel(1);
  channel.writer()->Write(std::make_unique<int>(1));

  auto candidate = std::make_unique<int>(2);
  EXPECT_EQ(Select({AlwaysSelectableCase(),
                    channel.writer()->OnWrite(std::move(candidate))}),
            0);
  ASSERT_NE(candidate, nullptr);
  // Reading it back after the move is the contract this test exists to pin: a
  // case that loses the Select does not consume its payload.
  EXPECT_EQ(*candidate, 2);  // NOLINT(bugprone-use-after-move)

  std::unique_ptr<int> value;
  ASSERT_TRUE(channel.reader()->Read(&value));
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(*value, 1);

  EXPECT_EQ(Select({channel.writer()->OnWrite(std::move(candidate))}), 0);
  EXPECT_EQ(candidate, nullptr);
  ASSERT_TRUE(channel.reader()->Read(&value));
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(*value, 2);
  channel.writer()->Close();
}

}  // namespace
}  // namespace thread
