// Copyright 2026 The A11 Authors.

#include "a11/concurrency/callback_scheduler.h"

#include <exception>
#include <utility>

#include <absl/log/log.h>

#include "thread/executor.h"

namespace a11::internal {

void CallbackScheduler::Schedule(absl::AnyInvocable<void() &&> callback) {
  if (callback == nullptr) {
    return;
  }
  bool post = false;
  {
    thread::MutexLock lock(&mu_);
    callbacks_.push_back(std::move(callback));
    // A turn already being in flight is not evidence that this callback will be
    // reached: see Run(). So post one whenever the cap allows, without comparing
    // against the queue length -- a suspended turn has already popped its
    // callback, so the queue it left behind is short precisely when nobody is
    // draining it, and any such comparison licenses exactly the stall this is
    // here to prevent.
    //
    // The waste this admits is one Post and one mutex round trip for a turn that
    // finds the queue already drained and returns immediately. That is the right
    // trade: Schedule() is reached from a pump's Wake(), which is already the
    // path taken only when a reader has to wait, while the inline drive in
    // ChunkStoreReader::Next() serves everything it can without coming here.
    if (active_turns_ < max_concurrent_turns_) {
      ++active_turns_;
      post = true;
    }
  }
  if (post) {
    thread::Post([this] { Run(); });
  }
}

// Callbacks queued here are state-machine continuations -- a store reader or
// writer pump drive -- and **a pump drive can suspend**: InlinePumpState
// documents that a store write tees to an attached stream and WireStream::Send
// parks on the peer's fibre-aware mutex. A suspended callback suspends the turn
// running it.
//
// So a single turn is not enough. While one flag said "a turn exists, therefore
// the queue is draining", a callback that parked stalled every *other* reader's
// wake in the process behind it: Schedule() saw the flag set and posted nothing,
// and nothing drained the queue until the parked fibre resumed -- which, if it
// was waiting on something whose progress needed a queued callback, was never.
// This is also a serial path by construction: every store reader in the process
// funnels its wakes through this one queue.
//
// **Honest status: reasoned, not reproduced.** This was changed while chasing a
// native echo server that lost calls under load, and it is *not* what that was --
// that turned out to be the benchmark's own client binding `bind_stream` on a
// caller's output port, so each reply was echoed back and the connection was
// corrupted. Raising the cap changed nothing there. It is fixed here on its own
// merits: a suspendable callback behind a one-deep gate is a deadlock waiting for
// a caller, and no test covers it yet.
void CallbackScheduler::Run() {
  size_t completed = 0;
  while (completed < max_callbacks_per_turn_) {
    absl::AnyInvocable<void() &&> callback;
    {
      thread::MutexLock lock(&mu_);
      if (callbacks_.empty()) {
        --active_turns_;
        return;
      }
      callback = std::move(callbacks_.front());
      callbacks_.pop_front();
    }
    // Every callback queued here is A11's own state-machine continuation, and
    // the queue takes them by value from Post() -- so there is nothing to wrap
    // at adoption and nothing that can throw. A caller's callable reaches a
    // state machine through Submit or a WireStream callback, both of which are
    // guarded where they are adopted.
    std::move(callback)();
    ++completed;
  }

  // Hand the next turn back to the worker pool, keeping this turn's slot in
  // `active_turns_` -- the turn continues, on a fresh frame. Releasing the slot
  // here and reacquiring it would let the count drop to zero with the queue
  // non-empty, which is the state Schedule() reads to decide whether anyone is
  // coming.
  thread::Post([this] { Run(); });
}

}  // namespace a11::internal
