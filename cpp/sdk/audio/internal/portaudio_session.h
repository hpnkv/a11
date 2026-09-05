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

/**
 * @file
 * @brief Process-wide reference-counted PortAudio initialization.
 */

#ifndef A11_SDK_AUDIO_INTERNAL_PORTAUDIO_SESSION_H_
#define A11_SDK_AUDIO_INTERNAL_PORTAUDIO_SESSION_H_

#include <memory>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <portaudio.h>

namespace a11::sdk::audio::internal {

/**
 * @brief Keeps PortAudio initialized for as long as any holder is alive.
 *
 * PortAudio's global state must be set up with @c Pa_Initialize before any
 * device or stream call and torn down with a matching @c Pa_Terminate. A shared
 * session lets device queries and every open @ref AudioInput share one
 * initialization: the first Acquire() calls @c Pa_Initialize and the last
 * released handle calls @c Pa_Terminate.
 */
class PortAudioSession {
 public:
  /// Return a handle that keeps PortAudio initialized, initializing if needed.
  static absl::StatusOr<std::shared_ptr<PortAudioSession>> Acquire();

  PortAudioSession(const PortAudioSession&) = delete;
  PortAudioSession& operator=(const PortAudioSession&) = delete;
  ~PortAudioSession();

 private:
  PortAudioSession() = default;
};

/// Translate a PortAudio error code into an A11 status; @p error must be
/// non-OK.
absl::Status PaErrorToStatus(std::string_view context, PaError error);

}  // namespace a11::sdk::audio::internal

#endif  // A11_SDK_AUDIO_INTERNAL_PORTAUDIO_SESSION_H_
