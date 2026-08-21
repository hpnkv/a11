// Copyright 2026 The Action Engine Authors.
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

#include "thread/fiber.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <absl/base/no_destructor.h>
#include <absl/log/check.h>

#include "thread/boost_primitives.h"
#include "thread/select.h"
#include "thread/thread_pool.h"

namespace thread {
namespace {

std::atomic<size_t> live_fibers = 0;
std::atomic<size_t> created_fibers = 0;

}  // namespace

namespace internal {

size_t LiveFiberCountForTesting() {
  return live_fibers.load(std::memory_order_acquire);
}

size_t CreatedFiberCountForTesting() {
  return created_fibers.load(std::memory_order_acquire);
}

}  // namespace internal

bool IsFiberDetached(const Fiber* absl_nonnull fiber) {
  return ABSL_TS_UNCHECKED_READ(fiber->detached_)
      .load(std::memory_order_relaxed);
}

struct ThreadLocalFiber {
  std::unique_ptr<Fiber> f;

  ~ThreadLocalFiber() {
    // This destructor is called while destroying thread-local storage. If it is
    // null, there is no dynamic fiber for this thread.
    DVLOG(2) << "PerThreadDynamicFiber destructor called: " << f.get();
    if (f != nullptr) {
      // Retire the placeholder without touching the fiber scheduler: at this
      // point the thread's TLS is being torn down, so the normal join path
      // (MarkFinished()/InternalJoin() -> fiber-aware Mutex/Select) would
      // dereference Boost's thread_local active-context through already-freed
      // TLS. See RetireUnstarted().
      f->RetireUnstarted();
      f.reset();
    }
  }
};

static thread_local ThreadLocalFiber kPerThreadNoOpFiber;

Fiber::Fiber(Unstarted, InvocableWork work, Fiber* parent)
    : work_(std::move(work)),
      parent_(parent),
      tree_options_(parent->tree_options_),
      next_sibling_(this),
      prev_sibling_(this) {
  ConstructBoostState();
  live_fibers.fetch_add(1, std::memory_order_relaxed);
  created_fibers.fetch_add(1, std::memory_order_relaxed);
  // Note: We become visible to cancellation as soon as we're added to parent.
  thread::MutexLock lock(&parent_->mu_);
  CHECK_EQ(parent_->state_, RUNNING);
  parent->PushBackChild(this);
  if (parent_->cancellation_.HasBeenNotified()) {
    // Fibers adjoined to a cancelled tree inherit implicit cancellation.
    DVLOG(2) << "F " << this << " joining cancelled sub-tree.";
    Cancel();
  }
}

Fiber::Fiber(Unstarted, InvocableWork work, TreeOptions&& tree_options)
    : work_(std::move(work)),
      parent_(nullptr),
      tree_options_(tree_options),
      next_sibling_(this),
      prev_sibling_(this) {
  ConstructBoostState();
  live_fibers.fetch_add(1, std::memory_order_relaxed);
  created_fibers.fetch_add(1, std::memory_order_relaxed);
}

Fiber::~Fiber() {
  CHECK_EQ(JOINED, state_) << "F " << this << " attempting to destroy an "
                           << "unjoined Fiber.  (Did you forget to Join() "
                           << "on a child?)";
  DCHECK(first_child_ == nullptr);

  DVLOG(2) << "F " << this << " destroyed";
  DestroyBoostState();
  live_fibers.fetch_sub(1, std::memory_order_release);
}

Fiber* absl_nullable GetPerThreadFiberPtr() {
  if (Fiber* fiber = internal::GetScheduledFiberPtr();
      ABSL_PREDICT_TRUE(fiber != nullptr)) {
    return fiber;
  }

  // Otherwise, return the thread-local no-op fiber (not caring if it has been
  // created or not).
  return kPerThreadNoOpFiber.f.get();
}

Fiber* absl_nonnull Fiber::Current() {
  if (Fiber* current_fiber = GetPerThreadFiberPtr();
      ABSL_PREDICT_TRUE(current_fiber != nullptr)) {
    return current_fiber;
  }

  // We only reach here if we're 1) not under any Fiber, 2) this thread does not
  // yet have a thread-local fiber. We can (and should) create and return it.
  struct MakeUniqueEnabler final : Fiber {
    MakeUniqueEnabler() : Fiber(Unstarted{}, InvocableWork(), TreeOptions{}) {}
  };

  kPerThreadNoOpFiber.f = std::make_unique<MakeUniqueEnabler>();
  DVLOG(2) << "Current() called (new static thread-local fiber created): "
           << kPerThreadNoOpFiber.f.get();

  return kPerThreadNoOpFiber.f.get();
}

void Fiber::Join() {
  // Join must be externally called and so can never be valid when detached.  It
  // is important to detect this since we may not safely proceed beyond Select()
  // in this case.
  DCHECK(!IsFiberDetached(this)) << "Join() on detached fiber.";

  {
    thread::MutexLock lock(&mu_);
    CHECK(state_ != JOINED) << "Join() called on already joined fiber.";
  }

  const Fiber* current_fiber = GetPerThreadFiberPtr();
  CHECK(this != current_fiber) << "Fiber trying to join itself!";
  if (parent_ != nullptr) {
    CHECK(parent_ == current_fiber) << "Join() called from non-parent fiber";
  }

  InternalJoin();
}

// Update *this to a FINISHED state.  Preparing it to be Join()-ed (and
// notifying any waiters) when applicable. Returns whether the fiber was
// detached when marked finished.
//
// REQUIRES: *this has not already been marked finished.
bool Fiber::MarkFinished() {
  thread::MutexLock lock(&mu_);
  DCHECK_EQ(state_, RUNNING);

  state_ = FINISHED;

  // Any fiber can have detached children.
  if (first_child_ == nullptr) {
    joinable_.Notify();
    // Although joinable_ is true, any foreign call to Join() also needs to
    // acquire mu_, thus we can't be deleted yet.
  }
  return detached_.load(std::memory_order_relaxed);
}

// Record that the Join() requirement has been satisfied.  In the case of a
// detached fiber this may have been internally generated.
//
// If *this was a child fiber it will be removed from its parent's active
// children.
//
// No-op if *this has already been Join()-ed.
void Fiber::MarkJoined() {
  DCHECK(joinable_.HasBeenNotified());

  bool has_parent;
  {
    thread::MutexLock lock(&mu_);
    DCHECK(first_child_ == nullptr);
    if (state_ == JOINED) {
      return;  // Already joined.
    }
    DCHECK_EQ(state_, FINISHED);
    DVLOG(2) << "F " << this << " joined";
    state_ = JOINED;
    has_parent = parent_ != nullptr;
  }
  if (has_parent) {
    thread::MutexLock lock(&parent_->mu_);
    parent_->UnlinkChild(this);
    if (parent_->first_child_ == nullptr && parent_->state_ == FINISHED) {
      parent_->joinable_.Notify();
    }
  } else {
    // // We were joined and have no parent. All of our children must already be
    // // joined. Release our ref on the scheduler.
    // tree_scheduler_.Unref();
  }
}

void Fiber::InternalJoin() {
  Select({joinable_.OnEvent()});
  MarkJoined();
}

namespace {

// Fibers handed over by ReapWhenFinished(), waiting to finish.
//
// A plain `std::mutex` and not this library's fiber-aware `Mutex`: the queue is
// touched from `ReapFinishedFibers()`, which pool workers call as they come round,
// and a fiber mutex there would be one more scheduler interaction per pass on the
// path this exists to make cheaper. The critical section is a vector push or a
// swap, so there is nothing to suspend for.
struct ReapEntry {
  std::unique_ptr<Fiber> fiber;
  absl::AnyInvocable<void() &&> on_finished;
};

std::mutex& ReapMutex() {
  static absl::NoDestructor<std::mutex> mu;
  return *mu;
}

std::vector<ReapEntry>& ReapQueue() {
  static absl::NoDestructor<std::vector<ReapEntry>> queue;
  return *queue;
}

// ReapQueue().size(), readable without the lock. Workers consult this on every
// pass round their loop, and an idle pool must not pay a lock acquisition to be
// told there is nothing there: `try_lock` on a process-wide mutex is a read-modify
// -write on one shared cache line, so every worker doing it every pass keeps that
// line bouncing between cores whether or not there is any work. A relaxed load of
// a line nobody is writing is free by comparison.
std::atomic<size_t>& ReapPending() {
  static absl::NoDestructor<std::atomic<size_t>> pending{0};
  return *pending;
}

}  // namespace

size_t PendingReapCount() {
  return ReapPending().load(std::memory_order_relaxed);
}

void ReapWhenFinished(std::unique_ptr<Fiber> fiber,
                      absl::AnyInvocable<void() &&> on_finished) {
  if (fiber == nullptr) {
    return;
  }
  const std::lock_guard<std::mutex> lock(ReapMutex());
  ReapQueue().push_back(ReapEntry{.fiber = std::move(fiber),
                                  .on_finished = std::move(on_finished)});
  // Written under the lock, so it always agrees with the queue as of the last
  // release. Readers are relaxed and so may lag by nanoseconds, which costs at
  // worst a drain deferred to the next pass round a worker's loop.
  ReapPending().store(ReapQueue().size(), std::memory_order_relaxed);
}

void ReapFinishedFibers() {
  // Bounded work per call, and never blocking.
  //
  // The first version scanned the whole queue under the lock on every worker
  // round. With a few hundred connections in flight the queue is never empty, so
  // that was an O(queue) walk behind one global mutex, repeatedly, on all workers
  // at once -- measured as a 28% throughput loss at 256 clients with the pool
  // *less* busy than before (3.65 cores against 4.43), which is what contention on
  // a shared lock looks like from the outside. So: `try_lock`, so a worker with
  // real work never waits for the reaper, and a rotating cursor over a bounded
  // window, so the cost per round does not grow with the number of live fibers.
  constexpr size_t kMaxScanPerPass = 32;
  static std::atomic<size_t> cursor{0};

  // The no-work gate. Workers call this on every pass round their loop, so an
  // otherwise idle pool used to spend a `try_lock` per worker per pass on a shared
  // cache line to learn that the queue was empty; this reads a line nobody is
  // writing instead. See ReapPending().
  if (PendingReapCount() == 0) {
    return;
  }

  std::vector<ReapEntry> ready;
  {
    std::unique_lock<std::mutex> lock(ReapMutex(), std::try_to_lock);
    if (!lock.owns_lock()) {
      return;
    }
    std::vector<ReapEntry>& queue = ReapQueue();
    if (queue.empty()) {
      return;
    }
    const size_t scan = std::min(kMaxScanPerPass, queue.size());
    size_t at =
        cursor.fetch_add(scan, std::memory_order_relaxed) % queue.size();
    for (size_t seen = 0; seen < scan; ++seen) {
      if (at >= queue.size()) {
        at = 0;
      }
      // `Joinable()` is a notified-event read: asks "has it finished" without ever
      // suspending on one that has not.
      if (queue[at].fiber->Joinable()) {
        ready.push_back(std::move(queue[at]));
        queue[at] = std::move(queue.back());
        queue.pop_back();
        continue;  // The swapped-in entry now sits at `at`, so look at it too.
      }
      ++at;
    }
    ReapPending().store(queue.size(), std::memory_order_relaxed);
  }
  for (ReapEntry& entry : ready) {
    // Join first: the fiber has finished, so this returns without suspending and
    // leaves it safe to destroy.
    entry.fiber->Join();
    // Then let the owner clear its pointer, under the owner's own lock, *before*
    // the fiber is destroyed. A `Cancel()` racing completion therefore either runs
    // against a finished-but-live fiber, which is harmless, or finds the handle
    // already cleared. Reversing these two lines is the bug this ordering exists
    // to prevent.
    if (entry.on_finished != nullptr) {
      std::move(entry.on_finished)();
    }
    entry.fiber.reset();
  }
}

void Fiber::RetireUnstarted() ABSL_NO_THREAD_SAFETY_ANALYSIS {
  // Called only from ~ThreadLocalFiber, on the per-thread placeholder that
  // Current() creates for a thread which never ran under a real fiber. That
  // destructor fires during OS-thread teardown, when the thread's TLS block may
  // already be freed. The normal teardown path is unsafe there: MarkFinished()
  // and InternalJoin() take the fiber-aware mu_ and Select() on joinable_, both
  // of which read Boost's thread_local active-context via __tls_get_addr and so
  // touch freed TLS (surfacing later as heap/GC corruption).
  //
  // A placeholder is never Start()ed (its Boost context is never scheduled), has
  // no parent, and -- in correct use -- has no live children, so it is
  // reachable only from this exiting thread. No synchronization is needed to
  // retire it: transition straight to JOINED so ~Fiber()'s invariant holds,
  // bypassing the scheduler entirely.
  CHECK(parent_ == nullptr)
      << "F " << this << " RetireUnstarted() on a non-root fiber.";
  CHECK(first_child_ == nullptr)
      << "F " << this << " RetireUnstarted() with live child fibers.";
  CHECK_EQ(state_, RUNNING)
      << "F " << this << " RetireUnstarted() on a started fiber.";
  state_ = JOINED;
}

void Fiber::Cancel() ABSL_NO_THREAD_SAFETY_ANALYSIS {
  auto current = this;
  while (true) {
    DCHECK(current != nullptr);
    // We visit nodes in post-order, traversing each child sub-tree by sibling
    // position before operating on the parent.  We hold all "mu_"s up to and
    // including the initiating parent fiber node.
    current->mu_.Lock();

    // Check whether the fiber we're currently visiting has already been
    // cancelled.
    bool cancelled = current->cancellation_.HasBeenNotified();

    // If we have children, and we're already cancelled, then they must be also.
    // We don't need to re-walk our children as future descendants will be
    // spawned into a cancelled state.
    // If we have children, and we're not cancelled, we must visit them before
    // operating on "fiber".
    if (!cancelled && current->first_child_ != nullptr) {
      // Equivalent recursion note: recursive call.
      current = current->first_child_;
      continue;
    }

    while (true) {
      if (!cancelled) {
        current->cancellation_.Notify();
      }

      class ScopedMutexUnlocker {
       public:
        explicit ScopedMutexUnlocker(thread::Mutex* mu) : mu_(*mu) {}

        ~ScopedMutexUnlocker() { mu_.Unlock(); }

       private:
        thread::Mutex& mu_;
      };

      ScopedMutexUnlocker unlock_mu(&current->mu_);

      // Once we reach the fiber (*this) parenting cancellation, we're finished.
      if (current == this) {
        return;
      }

      DCHECK(current->parent_ != nullptr);
      DCHECK(current->parent_->first_child_ != nullptr);

      // If there is an unvisited sibling, we go there to process it.
      // Equivalent recursion note: recursive call return and recursive call.
      // The sibling list is circular. Returning to the parent's first child,
      // rather than finding a self-link, marks the end of a multi-child list.
      if (current->next_sibling_ != current->parent_->first_child_) {
        current = current->next_sibling_;
        break;
      }

      // We've reached the final sibling in this subtree.  Continue traversing
      // from our parent, who has no more unvisited children and is next in the
      // traversal order.
      // Equivalent recursion note: recursive call return.
      current = current->parent_;

      // Reached child => traversal spans our parent, which must need
      // cancellation.
      cancelled = false;
    }
  }
}
}  // namespace thread
