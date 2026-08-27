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

#ifndef THREAD_INTERNAL_WORK_QUEUE_H_
#define THREAD_INTERNAL_WORK_QUEUE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <new>
#include <utility>

namespace thread::internal {

// One worker slot's queue of pending work: a bounded lock-free ring, with an
// overflow list behind a mutex for the case of the ring filling up.
template <typename T, size_t Capacity>
class WorkQueue {
 public:
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two, so that the position "
                "counters can wrap by masking.");

  WorkQueue() {
    for (size_t index = 0; index < Capacity; ++index) {
      cells_[index].sequence.store(index, std::memory_order_relaxed);
    }
  }

  WorkQueue(const WorkQueue&) = delete;
  WorkQueue& operator=(const WorkQueue&) = delete;

  // Appends `value`. Always succeeds: a full ring falls through to the overflow
  // list rather than refusing, because the callers have nowhere to put a
  // rejected item and must not block.
  void Push(T value) {
    // Once anything is in the overflow list, everything goes there until it
    // drains, or an item pushed now would come out ahead of one pushed earlier.
    if (overflow_size_.load(std::memory_order_acquire) == 0 &&
        TryPushRing(value)) {
      return;
    }
    std::lock_guard<std::mutex> lock(overflow_mu_);
    overflow_.push_back(std::move(value));
    overflow_size_.fetch_add(1, std::memory_order_release);
  }

  // Removes the oldest item into `out`, or returns false if there is none.
  bool Pop(T& out) {
    if (TryPopRing(out)) {
      return true;
    }
    if (overflow_size_.load(std::memory_order_acquire) == 0) {
      return false;
    }
    std::lock_guard<std::mutex> lock(overflow_mu_);
    if (overflow_.empty()) {
      return false;
    }
    out = std::move(overflow_.front());
    overflow_.pop_front();
    overflow_size_.fetch_sub(1, std::memory_order_release);
    return true;
  }

 private:
  // False only when the ring is full. `value` is left untouched in that case,
  // and is moved from only once a cell has been claimed for it.
  bool TryPushRing(T& value) {
    Cell* cell = nullptr;
    size_t position = tail_.load(std::memory_order_relaxed);
    while (true) {
      cell = &cells_[position & (Capacity - 1)];
      const size_t sequence = cell->sequence.load(std::memory_order_acquire);
      // A cell is free for position P exactly when its sequence is P. Anything
      // less means the ring has come all the way round and this cell still
      // holds an item nobody has taken.
      const auto lag = static_cast<std::intptr_t>(sequence) -
                       static_cast<std::intptr_t>(position);
      if (lag == 0) {
        if (tail_.compare_exchange_weak(position, position + 1,
                                        std::memory_order_relaxed)) {
          break;
        }
      } else if (lag < 0) {
        return false;
      } else {
        position = tail_.load(std::memory_order_relaxed);
      }
    }
    cell->value = std::move(value);
    // Publishing the sequence is what makes the item visible to a popper, so it
    // has to be the last thing that happens.
    cell->sequence.store(position + 1, std::memory_order_release);
    return true;
  }

  bool TryPopRing(T& out) {
    Cell* cell = nullptr;
    size_t position = head_.load(std::memory_order_relaxed);
    while (true) {
      cell = &cells_[position & (Capacity - 1)];
      const size_t sequence = cell->sequence.load(std::memory_order_acquire);
      // Filled for position P means sequence P + 1: see the store above.
      const auto lag = static_cast<std::intptr_t>(sequence) -
                       static_cast<std::intptr_t>(position + 1);
      if (lag == 0) {
        if (head_.compare_exchange_weak(position, position + 1,
                                        std::memory_order_relaxed)) {
          break;
        }
      } else if (lag < 0) {
        return false;
      } else {
        position = head_.load(std::memory_order_relaxed);
      }
    }
    out = std::move(cell->value);
    // Hands the cell to the pusher one lap ahead.
    cell->sequence.store(position + Capacity, std::memory_order_release);
    return true;
  }

  // Apple arm64 cores have 128-byte cache lines, and x86-64 prefetches in
  // 128-byte pairs.
  static constexpr size_t kCacheLine = 128;

  struct Cell {
    std::atomic<size_t> sequence{0};
    T value{};
  };

  // The two ends are written by different threads on every operation and must
  // not share a line, with each other or with the cells.
  alignas(kCacheLine) std::atomic<size_t> head_{0};
  alignas(kCacheLine) std::atomic<size_t> tail_{0};
  alignas(kCacheLine) Cell cells_[Capacity];

  // Read on every Push and on every Pop that finds the ring empty, and so worth
  // keeping out of the way of the ends; written only in the rare full case.
  alignas(kCacheLine) std::atomic<size_t> overflow_size_{0};
  std::mutex overflow_mu_;
  std::deque<T> overflow_;
};

}  // namespace thread::internal

#endif  // THREAD_INTERNAL_WORK_QUEUE_H_
