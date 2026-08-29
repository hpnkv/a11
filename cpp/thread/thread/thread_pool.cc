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
#include <charconv>
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
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <absl/base/call_once.h>
#include <absl/base/no_destructor.h>
#include <absl/base/optimization.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>
#include <absl/functional/any_invocable.h>
#include <absl/log/check.h>
#include <absl/log/log.h>
#include <absl/strings/ascii.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_split.h>
#include <absl/strings/string_view.h>
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
#include <execinfo.h>

#include "thread/boost_primitives.h"
#include "thread/executor.h"
#include "thread/fiber.h"
#include "thread/fiber_diagnostics.h"
#include "thread/internal/work_queue.h"
#include "thread/introspect.h"

// CPU affinity. Linux is the only platform here that has it: see the affinity
// block below for what macOS offers instead, which is nothing.
#if defined(__linux__)
#include <cerrno>
#include <cstring>

#include <pthread.h>
#include <sched.h>
#endif

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

// An exact census of *where* fibers are created, under A11_FIBER_CENSUS=1.
constexpr int kCensusFrames = 7;

struct CensusKey {
  std::array<void*, kCensusFrames> frames{};

  friend bool operator==(const CensusKey& left, const CensusKey& right) {
    return left.frames == right.frames;
  }

  template <typename H>
  friend H AbslHashValue(H hash, const CensusKey& key) {
    for (void* frame : key.frames) {
      hash = H::combine(std::move(hash), frame);
    }
    return hash;
  }
};

bool FiberCensusRequested() {
  static const bool requested = [] {
    const char* value = std::getenv("A11_FIBER_CENSUS");
    return value != nullptr && *value == '1';
  }();
  return requested;
}

// Uses a plain `std::mutex`, not this library's fiber-aware
// `Mutex`, which everything else here uses.
std::mutex& CensusMutex() {
  static absl::NoDestructor<std::mutex> mu;
  return *mu;
}

absl::flat_hash_map<CensusKey, std::uint64_t>& CensusCounts() {
  static absl::NoDestructor<absl::flat_hash_map<CensusKey, std::uint64_t>>
      counts;
  return *counts;
}

void RecordFiberCreation() {
  void* raw[kCensusFrames + 2];
  const int depth = backtrace(raw, kCensusFrames + 2);
  CensusKey key;
  // Skips this frame and Fiber::Start itself, so frame 0 is the caller.
  for (int index = 2; index < depth && index - 2 < kCensusFrames; ++index) {
    key.frames[index - 2] = raw[index];
  }
  const std::lock_guard<std::mutex> lock(CensusMutex());
  ++CensusCounts()[key];
}

void ReportFiberCensus() {
  if (!FiberCensusRequested()) {
    return;
  }
  std::vector<std::pair<CensusKey, std::uint64_t>> ordered;
  {
    const std::lock_guard<std::mutex> lock(CensusMutex());
    ordered.assign(CensusCounts().begin(), CensusCounts().end());
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const auto& left, const auto& right) {
              return left.second > right.second;
            });
  std::fprintf(stderr, "\n=== fiber creation census (%zu distinct sites)\n",
               ordered.size());
  const size_t show = std::min<size_t>(ordered.size(), 12);
  for (size_t index = 0; index < show; ++index) {
    std::fprintf(stderr, "%8llu fibers:\n",
                 static_cast<unsigned long long>(ordered[index].second));
    std::array<void*, kCensusFrames> frames = ordered[index].first.frames;
    int depth = 0;
    while (depth < kCensusFrames && frames[depth] != nullptr) {
      ++depth;
    }
    char** symbols = backtrace_symbols(frames.data(), depth);
    for (int frame = 0; frame < depth; ++frame) {
      std::fprintf(stderr, "         %s\n",
                   symbols != nullptr ? symbols[frame] : "?");
    }
    std::free(symbols);
  }
}

bool PoolStatsRequested() {
  static const bool requested = [] {
    const char* value = std::getenv("A11_POOL_STATS");
    return value != nullptr && *value == '1';
  }();
  return requested;
}

// How many fibers this process has created, under A11_POOL_STATS only.
std::atomic<std::uint64_t> g_fiber_starts{0};

std::uint64_t FiberStartCount() {
  return g_fiber_starts.load(std::memory_order_relaxed);
}

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

// An environment override read as an integer, or nullopt when it is unset or is
// not one.
/// An environment override read as an integer, or nullopt when it is unset or
/// is not one.
///
/// `std::atoi` cannot tell "0" from "not a number", which for a tuning knob
/// means a typo silently turns the knob down to zero instead of leaving the
/// default in place.
std::optional<int> EnvironmentInt(const char* name) {
  const char* setting = std::getenv(name);
  if (setting == nullptr) {
    return std::nullopt;
  }
  const std::string_view text(setting);
  int value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

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

// Distance from a fiber's entry frame to the top of its stack. Boost sets the
// stack pointer to the top and then builds the entry frame just below it.
constexpr size_t kStackTopSlack = 256;

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
// THREAD_DEFAULT_FIBER_STACK_SIZE gets a budget scaled to keep the *count* of
// cached stacks the same, since it's the size nearly every fiber requests.
constexpr size_t kDefaultPoolStacks = 256;
template <size_t StackSize>
constexpr size_t kPoolBudgetBytes =
    StackSize == kDefaultStackSize ? StackSize * kDefaultPoolStacks : 4 * kMiB;

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

size_t EffectiveStackSize(size_t requested_size) {
  const size_t minimum = boost::context::stack_traits::minimum_size();
  const size_t configured =
      requested_size == 0 ? static_cast<size_t>(THREAD_DEFAULT_FIBER_STACK_SIZE)
                          : requested_size;
  return std::max(minimum, configured);
}

PooledFixedSizeStack FixedSizeAllocator(size_t requested_size) {
  return PooledFixedSizeStack(EffectiveStackSize(requested_size));
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
  // This native lock lets Boost's suspend_until() park an OS
  // worker when no fiber can run, and notify() wakes that parked worker from
  // another OS thread.
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

class PoolState;

// Set on pool worker threads to their index, -1 everywhere else.
int& ThisWorkerIndex() {
  static thread_local int index = -1;
  return index;
}

// The CPU this worker was pinned to, -1 when unpinned. Diagnostics and tests.
int& ThisWorkerCpu() {
  static thread_local int cpu = -1;
  return cpu;
}

// True for the spellings that mean "leave the pool alone". Anything else is
// either "on" or a CPU list, and both go through ParseAffinitySpec.
bool AffinitySpecIsOff(absl::string_view spec) {
  const std::string lowered =
      absl::AsciiStrToLower(absl::StripAsciiWhitespace(spec));
  return lowered.empty() || lowered == "0" || lowered == "off" ||
         lowered == "no" || lowered == "false";
}

bool AffinitySpecIsAll(absl::string_view spec) {
  const std::string lowered =
      absl::AsciiStrToLower(absl::StripAsciiWhitespace(spec));
  return lowered == "1" || lowered == "on" || lowered == "yes" ||
         lowered == "true" || lowered == "all";
}

// Pins the calling thread to exactly `cpu`. Called from the worker's own
// thread,
// which is the only thread whose affinity a worker has any business setting.
bool SetThisThreadCpu(int cpu) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  const int error = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  if (error != 0) {
    LOG(WARNING) << "Worker pool could not pin a thread to CPU " << cpu << ": "
                 << std::strerror(error) << ". Continuing unpinned.";
    return false;
  }
  return true;
#else
  (void)cpu;
  return false;
#endif
}

}  // namespace

namespace internal {

// Read from the *process* mask rather than assumed to be 0..nproc-1, which is
// what makes this correct under `taskset`, a cgroup cpuset and a container with
// fewer CPUs than the host: a pool that pinned to a CPU outside.
std::vector<int> ProcessAllowedCpus() {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) != 0) {
    // Happens on a host with more than CPU_SETSIZE (1024) CPUs, where a fixed
    // cpu_set_t is too small. Reporting nothing is right: the pool then
    // declines to pin rather than pinning against a mask it could not read.
    return {};
  }
  std::vector<int> cpus;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &set)) {
      cpus.push_back(cpu);
    }
  }
  return cpus;
#else
  return {};
#endif
}

// A11_POOL_PIN's grammar, against the CPUs this process may actually use:
// unset, "", "0", "off", "no", "false" -> {} , pinning off "1", "on", "yes",
// "true", "all" -> every allowed CPU, in order "0,2,4,6" / "0-7" /.
std::vector<int> ParsePoolAffinitySpec(const char* absl_nullable spec,
                                       const std::vector<int>& allowed) {
  if (spec == nullptr || AffinitySpecIsOff(spec)) {
    return {};
  }
  if (AffinitySpecIsAll(spec)) {
    return allowed;
  }

  std::vector<int> cpus;
  for (absl::string_view token :
       absl::StrSplit(absl::string_view(spec), ',', absl::SkipEmpty())) {
    token = absl::StripAsciiWhitespace(token);
    int first = 0;
    int last = 0;
    const size_t dash = token.find('-', /*pos=*/1);
    if (dash == absl::string_view::npos) {
      if (!absl::SimpleAtoi(token, &first)) {
        return {};
      }
      last = first;
    } else if (!absl::SimpleAtoi(
                   absl::StripAsciiWhitespace(token.substr(0, dash)), &first) ||
               !absl::SimpleAtoi(
                   absl::StripAsciiWhitespace(token.substr(dash + 1)), &last)) {
      return {};
    }
    if (first < 0 || last < first) {
      return {};
    }
    for (int cpu = first; cpu <= last; ++cpu) {
      const bool permitted =
          std::find(allowed.begin(), allowed.end(), cpu) != allowed.end();
      const bool already =
          std::find(cpus.begin(), cpus.end(), cpu) != cpus.end();
      if (permitted && !already) {
        cpus.push_back(cpu);
      }
    }
  }
  return cpus;
}

int ThisWorkerAffinityCpu() {
  return ThisWorkerCpu();
}

}  // namespace internal

namespace {

// Apple arm64 cores have 128-byte cache lines, and x86-64 prefetches in
// 128-byte pairs, so that is the separation worth paying for.
constexpr size_t kCacheLine = 128;

// How much work a slot holds before it spills into its queue's overflow list.
constexpr size_t kQueueCapacity = 256;

// A pool worker's private work, and its park. `contexts` holds *detached* fiber
// contexts: both fibers freshly created by Fiber::Start and fibers that became
// ready again.
struct alignas(kCacheLine) WorkerSlot {
  // How many contexts and callbacks are queued. The two kinds are counted
  // separately because a fiber context and a stackless callback are runnable by
  // different parts of the worker.
  std::atomic<std::uint32_t> context_depth{0};
  std::atomic<std::uint32_t> callback_depth{0};

  // How much work this worker has run, ever.
  std::atomic<std::uint64_t> served{0};
  // Kept only under A11_POOL_STATS, and written only by this slot's own worker.
  std::atomic<std::uint64_t> parks{0};
  std::atomic<std::uint64_t> spin_hits{0};
  std::atomic<std::uint64_t> spin_misses{0};
  std::atomic<std::uint64_t> steals{0};
  // Signals aimed at this worker, by anybody.
  std::atomic<std::uint64_t> signals{0};
  // The CPU this worker actually pinned itself to, -1 when it did not.
  std::atomic<int> cpu{-1};

  std::uint32_t depth(std::memory_order order) const {
    return context_depth.load(order) + callback_depth.load(order);
  }

  // The work itself.
  alignas(kCacheLine)
      internal::WorkQueue<boost::fibers::context*, kQueueCapacity> contexts;
  alignas(kCacheLine) internal::WorkQueue<PoolWork, kQueueCapacity> callbacks;

  // Where an idle main context waits.
  alignas(kCacheLine) thread::Mutex idle_mu;
  thread::CondVar idle_cv;

  // The park. It is atomic so that a *spinning* worker can watch it without
  // taking park_mu.
  alignas(kCacheLine) std::mutex park_mu;
  std::condition_variable park_cv;
  std::atomic<std::uint64_t> wake_seq{0};
  // The deadline this worker's park is armed to, and int64 max whenever it is
  // not parked.
  std::atomic<std::int64_t> park_deadline_ns{
      std::numeric_limits<std::int64_t>::max()};
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

// How long a worker looks for work before parking.
absl::Duration SpinBudget() {
  static const absl::Duration budget = [] {
    const std::optional<int> override_us = EnvironmentInt("A11_POOL_SPIN_US");
    if (!override_us.has_value()) {
      return absl::Microseconds(30);
    }
    return absl::Microseconds(std::max(0, *override_us));
  }();
  return budget;
}

// Clock reads per spin round would dominate the round; this is how many
// rounds run between them.
constexpr int kSpinRoundsPerClockCheck = 32;

// How many spin rounds run between sweeps of the slot depths. See the spin loop
// in RunWorker for why a spinner should not read them on every round.
constexpr int kSpinRoundsPerScan = 8;

// A worker's *first* park with nothing to do. Nothing depends on it: it is a
// safety net that turns any residual lost wakeup into a latency blip instead of
// a hang.
constexpr absl::Duration kMaxPark = absl::Milliseconds(50);

// How far that park is allowed to stretch when a worker keeps waking to find
// nothing, and the factor it stretches by.
constexpr absl::Duration kMaxIdlePark = absl::Seconds(1);
constexpr int kParkBackoffFactor = 2;

// One second, and not longer, for a measured reason rather than caution.

class PoolState {
 public:
  PoolState() = default;

  PoolState(const PoolState&) = delete;
  PoolState& operator=(const PoolState&) = delete;

  void Start(size_t num_threads);

  size_t size() const { return num_workers_; }

  WorkerSlot& slot(size_t index) { return slots_[index]; }

  // The CPU worker `index` should pin itself to, or -1 when the pool is not
  // pinning. See affinity_cpus_.
  int cpu_for(size_t index) const {
    if (affinity_cpus_.empty()) {
      return -1;
    }
    return affinity_cpus_[index % affinity_cpus_.size()];
  }

  // Which slot a new piece of work should go to.
  size_t PreferredSlot() {
    const int index = ThisWorkerIndex();
    if (index >= 0) {
      return static_cast<size_t>(index);
    }
    // Zero disables recruitment entirely and spreads over every slot. Kept as
    // the control to measure the policy against.
    if (recruit_backlog_ == 0) {
      return NextVictim();
    }

    const std::uint64_t idle = idle_mask_.load(std::memory_order_relaxed);

    // The hot set first, then the rest. Inside a range the first sleeper wins
    // outright, otherwise the first worker under the threshold.
    const size_t hot = std::min<size_t>(hot_workers_, num_workers_);
    if (const size_t candidate = PreferredWithin(0, hot, idle);
        candidate != num_workers_) {
      return candidate;
    }
    if (const size_t candidate = PreferredWithin(hot, num_workers_, idle);
        candidate != num_workers_) {
      return candidate;
    }
    // Everyone is backlogged: nothing to be gained by preferring anybody.
    return NextVictim();
  }

  // "This push has to wake somebody, whatever the economy thinks." True only
  // for a push from outside the pool into a pool that had nothing to do.
  bool MustWake() const {
    return ThisWorkerIndex() < 0 && !AnyWork(std::memory_order_relaxed);
  }

  void PushContext(boost::fibers::context* absl_nonnull context, size_t index,
                   bool wake) {
    // Computed *before* the push, or this push's own item makes the pool look
    // busy to itself and the test can never fire.
    const bool force = wake && MustWake();
    slots_[index].contexts.Push(context);
    Published(slots_[index].context_depth);
    if (wake) {
      WakeSomeone(index, force);
    }
  }

  void PushCallback(PoolWork work, size_t index) {
    slots_[index].callbacks.Push(std::move(work));
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
        Count(slots_[index].steals);
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
        Count(slots_[index].steals);
        return true;
      }
    }
    return false;
  }

  // Is there anything to run anywhere?
  bool AnyWork(std::memory_order order = std::memory_order_seq_cst) const {
    for (size_t index = 0; index < num_workers_; ++index) {
      if (slots_[index].depth(order) != 0) {
        return true;
      }
    }
    return false;
  }

  // Only fiber contexts, which is all a dispatcher can run.
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

  // Wakes an OS thread only if one is actually needed. Measured: this is what
  // the deferred fiber reap exposed.
  void WakeSomeone(size_t preferred, bool force = false) {
    // Diagnostic: A11_POOL_ALWAYS_WAKE=1 disables the economy entirely, so a
    // push always signals somebody.
    if (always_wake_) {
      for (size_t index = 0; index < num_workers_; ++index) {
        WakeWorker(index);
      }
      return;
    }
    // seq_cst: pairs with the registration on the idle path so that a worker
    // about to go idle and a producer about to skip the wake cannot both
    // conclude the other will handle it.
    if (!force && spinning_.load(std::memory_order_seq_cst) > 0) {
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

  // Both halves of waking an idle worker, because it can be asleep at either of
  // two levels and a producer cannot cheaply tell which.
  void WakeWorker(size_t index) {
    slots_[index].idle_cv.Signal();
    Signal(index);
  }

  // Wakes a specific worker unconditionally: used for `notify()`, where a
  // context is bound to that worker's scheduler and nobody else can run it.
  void Signal(size_t index) {
    WorkerSlot& target = slots_[index];
    Count(target.signals);
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

  // Makes sure some worker will be awake by `deadline_ns` to run a timer that
  // just became the earliest one, and wakes nobody at all if one already will
  // be.
  void CoverDeadline(std::int64_t deadline_ns) {
    // Acquire rather than seq_cst, unlike the wake economy: this decides how
    // *soon* a worker wakes and not whether one does at all, and every way it
    // can be wrong is bounded.
    const std::uint64_t idle = idle_mask_.load(std::memory_order_acquire);
    // Nobody is parked. An awake worker collects due timers on every pass round
    // its loop, so there is nothing to arrange.
    if (idle == 0) {
      return;
    }
    for (std::uint64_t remaining = idle; remaining != 0;
         remaining &= remaining - 1) {
      const int index = std::countr_zero(remaining);
      if (slots_[index].park_deadline_ns.load(std::memory_order_acquire) <=
          deadline_ns) {
        return;
      }
    }
    WakeWorker(static_cast<size_t>(std::countr_zero(idle)));
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

  // The diagnostic counters, which are off unless A11_POOL_STATS asked for
  // them: `stats_` is written once during Start() and only read afterwards, so
  // this costs a well-predicted branch on a shared line rather than a write.
  void Count(std::atomic<std::uint64_t>& counter) const {
    if (stats_) {
      counter.fetch_add(1, std::memory_order_relaxed);
    }
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

  // seq_cst: this is one half of the Dekker pair with the idle path.
  static void Published(std::atomic<std::uint32_t>& counter) {
    counter.fetch_add(1, std::memory_order_seq_cst);
  }

  // Relaxed: an over-count only costs a worker one more trip round its loop.
  static void Consumed(std::atomic<std::uint32_t>& counter) {
    counter.fetch_sub(1, std::memory_order_relaxed);
  }

  size_t NextVictim() {
    return next_victim_.fetch_add(1, std::memory_order_relaxed) % num_workers_;
  }

  // The best slot in `[begin, end)`, or num_workers_ if every one of them is at
  // or over the backlog threshold.
  size_t PreferredWithin(size_t begin, size_t end, std::uint64_t idle) const {
    size_t awake = num_workers_;
    for (size_t candidate = begin; candidate < end; ++candidate) {
      if (slots_[candidate].depth(std::memory_order_relaxed) >=
          recruit_backlog_) {
        continue;
      }
      if ((idle & (std::uint64_t{1} << candidate)) != 0) {
        return candidate;
      }
      if (awake == num_workers_) {
        awake = candidate;
      }
    }
    return awake;
  }

  boost::fibers::context* absl_nullable TryPopContext(size_t index) {
    WorkerSlot& target = slots_[index];
    boost::fibers::context* context = nullptr;
    if (!target.contexts.Pop(context)) {
      return nullptr;
    }
    Consumed(target.context_depth);
    target.served.fetch_add(1, std::memory_order_relaxed);
    return context;
  }

  bool TryPopCallback(size_t index, PoolWork& out) {
    WorkerSlot& target = slots_[index];
    if (!target.callbacks.Pop(out)) {
      return false;
    }
    Consumed(target.callback_depth);
    target.served.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  std::unique_ptr<WorkerSlot[]> slots_;
  size_t num_workers_ = 0;
  // How many workers may look for work at once.
  std::uint32_t max_spinners_ = 1;
  // Diagnostics only; see the dials read in Start().
  bool always_wake_ = false;
  bool no_steal_ = false;
  bool stats_ = false;

  // How long a worker's queue has to get before it counts as unable to keep up.
  std::uint32_t recruit_backlog_ = 1;

  // How many workers external work is offered before the rest of the pool is.
  std::uint32_t hot_workers_ = 4;

  // Which CPU each worker is pinned to, indexed by worker modulo its size.
  std::vector<int> affinity_cpus_;

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
  std::int64_t head = 0;
  {
    std::lock_guard<std::mutex> lock(timed_mu_);
    timed_work_.push_back(TimedWork{
        .deadline = deadline,
        .sequence = next_timed_sequence_++,
        .work = std::move(work),
    });
    std::push_heap(timed_work_.begin(), timed_work_.end(), LaterDeadline{});
    head = absl::ToUnixNanos(timed_work_.front().deadline);
    // seq_cst, and paired with the seq_cst read of it on the park path: the
    // new head has to be visible to anyone who parks after this point, or the
    // signal below has to reach them.
    became_head =
        head != earliest_timer_ns_.exchange(head, std::memory_order_seq_cst);
  }
  // Only a timer that moved the head can shorten anybody's park, and only a
  // worker that is actually parked has a deadline to shorten.
  if (became_head) {
    CoverDeadline(head);
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
  // is idle enough to steal it. Due timers keep their deadline order, which the
  // queue preserves.
  for (PoolWork& work : due) {
    slots_[index].callbacks.Push(std::move(work));
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
    ctx->detach();
    state_->PushContext(ctx, index_, /*wake=*/false);
  }

  boost::fibers::context* absl_nullable pick_next() noexcept override {
    // The main context is what runs stackless callbacks, so preferring ready
    // fibers unconditionally would let a steady stream of fibers starve every
    // posted callback on this worker.
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

  // Called by the dispatcher when it has nothing to run. Boost's notify() still
  // reaches it directly for contexts bound to this scheduler, which is the case
  // that matters.
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
  void Schedule(const boost::intrusive_ptr<boost::fibers::context>& context);
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
  if (const std::optional<int> spinners = EnvironmentInt("A11_POOL_SPINNERS");
      spinners.has_value()) {
    max_spinners_ = static_cast<std::uint32_t>(std::max(0, *spinners));
  }
  if (const std::optional<int> recruit = EnvironmentInt("A11_POOL_RECRUIT");
      recruit.has_value()) {
    recruit_backlog_ = static_cast<std::uint32_t>(std::max(0, *recruit));
  }
  if (const std::optional<int> hot = EnvironmentInt("A11_POOL_HOT");
      hot.has_value()) {
    hot_workers_ = static_cast<std::uint32_t>(std::max(1, *hot));
  }

  always_wake_ = std::getenv("A11_POOL_ALWAYS_WAKE") != nullptr;
  no_steal_ = std::getenv("A11_POOL_NO_STEAL") != nullptr;

  // A11_POOL_PIN.
  if (const char* pin = std::getenv("A11_POOL_PIN");
      pin != nullptr && !AffinitySpecIsOff(pin)) {
    const std::vector<int> allowed = internal::ProcessAllowedCpus();
    affinity_cpus_ = internal::ParsePoolAffinitySpec(pin, allowed);
    if (affinity_cpus_.empty()) {
      LOG(WARNING)
          << "A11_POOL_PIN=" << pin
          << " asked the worker pool to pin its threads, and it will "
             "not: "
          << (allowed.empty()
                  ? "this platform has no CPU affinity (macOS included "
                    "-- THREAD_AFFINITY_POLICY is unimplemented on "
                    "Apple arm64 and is a cache hint rather than "
                    "pinning where it is implemented)"
                  : "the spec named no CPU this process is allowed to "
                    "run on")
          << ". The pool runs unpinned.";
    }
  }

  // A11_POOL_STATS=1 prints how the work actually landed, which is how the two
  // policies in this file are checked rather than assumed.
  const std::optional<int> stats = EnvironmentInt("A11_POOL_STATS");
  stats_ = stats.value_or(0) != 0;
  if (stats_) {
    static PoolState* reporting = this;
    // stderr rather than LOG: this runs at exit, where the Abseil sink may be
    // bridged into Python logging and touching it would mean taking the GIL
    // during interpreter teardown, which can abort the process.
    std::atexit([] {
      std::string line;
      std::uint64_t total = 0;
      std::uint64_t parks = 0;
      std::uint64_t hits = 0;
      std::uint64_t misses = 0;
      std::uint64_t steals = 0;
      std::uint64_t signals = 0;
      size_t used = 0;
      std::string cpus;
      for (size_t index = 0; index < reporting->num_workers_; ++index) {
        const WorkerSlot& slot = reporting->slots_[index];
        const std::uint64_t served =
            slot.served.load(std::memory_order_relaxed);
        total += served;
        parks += slot.parks.load(std::memory_order_relaxed);
        hits += slot.spin_hits.load(std::memory_order_relaxed);
        misses += slot.spin_misses.load(std::memory_order_relaxed);
        steals += slot.steals.load(std::memory_order_relaxed);
        signals += slot.signals.load(std::memory_order_relaxed);
        if (served != 0) {
          ++used;
        }
        absl::StrAppend(&line, index == 0 ? "" : " ", served);
        const int cpu = slot.cpu.load(std::memory_order_relaxed);
        absl::StrAppend(&cpus, index == 0 ? "" : " ",
                        cpu < 0 ? std::string("-") : absl::StrCat(cpu));
      }
      std::fprintf(stderr,
                   "pool served %llu items across %zu/%zu workers: %s\n"
                   "pool cpus %s\n"
                   "pool fibers started %llu\n"
                   "pool parks %llu (%.3f/item) spins %llu hit / %llu miss, "
                   "steals %llu (%.3f/item), signals %llu (%.3f/item)\n",
                   static_cast<unsigned long long>(total), used,
                   reporting->num_workers_, line.c_str(), cpus.c_str(),
                   static_cast<unsigned long long>(
                       g_fiber_starts.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(parks),
                   total == 0 ? 0.0 : static_cast<double>(parks) / total,
                   static_cast<unsigned long long>(hits),
                   static_cast<unsigned long long>(misses),
                   static_cast<unsigned long long>(steals),
                   total == 0 ? 0.0 : static_cast<double>(steals) / total,
                   static_cast<unsigned long long>(signals),
                   total == 0 ? 0.0 : static_cast<double>(signals) / total);
      ReportFiberCensus();
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
  // Before the scheduler, on purpose: this worker's dispatcher context, its
  // remote-ready queue and the first fiber stacks it takes out of the pooled
  // allocators are all first touched below, and a page's first touch is what.
  if (const int cpu = state_.cpu_for(index); cpu >= 0) {
    if (SetThisThreadCpu(cpu)) {
      ThisWorkerCpu() = cpu;
      state_.slot(index).cpu.store(cpu, std::memory_order_relaxed);
    }
  }

  ThisWorkerIndex() = static_cast<int>(index);
  EnsureThreadHasScheduler<PoolAlgorithm>(&state_, index);

  WorkerSlot& self = state_.slot(index);

  // How long this worker will park next time it finds nothing. Grows while it
  // keeps finding nothing and snaps back to kMaxPark the moment it does. See
  // kMaxIdlePark.
  absl::Duration idle_park = kMaxPark;

  // See the reap call below. Per-worker, so no sharing and no atomics.
  constexpr size_t kReapEveryPasses = 8;
  constexpr size_t kReapBacklog = 64;
  size_t passes_since_reap = 0;

  while (true) {
    if (state_.shutting_down()) {
      break;
    }

    // Reaping rides along on the workers rather than owning a fiber of its own:
    // see ReapWhenFinished().
    if (++passes_since_reap >= kReapEveryPasses ||
        PendingReapCount() >= kReapBacklog) {
      passes_since_reap = 0;
      ReapFinishedFibers();
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

    // Hand the thread to the fiber scheduler: this is where contexts pushed to
    // (or stolen into) this worker actually run. It returns once no fiber is
    // ready, having cost two context switches -- nanoseconds, not a wake.
    boost::this_fiber::yield();

    // Relaxed: a false negative here only means dropping into the spin, which
    // re-checks properly before anything actually parks.
    if (did_work || state_.AnyWork(std::memory_order_relaxed)) {
      idle_park = kMaxPark;
      continue;
    }

    // Nothing anywhere.
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
          // way a spinner learns about a fiber bound to its own scheduler that
          // another thread just made ready.
          if (self.WakePending() || state_.shutting_down()) {
            found = true;
            break;
          }
          if (round % kSpinRoundsPerScan == 0 &&
              state_.AnyWork(std::memory_order_relaxed)) {
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
      state_.Count(found ? self.spin_hits : self.spin_misses);
      if (found) {
        idle_park = kMaxPark;
        continue;
      }
    }

    // About to sleep, so reap now regardless of the throttle above. Here the
    // worker has nothing better to do and the queue is usually empty, in which
    // case this touches no lock at all.
    ReapFinishedFibers();
    passes_since_reap = 0;

    // Go idle by suspending the main *fiber*, not the thread.
    state_.MarkIdle(index);
    {
      thread::MutexLock lock(&self.idle_mu);
      // Re-check after publishing "idle" and under the lock. Both sides are
      // seq_cst, so a producer that skipped its signal because it saw nobody
      // idle must be visible here.
      if (!self.WakePending() && !state_.AnyWork() && !state_.shutting_down()) {
        // Stretched by how many parks in a row have come up empty, and never
        // past the earliest real timer -- so a deadline in the sleep queue is
        // honoured to the same precision as before.
        const absl::Time deadline =
            std::min(absl::Now() + idle_park, state_.EarliestTimer());
        // Published before the wait and withdrawn after it, so that a timer
        // registered while this worker is parked can tell whether this park
        // already covers it. See PoolState::CoverDeadline.
        self.park_deadline_ns.store(absl::ToUnixNanos(deadline),
                                    std::memory_order_release);
        state_.Count(self.parks);
        self.idle_cv.WaitWithDeadline(&self.idle_mu, deadline);
        self.park_deadline_ns.store(std::numeric_limits<std::int64_t>::max(),
                                    std::memory_order_release);
      }
    }
    state_.ClearIdle(index);
    // A park that was ended by an actual signal means work arrived, so the next
    // one starts short again; a park that simply expired means this worker is
    // still idle and may wait longer next time.
    if (self.WakePending()) {
      idle_park = kMaxPark;
    } else {
      idle_park = std::min(kMaxIdlePark, idle_park * kParkBackoffFactor);
    }
    self.consumed_seq = self.wake_seq.load(std::memory_order_acquire);
  }

  DLOG(INFO) << "Worker " << index << " exiting.";
}

void WorkerThreadPool::Schedule(
    const boost::intrusive_ptr<boost::fibers::context>& context) {
  CHECK(!state_.shutting_down())
      << "Cannot schedule work after worker-pool shutdown.";
  CHECK(context->get_scheduler() == nullptr)
      << "Cannot schedule an already scheduled context.";
  // The fiber keeps the reference alive until it is attached and running; the
  // pool holds a raw pointer, as Boost's own algorithms do.
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
    // A11_POOL_THREADS pins the worker count, which the pool's throughput
    // depends strongly on and which therefore has to be settable.
    const std::optional<int> override_count =
        EnvironmentInt("A11_POOL_THREADS");
    if (override_count.has_value()) {
      instance->Start(static_cast<size_t>(std::max(0, *override_count)));
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
  absl::call_once(worker_pool_once, [] {
    WorkerThreadPool::Instance();
    internal::InstallFiberDiagnosticsFromEnvironment();
  });
}

}  // namespace

void Fiber::Start() {
  EnsureWorkerThreadPool();
  if (PoolStatsRequested()) {
    g_fiber_starts.fetch_add(1, std::memory_order_relaxed);
  }
  if (FiberCensusRequested()) {
    RecordFiberCreation();
  }
  // EnsureThreadHasScheduler runs only once per thread, so a pool worker keeps
  // the PoolAlgorithm it was started with.
  EnsureThreadHasScheduler<InstrumentedRoundRobin>();

  const size_t stack_size = EffectiveStackSize(tree_options_.stack_size);
  auto body = [this, stack_size] {
    // Bounds published from inside the fiber: at entry the frame address is
    // within a few words of the stack top, and the size is known. They let a
    // report reject a corrupt frame-pointer chain instead of following it.
    auto* const top = static_cast<std::byte*>(__builtin_frame_address(0));
    diagnostics_.stack_hi = top + kStackTopSlack;
    diagnostics_.stack_lo = top + kStackTopSlack - stack_size;

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

  diagnostics_.context = GetBoostState()->context.get();

  // From a pool worker this is a push onto that worker's own slot -- its own
  // dispatcher runs the new fiber at the next suspension point, for the price
  // of a context switch rather than a thread wake.
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

FiberDiagnostics* absl_nullable CurrentFiberDiagnostics() {
  const boost::fibers::context* absl_nullable context =
      boost::fibers::context::active();
  if (context == nullptr) {
    return nullptr;
  }
  const auto* properties =
      dynamic_cast<const FiberProperties*>(context->get_properties());
  return properties == nullptr ? nullptr
                               : &properties->GetFiber()->Diagnostics();
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
