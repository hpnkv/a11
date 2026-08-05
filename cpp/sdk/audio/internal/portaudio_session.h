// Copyright 2026 The A11 Authors.

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

/// Translate a PortAudio error code into an A11 status; @p error must be non-OK.
absl::Status PaErrorToStatus(std::string_view context, PaError error);

}  // namespace a11::sdk::audio::internal

#endif  // A11_SDK_AUDIO_INTERNAL_PORTAUDIO_SESSION_H_
