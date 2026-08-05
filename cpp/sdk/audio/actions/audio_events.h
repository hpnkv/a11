// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Control and status event value types exchanged by the audio Actions.
 *
 * These small, serializable records are the payloads flowing on the audio
 * Actions' control and event ports: an @ref AudioControlEvent drives capture
 * (currently a graceful stop), while @ref AudioCaptureEvent and
 * @ref TranscriptionEvent report lifecycle and dropped-buffer information back
 * to the caller. Their serialization lives in audio_serialization.h.
 */

#ifndef A11_SDK_AUDIO_ACTIONS_AUDIO_EVENTS_H_
#define A11_SDK_AUDIO_ACTIONS_AUDIO_EVENTS_H_

#include <cstdint>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

namespace a11::sdk::audio {

/**
 * @brief A command sent on an Action's @c control_events input.
 *
 * The only command today is @c kStop, which asks the running capture (or
 * transcription) Action to finish gracefully: flush what it has and close its
 * outputs cleanly rather than aborting.
 */
struct AudioControlEvent {
  enum class Command {
    kStop,  ///< Gracefully stop capture and finish the Action.
  };

  Command command = Command::kStop;

  /// A stop command.
  static AudioControlEvent Stop() { return AudioControlEvent{Command::kStop}; }

  [[nodiscard]] bool is_stop() const { return command == Command::kStop; }

  /// Validate the command value.
  absl::Status Validate() const;

  friend bool operator==(const AudioControlEvent&,
                         const AudioControlEvent&) = default;
};

/// Wire spelling of an @ref AudioControlEvent::Command, or empty if unknown.
std::string_view ToString(AudioControlEvent::Command command);
/// Parse an @ref AudioControlEvent::Command from its wire spelling.
absl::StatusOr<AudioControlEvent::Command> ParseControlCommand(
    std::string_view text);

/**
 * @brief A lifecycle or drop notification emitted on @c capture_audio's
 *   @c events output.
 */
struct AudioCaptureEvent {
  enum class Kind {
    kStarted,        ///< Capture stream opened; buffers will follow.
    kBuffersDropped, ///< One or more buffers were dropped (consumer too slow).
    kStopped,        ///< Capture finished; no more buffers will follow.
  };

  Kind kind = Kind::kStarted;
  /// Number of buffers dropped since the previous event (kBuffersDropped only).
  std::uint64_t dropped = 0;

  static AudioCaptureEvent Started() {
    return AudioCaptureEvent{Kind::kStarted, 0};
  }
  static AudioCaptureEvent Stopped() {
    return AudioCaptureEvent{Kind::kStopped, 0};
  }
  static AudioCaptureEvent BuffersDropped(std::uint64_t count) {
    return AudioCaptureEvent{Kind::kBuffersDropped, count};
  }

  /// Validate the kind and drop count.
  absl::Status Validate() const;

  friend bool operator==(const AudioCaptureEvent&,
                         const AudioCaptureEvent&) = default;
};

std::string_view ToString(AudioCaptureEvent::Kind kind);
absl::StatusOr<AudioCaptureEvent::Kind> ParseCaptureKind(std::string_view text);

/**
 * @brief A lifecycle notification emitted on @c capture_transcription's
 *   @c events output. Raw @ref AudioCaptureEvent values are never relayed here.
 */
struct TranscriptionEvent {
  enum class Kind {
    kCaptureStarted,    ///< Microphone capture opened.
    kInferenceStarted,  ///< Speech recognition began running.
    kInferenceStopped,  ///< Speech recognition finished; no more pieces.
    kCaptureStopped,    ///< Microphone capture closed.
  };

  Kind kind = Kind::kCaptureStarted;

  static TranscriptionEvent CaptureStarted() {
    return TranscriptionEvent{Kind::kCaptureStarted};
  }
  static TranscriptionEvent InferenceStarted() {
    return TranscriptionEvent{Kind::kInferenceStarted};
  }
  static TranscriptionEvent InferenceStopped() {
    return TranscriptionEvent{Kind::kInferenceStopped};
  }
  static TranscriptionEvent CaptureStopped() {
    return TranscriptionEvent{Kind::kCaptureStopped};
  }

  /// Validate the kind.
  absl::Status Validate() const;

  friend bool operator==(const TranscriptionEvent&,
                         const TranscriptionEvent&) = default;
};

std::string_view ToString(TranscriptionEvent::Kind kind);
absl::StatusOr<TranscriptionEvent::Kind> ParseTranscriptionKind(
    std::string_view text);

}  // namespace a11::sdk::audio

#endif  // A11_SDK_AUDIO_ACTIONS_AUDIO_EVENTS_H_
