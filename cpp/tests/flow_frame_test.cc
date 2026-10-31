// Copyright 2026 The A11 Authors
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

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

#include <gtest/gtest.h>

#include "a11/flow/internal/frame.h"

namespace a11::flow::internal {
namespace {

TEST(FlowFrameTest, AlignmentAndIndependentStableBlocks) {
  struct alignas(128) Wide {
    int value = 17;
  };

  FrameLayout layout;
  auto bytes = layout.Add<char>(3);
  auto wide = layout.Add<Wide>(2);
  auto unused = layout.Add<std::optional<int>>(1);
  ASSERT_TRUE(bytes.ok());
  ASSERT_TRUE(wide.ok());
  ASSERT_TRUE(unused.ok());
  auto budget = std::make_shared<FrameBudget>(FrameOptions{});
  layout.Seal();
  FramePool pool(budget);
  auto first = pool.Acquire(layout);
  ASSERT_TRUE(first.ok());
  (*first)->Initialize();
  auto* address = (*first)->Get(*wide).data();
  EXPECT_EQ(reinterpret_cast<uintptr_t>(address) % alignof(Wide), 0);
  EXPECT_EQ(address[1].value, 17);
  EXPECT_FALSE((*first)->Get(*unused)[0].has_value());
  auto second = pool.Acquire(layout);
  ASSERT_TRUE(second.ok());
  (*second)->Initialize();
  EXPECT_NE(address, (*second)->Get(*wide).data());
  EXPECT_EQ(address, (*first)->Get(*wide).data());
  (*first)->Reset();
  pool.Return(*std::move(first));
  (*second)->Reset();
  pool.Return(*std::move(second));
}

TEST(FlowFrameTest, CheckedLayoutOverflowLeavesExistingSlotsValid) {
  FrameLayout layout;
  auto value = layout.Add<size_t>(1);
  ASSERT_TRUE(value.ok());
  const auto bytes = layout.bytes();
  EXPECT_EQ(
      layout.Add<size_t>(std::numeric_limits<size_t>::max()).status().code(),
      absl::StatusCode::kResourceExhausted);
  EXPECT_EQ(layout.bytes(), bytes);
  EXPECT_TRUE(layout.Add<char>(1).ok());
}

TEST(FlowFrameTest, PartialConstructionReleasesDataAndReusesCleanStorage) {
  struct Tracked {
    explicit Tracked(std::shared_ptr<int> payload, int* destroyed)
        : payload(std::move(payload)), destroyed(destroyed) {}

    ~Tracked() { ++*destroyed; }

    std::shared_ptr<int> payload;
    int* destroyed;
  };

  FrameLayout layout;
  auto slots = layout.Add<std::optional<Tracked>>(4);
  ASSERT_TRUE(slots.ok());
  auto stats = std::make_shared<FrameStats>();
  auto budget = std::make_shared<FrameBudget>(FrameOptions{.stats = stats});
  {
    layout.Seal();
    FramePool pool(budget);
    int destroyed = 0;
    auto payload = std::make_shared<int>(42);
    std::weak_ptr<int> weak = payload;
    auto acquired = pool.Acquire(layout);
    ASSERT_TRUE(acquired.ok());
    auto block = *std::move(acquired);
    auto* identity = block.get();
    block->Initialize();
    block->Get(*slots)[2].emplace(std::move(payload), &destroyed);
    auto escaped = block->Get(*slots)[2]->payload;
    block->Reset();
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(*escaped, 42);
    escaped.reset();
    EXPECT_TRUE(weak.expired());
    pool.Return(std::move(block));
    EXPECT_EQ(stats->live_bytes, 0);
    EXPECT_GT(stats->cached_bytes, 0);
    acquired = pool.Acquire(layout);
    ASSERT_TRUE(acquired.ok());
    EXPECT_EQ(acquired->get(), identity);
    (*acquired)->Initialize();
    for (const auto& slot : (*acquired)->Get(*slots)) {
      EXPECT_FALSE(slot.has_value());
    }
    (*acquired)->Reset();
    pool.Return(*std::move(acquired));
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(stats->allocations, 1);
    EXPECT_EQ(stats->reuses, 1);
  }
  EXPECT_EQ(stats->reserved_bytes, 0);
  EXPECT_EQ(stats->cached_bytes, 0);
}

TEST(FlowFrameTest, RetentionCapAndDisabledCaching) {
  for (const size_t cap : {size_t{0}, size_t{256}}) {
    FrameLayout layout;
    ASSERT_TRUE(layout.Add<char>(128).ok());
    auto stats = std::make_shared<FrameStats>();
    auto budget = std::make_shared<FrameBudget>(
        FrameOptions{.cache_limit_bytes = cap, .stats = stats});
    layout.Seal();
    FramePool pool(budget);
    auto one = pool.Acquire(layout);
    auto two = pool.Acquire(layout);
    ASSERT_TRUE(one.ok());
    ASSERT_TRUE(two.ok());
    pool.Return(*std::move(one));
    pool.Return(*std::move(two));
    EXPECT_LE(stats->cached_bytes, cap);
    EXPECT_EQ(stats->active_frames, 0);
    EXPECT_EQ(stats->reserved_bytes, stats->cached_bytes);
    if (cap == 0) {
      EXPECT_EQ(stats->reserved_bytes, 0);
    }
  }
}

TEST(FlowFrameTest, BudgetChargesCachedCapacityAndEvictsIncompatibleBlocks) {
  FrameLayout small;
  FrameLayout large;
  ASSERT_TRUE(small.Add<char>(128).ok());
  ASSERT_TRUE(large.Add<char>(512).ok());
  auto stats = std::make_shared<FrameStats>();
  auto budget = std::make_shared<FrameBudget>(FrameOptions{
      .max_reserved_bytes = 512 + sizeof(FrameBlock), .stats = stats});
  small.Seal();
  large.Seal();
  FramePool pool(budget);
  auto one = pool.Acquire(small);
  ASSERT_TRUE(one.ok());
  EXPECT_EQ(pool.Acquire(large).status().code(),
            absl::StatusCode::kResourceExhausted);
  pool.Return(*std::move(one));
  auto bigger = pool.Acquire(large);
  ASSERT_TRUE(bigger.ok());
  EXPECT_EQ(stats->cached_bytes, 0);
  EXPECT_EQ(stats->reserved_bytes, 512 + sizeof(FrameBlock));
  pool.Return(*std::move(bigger));
}

TEST(FlowFrameTest, FallibleBlockFailureReleasesBudget) {
  FrameLayout layout;
  ASSERT_TRUE(layout.Add<int>(1).ok());
  auto stats = std::make_shared<FrameStats>();
  auto budget = std::make_shared<FrameBudget>(FrameOptions{.stats = stats});
  layout.Seal();
  FramePool pool(budget,
                 [](size_t, std::align_val_t, const std::nothrow_t&) -> void* {
                   return nullptr;
                 });
  EXPECT_EQ(pool.Acquire(layout).status().code(),
            absl::StatusCode::kResourceExhausted);
  EXPECT_EQ(stats->reserved_bytes, 0);
  EXPECT_EQ(stats->allocations, 0);
}

TEST(FlowFrameTest, LazyObjectsDestroyExactlyOnceAndUnregisterBeforeReuse) {
  FrameLayout layout;
  FrameObjectLayout<std::shared_ptr<int>> objects;
  objects.Add(400);
  objects.Add(3);
  ASSERT_TRUE(objects.Finalize(layout).ok());
  auto budget = std::make_shared<FrameBudget>(FrameOptions{});
  layout.Seal();
  FramePool pool(budget);
  auto block = pool.Acquire(layout);
  ASSERT_TRUE(block.ok());
  (*block)->Initialize();
  FrameObjects<std::shared_ptr<int>> view;
  view.Bind(**block, objects);
  EXPECT_EQ(view.begin(), view.end());
  auto value = std::make_shared<int>(7);
  std::weak_ptr<int> weak = value;
  view.Construct(400, std::move(value));
  EXPECT_EQ(view.size(), 1);
  EXPECT_EQ(**view.find(400)->second, 7);
  EXPECT_EQ(view.find(3), view.end());
  view.clear();
  EXPECT_TRUE(weak.expired());
  (*block)->Reset();
  pool.Return(*std::move(block));
}

}  // namespace
}  // namespace a11::flow::internal
