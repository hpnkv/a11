// Copyright 2026 The A11 Authors.

#include "sdk/audio/internal/audio_processing.h"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include <absl/types/span.h>
#include <gtest/gtest.h>

#include "a11/time.h"
#include "sdk/audio/audio_buffer.h"

namespace a11::sdk::audio::internal {
namespace {

AudioBuffer Buffer(std::vector<float> samples, size_t channels, size_t frames,
                   double sample_rate) {
  return AudioBuffer{
      .samples = std::move(samples),
      .num_channels = channels,
      .num_frames = frames,
      .sample_rate = sample_rate,
      .end_time = a11::InfinitePast(),
  };
}

std::vector<float> ConstantSamples(size_t millis, float value) {
  const size_t samples = static_cast<size_t>(
      kWhisperSampleRate * static_cast<double>(millis) / 1000.0);
  return std::vector<float>(samples, value);
}

TEST(StreamingMonoResamplerTest, DownmixesPlanarChannels) {
  StreamingMonoResampler resampler(/*input_sample_rate=*/16000.0,
                                   /*input_channels=*/2);
  const AudioBuffer buffer = Buffer(
      /*samples=*/{1.0f, 0.0f, -1.0f, 0.5f, 0.0f, 1.0f, 0.0f, -0.5f},
      /*channels=*/2, /*frames=*/4, /*sample_rate=*/16000.0);
  std::vector<float> output;
  ASSERT_TRUE(resampler.Process(buffer, &output).ok());
  resampler.Flush(&output);

  ASSERT_EQ(output.size(), 4u);
  EXPECT_FLOAT_EQ(output[0], 0.5f);
  EXPECT_FLOAT_EQ(output[1], 0.5f);
  EXPECT_FLOAT_EQ(output[2], -0.5f);
  EXPECT_FLOAT_EQ(output[3], 0.0f);
}

TEST(StreamingMonoResamplerTest, PreservesFractionalCursorAcrossBuffers) {
  StreamingMonoResampler resampler(/*input_sample_rate=*/48000.0,
                                   /*input_channels=*/1);
  std::vector<float> output;
  for (int block = 0; block < 2; ++block) {
    const AudioBuffer buffer =
        Buffer(std::vector<float>(480, 0.25f), 1, 480, 48000.0);
    ASSERT_TRUE(resampler.Process(buffer, &output).ok());
  }
  resampler.Flush(&output);

  EXPECT_EQ(output.size(), 320u);
  for (float sample : output) {
    EXPECT_FLOAT_EQ(sample, 0.25f);
  }
}

TEST(VoiceActivityDetectorTest, SilenceDoesNotEmit) {
  VoiceActivityDetector vad(VoiceActivityOptions{
      .energy_threshold = 0.01f,
      .noise_ratio = 2.5f,
      .window_millis = 20,
      .min_speech_millis = 100,
      .min_silence_millis = 100,
      .speech_pad_millis = 40,
      .max_speech_seconds = 2,
  });
  const std::vector<float> silence = ConstantSamples(1000, 0.001f);
  std::vector<std::vector<float>> completed;
  vad.Process(silence, &completed);

  EXPECT_TRUE(completed.empty());
  EXPECT_FALSE(vad.Flush().has_value());
}

TEST(VoiceActivityDetectorTest, EndpointsSpeechAndKeepsBoundedPadding) {
  VoiceActivityDetector vad(VoiceActivityOptions{
      .energy_threshold = 0.01f,
      .noise_ratio = 2.5f,
      .window_millis = 20,
      .min_speech_millis = 100,
      .min_silence_millis = 100,
      .speech_pad_millis = 40,
      .max_speech_seconds = 2,
  });
  std::vector<float> samples = ConstantSamples(200, 0.001f);
  const std::vector<float> speech = ConstantSamples(200, 0.2f);
  const std::vector<float> silence = ConstantSamples(120, 0.001f);
  samples.insert(samples.end(), speech.begin(), speech.end());
  samples.insert(samples.end(), silence.begin(), silence.end());

  std::vector<std::vector<float>> completed;
  vad.Process(samples, &completed);

  ASSERT_EQ(completed.size(), 1u);
  EXPECT_EQ(completed.front().size(), ConstantSamples(280, 0.0f).size());
  EXPECT_FALSE(vad.active());
}

TEST(VoiceActivityDetectorTest, RejectsTooShortNoiseBurst) {
  VoiceActivityDetector vad(VoiceActivityOptions{
      .energy_threshold = 0.01f,
      .noise_ratio = 2.5f,
      .window_millis = 20,
      .min_speech_millis = 120,
      .min_silence_millis = 100,
      .speech_pad_millis = 40,
      .max_speech_seconds = 2,
  });
  std::vector<float> samples = ConstantSamples(40, 0.2f);
  const std::vector<float> silence = ConstantSamples(120, 0.0f);
  samples.insert(samples.end(), silence.begin(), silence.end());
  std::vector<std::vector<float>> completed;
  vad.Process(samples, &completed);

  EXPECT_TRUE(completed.empty());
  EXPECT_FALSE(vad.Flush().has_value());
}

}  // namespace
}  // namespace a11::sdk::audio::internal
