// Copyright 2026 The A11 Authors.

// A process that deadlocks its fibers, for exercising the tooling in
// thread/introspect.h against a hang a debugger cannot otherwise read.
//
//   fiber_deadlock_demo mutex-cycle    two fibers, two mutexes, opposite order
//   fiber_deadlock_demo orphan-select  fibers parked on a channel nobody writes
//   fiber_deadlock_demo join-cycle     a fiber joining a child that never ends
//
// Then, from another terminal:
//
//   kill -USR2 <pid>                    prints a report
//   lldb -p <pid> -o 'command script import scripts/a11_fibers.py' \
//        -o 'a11-fibers'
//
// Or let the process report on itself:
//
//   A11_FIBER_WATCHDOG=3 fiber_deadlock_demo mutex-cycle

#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include <absl/log/initialize.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "thread/concurrency.h"
#include "thread/introspect.h"

namespace {

// Two fibers take two mutexes in opposite orders. Unrecoverable;
// FindWaitCycles() names both fibers and the mutexes they hold.
void MutexCycle() {
  static thread::Mutex first;
  static thread::Mutex second;
  static thread::PermanentEvent first_held;
  static thread::PermanentEvent second_held;

  thread::Detach({.name = "cycle-left"}, [] {
    thread::MutexLock lock(&first);
    first_held.Notify();
    thread::Select({second_held.OnEvent()});
    thread::MutexLock inner(&second);
  });
  thread::Detach({.name = "cycle-right"}, [] {
    thread::MutexLock lock(&second);
    second_held.Notify();
    thread::Select({first_held.OnEvent()});
    thread::MutexLock inner(&first);
  });
}

// The shape a condition-variable deadlock takes: several fibers parked on one
// object with no producer left. There is no cycle to find; the report's
// grouping by wait object names it.
void OrphanSelect() {
  static thread::Channel<int> channel(0);
  for (int index = 0; index < 3; ++index) {
    thread::Detach({.name = "orphan-reader"}, [] {
      int value = 0;
      bool ok = false;
      thread::Select({channel.reader()->OnRead(&value, &ok)});
    });
  }
}

void JoinCycle() {
  thread::Detach({.name = "join-parent"}, [] {
    auto child = std::make_unique<thread::Fiber>([] {
      static thread::PermanentEvent never;
      thread::Select({never.OnEvent()});
    });
    child->Join();
  });
}

}  // namespace

int main(int argc, char** argv) {
  absl::InitializeLog();
  const std::string_view scenario = argc > 1 ? argv[1] : "mutex-cycle";

  if (scenario == "mutex-cycle") {
    MutexCycle();
  } else if (scenario == "orphan-select") {
    OrphanSelect();
  } else if (scenario == "join-cycle") {
    JoinCycle();
  } else {
    std::fprintf(stderr, "usage: %s [mutex-cycle|orphan-select|join-cycle]\n",
                 argv[0]);
    return 2;
  }

  thread::InstallFiberDumpSignalHandler();
  std::fprintf(stderr, "deadlocked; kill -USR2 %d for a report\n",
               static_cast<int>(getpid()));

  // Give the fibers time to reach their waits, then report once from inside
  // the process before waiting for a signal or the watchdog.
  thread::SleepFor(absl::Milliseconds(200));
  std::fputs(thread::FormatFiberReport({.max_frames = 12}).c_str(), stderr);

  while (true) {
    thread::SleepFor(absl::Seconds(1));
  }
}
