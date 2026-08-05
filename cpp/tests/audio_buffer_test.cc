// Copyright 2026 The A11 Authors.

#include "sdk/audio/audio_buffer.h"

#include <gtest/gtest.h>

namespace a11::sdk::audio {
namespace {

AudioBuffer MakePlanarBuffer() {
  // Two channels, three frames each, channel-major:
  //   channel 0 = {1, 2, 3}, channel 1 = {4, 5, 6}.
  AudioBuffer buffer;
  buffer.samples = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  buffer.num_channels = 2;
  buffer.num_frames = 3;
  buffer.sample_rate = 48000.0;
  return buffer;
}

TEST(AudioBufferTest, ChannelReturnsContiguousView) {
  const AudioBuffer buffer = MakePlanarBuffer();

  const absl::Span<const float> channel0 = buffer.Channel(0);
  ASSERT_EQ(channel0.size(), 3u);
  EXPECT_FLOAT_EQ(channel0[0], 1.0f);
  EXPECT_FLOAT_EQ(channel0[2], 3.0f);

  const absl::Span<const float> channel1 = buffer.Channel(1);
  ASSERT_EQ(channel1.size(), 3u);
  EXPECT_FLOAT_EQ(channel1[0], 4.0f);
  EXPECT_FLOAT_EQ(channel1[2], 6.0f);
}

TEST(AudioBufferTest, OutOfRangeChannelIsEmpty) {
  const AudioBuffer buffer = MakePlanarBuffer();
  EXPECT_TRUE(buffer.Channel(2).empty());
}

TEST(AudioBufferTest, EmptyBufferChannelIsEmpty) {
  const AudioBuffer buffer;
  EXPECT_TRUE(buffer.Channel(0).empty());
}

}  // namespace
}  // namespace a11::sdk::audio
