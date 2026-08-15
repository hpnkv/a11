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

#include "thread/thread_pool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <absl/base/call_once.h>
#include <absl/base/no_destructor.h>
#include <absl/base/optimization.h>
#include <absl/container/inlined_vector.h>
#include <absl/functional/any_invocable.h>
#include <absl/log/check.h>
#include <absl/log/log.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <boost/context/detail/prefetch.hpp>
#include <boost/context/fixedsize_stack.hpp>
#include <boost/context/segmented_stack.hpp>  // IWYU pragma: keep
#include <boost/fiber/algo/algorithm.hpp>
#include <boost/fiber/all.hpp>
#include <boost/fiber/context.hpp>
#include <boost/fiber/detail/cpu_relax.hpp>
#include <boost/fiber/operations.hpp>
#include <boost/fiber/properties.hpp>
#include <boost/fiber/scheduler.hpp>
#include <boost/fiber/type.hpp>
#include <boost/intrusive_ptr.hpp>

#include "thread/boost_primitives.h"
#include "thread/executor.h"
#include "thread/fiber.h"

#if !defined(BOOST_USE_SEGMENTED_STACKS)
namespace boost::context {
class segmented_stack {
 public:
  explicit segmented_stack(size_t) {}

  stack_context allocate() {
    LOG(FATAL) << "Segmented stacks are not supported.";
    ABSL_ASSUME(false);
    return {};
  }

  void deallocate(boost::context::stack_context&) BOOST_NOEXCEPT_OR_NOTHROW {
    LOG(FATAL) << "Segmented stacks are not supported.";
    ABSL_ASSUME(false);
  }
};
}  // namespace boost::context
#endif

namespace thread {

struct Fiber::BoostState {
  boost::intrusive_ptr<boost::fibers::context> context;
};

Fiber::BoostState* Fiber::GetBoostState() {
  return std::launder(reinterpret_cast<BoostState*>(boost_state_));
}

const Fiber::BoostState* Fiber::GetBoostState() const {
  return std::launder(reinterpret_cast<const BoostState*>(boost_state_));
}

void Fiber::ConstructBoostState() {
  static_assert(sizeof(BoostState) <= kBoostStateSize);
  static_assert(alignof(BoostState) <= kBoostStateAlignment);
  std::construct_at(reinterpret_cast<BoostState*>(boost_state_));
}

void Fiber::DestroyBoostState() {
  std::destroy_at(GetBoostState());
}

namespace {

#if !defined(BOOST_USE_SEGMENTED_STACKS)
boost::context::segmented_stack SegmentedAllocator(size_t) {
  LOG(FATAL) << "Segmented stacks are not supported. You need to compile Boost "
                "with BOOST_USE_SEGMENTED_STACKS.";
  ABSL_ASSUME(false);
}
#else
boost::context::segmented_stack SegmentedAllocator(size_t requested_size) {
  const size_t minimum = boost::context::stack_traits::minimum_size();
  const size_t configured =
      requested_size == 0 ? static_cast<size_t>(THREAD_DEFAULT_FIBER_STACK_SIZE)
                          : requested_size;
  return {std::max(minimum, configured)};
}
#endif

constexpr size_t kMiB = 1024 * 1024;

constexpr bool IsPowerOfTwo(size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

// "Included" power of two: one of 2^7 (128 B) through 2^16 (64 KiB), each
// with its own dedicated pool.
constexpr bool IsIncludedPowerOfTwo(size_t value) {
  return value >= 128 && value <= 65536 && IsPowerOfTwo(value);
}

constexpr size_t kDefaultStackSize = THREAD_DEFAULT_FIBER_STACK_SIZE;
constexpr bool kDefaultIsIncluded = IsIncludedPowerOfTwo(kDefaultStackSize);

// Every included power of two gets a 4 MiB pool; whichever size matches
// THREAD_DEFAULT_FIBER_STACK_SIZE gets 16 MiB instead, since it's the size
// nearly every fiber requests (TreeOptions default to stack_size 0).
template <size_t StackSize>
constexpr size_t kPoolBudgetBytes =
    StackSize == kDefaultStackSize ? 16 * kMiB : 4 * kMiB;

template <size_t StackSize>
constexpr size_t kPoolCapacity = kPoolBudgetBytes<StackSize> / StackSize;

// Lock-free pool of stacks of exactly StackSize bytes. Each slot is an
// independent atomic pointer rather than a linked free list, so reuse can
// never hit the classic Treiber-stack ABA hazard.
template <size_t StackSize, size_t Capacity>
std::array<std::atomic<void*>, Capacity>& StackPool() {
  static absl::NoDestructor<std::array<std::atomic<void*>, Capacity>> slots;
  return *slots;
}

template <size_t Capacity>
void* TryClaim(std::array<std::atomic<void*>, Capacity>& slots) {
  for (std::atomic<void*>& slot : slots) {
    if (void* sp = slot.exchange(nullptr, std::memory_order_acquire)) {
      return sp;
    }
  }
  return nullptr;
}

template <size_t Capacity>
bool TryRelease(std::array<std::atomic<void*>, Capacity>& slots, void* sp) {
  for (std::atomic<void*>& slot : slots) {
    if (void* expected = nullptr;
        slot.compare_exchange_strong(expected, sp, std::memory_order_release,
                                     std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

// Pools exist for the 10 included powers of two plus, if it isn't already
// one of those, THREAD_DEFAULT_FIBER_STACK_SIZE. Any other size (including a
// non-power-of-two request) falls through to the caller's backing allocator.
void* TryClaimFromPool(size_t stack_size) {
  switch (stack_size) {
    case 128:
      return TryClaim(StackPool<128, kPoolCapacity<128>>());
    case 256:
      return TryClaim(StackPool<256, kPoolCapacity<256>>());
    case 512:
      return TryClaim(StackPool<512, kPoolCapacity<512>>());
    case 1024:
      return TryClaim(StackPool<1024, kPoolCapacity<1024>>());
    case 2048:
      return TryClaim(StackPool<2048, kPoolCapacity<2048>>());
    case 4096:
      return TryClaim(StackPool<4096, kPoolCapacity<4096>>());
    case 8192:
      return TryClaim(StackPool<8192, kPoolCapacity<8192>>());
    case 16384:
      return TryClaim(StackPool<16384, kPoolCapacity<16384>>());
    case 32768:
      return TryClaim(StackPool<32768, kPoolCapacity<32768>>());
    case 65536:
      return TryClaim(StackPool<65536, kPoolCapacity<65536>>());
    default:
      if constexpr (!kDefaultIsIncluded) {
        if (stack_size == kDefaultStackSize) {
          return TryClaim(
              StackPool<kDefaultStackSize, 16 * kMiB / kDefaultStackSize>());
        }
      }
      return nullptr;
  }
}

bool TryReleaseToPool(size_t stack_size, void* sp) {
  switch (stack_size) {
    case 128:
      return TryRelease(StackPool<128, kPoolCapacity<128>>(), sp);
    case 256:
      return TryRelease(StackPool<256, kPoolCapacity<256>>(), sp);
    case 512:
      return TryRelease(StackPool<512, kPoolCapacity<512>>(), sp);
    case 1024:
      return TryRelease(StackPool<1024, kPoolCapacity<1024>>(), sp);
    case 2048:
      return TryRelease(StackPool<2048, kPoolCapacity<2048>>(), sp);
    case 4096:
      return TryRelease(StackPool<4096, kPoolCapacity<4096>>(), sp);
    case 8192:
      return TryRelease(StackPool<8192, kPoolCapacity<8192>>(), sp);
    case 16384:
      return TryRelease(StackPool<16384, kPoolCapacity<16384>>(), sp);
    case 32768:
      return TryRelease(StackPool<32768, kPoolCapacity<32768>>(), sp);
    case 65536:
      return TryRelease(StackPool<65536, kPoolCapacity<65536>>(), sp);
    default:
      if constexpr (!kDefaultIsIncluded) {
        if (stack_size == kDefaultStackSize) {
          return TryRelease(
              StackPool<kDefaultStackSize, 16 * kMiB / kDefaultStackSize>(),
              sp);
        }
      }
      return false;
  }
}

class PooledFixedSizeStack {
 public:
  using traits_type = boost::context::stack_traits;

  explicit PooledFixedSizeStack(size_t stack_size)
      : stack_size_(stack_size), backing_(stack_size) {}

  boost::context::stack_context allocate() {
    if (void* sp = TryClaimFromPool(stack_size_)) {
      boost::context::stack_context context;
      context.size = stack_size_;
      context.sp = sp;
      return context;
    }
    return backing_.allocate();
  }

  void deallocate(boost::context::stack_context& context) {
    if (TryReleaseToPool(context.size, context.sp)) {
      return;
    }
    backing_.deallocate(context);
  }

 private:
  size_t stack_size_;
  boost::context::fixedsize_stack backing_;
};

PooledFixedSizeStack FixedSizeAllocator(size_t requested_size) {
  const size_t minimum = boost::context::stack_traits::minimum_size();
  const size_t configured =
      requested_size == 0 ? static_cast<size_t>(THREAD_DEFAULT_FIBER_STACK_SIZE)
                          : requested_size;
  return PooledFixedSizeStack(std::max(minimum, configured));
}

using PoolWork = absl::AnyInvocable<void() &&>;

class FiberProperties final : public boost::fibers::fiber_properties {
 public:
  explicit FiberProperties(Fiber* absl_nonnull fiber)
      : boost::fibers::fiber_properties(nullptr), fiber_(fiber) {}

  [[nodiscard]] Fiber* absl_nonnull GetFiber() const { return fiber_; }

 private:
  Fiber* absl_nonnull const fiber_;
};

class InstrumentedRoundRobin final : public boost::fibers::algo::algorithm {
 public:
  InstrumentedRoundRobin() = default;

  InstrumentedRoundRobin(const InstrumentedRoundRobin&) = delete;
  InstrumentedRoundRobin& operator=(const InstrumentedRoundRobin&) = delete;

  void awakened(boost::fibers::context* absl_nonnull ctx) noexcept override {
    CHECK(ctx != nullptr);
    CHECK(!ctx->ready_is_linked());
    CHECK(ctx->is_resumable());
    ctx->ready_link(ready_queue_);
  }

  boost::fibers::context* absl_nullable pick_next() noexcept override {
    boost::fibers::context* absl_nullable victim = nullptr;
    if (!ready_queue_.empty()) {
      victim = &ready_queue_.front();
      ready_queue_.pop_front();
      boost::context::detail::prefetch_range(victim,
                                             sizeof(boost::fibers::context));
      CHECK(victim != nullptr);
      CHECK(!victim->ready_is_linked());
      CHECK(victim->is_resumable());
    }
    return victim;
  }

  bool has_ready_fibers() const noexcept override {
    return !ready_queue_.empty();
  }

  void suspend_until(
      const std::chrono::steady_clock::time_point& time_point) noexcept override
      ABSL_NO_THREAD_SAFETY_ANALYSIS {
    std::unique_lock lock(mu_);
    if (time_point == std::chrono::steady_clock::time_point::max()) {
      cv_.wait(lock, [this] { return WakePending(); });
      consumed_wake_seq_ = wake_seq_;
      return;
    }

    if (cv_.wait_until(lock, time_point, [this] { return WakePending(); })) {
      consumed_wake_seq_ = wake_seq_;
    }
  }

  void notify() noexcept override ABSL_NO_THREAD_SAFETY_ANALYSIS {
    std::unique_lock lock(mu_);
    ++wake_seq_;
    lock.unlock();
    cv_.notify_all();
  }

 private:
  using ReadyQueue = boost::fibers::scheduler::ready_queue_type;

  bool WakePending() const ABSL_NO_THREAD_SAFETY_ANALYSIS {
    return wake_seq_ != consumed_wake_seq_;
  }

  ReadyQueue ready_queue_;
  // This lock is deliberately native: Boost calls suspend_until() to park an
  // OS worker when no fiber can run, and notify() wakes that parked worker
  // from another OS thread. Fiber-aware primitives would need a runnable
  // scheduler here and could deadlock the scheduler itself.
  std::mutex mu_;
  std::condition_variable cv_;
  std::uint64_t wake_seq_ = 0;
  std::uint64_t consumed_wake_seq_ = 0;
};

bool& ThreadHasScheduler() {
  static thread_local bool thread_has_scheduler = false;
  return thread_has_scheduler;
}

template <typename Algorithm, typename... Args>
void EnsureThreadHasScheduler(Args&&... args) {
  if (ThreadHasScheduler()) {
    return;
  }
  boost::fibers::use_scheduling_algorithm<Algorithm>(
      std::forward<Args>(args)...);
  ThreadHasScheduler() = true;
}

// ---------------------------------------------------------------------------
// The worker pool
// ---------------------------------------------------------------------------
//
// Everything below replaces two things Boost gives us that are wrong for this
// workload, and it is worth saying exactly what they are, because the shape of
// the code follows from them.
//
// 1. `boost::fibers::algo::shared_work` puts *every* ready worker context on
//    one process-global `std::deque` behind one process-global `std::mutex`:
//    `awakened()` detaches the context and pushes, `pick_next()` pops and
//    re-attaches. So two acquisitions of a single global lock per context
//    switch, contended by every pool worker at once -- for a switch that
//    otherwise costs about 0.1us. A fibre that becomes ready on the very
//    worker it is already running on is round-tripped through that lock rather
//    than simply resumed.
//
// 2. `shared_work::notify()` signals only *its own* thread's condition
//    variable, and `suspend_until()` parks unconditionally on any momentary
//    gap. Pushing to the shared ready queue therefore wakes nobody, while an
//    idle worker sleeps at the first opportunity. Measured at full width that
//    came to roughly one park and one unpark per work item: ~8-10us of thread
//    wake for a unit of work whose actual switch is ~0.1us, and it got *worse*
//    with more workers (5.69M callbacks/s at one worker, 0.12M at fourteen).
//
// The replacement, `PoolAlgorithm` plus `PoolState`, is one scheduling
// algorithm shared by all pool workers with:
//
//   * a per-worker ready deque instead of a global one, so scheduling from
//     inside the pool touches only the pushing worker's own uncontended lock;
//   * stealing between those deques for balance, and so that work pushed by a
//     worker that then blocks is never stranded;
//   * a wake economy: a push signals an OS thread only when no worker is
//     already awake and looking for work. That is the ~9us this is all about,
//     and under any sustained load it is skipped entirely;
//   * exactly one place per worker where the OS thread can sleep, reached
//     through the fibre scheduler rather than around it.
//
// That last point is the subtle one, and it is why an idle worker goes to
// sleep in two steps rather than one. Its main context suspends on a
// fibre-aware condition variable (`WorkerSlot::idle_cv`), which hands the
// thread to Boost's dispatcher; the dispatcher takes the earliest of that
// deadline and everything in its own sleep queue and parks the thread in
// `PoolAlgorithm::suspend_until`, on native primitives.
//
// Parking the thread directly from the main context is faster and wrong.
// Boost's sleep queue is where every fibre-level deadline in A11 lives --
// `SelectUntil`, `Future::Await(deadline)`, and so every session and request
// timeout -- and only the dispatcher can see it. A thread asleep on its own
// condition variable honours none of them; they come due whenever something
// unrelated next wakes the thread. Nothing in the C++ suite or the benchmarks
// catches that, and it makes every timeout in the process a 50ms timeout.
//
// The primitives underneath the thread park are deliberately native
// `std::mutex`/`std::condition_variable`: they are what the *dispatcher*
// sleeps on when it has no runnable fibre, so a fibre-aware primitive there
// would need the very scheduler that is out of work. None of them is ever held
// across a fibre suspension point. `idle_mu`/`idle_cv` are the opposite case
// and are fibre-aware for the reason above; they are per worker, and only that
// worker's main context ever locks `idle_mu`, so they are never contended.

class PoolState;

// Set on pool worker threads to their index, -1 everywhere else. This is how
// `Post` and `Fiber::Start` tell "already inside the pool" (push locally, no
// thread wake) from "outside" (round-robin into a slot, wake if nobody is
// looking).
int& ThisWorkerIndex() {
  static thread_local int index = -1;
  return index;
}

// Apple arm64 cores have 128-byte cache lines, and x86-64 prefetches in
// 128-byte pairs, so that is the separation worth paying for.
// `std::hardware_destructive_interference_size` reports 64 on this toolchain
// and would leave the two halves of a line shared.
constexpr size_t kCacheLine = 128;

// A pool worker's private work, and its park.
//
// `contexts` holds *detached* fibre contexts: both fibres freshly created by
// Fiber::Start and fibres that became ready again. Detaching on push and
// attaching on pop is what makes them stealable, and it is the one thing this
// keeps from shared_work -- the difference is that the queue is per worker
// rather than global, so the pop is normally the pushing worker's own.
//
// The three groups below are separated onto their own cache lines. Slots are
// the one structure every worker touches: an idle worker reads `depth` on all
// of them before parking, so `depth` must not share a line with a queue its
// owner is writing, or every probe would steal the line back off the owner.
struct alignas(kCacheLine) WorkerSlot {
  // contexts.size() and callbacks.size(). Read without the lock, by every
  // other worker, so a thief can skip empty slots and an idle worker can
  // decide whether to park. Read-mostly and written only by whoever pushes or
  // pops.
  //
  // The two are counted separately because a fibre context and a stackless
  // callback are runnable by different parts of the worker. The dispatcher can
  // run a context but not a callback -- only the main context runs those -- so
  // a dispatcher that treated a pending callback as "work available" would
  // decline to park and then find nothing to pick, over and over, burning the
  // core in a tight loop between dispatch() and suspend_until().
  std::atomic<std::uint32_t> context_depth{0};
  std::atomic<std::uint32_t> callback_depth{0};

  // How much work this worker has run, ever. Only ever incremented by whoever
  // pops, on a line it already owns, so it costs about a nanosecond -- and it
  // is the only way to see whether the recruitment policy in PreferredSlot is
  // actually concentrating. A11_POOL_STATS=1 prints the distribution at exit.
  std::atomic<std::uint64_t> served{0};

  std::uint32_t depth(std::memory_order order) const {
    return context_depth.load(order) + callback_depth.load(order);
  }

  // Guards `contexts` and `callbacks`. Native, uncontended in the common case
  // (a worker pushing to and popping from its own slot), and contended only by
  // a thief or an external submitter.
  alignas(kCacheLine) std::mutex mu;
  std::deque<boost::fibers::context*> contexts;
  std::deque<PoolWork> callbacks;

  // Where an idle main context waits.
  //
  // Fibre-aware, and that is the whole point: suspending the main *fibre*
  // hands the thread to Boost's dispatcher, which knows its own sleep queue
  // and passes the earliest deadline in it to suspend_until() below. Parking
  // the OS thread here instead would be faster and wrong -- every fibre-level
  // timed wait in the system (`SelectUntil`, `Future::Await(deadline)`, every
  // session and request timeout) lives in that sleep queue, and a thread
  // asleep on its own condition variable cannot honour any of them. They come
  // due only when something else happens to wake the thread.
  //
  // Never contended: only this worker's main context locks `idle_mu`, and
  // signalling a condition variable does not require holding it.
  alignas(kCacheLine) thread::Mutex idle_mu;
  thread::CondVar idle_cv;

  // The park. `wake_seq` is bumped by every wake attempt and consumed by the
  // sleeper, so a signal that arrives between "decided to park" and "actually
  // parked" is not lost.
  //
  // It is atomic so that a *spinning* worker can watch it without taking
  // park_mu. That matters more than it looks: `notify()` is how Boost tells a
  // worker that a fibre bound to its own scheduler became ready from another
  // thread, and such a fibre sits in the scheduler's private remote-ready
  // queue, where no amount of scanning slot depths will find it. A spinner
  // that watched only the slots would sit out its whole budget while the fibre
  // it should be running was already runnable.
  alignas(kCacheLine) std::mutex park_mu;
  std::condition_variable park_cv;
  std::atomic<std::uint64_t> wake_seq{0};
  // Only ever read or written by this slot's own worker.
  std::uint64_t consumed_seq = 0;

  bool WakePending() const {
    return wake_seq.load(std::memory_order_acquire) != consumed_seq;
  }

  std::thread thread;
};

// At most 64 workers, so "is anybody parked, and who" is a single atomic load
// and a count-trailing-zeros rather than a scan.
constexpr size_t kMaxWorkers = 64;

// How long an idle worker looks for work before parking. The point is to
// absorb the gaps that come from several workers draining the same stream of
// items: parking there is what used to cost a wake per item.
//
// The budget is a duration rather than a spin count because what it has to be
// compared against is a duration: parking and being woken again costs about
// 8-10us on this machine, so looking for work for a comparable stretch is free
// whenever it succeeds even once. A11_POOL_SPIN_US overrides it -- the balance
// depends on the machine and on the offered load, and measuring it needs it
// settable; see `a11_bench --suite scheduling`.
//
// It matters much less than the number of searchers does (see max_spinners_):
// 30us, 100us and 300us measure the same once the cap is one.
absl::Duration SpinBudget() {
  static const absl::Duration budget = [] {
    const char* override_us = std::getenv("A11_POOL_SPIN_US");
    if (override_us == nullptr) {
      return absl::Microseconds(30);
    }
    return absl::Microseconds(std::max(0, std::atoi(override_us)));
  }();
  return budget;
}

// Clock reads per spin round would dominate the round; this is how many
// rounds run between them.
constexpr int kSpinRoundsPerClockCheck = 32;

// A worker parks for at most this long even with no signal pending. Nothing
// depends on it: it is a safety net that turns any residual lost wakeup into a
// latency blip instead of a hang.
constexpr absl::Duration kMaxPark = absl::Milliseconds(50);

class PoolState {
 public:
  PoolState() = default;

  PoolState(const PoolState&) = delete;
  PoolState& operator=(const PoolState&) = delete;

  void Start(size_t num_threads);

  size_t size() const { return num_workers_; }

  WorkerSlot& slot(size_t index) { return slots_[index]; }

  // Which slot a new piece of work should go to.
  //
  // From inside the pool: this worker's own, which is the whole point -- its
  // dispatcher picks the work up at the next suspension point for the price of
  // a context switch.
  //
  // From outside: the narrowest set of workers that is keeping up. The scan
  // starts at slot 0 every time and stops at the first worker whose queue is
  // shorter than `recruit_backlog_`, so work concentrates on a few hot workers
  // and the rest stay parked.
  //
  // **A queue is the signal to widen.** Depth counts work that has arrived and
  // not yet been picked up, so a slot over the threshold means that worker
  // cannot drain as fast as work is arriving -- which is exactly, and only,
  // when a second thread earns the ~9us it costs to wake it. Spreading by
  // default instead (a plain round robin over all N slots) wakes every worker
  // in the pool for a burst that one of them could have absorbed: worth
  // `pool_post_pipelined` 705k -> 837k/s and `submit_round_trip` 4.75us ->
  // 2.38us, with the latency rows unmoved.
  //
  // The converse case is why the threshold cannot simply be enormous. Sixteen
  // fibres that each want a core for a millisecond pile up on slot 0 the
  // instant they are submitted, cross the threshold immediately, and recruit
  // the whole pool -- `parallel_cpu` in the scheduling suite is the row that
  // checks this still happens, and a pool pinned to the 2-3 workers this
  // policy settles on for small items measures 4x worse on it.
  size_t PreferredSlot() {
    const int index = ThisWorkerIndex();
    if (index >= 0) {
      return static_cast<size_t>(index);
    }
    // Zero disables recruitment entirely and spreads over every slot, which is
    // what this did before the policy existed. Kept as the control for
    // `a11_bench --suite scheduling`.
    if (recruit_backlog_ != 0) {
      // An idle worker first, among those not already backlogged. On a
      // quiescent pool this is what keeps a strictly serial request/response
      // from landing on the one worker that is still tearing down the previous
      // fibre -- concentrating blindly costs `submit_round_trip[from=fiber]`
      // about 2us, all of it waiting for that teardown. Under a burst nobody
      // is idle, this finds nothing, and the rule below concentrates.
      const std::uint64_t idle = idle_mask_.load(std::memory_order_relaxed);
      if (idle != 0) {
        for (size_t candidate = 0; candidate < num_workers_; ++candidate) {
          if ((idle & (std::uint64_t{1} << candidate)) != 0 &&
              slots_[candidate].depth(std::memory_order_relaxed) <
                  recruit_backlog_) {
            return candidate;
          }
        }
      }
      for (size_t candidate = 0; candidate < num_workers_; ++candidate) {
        if (slots_[candidate].depth(std::memory_order_relaxed) <
            recruit_backlog_) {
          return candidate;
        }
      }
    }
    // Everyone is backlogged: nothing to be gained by preferring anybody.
    return next_victim_.fetch_add(1, std::memory_order_relaxed) % num_workers_;
  }

  void PushContext(boost::fibers::context* absl_nonnull context, size_t index,
                   bool wake) {
    {
      std::lock_guard<std::mutex> lock(slots_[index].mu);
      slots_[index].contexts.push_back(context);
    }
    Published(slots_[index].context_depth);
    if (wake) {
      WakeSomeone(index);
    }
  }

  void PushCallback(PoolWork work, size_t index) {
    {
      std::lock_guard<std::mutex> lock(slots_[index].mu);
      slots_[index].callbacks.push_back(std::move(work));
    }
    Published(slots_[index].callback_depth);
    WakeSomeone(index);
  }

  // Pops a ready context for worker `index`: its own slot first, then a
  // rotating sweep over the others.
  boost::fibers::context* absl_nullable PopContext(size_t index) {
    if (boost::fibers::context* context = TryPopContext(index)) {
      return context;
    }
    if (no_steal_) {
      return nullptr;
    }
    for (size_t offset = 1; offset < num_workers_; ++offset) {
      const size_t victim = (index + offset) % num_workers_;
      if (slots_[victim].context_depth.load(std::memory_order_relaxed) == 0) {
        continue;
      }
      if (boost::fibers::context* context = TryPopContext(victim)) {
        return context;
      }
    }
    return nullptr;
  }

  bool PopCallback(size_t index, PoolWork& out) {
    if (TryPopCallback(index, out)) {
      return true;
    }
    for (size_t offset = 1; offset < num_workers_; ++offset) {
      const size_t victim = (index + offset) % num_workers_;
      if (slots_[victim].callback_depth.load(std::memory_order_relaxed) == 0) {
        continue;
      }
      if (TryPopCallback(victim, out)) {
        return true;
      }
    }
    return false;
  }

  // Is there anything to run anywhere?
  //
  // A scan rather than one global counter on purpose. A counter would be a
  // read-modify-write on a single line by every push and every pop in the
  // pool, which serialises exactly the operation this file exists to make
  // cheap; the scan is read-only, so the lines stay shared and no ownership
  // moves. `order` is seq_cst on the path that decides to park -- see
  // WakeSomeone for why that has to participate in the total order -- and
  // relaxed while spinning, where a stale answer only costs another round.
  bool AnyWork(std::memory_order order = std::memory_order_seq_cst) const {
    for (size_t index = 0; index < num_workers_; ++index) {
      if (slots_[index].depth(order) != 0) {
        return true;
      }
    }
    return false;
  }

  // Only fibre contexts, which is all a dispatcher can run.
  bool AnyContexts(std::memory_order order = std::memory_order_seq_cst) const {
    for (size_t index = 0; index < num_workers_; ++index) {
      if (slots_[index].context_depth.load(order) != 0) {
        return true;
      }
    }
    return false;
  }

  // Only stackless callbacks, which only a main context can run.
  bool AnyCallbacks(std::memory_order order = std::memory_order_seq_cst) const {
    for (size_t index = 0; index < num_workers_; ++index) {
      if (slots_[index].callback_depth.load(order) != 0) {
        return true;
      }
    }
    return false;
  }

  // Wakes an OS thread only if one is actually needed.
  //
  // `spinning_ > 0` means some worker is already awake and sweeping the slots;
  // it will find this item without a syscall. That test is the whole point of
  // the exercise -- under load it holds nearly always, and the ~9us handoff
  // disappears.
  void WakeSomeone(size_t preferred) {
    // Diagnostic: A11_POOL_ALWAYS_WAKE=1 disables the economy entirely, so a
    // push always signals somebody. Used to tell "the economy is dropping a
    // wake" from "a context is being lost".
    if (always_wake_) {
      for (size_t index = 0; index < num_workers_; ++index) {
        WakeWorker(index);
      }
      return;
    }
    // seq_cst: pairs with the registration on the idle path so that a worker
    // about to go idle and a producer about to skip the wake cannot both
    // conclude the other will handle it.
    if (spinning_.load(std::memory_order_seq_cst) > 0) {
      return;
    }
    std::uint64_t idle = idle_mask_.load(std::memory_order_seq_cst);
    if (idle == 0) {
      return;
    }
    // Prefer the slot the work landed in: it runs the item without stealing.
    const std::uint64_t preferred_bit = std::uint64_t{1} << preferred;
    WakeWorker((idle & preferred_bit) != 0
                   ? preferred
                   : static_cast<size_t>(std::countr_zero(idle)));
  }

  // Both halves of waking an idle worker, because it can be asleep at either
  // of two levels and a producer cannot cheaply tell which.
  //
  // Its main context may be suspended on `idle_cv` -- only that makes it
  // runnable again, and only a runnable main context runs stackless callbacks.
  // Its OS thread may additionally be parked in suspend_until, which only
  // `park_cv` reaches. Cross-thread, the first signal usually implies the
  // second (Boost routes it through schedule_from_remote and notify), but not
  // when the main context is busy and only the dispatcher is parked. Both are
  // no-ops against a waiter that is not there.
  void WakeWorker(size_t index) {
    slots_[index].idle_cv.Signal();
    Signal(index);
  }

  // Wakes a specific worker unconditionally: used for `notify()`, where a
  // context is bound to that worker's scheduler and nobody else can run it.
  void Signal(size_t index) {
    WorkerSlot& target = slots_[index];
    {
      // The lock is what orders this against a worker deciding to park; the
      // increment itself is atomic so a spinner can watch it lock-free.
      std::lock_guard<std::mutex> lock(target.park_mu);
      target.wake_seq.fetch_add(1, std::memory_order_release);
    }
    target.park_cv.notify_one();
  }

  void SignalAll() {
    for (size_t index = 0; index < num_workers_; ++index) {
      WakeWorker(index);
    }
  }

  // Wakes only workers that are actually idle. Used to re-arm wait deadlines
  // when a timer moves the head: signalling every worker unconditionally would
  // cost a wake per PostAt, which is the cost this whole file is about.
  void SignalIdle() {
    std::uint64_t idle = idle_mask_.load(std::memory_order_seq_cst);
    while (idle != 0) {
      const int index = std::countr_zero(idle);
      idle &= idle - 1;
      WakeWorker(static_cast<size_t>(index));
    }
  }

  void PostAt(absl::Time deadline, PoolWork work);
  // Moves every due timer into `index`'s callback queue. Returns whether any
  // moved.
  bool CollectDueTimers(size_t index);
  absl::Time EarliestTimer() const;

  bool shutting_down() const {
    return shutdown_.load(std::memory_order_acquire);
  }

  void Shutdown() {
    shutdown_.store(true, std::memory_order_release);
    SignalAll();
  }

  // Idle bookkeeping, called only from the worker loop.
  void BeginSpinning() { spinning_.fetch_add(1, std::memory_order_seq_cst); }

  void EndSpinning() { spinning_.fetch_sub(1, std::memory_order_seq_cst); }

  bool MaySpin() const {
    return spinning_.load(std::memory_order_relaxed) < max_spinners_;
  }

  // "This worker has nothing to do and is about to stop looking." Set before
  // the idle wait and cleared after, and it covers the OS-thread park too,
  // since the thread only parks while the main context is idle.
  void MarkIdle(size_t index) {
    idle_mask_.fetch_or(std::uint64_t{1} << index, std::memory_order_seq_cst);
  }

  void ClearIdle(size_t index) {
    idle_mask_.fetch_and(~(std::uint64_t{1} << index),
                         std::memory_order_seq_cst);
  }

 private:
  struct TimedWork {
    absl::Time deadline;
    std::uint64_t sequence = 0;
    PoolWork work;
  };

  struct LaterDeadline {
    bool operator()(const TimedWork& left, const TimedWork& right) const {
      if (left.deadline != right.deadline) {
        return left.deadline > right.deadline;
      }
      return left.sequence > right.sequence;
    }
  };

  // seq_cst: this is one half of the Dekker pair with the idle path. A
  // producer publishes here and then reads `spinning_`/`idle_mask_`; a
  // worker publishes that it is parking and then reads the depths. Both sides
  // must be in the single total order or both could decide the other will
  // handle the item.
  static void Published(std::atomic<std::uint32_t>& counter) {
    counter.fetch_add(1, std::memory_order_seq_cst);
  }

  // Relaxed: an over-count only costs a worker one more trip round its loop.
  static void Consumed(std::atomic<std::uint32_t>& counter) {
    counter.fetch_sub(1, std::memory_order_relaxed);
  }

  boost::fibers::context* absl_nullable TryPopContext(size_t index) {
    WorkerSlot& target = slots_[index];
    boost::fibers::context* context = nullptr;
    {
      std::lock_guard<std::mutex> lock(target.mu);
      if (target.contexts.empty()) {
        return nullptr;
      }
      context = target.contexts.front();
      target.contexts.pop_front();
    }
    Consumed(target.context_depth);
    target.served.fetch_add(1, std::memory_order_relaxed);
    return context;
  }

  bool TryPopCallback(size_t index, PoolWork& out) {
    WorkerSlot& target = slots_[index];
    {
      std::lock_guard<std::mutex> lock(target.mu);
      if (target.callbacks.empty()) {
        return false;
      }
      out = std::move(target.callbacks.front());
      target.callbacks.pop_front();
    }
    Consumed(target.callback_depth);
    target.served.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  std::unique_ptr<WorkerSlot[]> slots_;
  size_t num_workers_ = 0;
  // How many workers may look for work at once, and the single most important
  // number in this file after the wake economy itself.
  //
  // One searcher is already enough for every producer to skip its signal --
  // that is the only thing the count is consulted for -- while each additional
  // one occupies a core that a runnable fibre could have had. At half the pool
  // (7 of 14 here) the searchers reproduce the whole bulk-versus-latency trade
  // this rewrite was supposed to remove: `pool_post_pipelined` 650k -> 1.03M/s
  // but `submit_round_trip[from=fiber]` 8.8us -> 21.7us. Bulk-dominated
  // workloads may still want more; A11_POOL_SPINNERS is the dial.
  std::uint32_t max_spinners_ = 1;
  // Diagnostics only; see the dials read in Start().
  bool always_wake_ = false;
  bool no_steal_ = false;

  // How long a worker's queue has to get before work is routed to the next
  // worker instead -- the pool's answer to "is another thread worth waking".
  // Zero disables the policy and spreads over every slot. A11_POOL_RECRUIT
  // overrides it. See PreferredSlot.
  //
  // One, measured. Every larger value trades the latency rows for the bulk
  // one at a rate that is not worth it: 2 buys another 29% of
  // `pool_post_pipelined` and costs `submit_round_trip[from=fiber]` 8.8us ->
  // 13.5us, and 4 costs `parallel_cpu` 30%.
  std::uint32_t recruit_backlog_ = 1;

  // Written on every park and unpark, read on every wake decision: worth a
  // line of their own, away from the slots.
  alignas(kCacheLine) std::atomic<std::uint32_t> spinning_{0};
  std::atomic<std::uint64_t> idle_mask_{0};
  std::atomic<std::uint32_t> next_victim_{0};
  std::atomic<bool> shutdown_{false};

  mutable std::mutex timed_mu_;
  std::vector<TimedWork> timed_work_;
  std::uint64_t next_timed_sequence_ = 0;
  // The head of `timed_work_`, so a worker can bound its park without taking
  // `timed_mu_`.
  std::atomic<std::int64_t> earliest_timer_ns_{
      std::numeric_limits<std::int64_t>::max()};
};

void PoolState::PostAt(absl::Time deadline, PoolWork work) {
  bool became_head = false;
  {
    std::lock_guard<std::mutex> lock(timed_mu_);
    timed_work_.push_back(TimedWork{
        .deadline = deadline,
        .sequence = next_timed_sequence_++,
        .work = std::move(work),
    });
    std::push_heap(timed_work_.begin(), timed_work_.end(), LaterDeadline{});
    const std::int64_t head = absl::ToUnixNanos(timed_work_.front().deadline);
    // seq_cst, and paired with the seq_cst read of it on the park path: the
    // new head has to be visible to anyone who parks after this point, or the
    // signal below has to reach them.
    became_head =
        head != earliest_timer_ns_.exchange(head, std::memory_order_seq_cst);
  }
  // Only a timer that moved the head can shorten anybody's park, and only a
  // worker that is actually parked has a deadline to shorten.
  if (became_head) {
    SignalIdle();
  }
}

absl::Time PoolState::EarliestTimer() const {
  const std::int64_t nanos = earliest_timer_ns_.load(std::memory_order_seq_cst);
  if (nanos == std::numeric_limits<std::int64_t>::max()) {
    return absl::InfiniteFuture();
  }
  return absl::FromUnixNanos(nanos);
}

bool PoolState::CollectDueTimers(size_t index) {
  if (absl::ToUnixNanos(absl::Now()) <
      earliest_timer_ns_.load(std::memory_order_acquire)) {
    return false;
  }

  absl::InlinedVector<PoolWork, 4> due;
  {
    std::lock_guard<std::mutex> lock(timed_mu_);
    const absl::Time now = absl::Now();
    while (!timed_work_.empty() && timed_work_.front().deadline <= now) {
      std::pop_heap(timed_work_.begin(), timed_work_.end(), LaterDeadline{});
      due.push_back(std::move(timed_work_.back().work));
      timed_work_.pop_back();
    }
    earliest_timer_ns_.store(
        timed_work_.empty() ? std::numeric_limits<std::int64_t>::max()
                            : absl::ToUnixNanos(timed_work_.front().deadline),
        std::memory_order_release);
  }

  if (due.empty()) {
    return false;
  }
  // Into this worker's own slot, so the timer runs here unless somebody else
  // is idle enough to steal it.
  {
    std::lock_guard<std::mutex> lock(slots_[index].mu);
    for (PoolWork& work : due) {
      slots_[index].callbacks.push_back(std::move(work));
    }
  }
  for (size_t count = 0; count < due.size(); ++count) {
    Published(slots_[index].callback_depth);
  }
  return true;
}

// The pool's scheduling algorithm. One instance per worker thread, all sharing
// one PoolState; `index_` is which slot this instance owns.
class PoolAlgorithm final : public boost::fibers::algo::algorithm {
 public:
  PoolAlgorithm(PoolState* absl_nonnull state, size_t index)
      : state_(state), index_(index) {}

  PoolAlgorithm(const PoolAlgorithm&) = delete;
  PoolAlgorithm& operator=(const PoolAlgorithm&) = delete;

  void awakened(boost::fibers::context* absl_nonnull ctx) noexcept override {
    if (ctx->is_context(boost::fibers::type::pinned_context)) {
      // The main and dispatcher contexts belong to this thread and cannot
      // migrate; they never go near the shared structures.
      ctx->ready_link(lqueue_);
      return;
    }
    // Detach here, on the owning thread, so any worker may later attach it.
    // This is shared_work's invariant, kept; what changes is that the queue it
    // lands in is this worker's own rather than a process-global one, so the
    // usual case -- a fibre made ready by the worker that will also run it --
    // costs one uncontended lock and no wake at all.
    ctx->detach();
    state_->PushContext(ctx, index_, /*wake=*/false);
  }

  boost::fibers::context* absl_nullable pick_next() noexcept override {
    // The main context is what runs stackless callbacks, so preferring ready
    // fibres unconditionally -- as shared_work does -- lets a steady stream of
    // fibres starve every posted callback on this worker. Alternating fixes
    // that, but only matters when there is actually a callback waiting: with
    // none, giving the main context a turn just sends it round its loop to
    // find nothing and yield again, and that hop is on the critical path of
    // every fibre this worker starts.
    if (!lqueue_.empty() && state_->AnyCallbacks(std::memory_order_relaxed)) {
      prefer_pinned_ = !prefer_pinned_;
      if (prefer_pinned_) {
        return PopPinned();
      }
    }
    if (boost::fibers::context* ctx = state_->PopContext(index_)) {
      boost::context::detail::prefetch_range(ctx,
                                             sizeof(boost::fibers::context));
      // Adopt it into this thread's scheduler. For a freshly created context
      // this is its first attach; for a stolen one it is the migration.
      boost::fibers::context::active()->attach(ctx);
      return ctx;
    }
    return PopPinned();
  }

  bool has_ready_fibers() const noexcept override {
    return !lqueue_.empty() || state_->slot(index_).context_depth.load(
                                   std::memory_order_relaxed) != 0;
  }

  // Called by the dispatcher when it has nothing to run. On a pool worker this
  // happens only when the main context is itself blocked inside a callback --
  // normally the main context parks instead, on this same condition variable,
  // so the thread has exactly one place to sleep.
  //
  // Deliberately *not* registered in the parked mask. A thread parked here has
  // its main context blocked, so it can run ready fibres but not stackless
  // callbacks; letting a producer aim a wake at it would consume the wake
  // without running the callback. Boost's notify() still reaches it directly
  // for contexts bound to this scheduler, which is the case that matters.
  void suspend_until(const std::chrono::steady_clock::time_point&
                         time_point) noexcept override {
    WorkerSlot& self = state_->slot(index_);
    std::unique_lock<std::mutex> lock(self.park_mu);
    // Contexts, not all work: a pending stackless callback is not something a
    // dispatcher can run, and treating it as a reason to stay awake spins this
    // thread between here and dispatch() until somebody else takes it.
    if (self.WakePending() || state_->AnyContexts()) {
      self.consumed_seq = self.wake_seq.load(std::memory_order_acquire);
      return;
    }
    const auto cap =
        std::chrono::steady_clock::now() +
        std::chrono::nanoseconds(absl::ToInt64Nanoseconds(kMaxPark));
    self.park_cv.wait_until(lock, std::min(time_point, cap),
                            [&self] { return self.WakePending(); });
    self.consumed_seq = self.wake_seq.load(std::memory_order_acquire);
  }

  // A context bound to *this* scheduler became ready from another thread.
  // Nobody else can run it, so this wake is not subject to the economy above;
  // it is cheap when the worker is not parked, because notifying a condition
  // variable with no waiter does not enter the kernel.
  void notify() noexcept override { state_->Signal(index_); }

 private:
  boost::fibers::context* absl_nullable PopPinned() {
    if (lqueue_.empty()) {
      return nullptr;
    }
    boost::fibers::context* ctx = &lqueue_.front();
    lqueue_.pop_front();
    return ctx;
  }

  boost::fibers::scheduler::ready_queue_type lqueue_;
  PoolState* absl_nonnull const state_;
  const size_t index_;
  bool prefer_pinned_ = false;
};

class WorkerThreadPool {
 public:
  WorkerThreadPool() = default;
  ~WorkerThreadPool();

  WorkerThreadPool(const WorkerThreadPool&) = delete;
  WorkerThreadPool& operator=(const WorkerThreadPool&) = delete;

  void Start(size_t num_threads = std::thread::hardware_concurrency());
  void Schedule(boost::intrusive_ptr<boost::fibers::context> context);
  void Post(PoolWork work);
  void PostAt(absl::Time deadline, PoolWork work);

  static WorkerThreadPool& Instance();

 private:
  void RunWorker(size_t index);

  PoolState state_;
};

void PoolState::Start(size_t num_threads) {
  num_workers_ = num_threads;
  slots_ = std::make_unique<WorkerSlot[]>(num_threads);
  const char* override_spinners = std::getenv("A11_POOL_SPINNERS");
  if (override_spinners != nullptr) {
    max_spinners_ =
        static_cast<std::uint32_t>(std::max(0, std::atoi(override_spinners)));
  }
  const char* override_recruit = std::getenv("A11_POOL_RECRUIT");
  if (override_recruit != nullptr) {
    recruit_backlog_ =
        static_cast<std::uint32_t>(std::max(0, std::atoi(override_recruit)));
  }

  // A11_POOL_STATS=1 prints how the work actually landed. The recruitment
  // policy is a claim about the shape of that distribution -- narrow for a
  // stream of small items, wide when the items are big enough to want cores --
  // and this is what checks the claim rather than inferring it from a rate.
  always_wake_ = std::getenv("A11_POOL_ALWAYS_WAKE") != nullptr;
  no_steal_ = std::getenv("A11_POOL_NO_STEAL") != nullptr;

  const char* stats = std::getenv("A11_POOL_STATS");
  if (stats != nullptr && std::atoi(stats) != 0) {
    static PoolState* reporting = this;
    // stderr rather than LOG: this runs at exit, where the Abseil sink may be
    // bridged into Python logging and touching it would mean taking the GIL
    // during interpreter teardown. That has aborted this process before.
    std::atexit([] {
      std::string line;
      std::uint64_t total = 0;
      size_t used = 0;
      for (size_t index = 0; index < reporting->num_workers_; ++index) {
        const std::uint64_t served =
            reporting->slots_[index].served.load(std::memory_order_relaxed);
        total += served;
        if (served != 0) {
          ++used;
        }
        absl::StrAppend(&line, index == 0 ? "" : " ", served);
      }
      std::fprintf(stderr,
                   "pool served %llu items across %zu/%zu workers: %s\n",
                   static_cast<unsigned long long>(total), used,
                   reporting->num_workers_, line.c_str());
    });
  }
}

WorkerThreadPool::~WorkerThreadPool() {
  state_.Shutdown();
  for (size_t index = 0; index < state_.size(); ++index) {
    std::thread& worker = state_.slot(index).thread;
    if (worker.joinable()) {
      worker.join();
    }
  }
}

void WorkerThreadPool::Start(size_t num_threads) {
  if (num_threads == 0) {
    num_threads = 1;
  }
  if (num_threads > kMaxWorkers) {
    LOG(WARNING) << "Worker pool clamped from " << num_threads << " to "
                 << kMaxWorkers << " threads.";
    num_threads = kMaxWorkers;
  }
  state_.Start(num_threads);
  for (size_t index = 0; index < num_threads; ++index) {
    state_.slot(index).thread =
        std::thread([this, index] { RunWorker(index); });
  }
}

void WorkerThreadPool::RunWorker(size_t index) {
  ThisWorkerIndex() = static_cast<int>(index);
  EnsureThreadHasScheduler<PoolAlgorithm>(&state_, index);

  WorkerSlot& self = state_.slot(index);

  while (true) {
    if (state_.shutting_down()) {
      break;
    }

    bool did_work = state_.CollectDueTimers(index);

    PoolWork callback;
    if (state_.PopCallback(index, callback)) {
      did_work = true;
      try {
        std::move(callback)();
      } catch (const std::exception& error) {
        LOG(ERROR) << "Unobserved stackless callback exception: "
                   << error.what();
      } catch (...) {
        LOG(ERROR) << "Unobserved stackless callback non-standard exception";
      }
    }

    // Hand the thread to the fibre scheduler: this is where contexts pushed to
    // (or stolen into) this worker actually run. It returns once no fibre is
    // ready, having cost two context switches -- nanoseconds, not a wake.
    boost::this_fiber::yield();

    // Relaxed: a false negative here only means dropping into the spin, which
    // re-checks properly before anything actually parks.
    if (did_work || state_.AnyWork(std::memory_order_relaxed)) {
      continue;
    }

    // Nothing anywhere. Look a little longer before paying for a park: with
    // several workers draining one stream, the gap is usually shorter than the
    // wake it would cost to sleep through it.
    if (state_.MaySpin()) {
      // Being counted as spinning is what lets producers skip their wake
      // entirely, so the counter goes up before the first look and comes down
      // only after the last one.
      state_.BeginSpinning();
      const absl::Time spin_until = absl::Now() + SpinBudget();
      bool found = false;
      while (!found) {
        for (int round = 0; round < kSpinRoundsPerClockCheck; ++round) {
          // `WakePending` is not redundant with the slot scan: it is the only
          // way a spinner learns about a fibre bound to its own scheduler that
          // another thread just made ready. Without it, that fibre waits out
          // the whole budget -- worth 11.5us -> 25us on
          // `submit_round_trip[from=fiber]`, which is precisely the shape of
          // the regression that sank the previous attempt at spinning.
          if (self.WakePending() || state_.AnyWork(std::memory_order_relaxed) ||
              state_.shutting_down()) {
            found = true;
            break;
          }
          cpu_relax();
        }
        if (found || absl::Now() >= spin_until) {
          break;
        }
      }
      state_.EndSpinning();
      if (found) {
        continue;
      }
    }

    // Go idle by suspending the main *fibre*, not the thread.
    //
    // This is the one place where the obvious optimisation is wrong. Parking
    // the OS thread directly here is cheaper and measures better, and it
    // silently breaks every fibre-level deadline in the process: `SelectUntil`,
    // `Future::Await(deadline)`, session and request timeouts all park their
    // fibre in Boost's sleep queue, and only Boost's dispatcher knows when the
    // earliest of them is due. Suspending the main fibre hands the thread to
    // that dispatcher, which combines this deadline with its sleep queue and
    // parks the thread once, in suspend_until, on the true minimum.
    //
    // The bug this cost: a 1ms session timeout that had not fired 10ms later,
    // because the worker was asleep on its own condition variable for the full
    // kMaxPark and nothing had happened to wake it.
    state_.MarkIdle(index);
    {
      thread::MutexLock lock(&self.idle_mu);
      // Re-check after publishing "idle" and under the lock. Both sides are
      // seq_cst, so a producer that skipped its signal because it saw nobody
      // idle must be visible here.
      if (!self.WakePending() && !state_.AnyWork() && !state_.shutting_down()) {
        self.idle_cv.WaitWithDeadline(
            &self.idle_mu,
            std::min(absl::Now() + kMaxPark, state_.EarliestTimer()));
      }
    }
    state_.ClearIdle(index);
    self.consumed_seq = self.wake_seq.load(std::memory_order_acquire);
  }

  DLOG(INFO) << "Worker " << index << " exiting.";
}

void WorkerThreadPool::Schedule(
    boost::intrusive_ptr<boost::fibers::context> context) {
  CHECK(!state_.shutting_down())
      << "Cannot schedule work after worker-pool shutdown.";
  CHECK(context->get_scheduler() == nullptr)
      << "Cannot schedule an already scheduled context.";
  // The fibre keeps the reference alive until it is attached and running; the
  // pool holds a raw pointer, as Boost's own algorithms do.
  //
  // From inside the pool this lands in the calling worker's own slot, where
  // its dispatcher picks it up on the next suspension point without any thread
  // being woken. The wake is still attempted -- but the economy usually
  // declines it -- because the pushing worker may block somewhere that never
  // reaches a suspension point, and then a sibling has to be able to steal
  // this context rather than have it stranded.
  state_.PushContext(context.get(), state_.PreferredSlot(), /*wake=*/true);
}

void WorkerThreadPool::Post(PoolWork work) {
  if (work == nullptr) {
    return;
  }
  CHECK(!state_.shutting_down())
      << "Cannot post work after worker-pool shutdown.";
  state_.PushCallback(std::move(work), state_.PreferredSlot());
}

void WorkerThreadPool::PostAt(absl::Time deadline, PoolWork work) {
  if (work == nullptr) {
    return;
  }
  CHECK(!state_.shutting_down())
      << "Cannot post timed work after worker-pool shutdown.";
  state_.PostAt(deadline, std::move(work));
}

WorkerThreadPool& WorkerThreadPool::Instance() {
  static absl::NoDestructor<WorkerThreadPool> instance;
  static const bool started = [] {
    // A11_POOL_THREADS pins the worker count. The pool's throughput depends
    // strongly on its width -- see `a11_bench --suite scheduling` and FINDINGS
    // -- and measuring that needs the count to be settable.
    const char* override_count = std::getenv("A11_POOL_THREADS");
    if (override_count != nullptr) {
      instance->Start(static_cast<size_t>(std::atoi(override_count)));
    } else {
      instance->Start();
    }
    return true;
  }();
  (void)started;
  return *instance;
}

absl::once_flag worker_pool_once;

void EnsureWorkerThreadPool() {
  absl::call_once(worker_pool_once, WorkerThreadPool::Instance);
}

}  // namespace

void Fiber::Start() {
  EnsureWorkerThreadPool();
  // EnsureThreadHasScheduler runs only once, so from within the worker pool,
  // threads will keep their shared_work nature. Outside the pool, threads are
  // initialized with round_robin to avoid participation in the pool, but be
  // compatible with fibers.
  EnsureThreadHasScheduler<InstrumentedRoundRobin>();

  auto body = [this] {
    std::move(work_)();
    work_ = nullptr;

    if (MarkFinished()) {
      // Detached fibers are self-joining and own their released unique_ptr.
      InternalJoin();
      delete this;
    }
  };

  // FiberProperties is owned and eventually destroyed by the context.
  thread::MutexLock lock(&mu_);
  const auto properties = new FiberProperties(this);  // NOLINT
  if (tree_options_.stack_type == StackType::kFixedSize) {
    GetBoostState()->context =
        boost::fibers::make_worker_context_with_properties(
            boost::fibers::launch::post, properties,
            FixedSizeAllocator(tree_options_.stack_size), std::move(body));
  } else {
    GetBoostState()->context =
        boost::fibers::make_worker_context_with_properties(
            boost::fibers::launch::post, properties,
            SegmentedAllocator(tree_options_.stack_size), std::move(body));
  }

  // From a pool worker this is a push onto that worker's own slot -- its own
  // dispatcher runs the new fibre at the next suspension point, for the price
  // of a context switch rather than a thread wake. From anywhere else it
  // round-robins into a slot and wakes a worker only if none is already awake
  // and looking. Either way the context is stealable, which is what makes the
  // local push safe: see the note above WorkerThreadPool.
  WorkerThreadPool::Instance().Schedule(GetBoostState()->context);
}

namespace internal {

Fiber* absl_nullable GetScheduledFiberPtr() {
  const boost::fibers::context* absl_nullable context =
      boost::fibers::context::active();
  if (context == nullptr) {
    LOG(FATAL) << "Current() called outside of a fiber context.";
    ABSL_ASSUME(false);
  }

  const auto* properties =
      dynamic_cast<const FiberProperties*>(context->get_properties());
  return properties == nullptr ? nullptr : properties->GetFiber();
}

}  // namespace internal

void Post(absl::AnyInvocable<void() &&> work) {
  EnsureWorkerThreadPool();
  WorkerThreadPool::Instance().Post(std::move(work));
}

void PostAt(absl::Time deadline, absl::AnyInvocable<void() &&> work) {
  EnsureWorkerThreadPool();
  WorkerThreadPool::Instance().PostAt(deadline, std::move(work));
}

}  // namespace thread
