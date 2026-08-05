// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief ADL serialization for the audio Action value types.
 *
 * Declares the a11::data customization points (A11SerialTag / A11ToJson /
 * A11FromJson / A11ToMsgpackBytes / A11FromMsgpackBytes) for every type that
 * appears on an audio Action's ports, and @ref RegisterAudioTypes to install
 * their codecs into a SerializationRegistry. Type tags are language-agnostic,
 * e.g. @c "application/x-msgpack;type=a11.sdk.audio.AudioBuffer".
 *
 * Representations:
 *   - AudioBuffer            -- MessagePack only (samples packed as binary).
 *   - AudioInputOptions      -- JSON and MessagePack.
 *   - SpeechRecognizerOptions-- JSON and MessagePack.
 *   - DeviceInfo             -- JSON and MessagePack (Python: AudioDeviceInfo).
 *   - AudioControlEvent      -- JSON and MessagePack.
 *   - AudioCaptureEvent      -- JSON and MessagePack.
 *   - TranscriptionEvent     -- JSON and MessagePack.
 *
 * JSON follows the project convention: fields equal to their default are
 * omitted on serialize and filled from defaults on deserialize, and the value
 * is Validate()'d after decode.
 */

#ifndef A11_SDK_AUDIO_ACTIONS_AUDIO_SERIALIZATION_H_
#define A11_SDK_AUDIO_ACTIONS_AUDIO_SERIALIZATION_H_

#include <cstdint>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <nlohmann/json.hpp>

#include "a11/data/serializable.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "sdk/audio/actions/audio_events.h"
#include "sdk/audio/audio_buffer.h"
#include "sdk/audio/audio_input.h"
#include "sdk/audio/device.h"
#include "sdk/audio/speech_recognizer.h"

namespace a11::sdk::audio {

// --- Type tags (also the Python-visible, language-agnostic names) -----------

inline constexpr std::string_view kAudioBufferTypeTag = "a11.sdk.audio.AudioBuffer";
inline constexpr std::string_view kAudioInputOptionsTypeTag =
    "a11.sdk.audio.AudioInputOptions";
inline constexpr std::string_view kSpeechRecognizerOptionsTypeTag =
    "a11.sdk.audio.SpeechRecognizerOptions";
inline constexpr std::string_view kAudioDeviceInfoTypeTag =
    "a11.sdk.audio.AudioDeviceInfo";
inline constexpr std::string_view kAudioControlEventTypeTag =
    "a11.sdk.audio.AudioControlEvent";
inline constexpr std::string_view kAudioCaptureEventTypeTag =
    "a11.sdk.audio.AudioCaptureEvent";
inline constexpr std::string_view kTranscriptionEventTypeTag =
    "a11.sdk.audio.TranscriptionEvent";

// --- AudioBuffer: MessagePack only ------------------------------------------

std::string_view A11SerialTag(a11::data::TypeTag<AudioBuffer>);
absl::StatusOr<std::string> A11ToMsgpackBytes(const AudioBuffer& value);
absl::StatusOr<AudioBuffer> A11FromMsgpackBytes(a11::data::TypeTag<AudioBuffer>,
                                                std::string_view bytes);

// --- AudioInputOptions: JSON + MessagePack ----------------------------------

std::string_view A11SerialTag(a11::data::TypeTag<AudioInputOptions>);
absl::StatusOr<nlohmann::json> A11ToJson(const AudioInputOptions& value);
absl::StatusOr<AudioInputOptions> A11FromJson(
    a11::data::TypeTag<AudioInputOptions>, const nlohmann::json& json);

// --- SpeechRecognizerOptions: JSON + MessagePack ----------------------------

std::string_view A11SerialTag(a11::data::TypeTag<SpeechRecognizerOptions>);
absl::StatusOr<nlohmann::json> A11ToJson(const SpeechRecognizerOptions& value);
absl::StatusOr<SpeechRecognizerOptions> A11FromJson(
    a11::data::TypeTag<SpeechRecognizerOptions>, const nlohmann::json& json);

// --- DeviceInfo: JSON + MessagePack -----------------------------------------

std::string_view A11SerialTag(a11::data::TypeTag<DeviceInfo>);
absl::StatusOr<nlohmann::json> A11ToJson(const DeviceInfo& value);
absl::StatusOr<DeviceInfo> A11FromJson(a11::data::TypeTag<DeviceInfo>,
                                       const nlohmann::json& json);

// --- Events: JSON + MessagePack ---------------------------------------------

std::string_view A11SerialTag(a11::data::TypeTag<AudioControlEvent>);
absl::StatusOr<nlohmann::json> A11ToJson(const AudioControlEvent& value);
absl::StatusOr<AudioControlEvent> A11FromJson(
    a11::data::TypeTag<AudioControlEvent>, const nlohmann::json& json);

std::string_view A11SerialTag(a11::data::TypeTag<AudioCaptureEvent>);
absl::StatusOr<nlohmann::json> A11ToJson(const AudioCaptureEvent& value);
absl::StatusOr<AudioCaptureEvent> A11FromJson(
    a11::data::TypeTag<AudioCaptureEvent>, const nlohmann::json& json);

std::string_view A11SerialTag(a11::data::TypeTag<TranscriptionEvent>);
absl::StatusOr<nlohmann::json> A11ToJson(const TranscriptionEvent& value);
absl::StatusOr<TranscriptionEvent> A11FromJson(
    a11::data::TypeTag<TranscriptionEvent>, const nlohmann::json& json);

/**
 * @brief Registers codecs for every audio Action value type into @p registry.
 *
 * Idempotent per registry only in the sense that a second call returns the
 * first AlreadyExists error; callers typically install once into a fresh or
 * process-wide registry.
 */
absl::Status RegisterAudioTypes(a11::data::SerializationRegistry& registry);

// --- Direct chunk encode/decode (no std::any) -------------------------------
//
// The Action handlers use these instead of the registry's typed Put/NextObject
// so that a value is encoded and decoded within one translation unit. The
// registry path routes through std::any, whose libc++ type identity is not
// reliably shared across translation units inside a hidden-visibility Python
// extension, which would surface as a spurious "bad any cast".

/// Encode a JSON-serializable value into a tagged @c application/json chunk.
template <typename T>
  requires a11::data::JsonSerializable<T>
absl::StatusOr<a11::data::Chunk> EncodeJsonChunk(const T& value) {
  absl::StatusOr<nlohmann::json> json = A11ToJson(value);
  if (!json.ok()) {
    return json.status();
  }
  a11::data::Chunk chunk;
  try {
    chunk.data = json->dump();
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Failed to serialize JSON: ", error.what()));
  }
  chunk.metadata = a11::data::ChunkMetadata{
      .mimetype = absl::StrCat(a11::data::kJsonMimetype, ";type=",
                               A11SerialTag(a11::data::TypeTag<T>{}))};
  return chunk;
}

/// Decode a JSON-serializable value from a chunk, accepting either the JSON or
/// (JSON-derived) MessagePack representation.
template <typename T>
  requires a11::data::JsonSerializable<T>
absl::StatusOr<T> DecodeJsonChunk(const a11::data::Chunk& chunk) {
  nlohmann::json json;
  try {
    if (absl::StartsWith(chunk.GetMimetype(), a11::data::kMsgpackMimetype)) {
      const auto* first =
          reinterpret_cast<const std::uint8_t*>(chunk.data.data());
      json = nlohmann::json::from_msgpack(first, first + chunk.data.size(),
                                          true, true);
    } else {
      json = nlohmann::json::parse(chunk.data);
    }
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid encoded value: ", error.what()));
  }
  return A11FromJson(a11::data::TypeTag<T>{}, json);
}

/// Encode an AudioBuffer into a tagged @c application/x-msgpack chunk.
absl::StatusOr<a11::data::Chunk> EncodeAudioBufferChunk(
    const AudioBuffer& value);

/// Decode an AudioBuffer from a @c application/x-msgpack chunk.
absl::StatusOr<AudioBuffer> DecodeAudioBufferChunk(const a11::data::Chunk& chunk);

}  // namespace a11::sdk::audio

#endif  // A11_SDK_AUDIO_ACTIONS_AUDIO_SERIALIZATION_H_
