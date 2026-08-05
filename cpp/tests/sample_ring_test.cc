// Copyright 2026 The A11 Authors.

#include "sdk/audio/internal/sample_ring.h"

#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include "a11/time.h"

namespace a11::sdk::audio::internal {
namespace {

std::vector<float> Frame(size_t frames, size_t channels, float base) {
  std::vector<float> data(frames * channels);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = base + static_cast<float>(i);
  }
  return data;
}

TEST(SampleRingTest, PushPeekPopRoundTrips) {
  SampleRing ring(/*block_count=*/4, /*frames_per_block=*/2, /*channels=*/2);
  const std::vector<float> input = Frame(2, 2, 10.0f);

  EXPECT_EQ(ring.Peek(), nullptr);
  EXPECT_TRUE(ring.Push(input.data(), 2, a11::Now()));

  const SampleBlock* block = ring.Peek();
  ASSERT_NE(block, nullptr);
  EXPECT_EQ(block->frames, 2u);
  for (size_t i = 0; i < input.size(); ++i) {
    EXPECT_FLOAT_EQ(block->interleaved[i], input[i]);
  }

  ring.Pop();
  EXPECT_EQ(ring.Peek(), nullptr);
}

TEST(SampleRingTest, PreservesFifoOrder) {
  SampleRing ring(/*block_count=*/4, /*frames_per_block=*/1, /*channels=*/1);
  for (float value : {1.0f, 2.0f, 3.0f}) {
    ASSERT_TRUE(ring.Push(&value, 1, a11::Now()));
  }
  for (float expected : {1.0f, 2.0f, 3.0f}) {
    const SampleBlock* block = ring.Peek();
    ASSERT_NE(block, nullptr);
    EXPECT_FLOAT_EQ(block->interleaved[0], expected);
    ring.Pop();
  }
}

TEST(SampleRingTest, DropsAndCountsOverruns) {
  // Capacity rounds up to at least 2 blocks; fill it, then overflow.
  SampleRing ring(/*block_count=*/2, /*frames_per_block=*/1, /*channels=*/1);
  float value = 1.0f;
  EXPECT_TRUE(ring.Push(&value, 1, a11::Now()));
  EXPECT_TRUE(ring.Push(&value, 1, a11::Now()));
  EXPECT_FALSE(ring.Push(&value, 1, a11::Now()));
  EXPECT_EQ(ring.overruns(), 1u);

  // Freeing a slot lets the producer proceed again.
  ring.Pop();
  EXPECT_TRUE(ring.Push(&value, 1, a11::Now()));
}

TEST(SampleRingTest, ClampsOversizedPush) {
  SampleRing ring(/*block_count=*/2, /*frames_per_block=*/2, /*channels=*/1);
  const std::vector<float> input = Frame(4, 1, 0.0f);
  EXPECT_TRUE(ring.Push(input.data(), 4, a11::Now()));
  const SampleBlock* block = ring.Peek();
  ASSERT_NE(block, nullptr);
  EXPECT_EQ(block->frames, 2u);  // clamped to the block capacity
}

}  // namespace
}  // namespace a11::sdk::audio::internal
