/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef A11_FLOW_INTERNAL_FRAME_H_
#define A11_FLOW_INTERNAL_FRAME_H_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/log/check.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/types/span.h>

#include "a11/flow/runtime.h"

namespace a11::flow::internal {

class FrameBlock;

// Typed regions are valid only with their originating, immutable layout.
class FrameLayout {
 public:
  template <typename T>
  class Slots {
   public:
    Slots() = default;

   private:
    friend class FrameLayout;
    friend class FrameBlock;
    const FrameLayout* absl_nullable owner = nullptr;
    size_t region = 0;
    size_t offset = 0;
    size_t count = 0;
  };

  template <typename T>
  absl::StatusOr<Slots<T>> Add(size_t count) {
    CHECK(!sealed_);
    constexpr size_t limit = std::numeric_limits<size_t>::max();
    const size_t padding = (alignof(T) - bytes_ % alignof(T)) % alignof(T);
    if (bytes_ > limit - padding ||
        count > (limit - bytes_ - padding) / sizeof(T)) {
      return absl::ResourceExhaustedError("Flow frame layout size overflow.");
    }
    Slots<T> slots;
    slots.owner = this;
    slots.region = regions_.size();
    slots.offset = bytes_ + padding;
    slots.count = count;
    regions_.push_back(Region{.offset = slots.offset,
                              .count = count,
                              .construct =
                                  [](void* storage, size_t n) {
                                    auto* values = static_cast<T*>(storage);
                                    for (size_t i = 0; i < n; ++i) {
                                      std::construct_at(values + i);
                                    }
                                  },
                              .destroy =
                                  [](void* storage, size_t n) {
                                    auto* values = static_cast<T*>(storage);
                                    while (n != 0) {
                                      std::destroy_at(values + --n);
                                    }
                                  }});
    bytes_ = slots.offset + count * sizeof(T);
    alignment_ = std::max(alignment_, alignof(T));
    return slots;
  }

  void Seal() { sealed_ = true; }

  bool sealed() const { return sealed_; }

  size_t bytes() const { return std::max(bytes_, size_t{1}); }

  size_t alignment() const { return alignment_; }

  FrameLayout() = default;
  FrameLayout(const FrameLayout&) = delete;
  FrameLayout& operator=(const FrameLayout&) = delete;

 private:
  friend class FrameBlock;

  struct Region {
    size_t offset;
    size_t count;
    void (*absl_nonnull construct)(void* absl_nonnull, size_t);
    void (*absl_nonnull destroy)(void* absl_nonnull, size_t);
  };

  std::vector<Region> regions_;
  bool sealed_ = false;
  size_t bytes_ = 0;
  size_t alignment_ = alignof(std::max_align_t);
};

template <typename T>
struct FrameObjectLayout {
  struct Entry {
    size_t first = 0;
    std::optional<T> second;
  };

  FrameLayout::Slots<Entry> storage;
  absl::flat_hash_map<size_t, size_t> index;

  void Add(size_t key) { index.try_emplace(key, index.size()); }

  absl::Status Finalize(FrameLayout& layout) {
    auto slots = layout.Add<Entry>(index.size());
    if (!slots.ok()) {
      return slots.status();
    }
    storage = *slots;
    return absl::OkStatus();
  }
};

inline void FramePeak(std::atomic<size_t>& peak, size_t value) {
  size_t previous = peak.load(std::memory_order_relaxed);
  while (previous < value && !peak.compare_exchange_weak(
                                 previous, value, std::memory_order_relaxed)) {}
}

// Shared accounting spans transitive local calls; each runner owns its pool.
class FrameBudget {
 public:
  explicit FrameBudget(FrameOptions options) : options_(std::move(options)) {}

  bool Reserve(size_t bytes) {
    size_t previous = reserved_.load(std::memory_order_relaxed);
    do {
      if (bytes > options_.max_reserved_bytes - previous) {
        return false;
      }
    } while (!reserved_.compare_exchange_weak(previous, previous + bytes,
                                              std::memory_order_relaxed));
    if (auto* stats = options_.stats.get(); stats != nullptr) {
      const size_t now = stats->reserved_bytes.fetch_add(bytes) + bytes;
      FramePeak(stats->peak_reserved_bytes, now);
    }
    return true;
  }

  void Release(size_t bytes) {
    reserved_.fetch_sub(bytes, std::memory_order_relaxed);
    if (options_.stats != nullptr) {
      options_.stats->reserved_bytes.fetch_sub(bytes);
    }
  }

  const FrameOptions& options() const { return options_; }

 private:
  FrameOptions options_;
  std::atomic<size_t> reserved_{0};
};

class FrameBlock {
 public:
  ~FrameBlock() {
    Reset();
    ::operator delete(storage_, std::align_val_t(layout_->alignment()));
    budget_->Release(capacity());
  }

  FrameBlock(const FrameBlock&) = delete;
  FrameBlock& operator=(const FrameBlock&) = delete;

  template <typename T>
  absl::Span<T> Get(FrameLayout::Slots<T> slots) {
    CHECK(slots.owner == layout_);
    CHECK_LT(slots.region, constructed_);
    return {
        reinterpret_cast<T*>(static_cast<std::byte*>(storage_) + slots.offset),
        slots.count};
  }

  void Initialize() {
    CHECK_EQ(constructed_, 0);
    for (const auto& region : layout_->regions_) {
      region.construct(static_cast<std::byte*>(storage_) + region.offset,
                       region.count);
      ++constructed_;
    }
  }

  void Reset() {
    while (constructed_ != 0) {
      const auto& region = layout_->regions_[--constructed_];
      region.destroy(static_cast<std::byte*>(storage_) + region.offset,
                     region.count);
    }
  }

  size_t capacity() const { return layout_->bytes() + sizeof(FrameBlock); }

 private:
  friend class FramePool;

  FrameBlock(const FrameLayout& layout, FrameBudget& budget,
             void* absl_nonnull storage)
      : layout_(&layout), budget_(&budget), storage_(storage) {}

  const FrameLayout* absl_nonnull layout_;
  FrameBudget* absl_nonnull budget_;
  void* absl_nonnull storage_;
  size_t constructed_ = 0;
  std::unique_ptr<FrameBlock> next_;
};

// A view over lazy objects in a frame, indexed by shared logical metadata.
template <typename T>
class FrameObjects {
 public:
  using Entry = typename FrameObjectLayout<T>::Entry;

  class Iterator {
   public:
    Iterator(Entry* absl_nullable current, Entry* absl_nullable end)
        : current_(current), end_(end) {
      Skip();
    }

    Entry& operator*() const { return *current_; }

    Entry* absl_nonnull operator->() const { return current_; }

    Iterator& operator++() {
      ++current_;
      Skip();
      return *this;
    }

    bool operator==(const Iterator& other) const {
      return current_ == other.current_;
    }

   private:
    void Skip() {
      while (current_ != end_ && !current_->second.has_value()) {
        ++current_;
      }
    }

    Entry* absl_nullable current_;
    Entry* absl_nullable end_;
  };

  void Bind(FrameBlock& block, const FrameObjectLayout<T>& layout) {
    entries_ = block.Get(layout.storage);
    index_ = &layout.index;
    for (const auto& [key, local] : layout.index) {
      entries_[local].first = key;
    }
  }

  Iterator begin() const { return {entries_.data(), End()}; }

  Iterator end() const { return {End(), End()}; }

  Iterator find(size_t key) const {
    if (index_ == nullptr) {
      return end();
    }
    const auto found = index_->find(key);
    if (found == index_->end() || !entries_[found->second].second.has_value()) {
      return end();
    }
    return {entries_.data() + found->second, End()};
  }

  bool contains(size_t key) const { return find(key) != end(); }

  T& at(size_t key) const {
    auto found = find(key);
    CHECK(found != end());
    return *found->second;
  }

  template <typename... Args>
  T& Construct(size_t key, Args&&... args) {
    CHECK(index_ != nullptr);
    auto& value = entries_[index_->at(key)].second;
    CHECK(!value.has_value());
    ++size_;
    return value.emplace(std::forward<Args>(args)...);
  }

  size_t size() const { return size_; }

  void clear() {
    for (auto& entry : entries_) {
      entry.second.reset();
    }
    size_ = 0;
  }

 private:
  Entry* absl_nullable End() const {
    return entries_.empty() ? entries_.data()
                            : entries_.data() + entries_.size();
  }

  absl::Span<Entry> entries_;
  const absl::flat_hash_map<size_t, size_t>* absl_nullable index_ = nullptr;
  size_t size_ = 0;
};

// Acquire/Return hold the run monitor. Reset runs outside it after quiescence.
class FramePool {
 public:
  using Allocate = void* absl_nullable (*)(size_t, std::align_val_t,
                                           const std::nothrow_t&);

  explicit FramePool(std::shared_ptr<FrameBudget> budget,
                     Allocate absl_nonnull allocate = &::operator new)
      : budget_(std::move(budget)), allocate_(allocate) {}

  ~FramePool() { Trim(); }

  absl::StatusOr<std::unique_ptr<FrameBlock>> Acquire(
      const FrameLayout& layout) {
    CHECK(layout.sealed());
    std::unique_ptr<FrameBlock> frame;
    for (auto* link = &cached_; *link != nullptr; link = &(*link)->next_) {
      if ((*link)->layout_ == &layout) {
        frame = std::move(*link);
        *link = std::move(frame->next_);
        cached_bytes_ -= frame->capacity();
        if (auto* stats = Stats(); stats != nullptr) {
          stats->cached_bytes.fetch_sub(frame->capacity());
          stats->reuses.fetch_add(1);
        }
        break;
      }
    }
    if (frame == nullptr) {
      if (layout.bytes() >
          std::numeric_limits<size_t>::max() - sizeof(FrameBlock)) {
        return Exhausted();
      }
      const size_t capacity = layout.bytes() + sizeof(FrameBlock);
      if (!budget_->Reserve(capacity)) {
        Trim();
        if (!budget_->Reserve(capacity)) {
          return Exhausted();
        }
      }
      void* storage = allocate_(
          layout.bytes(), std::align_val_t(layout.alignment()), std::nothrow);
      if (storage == nullptr) {
        budget_->Release(capacity);
        return Exhausted();
      }
      frame.reset(new (std::nothrow) FrameBlock(layout, *budget_, storage));
      if (frame == nullptr) {
        ::operator delete(storage, std::align_val_t(layout.alignment()));
        budget_->Release(capacity);
        return Exhausted();
      }
      if (Stats() != nullptr) {
        Stats()->allocations.fetch_add(1);
      }
    }
    if (auto* stats = Stats(); stats != nullptr) {
      stats->activations.fetch_add(1);
      FramePeak(
          stats->peak_live_bytes,
          stats->live_bytes.fetch_add(frame->capacity()) + frame->capacity());
      FramePeak(stats->peak_active_frames,
                stats->active_frames.fetch_add(1) + 1);
    }
    return frame;
  }

  void Return(std::unique_ptr<FrameBlock> frame) {
    CHECK_EQ(frame->constructed_, 0);
    const size_t capacity = frame->capacity();
    if (auto* stats = Stats(); stats != nullptr) {
      stats->live_bytes.fetch_sub(capacity);
      stats->active_frames.fetch_sub(1);
    }
    if (capacity > budget_->options().cache_limit_bytes - cached_bytes_) {
      return;
    }
    cached_bytes_ += capacity;
    if (auto* stats = Stats(); stats != nullptr) {
      FramePeak(stats->peak_cached_bytes,
                stats->cached_bytes.fetch_add(capacity) + capacity);
    }
    frame->next_ = std::move(cached_);
    cached_ = std::move(frame);
  }

 private:
  static absl::Status Exhausted() {
    return absl::ResourceExhaustedError("Flow frame storage exhausted.");
  }

  FrameStats* absl_nullable Stats() const {
    return budget_->options().stats.get();
  }

  void Trim() {
    while (cached_ != nullptr) {
      auto frame = std::move(cached_);
      cached_ = std::move(frame->next_);
      if (Stats() != nullptr) {
        Stats()->cached_bytes.fetch_sub(frame->capacity());
      }
    }
    cached_bytes_ = 0;
  }

  std::shared_ptr<FrameBudget> budget_;
  Allocate absl_nonnull allocate_;
  std::unique_ptr<FrameBlock> cached_;
  size_t cached_bytes_ = 0;
};

}  // namespace a11::flow::internal
#endif  // A11_FLOW_INTERNAL_FRAME_H_
