// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief A11's one libuv loop, and how to get work onto it.
 *
 * A process-wide thread owns every uvw handle. Fibres submit work through
 * Post() or RunOnUv() and receive results through A11 Futures.
 *
 * Work sharing an @c order_key runs in posting order; use the connection as the
 * key for operations on one socket. Drain() alternates between keys so a busy
 * connection does not hold up unrelated work. `A11_UV_FAIR=0` selects FIFO
 * draining, and `A11_UV_DRAIN_STATS=1` reports queue statistics at exit.
 */

#ifndef A11_UV_LOOP_H_
#define A11_UV_LOOP_H_

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <uvw.hpp>
#include <vector>

#include <absl/base/no_destructor.h>
#include <absl/base/thread_annotations.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "thread/boost_primitives.h"

namespace a11::uv {

/** @brief The status a failed libuv call deserves. */
inline absl::Status UvError(int code, std::string_view operation) {
  return absl::UnavailableError(
      absl::StrCat(operation, " failed: ", uv_strerror(code)));
}

// One libuv loop is shared by native HTTP clients and servers. All uvw and
// protocol mutation is serialized on this thread; fiber callbacks communicate
// through A11 Futures and never block the loop.
class UvExecutor {
 public:
  static UvExecutor& Instance() {
    // The process-wide I/O scheduler lives until process exit.
    static absl::NoDestructor<UvExecutor> executor;
    // Wait outside the static initialization guard because the fibre-aware wait
    // may yield and allow another fibre to enter Instance().
    executor->EnsureStarted();
    return *executor;
  }

  /**
   * @brief Queue work for the loop thread.
   *
   * @param work  What to run on the loop.
   * @param order_key
   *   Ordering group for the work. Pass the connection for operations on one
   *   socket so headers, data, and completion remain ordered. Leave it null
   *   when no per-connection ordering is required.
   */
  absl::Status Post(std::function<void()> work,
                    const void* order_key = nullptr) {
    if (!work) {
      return absl::InvalidArgumentError("uv work must be callable");
    }
    {
      thread::MutexLock lock(&mu_);
      if (!running_) {
        return absl::FailedPreconditionError("The A11 libuv loop is stopped");
      }
      work_.push_back(Item{.key = order_key, .work = std::move(work)});
    }
    const int result = async_->send();
    if (result != 0) {
      return UvError(result, "uv_async_send");
    }
    return absl::OkStatus();
  }

  [[nodiscard]] std::shared_ptr<uvw::loop> loop() const { return loop_; }

  [[nodiscard]] bool IsLoopThread() const {
    thread::MutexLock lock(&mu_);
    return loop_thread_id_.has_value() &&
           *loop_thread_id_ == std::this_thread::get_id();
  }

 private:
  friend class absl::NoDestructor<UvExecutor>;

  UvExecutor() {
    {
      loop_ = uvw::loop::create();
      async_ = loop_->resource<uvw::async_handle>();
      const int initialized = async_->init();
      if (initialized != 0) {
        LOG(FATAL) << "Could not initialize the A11 libuv executor: "
                   << uv_strerror(initialized);
      }
      async_->on<uvw::async_event>(
          [this](const uvw::async_event&, uvw::async_handle&) { Drain(); });
      thread_ = std::thread([this]() {
        {
          thread::MutexLock lock(&mu_);
          loop_thread_id_ = std::this_thread::get_id();
          cv_.SignalAll();
        }
        loop_->run();
        thread::MutexLock lock(&mu_);
        running_ = false;
      });
    }
  }

  /// Blocks until the loop thread starts. Safe to call from multiple fibres.
  void EnsureStarted() {
    thread::MutexLock lock(&mu_);
    while (!loop_thread_id_.has_value()) {
      cv_.Wait(&mu_);
    }
  }

  /**
   * @brief Run everything queued, round-robin across order keys.
   *
   * Runs one item per key per round while preserving posting order within each
   * key. Single-item and single-key batches skip grouping.
   */
  void Drain() {
    std::deque<Item> batch;
    {
      thread::MutexLock lock(&mu_);
      batch.swap(work_);
    }
    if (DrainStatsEnabled()) {
      RecordDrainBatch(batch);
      // Time each item separately when drain statistics are enabled.
      for (Item& item : batch) {
        const absl::Time started = absl::Now();
        item.work();
        RecordItemDuration(absl::ToInt64Nanoseconds(absl::Now() - started));
      }
      return;
    }
    if (batch.size() <= 1 || !FairDraining()) {
      for (Item& item : batch) {
        item.work();
      }
      return;
    }

    std::vector<const void*> keys;
    absl::flat_hash_map<const void*, std::deque<std::function<void()>>> lanes;
    for (Item& item : batch) {
      auto [lane, fresh] = lanes.try_emplace(item.key);
      if (fresh) {
        keys.push_back(item.key);
      }
      lane->second.push_back(std::move(item.work));
    }
    if (keys.size() == 1) {
      for (std::function<void()>& work : lanes.begin()->second) {
        work();
      }
      return;
    }
    size_t remaining = batch.size();
    while (remaining > 0) {
      for (const void* key : keys) {
        std::deque<std::function<void()>>& lane = lanes.at(key);
        if (lane.empty()) {
          continue;
        }
        std::function<void()> work = std::move(lane.front());
        lane.pop_front();
        --remaining;
        work();
      }
    }
  }

  struct Item {
    /// What this work must stay ordered against; null means "only other nulls".
    const void* key = nullptr;
    std::function<void()> work;
  };

  /// A11_UV_DRAIN_STATS=1 reports batch and key-count statistics at exit.
  static bool DrainStatsEnabled() {
    static const bool on = [] {
      const char* setting = std::getenv("A11_UV_DRAIN_STATS");
      int value = 0;
      return setting != nullptr && absl::SimpleAtoi(setting, &value) &&
             value != 0;
    }();
    return on;
  }

  /// Record how long one queued item held the loop thread.
  static void RecordItemDuration(std::int64_t nanos) {
    struct Buckets {
      std::atomic<std::uint64_t> under_10us{0};
      std::atomic<std::uint64_t> under_100us{0};
      std::atomic<std::uint64_t> under_1ms{0};
      std::atomic<std::uint64_t> over_1ms{0};
      std::atomic<std::uint64_t> total_nanos{0};
      std::atomic<std::int64_t> worst_nanos{0};
    };

    static absl::NoDestructor<Buckets> buckets;
    static const bool registered = [] {
      std::atexit([] {
        std::fprintf(
            stderr,
            "uv item: <10us %llu, <100us %llu, <1ms %llu, >=1ms %llu, "
            "busy %.1f ms total, worst %.0f us\n",
            static_cast<unsigned long long>(buckets->under_10us.load()),
            static_cast<unsigned long long>(buckets->under_100us.load()),
            static_cast<unsigned long long>(buckets->under_1ms.load()),
            static_cast<unsigned long long>(buckets->over_1ms.load()),
            static_cast<double>(buckets->total_nanos.load()) / 1e6,
            static_cast<double>(buckets->worst_nanos.load()) / 1e3);
      });
      return true;
    }();
    (void)registered;
    // Plain literals, no digit separators: clang-format has been seen to read
    // 1'000'000 as character literals when reflowing this region.
    constexpr std::int64_t kTenMicros = 10000;
    constexpr std::int64_t kHundredMicros = 100000;
    constexpr std::int64_t kMilli = 1000000;
    buckets->total_nanos.fetch_add(static_cast<std::uint64_t>(nanos),
                                   std::memory_order_relaxed);
    if (nanos < kTenMicros) {
      buckets->under_10us.fetch_add(1, std::memory_order_relaxed);
    } else if (nanos < kHundredMicros) {
      buckets->under_100us.fetch_add(1, std::memory_order_relaxed);
    } else if (nanos < kMilli) {
      buckets->under_1ms.fetch_add(1, std::memory_order_relaxed);
    } else {
      buckets->over_1ms.fetch_add(1, std::memory_order_relaxed);
    }
    std::int64_t seen = buckets->worst_nanos.load(std::memory_order_relaxed);
    while (nanos > seen && !buckets->worst_nanos.compare_exchange_weak(
                               seen, nanos, std::memory_order_relaxed)) {}
  }

  static void RecordDrainBatch(const std::deque<Item>& batch) {
    struct Stats {
      std::atomic<std::uint64_t> drains{0};
      std::atomic<std::uint64_t> items{0};
      std::atomic<std::uint64_t> multi_item{0};
      std::atomic<std::uint64_t> multi_key{0};
      std::atomic<std::uint64_t> largest{0};
    };

    static absl::NoDestructor<Stats> stats;
    static const bool registered = [] {
      std::atexit([] {
        const std::uint64_t drains = stats->drains.load();
        const double per = drains == 0 ? 1.0 : static_cast<double>(drains);
        std::fprintf(
            stderr,
            "uv drain: %llu drains, %llu items (%.2f/drain), "
            "%llu with >1 item (%.1f%%), %llu with >1 key (%.1f%%), "
            "largest %llu\n",
            static_cast<unsigned long long>(drains),
            static_cast<unsigned long long>(stats->items.load()),
            drains == 0 ? 0.0 : static_cast<double>(stats->items.load()) / per,
            static_cast<unsigned long long>(stats->multi_item.load()),
            drains == 0
                ? 0.0
                : 100.0 * static_cast<double>(stats->multi_item.load()) / per,
            static_cast<unsigned long long>(stats->multi_key.load()),
            drains == 0
                ? 0.0
                : 100.0 * static_cast<double>(stats->multi_key.load()) / per,
            static_cast<unsigned long long>(stats->largest.load()));
      });
      return true;
    }();
    (void)registered;
    stats->drains.fetch_add(1, std::memory_order_relaxed);
    stats->items.fetch_add(batch.size(), std::memory_order_relaxed);
    if (batch.size() > 1) {
      stats->multi_item.fetch_add(1, std::memory_order_relaxed);
      absl::flat_hash_set<const void*> keys;
      for (const Item& item : batch) {
        keys.insert(item.key);
      }
      if (keys.size() > 1) {
        stats->multi_key.fetch_add(1, std::memory_order_relaxed);
      }
    }
    std::uint64_t seen = stats->largest.load(std::memory_order_relaxed);
    while (batch.size() > seen &&
           !stats->largest.compare_exchange_weak(seen, batch.size(),
                                                 std::memory_order_relaxed)) {}
  }

  /// A11_UV_FAIR=0 selects strictly FIFO draining.
  static bool FairDraining() {
    static const bool fair = [] {
      const char* setting = std::getenv("A11_UV_FAIR");
      // A value that is not a number leaves fair draining on: only an explicit
      // 0 turns it off, so a typo cannot quietly change how the loop drains.
      int value = 0;
      return setting == nullptr || !absl::SimpleAtoi(setting, &value) ||
             value != 0;
    }();
    return fair;
  }

  mutable thread::Mutex mu_;
  thread::CondVar cv_;
  bool running_ ABSL_GUARDED_BY(mu_) = true;
  std::optional<std::thread::id> loop_thread_id_ ABSL_GUARDED_BY(mu_);
  std::deque<Item> work_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<uvw::loop> loop_;
  std::shared_ptr<uvw::async_handle> async_;
  std::thread thread_;
};

/**
 * @brief Run @p operation on the loop thread and wait for its result.
 *
 * @param operation Work to execute on the loop thread.
 * @param order_key  See UvExecutor::Post. **Pass the connection whenever the
 *   operation touches one**, or its awaited work can be reordered ahead of writes
 *   posted for the same socket. `HttpTransport::RunOnUvForConnection` supplies it
 *   automatically and is what connection code should call.
 */
template <typename T>
absl::StatusOr<T> RunOnUv(std::function<absl::StatusOr<T>()> operation,
                          const void* order_key = nullptr) {
  if (UvExecutor::Instance().IsLoopThread()) {
    return operation();
  }
  auto promise = std::make_shared<a11::Promise<T>>();
  a11::Future<T> future = promise->future();
  ABSL_RETURN_IF_ERROR(UvExecutor::Instance().Post(
      [promise, operation = std::move(operation)]() mutable {
        (void)promise->SetResult(operation());
      },
      order_key));
  return future.Await();
}

inline absl::Status RunStatusOnUv(std::function<absl::Status()> operation,
                                  const void* order_key = nullptr) {
  absl::StatusOr<a11::Unit> result = RunOnUv<a11::Unit>(
      [operation =
           std::move(operation)]() mutable -> absl::StatusOr<a11::Unit> {
        ABSL_RETURN_IF_ERROR(operation());
        return a11::Unit{};
      },
      order_key);
  return result.status();
}

}  // namespace a11::uv

#endif  // A11_UV_LOOP_H_
