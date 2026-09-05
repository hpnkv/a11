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
