// Copyright 2026 The A11 Authors.

#include "sdk/audio/actions/audio_events.h"

#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>

namespace a11::sdk::audio {

std::string_view ToString(AudioControlEvent::Command command) {
  switch (command) {
    case AudioControlEvent::Command::kStop:
      return "stop";
  }
  return {};
}

absl::StatusOr<AudioControlEvent::Command> ParseControlCommand(
    std::string_view text) {
  if (text == "stop") {
    return AudioControlEvent::Command::kStop;
  }
  return absl::InvalidArgumentError(
      absl::StrCat("Unknown audio control command '", text, "'"));
}

absl::Status AudioControlEvent::Validate() const {
  if (ToString(command).empty()) {
    return absl::InvalidArgumentError(
        "AudioControlEvent has an unknown command");
  }
  return absl::OkStatus();
}

std::string_view ToString(AudioCaptureEvent::Kind kind) {
  switch (kind) {
    case AudioCaptureEvent::Kind::kStarted:
      return "started";
    case AudioCaptureEvent::Kind::kBuffersDropped:
      return "buffers_dropped";
    case AudioCaptureEvent::Kind::kStopped:
      return "stopped";
  }
  return {};
}

absl::StatusOr<AudioCaptureEvent::Kind> ParseCaptureKind(
    std::string_view text) {
  if (text == "started") {
    return AudioCaptureEvent::Kind::kStarted;
  }
  if (text == "buffers_dropped") {
    return AudioCaptureEvent::Kind::kBuffersDropped;
  }
  if (text == "stopped") {
    return AudioCaptureEvent::Kind::kStopped;
  }
  return absl::InvalidArgumentError(
      absl::StrCat("Unknown audio capture event kind '", text, "'"));
}

absl::Status AudioCaptureEvent::Validate() const {
  if (ToString(kind).empty()) {
    return absl::InvalidArgumentError("AudioCaptureEvent has an unknown kind");
  }
  if (kind != Kind::kBuffersDropped && dropped != 0) {
    return absl::InvalidArgumentError(
        "AudioCaptureEvent.dropped is only valid for a buffers_dropped event");
  }
  return absl::OkStatus();
}

std::string_view ToString(TranscriptionEvent::Kind kind) {
  switch (kind) {
    case TranscriptionEvent::Kind::kCaptureStarted:
      return "capture_started";
    case TranscriptionEvent::Kind::kInferenceStarted:
      return "inference_started";
    case TranscriptionEvent::Kind::kInferenceStopped:
      return "inference_stopped";
    case TranscriptionEvent::Kind::kCaptureStopped:
      return "capture_stopped";
  }
  return {};
}

absl::StatusOr<TranscriptionEvent::Kind> ParseTranscriptionKind(
    std::string_view text) {
  if (text == "capture_started") {
    return TranscriptionEvent::Kind::kCaptureStarted;
  }
  if (text == "inference_started") {
    return TranscriptionEvent::Kind::kInferenceStarted;
  }
  if (text == "inference_stopped") {
    return TranscriptionEvent::Kind::kInferenceStopped;
  }
  if (text == "capture_stopped") {
    return TranscriptionEvent::Kind::kCaptureStopped;
  }
  return absl::InvalidArgumentError(
      absl::StrCat("Unknown transcription event kind '", text, "'"));
}

absl::Status TranscriptionEvent::Validate() const {
  if (ToString(kind).empty()) {
    return absl::InvalidArgumentError("TranscriptionEvent has an unknown kind");
  }
  return absl::OkStatus();
}

}  // namespace a11::sdk::audio
