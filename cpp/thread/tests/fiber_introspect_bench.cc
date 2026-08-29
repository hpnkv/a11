// Copyright 2026 The A11 Authors.

// What the instrumentation in thread/introspect.h costs. Run it with owner
// tracking on and off; the numbers in
// doc/docs/guides/debugging-concurrency.md come from here.
//
//   fiber_introspect_bench
//   A11_FIBER_OWNER_TRACKING=0 fiber_introspect_bench

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include <absl/log/initialize.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "thread/concurrency.h"
#include "thread/introspect.h"

namespace {

void Report(const char* name, std::int64_t operations, absl::Duration elapsed) {
  const double nanos = static_cast<double>(absl::ToInt64Nanoseconds(elapsed)) /
                       static_cast<double>(operations);
  std::printf("%-34s %10lld ops  %8.1f ns/op\n", name,
              static_cast<long long>(operations), nanos);
}

// The path owner tracking touches: one relaxed load, one relaxed store and a
// context::active() call on top of an uncontended boost::fibers::mutex.
void UncontendedLock() {
  constexpr std::int64_t kOperations = 20000000;
  thread::Mutex mu;
  const absl::Time start = absl::Now();
  for (std::int64_t index = 0; index < kOperations; ++index) {
    mu.Lock();
    mu.Unlock();
  }
  Report("uncontended lock/unlock", kOperations, absl::Now() - start);
}

// A blocking path, where the wait record is amortised against a context switch.
void ChannelPingPong() {
  constexpr std::int64_t kRounds = 200000;
  thread::Channel<int> there(0);
  thread::Channel<int> back(0);
  auto responder = thread::NewTree({.name = "bench-responder"}, [&] {
    int value = 0;
    bool ok = false;
    while (true) {
      if (thread::Select({there.reader()->OnRead(&value, &ok)}) != 0 || !ok) {
        return;
      }
      back.writer()->Write(value);
    }
  });

  const absl::Time start = absl::Now();
  for (std::int64_t index = 0; index < kRounds; ++index) {
    there.writer()->Write(1);
    int value = 0;
    bool ok = false;
    thread::Select({back.reader()->OnRead(&value, &ok)});
  }
  const absl::Duration elapsed = absl::Now() - start;
  there.writer()->Close();
  responder->Join();
  Report("channel round trip", kRounds, elapsed);
}

void FiberCreateJoin() {
  constexpr std::int64_t kFibers = 200000;
  const absl::Time start = absl::Now();
  auto driver = thread::NewTree({.name = "bench-driver"}, [&] {
    for (std::int64_t index = 0; index < kFibers; ++index) {
      auto child = std::make_unique<thread::Fiber>([] {});
      child->Join();
    }
  });
  driver->Join();
  Report("fiber create + join", kFibers, absl::Now() - start);
}

void SnapshotCost() {
  constexpr std::int64_t kSnapshots = 2000;
  std::vector<std::unique_ptr<thread::Fiber>> parked;
  thread::PermanentEvent finish;
  auto host = thread::NewTree({.name = "bench-host"}, [&] {
    std::vector<std::unique_ptr<thread::Fiber>> children;
    for (int index = 0; index < 64; ++index) {
      children.push_back(std::make_unique<thread::Fiber>(
          [&] { thread::Select({finish.OnEvent()}); }));
    }
    thread::Select({finish.OnEvent()});
    for (auto& child : children) {
      child->Join();
    }
  });
  thread::SleepFor(absl::Milliseconds(50));

  absl::Time start = absl::Now();
  for (std::int64_t index = 0; index < kSnapshots; ++index) {
    (void)thread::SnapshotFibers(0);
  }
  Report("snapshot, 65 fibers, no unwind", kSnapshots, absl::Now() - start);

  start = absl::Now();
  for (std::int64_t index = 0; index < kSnapshots; ++index) {
    (void)thread::SnapshotFibers(24);
  }
  Report("snapshot, 65 fibers, unwound", kSnapshots, absl::Now() - start);

  finish.Notify();
  host->Join();
}

}  // namespace

int main() {
  absl::InitializeLog();
  std::printf("A11_FIBER_OWNER_TRACKING=%s\n",
              thread::internal::OwnerTrackingEnabled() ? "1" : "0");
  UncontendedLock();
  ChannelPingPong();
  FiberCreateJoin();
  SnapshotCost();
  return 0;
}
