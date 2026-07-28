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

namespace thread {
namespace {

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
  for (int attempt = 0; attempt < 100 && !detached_capture.expired();
       ++attempt) {
    thread::SleepFor(absl::Microseconds(100));
  }
  EXPECT_TRUE(detached_capture.expired());
  EXPECT_EQ(internal::LiveFiberCountForTesting(), baseline);
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
  EXPECT_EQ(*candidate, 2);

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
