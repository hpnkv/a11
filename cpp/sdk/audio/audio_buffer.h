// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief A block of captured audio samples with its dimensions and timing.
 */

#ifndef A11_SDK_AUDIO_AUDIO_BUFFER_H_
#define A11_SDK_AUDIO_AUDIO_BUFFER_H_

#include <cstddef>
#include <vector>

#include <absl/types/span.h>

#include "a11/time.h"

namespace a11::sdk::audio {

/**
 * @brief One delivered block of captured input samples.
 *
 * Samples are stored flat in channel-major (planar) order: all @c num_frames
 * samples of channel 0, then all of channel 1, and so on, so
 * `samples.size() == num_channels * num_frames`. This layout makes Channel()
 * a contiguous view with no copy. Every value is a normalized 32-bit float in
 * the usual [-1.0, 1.0] range PortAudio produces for @c paFloat32 input.
 */
struct AudioBuffer {
  /// Flat channel-major samples; size is `num_channels * num_frames`.
  std::vector<float> samples;
  /// Number of channels interleaved across the flat buffer.
  size_t num_channels = 0;
  /// Number of samples per channel (the subscription's requested buffer size).
  size_t num_frames = 0;
  /// Sample rate, in hertz, the input is being captured at.
  double sample_rate = 0.0;
  /// Best-effort wall-clock instant the final sample in this block was taken.
  a11::Time end_time = a11::InfinitePast();

  /**
   * @brief Returns a contiguous view of one channel's samples.
   * @param index Zero-based channel index.
   * @return A span of @c num_frames samples, or an empty span when @p index is
   *   out of range.
   */
  [[nodiscard]] absl::Span<const float> Channel(size_t index) const;
};

}  // namespace a11::sdk::audio

#endif  // A11_SDK_AUDIO_AUDIO_BUFFER_H_
