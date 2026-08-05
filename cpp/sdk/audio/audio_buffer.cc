// Copyright 2026 The A11 Authors.

#include "sdk/audio/audio_buffer.h"

#include <cstddef>

#include <absl/types/span.h>

namespace a11::sdk::audio {

absl::Span<const float> AudioBuffer::Channel(size_t index) const {
  if (index >= num_channels || num_frames == 0) {
    return {};
  }
  return {samples.data() + index * num_frames, num_frames};
}

}  // namespace a11::sdk::audio
