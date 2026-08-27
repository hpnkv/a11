// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief ADL serialization for the audio Action value types.
 *
 * Declares the a11::data customization points (A11SerialTag / A11ToJson /
 * A11FromJson / A11ToMsgpackBytes / A11FromMsgpackBytes) for every type that
 * appears on an audio Action's ports, and @ref RegisterAudioTypes to install
 * their codecs into a SerializationRegistry. Type tags are language-agnostic
 * and come from a11/data/serial_tags.h, e.g.
 * @c "application/x-msgpack;type=a11.sdk.AudioBuffer".
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
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <nlohmann/json.hpp>

#include "a11/data/serial_tags.h"
#include "a11/data/serializable.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/json_codec.h"
#include "sdk/audio/actions/audio_events.h"
#include "sdk/audio/audio_buffer.h"
#include "sdk/audio/audio_input.h"
#include "sdk/audio/device.h"
#include "sdk/audio/speech_recognizer.h"

namespace a11::sdk::audio {

// --- Type tags --------------------------------------------------------------
// Aliases of the canonical cross-language table in a11/data/serial_tags.h;
// every language must agree on these strings, so they are defined once.

inline constexpr std::string_view kAudioBufferTypeTag =
    a11::data::kAudioBufferTag;
inline constexpr std::string_view kAudioInputOptionsTypeTag =
    a11::data::kAudioInputOptionsTag;
inline constexpr std::string_view kSpeechRecognizerOptionsTypeTag =
    a11::data::kSpeechRecognizerOptionsTag;
inline constexpr std::string_view kAudioDeviceInfoTypeTag =
    a11::data::kAudioDeviceInfoTag;
inline constexpr std::string_view kAudioControlEventTypeTag =
    a11::data::kAudioControlEventTag;
inline constexpr std::string_view kAudioCaptureEventTypeTag =
    a11::data::kAudioCaptureEventTag;
inline constexpr std::string_view kTranscriptionEventTypeTag =
    a11::data::kTranscriptionEventTag;

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
// The Action handlers use these instead of the registry's typed Put/NextObject
// so that a value is encoded and decoded within one translation.

/// Encode a JSON-serializable value into a tagged @c application/json chunk.
template <typename T>
requires a11::data::JsonSerializable<T> absl::StatusOr<a11::data::Chunk>
EncodeJsonChunk(const T& value) {
  absl::StatusOr<nlohmann::json> json = A11ToJson(value);
  if (!json.ok()) {
    return json.status();
  }
  a11::data::Chunk chunk;
  ABSL_ASSIGN_OR_RETURN(chunk.data, a11::DumpJson(*json, "JSON"));
  chunk.metadata = a11::data::ChunkMetadata{
      .mimetype = absl::StrCat(a11::data::kJsonMimetype, ";type=",
                               A11SerialTag(a11::data::TypeTag<T>{}))};
  return chunk;
}

/// Decode a JSON-serializable value from a chunk, accepting either the JSON or
/// (JSON-derived) MessagePack representation.
template <typename T>
requires a11::data::JsonSerializable<T> absl::StatusOr<T> DecodeJsonChunk(
    const a11::data::Chunk& chunk) {
  absl::StatusOr<nlohmann::json> json =
      absl::StartsWith(chunk.GetMimetype(), a11::data::kMsgpackMimetype)
          ? a11::UnpackMsgpack(chunk.data, "encoded value")
          : a11::ParseJson(chunk.data, "encoded value");
  if (!json.ok()) {
    return json.status();
  }
  return A11FromJson(a11::data::TypeTag<T>{}, *json);
}

/// Encode an AudioBuffer into a tagged @c application/x-msgpack chunk.
absl::StatusOr<a11::data::Chunk> EncodeAudioBufferChunk(
    const AudioBuffer& value);

/// Decode an AudioBuffer from a @c application/x-msgpack chunk.
absl::StatusOr<AudioBuffer> DecodeAudioBufferChunk(
    const a11::data::Chunk& chunk);

}  // namespace a11::sdk::audio

#endif  // A11_SDK_AUDIO_ACTIONS_AUDIO_SERIALIZATION_H_
