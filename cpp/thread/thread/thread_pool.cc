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
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
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
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <boost/context/detail/prefetch.hpp>
#include <boost/context/fixedsize_stack.hpp>
#include <boost/context/segmented_stack.hpp>  // IWYU pragma: keep
#include <boost/fiber/algo/algorithm.hpp>
#include <boost/fiber/algo/shared_work.hpp>
#include <boost/fiber/all.hpp>
#include <boost/fiber/context.hpp>
#include <boost/fiber/operations.hpp>
#include <boost/fiber/properties.hpp>
#include <boost/fiber/scheduler.hpp>
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
    if (void* sp = slot.exchange(nullptr, std::memory_order_acquire))
      return sp;
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
    if (TryReleaseToPool(context.size, context.sp))
      return;
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
  std::uint64_t wake_seq_ ABSL_GUARDED_BY(mu_) = 0;
  std::uint64_t consumed_wake_seq_ ABSL_GUARDED_BY(mu_) = 0;
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

  // boost::context::

 private:
  struct Worker {
    std::thread thread;
  };

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

  thread::Mutex mu_;
  thread::CondVar cv_;
  absl::InlinedVector<Worker, 16> workers_;
  std::deque<boost::intrusive_ptr<boost::fibers::context>> work_queue_
      ABSL_GUARDED_BY(mu_);
  std::deque<PoolWork> callback_queue_ ABSL_GUARDED_BY(mu_);
  std::vector<TimedWork> timed_work_ ABSL_GUARDED_BY(mu_);
  std::uint64_t next_timed_sequence_ ABSL_GUARDED_BY(mu_) = 0;
  bool shutdown_ ABSL_GUARDED_BY(mu_) = false;
};

WorkerThreadPool::~WorkerThreadPool() {
  {
    thread::MutexLock lock(&mu_);
    shutdown_ = true;
    cv_.SignalAll();
  }
  for (auto& worker : workers_) {
    if (worker.thread.joinable()) {
      worker.thread.join();
    }
  }
}

void WorkerThreadPool::Start(size_t num_threads) {
  if (num_threads == 0) {
    num_threads = 1;
  }
  for (size_t index = 0; index < num_threads; ++index) {
    workers_.push_back({
        .thread = std::thread([this, index] {
          EnsureThreadHasScheduler<boost::fibers::algo::shared_work>(
              /*suspend=*/true);

          while (true) {
            PoolWork callback;
            {
              thread::MutexLock lock(&mu_);
              while (!work_queue_.empty()) {
                boost::intrusive_ptr<boost::fibers::context> context =
                    std::move(work_queue_.front());
                work_queue_.pop_front();

                // Attach on a scheduler-bearing worker; shared_work may then
                // migrate the context among workers.
                boost::fibers::context* active_context =
                    boost::fibers::context::active();
                active_context->attach(context.get());
                active_context->schedule(context.get());
              }

              if (shutdown_) {
                break;
              }
              if (!callback_queue_.empty()) {
                callback = std::move(callback_queue_.front());
                callback_queue_.pop_front();
              } else if (!timed_work_.empty() &&
                         timed_work_.front().deadline <= absl::Now()) {
                std::pop_heap(timed_work_.begin(), timed_work_.end(),
                              LaterDeadline{});
                callback = std::move(timed_work_.back().work);
                timed_work_.pop_back();
              } else {
                const absl::Duration wait =
                    timed_work_.empty()
                        ? absl::Milliseconds(50)
                        : std::max(absl::ZeroDuration(),
                                   timed_work_.front().deadline - absl::Now());
                cv_.WaitWithTimeout(&mu_, wait);
              }
            }

            if (callback != nullptr) {
              try {
                std::move(callback)();
              } catch (const std::exception& error) {
                LOG(ERROR) << "Unobserved stackless callback exception: "
                           << error.what();
              } catch (...) {
                LOG(ERROR)
                    << "Unobserved stackless callback non-standard exception";
              }
            }
            // Give attached fiber contexts an opportunity to run after each
            // stackless callback or idle wait.
            boost::this_fiber::yield();
          }
          DLOG(INFO) << "Worker " << index << " exiting.";
        }),
    });
  }
}

void WorkerThreadPool::Schedule(
    boost::intrusive_ptr<boost::fibers::context> context) {
  thread::MutexLock lock(&mu_);
  CHECK(!shutdown_) << "Cannot schedule work after worker-pool shutdown.";
  CHECK(context->get_scheduler() == nullptr)
      << "Cannot schedule an already scheduled context.";
  work_queue_.push_back(std::move(context));
  cv_.Signal();
}

void WorkerThreadPool::Post(PoolWork work) {
  if (work == nullptr) {
    return;
  }
  thread::MutexLock lock(&mu_);
  CHECK(!shutdown_) << "Cannot post work after worker-pool shutdown.";
  callback_queue_.push_back(std::move(work));
  cv_.Signal();
}

void WorkerThreadPool::PostAt(absl::Time deadline, PoolWork work) {
  if (work == nullptr) {
    return;
  }
  thread::MutexLock lock(&mu_);
  CHECK(!shutdown_) << "Cannot post timed work after worker-pool shutdown.";
  timed_work_.push_back(TimedWork{
      .deadline = deadline,
      .sequence = next_timed_sequence_++,
      .work = std::move(work),
  });
  std::push_heap(timed_work_.begin(), timed_work_.end(), LaterDeadline{});
  cv_.SignalAll();
}

WorkerThreadPool& WorkerThreadPool::Instance() {
  static absl::NoDestructor<WorkerThreadPool> instance;
  static const bool started = [] {
    instance->Start();
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
