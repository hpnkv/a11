// Copyright 2026 The A11 Authors.

#include "sdk/audio/actions/audio_serialization.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/data/msgpack.h"
#include "a11/data/serializable.h"
#include "a11/data/serialization.h"
#include "a11/time.h"
#include "sdk/audio/actions/audio_events.h"
#include "sdk/audio/audio_buffer.h"
#include "sdk/audio/audio_input.h"
#include "sdk/audio/device.h"
#include "sdk/audio/speech_recognizer.h"

namespace a11::sdk::audio {
namespace {

using ::a11::data::GetBinary;
using ::a11::data::MsgpackReader;
using ::a11::data::MsgpackWriter;

// Reads json[key] as type T, or returns `fallback` when the key is absent.
// Unlike nlohmann::json::value, this reports a typed error rather than throwing
// when the key is present but has the wrong type.
template <typename T>
absl::StatusOr<T> GetOr(const nlohmann::json& json, std::string_view key,
                        T fallback) {
  const auto it = json.find(key);
  if (it == json.end() || it->is_null()) {
    return fallback;
  }
  try {
    return it->get<T>();
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid value for '", key, "': ", error.what()));
  }
}

}  // namespace

// ============================ AudioBuffer ==================================

std::string_view A11SerialTag(a11::data::TypeTag<AudioBuffer>) {
  return kAudioBufferTypeTag;
}

absl::StatusOr<std::string> A11ToMsgpackBytes(const AudioBuffer& value) {
  if (value.samples.size() != value.num_channels * value.num_frames) {
    return absl::InvalidArgumentError(
        "AudioBuffer.samples size does not match num_channels * num_frames");
  }
  MsgpackWriter writer;
  // Samples are packed as a raw little-endian float32 binary blob for
  // compactness; end_time is nanoseconds since the Unix epoch, or null when
  // it carries the infinite-past sentinel.
  const std::string_view sample_bytes(
      reinterpret_cast<const char*>(value.samples.data()),
      value.samples.size() * sizeof(float));
  if (absl::Status s = writer.Pack(a11::data::Binary(sample_bytes)); !s.ok())
    return s;
  if (absl::Status s = writer.Pack(value.num_channels); !s.ok())
    return s;
  if (absl::Status s = writer.Pack(value.num_frames); !s.ok())
    return s;
  if (absl::Status s = writer.Pack(value.sample_rate); !s.ok())
    return s;
  nlohmann::json end_time = nlohmann::json(nullptr);
  if (absl::StatusOr<std::int64_t> nanos =
          a11::TimeNanosecondsSinceEpoch(value.end_time);
      nanos.ok()) {
    end_time = *nanos;
  }
  if (absl::Status s = writer.Pack(end_time); !s.ok())
    return s;
  return writer.TakeBytes();
}

absl::StatusOr<AudioBuffer> A11FromMsgpackBytes(a11::data::TypeTag<AudioBuffer>,
                                                std::string_view bytes) {
  MsgpackReader reader(bytes);
  absl::StatusOr<nlohmann::json> samples_field = reader.Read();
  if (!samples_field.ok())
    return samples_field.status();
  absl::StatusOr<std::string> raw =
      GetBinary(*samples_field, "AudioBuffer.samples");
  if (!raw.ok())
    return raw.status();
  if (raw->size() % sizeof(float) != 0) {
    return absl::InvalidArgumentError(
        "AudioBuffer.samples byte length is not a multiple of sizeof(float)");
  }

  absl::StatusOr<nlohmann::json> num_channels = reader.Read();
  if (!num_channels.ok())
    return num_channels.status();
  absl::StatusOr<nlohmann::json> num_frames = reader.Read();
  if (!num_frames.ok())
    return num_frames.status();
  absl::StatusOr<nlohmann::json> sample_rate = reader.Read();
  if (!sample_rate.ok())
    return sample_rate.status();
  absl::StatusOr<nlohmann::json> end_time = reader.Read();
  if (!end_time.ok())
    return end_time.status();
  if (absl::Status consumed = reader.EnsureFullyConsumed(); !consumed.ok())
    return consumed;

  AudioBuffer result;
  try {
    result.num_channels = num_channels->get<std::size_t>();
    result.num_frames = num_frames->get<std::size_t>();
    result.sample_rate = sample_rate->get<double>();
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid AudioBuffer scalar field: ", error.what()));
  }
  result.samples.resize(raw->size() / sizeof(float));
  std::memcpy(result.samples.data(), raw->data(), raw->size());
  if (end_time->is_null()) {
    result.end_time = a11::InfinitePast();
  } else {
    try {
      result.end_time =
          absl::FromUnixNanos(end_time->get<std::int64_t>());
    } catch (const std::exception& error) {
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid AudioBuffer.end_time: ", error.what()));
    }
  }
  if (result.samples.size() != result.num_channels * result.num_frames) {
    return absl::InvalidArgumentError(
        "AudioBuffer.samples size does not match num_channels * num_frames");
  }
  return result;
}

// ========================== AudioInputOptions ==============================

std::string_view A11SerialTag(a11::data::TypeTag<AudioInputOptions>) {
  return kAudioInputOptionsTypeTag;
}

absl::StatusOr<nlohmann::json> A11ToJson(const AudioInputOptions& value) {
  const AudioInputOptions defaults;
  nlohmann::json json = nlohmann::json::object();
  if (value.device_index != defaults.device_index)
    json["device_index"] = value.device_index;
  if (value.device_name != defaults.device_name)
    json["device_name"] = value.device_name;
  if (value.sample_rate != defaults.sample_rate)
    json["sample_rate"] = value.sample_rate;
  if (value.channels != defaults.channels)
    json["channels"] = value.channels;
  if (value.block_frames != defaults.block_frames)
    json["block_frames"] = value.block_frames;
  if (value.ring_blocks != defaults.ring_blocks)
    json["ring_blocks"] = value.ring_blocks;
  if (value.buffer_frames != defaults.buffer_frames)
    json["buffer_frames"] = value.buffer_frames;
  return json;
}

absl::StatusOr<AudioInputOptions> A11FromJson(
    a11::data::TypeTag<AudioInputOptions>, const nlohmann::json& json) {
  if (!json.is_object()) {
    return absl::InvalidArgumentError("AudioInputOptions JSON must be an object");
  }
  AudioInputOptions value;
  ABSL_ASSIGN_OR_RETURN(value.device_index,
                   GetOr(json, "device_index", value.device_index));
  ABSL_ASSIGN_OR_RETURN(value.device_name,
                   GetOr(json, "device_name", value.device_name));
  ABSL_ASSIGN_OR_RETURN(value.sample_rate,
                   GetOr(json, "sample_rate", value.sample_rate));
  ABSL_ASSIGN_OR_RETURN(value.channels, GetOr(json, "channels", value.channels));
  ABSL_ASSIGN_OR_RETURN(value.block_frames,
                   GetOr(json, "block_frames", value.block_frames));
  ABSL_ASSIGN_OR_RETURN(value.ring_blocks,
                   GetOr(json, "ring_blocks", value.ring_blocks));
  ABSL_ASSIGN_OR_RETURN(value.buffer_frames,
                   GetOr(json, "buffer_frames", value.buffer_frames));
  if (absl::Status valid = value.Validate(); !valid.ok())
    return valid;
  return value;
}

// ======================= SpeechRecognizerOptions ===========================

std::string_view A11SerialTag(a11::data::TypeTag<SpeechRecognizerOptions>) {
  return kSpeechRecognizerOptionsTypeTag;
}

absl::StatusOr<nlohmann::json> A11ToJson(const SpeechRecognizerOptions& value) {
  const SpeechRecognizerOptions d;
  nlohmann::json json = nlohmann::json::object();
  if (value.model_path != d.model_path) json["model_path"] = value.model_path;
  if (value.language != d.language) json["language"] = value.language;
  if (value.translate != d.translate) json["translate"] = value.translate;
  if (value.inference_threads != d.inference_threads)
    json["inference_threads"] = value.inference_threads;
  if (value.use_gpu != d.use_gpu) json["use_gpu"] = value.use_gpu;
  if (value.flash_attention != d.flash_attention)
    json["flash_attention"] = value.flash_attention;
  if (value.use_context != d.use_context)
    json["use_context"] = value.use_context;
  if (value.initial_prompt != d.initial_prompt)
    json["initial_prompt"] = value.initial_prompt;
  if (value.subscription_buffer_millis != d.subscription_buffer_millis)
    json["subscription_buffer_millis"] = value.subscription_buffer_millis;
  if (value.vad_threshold != d.vad_threshold)
    json["vad_threshold"] = value.vad_threshold;
  if (value.vad_noise_ratio != d.vad_noise_ratio)
    json["vad_noise_ratio"] = value.vad_noise_ratio;
  if (value.vad_window_millis != d.vad_window_millis)
    json["vad_window_millis"] = value.vad_window_millis;
  if (value.min_speech_millis != d.min_speech_millis)
    json["min_speech_millis"] = value.min_speech_millis;
  if (value.min_silence_millis != d.min_silence_millis)
    json["min_silence_millis"] = value.min_silence_millis;
  if (value.speech_pad_millis != d.speech_pad_millis)
    json["speech_pad_millis"] = value.speech_pad_millis;
  if (value.max_speech_seconds != d.max_speech_seconds)
    json["max_speech_seconds"] = value.max_speech_seconds;
  if (value.vad_model_path != d.vad_model_path)
    json["vad_model_path"] = value.vad_model_path;
  if (value.silero_threshold != d.silero_threshold)
    json["silero_threshold"] = value.silero_threshold;
  return json;
}

absl::StatusOr<SpeechRecognizerOptions> A11FromJson(
    a11::data::TypeTag<SpeechRecognizerOptions>, const nlohmann::json& json) {
  if (!json.is_object()) {
    return absl::InvalidArgumentError(
        "SpeechRecognizerOptions JSON must be an object");
  }
  SpeechRecognizerOptions v;
  ABSL_ASSIGN_OR_RETURN(v.model_path, GetOr(json, "model_path", v.model_path));
  ABSL_ASSIGN_OR_RETURN(v.language, GetOr(json, "language", v.language));
  ABSL_ASSIGN_OR_RETURN(v.translate, GetOr(json, "translate", v.translate));
  ABSL_ASSIGN_OR_RETURN(v.inference_threads,
                   GetOr(json, "inference_threads", v.inference_threads));
  ABSL_ASSIGN_OR_RETURN(v.use_gpu, GetOr(json, "use_gpu", v.use_gpu));
  ABSL_ASSIGN_OR_RETURN(v.flash_attention,
                   GetOr(json, "flash_attention", v.flash_attention));
  ABSL_ASSIGN_OR_RETURN(v.use_context, GetOr(json, "use_context", v.use_context));
  ABSL_ASSIGN_OR_RETURN(v.initial_prompt,
                   GetOr(json, "initial_prompt", v.initial_prompt));
  ABSL_ASSIGN_OR_RETURN(
      v.subscription_buffer_millis,
      GetOr(json, "subscription_buffer_millis", v.subscription_buffer_millis));
  ABSL_ASSIGN_OR_RETURN(v.vad_threshold,
                   GetOr(json, "vad_threshold", v.vad_threshold));
  ABSL_ASSIGN_OR_RETURN(v.vad_noise_ratio,
                   GetOr(json, "vad_noise_ratio", v.vad_noise_ratio));
  ABSL_ASSIGN_OR_RETURN(v.vad_window_millis,
                   GetOr(json, "vad_window_millis", v.vad_window_millis));
  ABSL_ASSIGN_OR_RETURN(v.min_speech_millis,
                   GetOr(json, "min_speech_millis", v.min_speech_millis));
  ABSL_ASSIGN_OR_RETURN(v.min_silence_millis,
                   GetOr(json, "min_silence_millis", v.min_silence_millis));
  ABSL_ASSIGN_OR_RETURN(v.speech_pad_millis,
                   GetOr(json, "speech_pad_millis", v.speech_pad_millis));
  ABSL_ASSIGN_OR_RETURN(v.max_speech_seconds,
                   GetOr(json, "max_speech_seconds", v.max_speech_seconds));
  ABSL_ASSIGN_OR_RETURN(v.vad_model_path,
                   GetOr(json, "vad_model_path", v.vad_model_path));
  ABSL_ASSIGN_OR_RETURN(v.silero_threshold,
                   GetOr(json, "silero_threshold", v.silero_threshold));
  if (absl::Status valid = v.Validate(); !valid.ok())
    return valid;
  return v;
}

// ============================== DeviceInfo =================================

std::string_view A11SerialTag(a11::data::TypeTag<DeviceInfo>) {
  return kAudioDeviceInfoTypeTag;
}

absl::StatusOr<nlohmann::json> A11ToJson(const DeviceInfo& value) {
  const DeviceInfo d;
  nlohmann::json json = nlohmann::json::object();
  if (value.index != d.index) json["index"] = value.index;
  if (value.name != d.name) json["name"] = value.name;
  if (value.host_api != d.host_api) json["host_api"] = value.host_api;
  if (value.max_input_channels != d.max_input_channels)
    json["max_input_channels"] = value.max_input_channels;
  if (value.max_output_channels != d.max_output_channels)
    json["max_output_channels"] = value.max_output_channels;
  if (value.default_sample_rate != d.default_sample_rate)
    json["default_sample_rate"] = value.default_sample_rate;
  if (value.default_low_input_latency != d.default_low_input_latency)
    json["default_low_input_latency"] =
        absl::ToDoubleSeconds(value.default_low_input_latency);
  if (value.default_high_input_latency != d.default_high_input_latency)
    json["default_high_input_latency"] =
        absl::ToDoubleSeconds(value.default_high_input_latency);
  if (value.is_default_input != d.is_default_input)
    json["is_default_input"] = value.is_default_input;
  if (value.is_default_output != d.is_default_output)
    json["is_default_output"] = value.is_default_output;
  return json;
}

absl::StatusOr<DeviceInfo> A11FromJson(a11::data::TypeTag<DeviceInfo>,
                                       const nlohmann::json& json) {
  if (!json.is_object()) {
    return absl::InvalidArgumentError("AudioDeviceInfo JSON must be an object");
  }
  DeviceInfo v;
  ABSL_ASSIGN_OR_RETURN(v.index, GetOr(json, "index", v.index));
  ABSL_ASSIGN_OR_RETURN(v.name, GetOr(json, "name", v.name));
  ABSL_ASSIGN_OR_RETURN(v.host_api, GetOr(json, "host_api", v.host_api));
  ABSL_ASSIGN_OR_RETURN(v.max_input_channels,
                   GetOr(json, "max_input_channels", v.max_input_channels));
  ABSL_ASSIGN_OR_RETURN(v.max_output_channels,
                   GetOr(json, "max_output_channels", v.max_output_channels));
  ABSL_ASSIGN_OR_RETURN(v.default_sample_rate,
                   GetOr(json, "default_sample_rate", v.default_sample_rate));
  double low = absl::ToDoubleSeconds(v.default_low_input_latency);
  double high = absl::ToDoubleSeconds(v.default_high_input_latency);
  ABSL_ASSIGN_OR_RETURN(low, GetOr(json, "default_low_input_latency", low));
  ABSL_ASSIGN_OR_RETURN(high, GetOr(json, "default_high_input_latency", high));
  v.default_low_input_latency = absl::Seconds(low);
  v.default_high_input_latency = absl::Seconds(high);
  ABSL_ASSIGN_OR_RETURN(v.is_default_input,
                   GetOr(json, "is_default_input", v.is_default_input));
  ABSL_ASSIGN_OR_RETURN(v.is_default_output,
                   GetOr(json, "is_default_output", v.is_default_output));
  return v;
}

// ================================ Events ===================================

std::string_view A11SerialTag(a11::data::TypeTag<AudioControlEvent>) {
  return kAudioControlEventTypeTag;
}

absl::StatusOr<nlohmann::json> A11ToJson(const AudioControlEvent& value) {
  if (absl::Status valid = value.Validate(); !valid.ok())
    return valid;
  return nlohmann::json{{"command", std::string(ToString(value.command))}};
}

absl::StatusOr<AudioControlEvent> A11FromJson(
    a11::data::TypeTag<AudioControlEvent>, const nlohmann::json& json) {
  if (!json.is_object()) {
    return absl::InvalidArgumentError("AudioControlEvent JSON must be an object");
  }
  AudioControlEvent value;
  std::string command;
  ABSL_ASSIGN_OR_RETURN(command, GetOr(json, "command", std::string("stop")));
  ABSL_ASSIGN_OR_RETURN(value.command, ParseControlCommand(command));
  return value;
}

std::string_view A11SerialTag(a11::data::TypeTag<AudioCaptureEvent>) {
  return kAudioCaptureEventTypeTag;
}

absl::StatusOr<nlohmann::json> A11ToJson(const AudioCaptureEvent& value) {
  if (absl::Status valid = value.Validate(); !valid.ok())
    return valid;
  nlohmann::json json{{"kind", std::string(ToString(value.kind))}};
  if (value.dropped != 0) json["dropped"] = value.dropped;
  return json;
}

absl::StatusOr<AudioCaptureEvent> A11FromJson(
    a11::data::TypeTag<AudioCaptureEvent>, const nlohmann::json& json) {
  if (!json.is_object()) {
    return absl::InvalidArgumentError("AudioCaptureEvent JSON must be an object");
  }
  AudioCaptureEvent value;
  std::string kind;
  ABSL_ASSIGN_OR_RETURN(kind, GetOr(json, "kind", std::string("started")));
  ABSL_ASSIGN_OR_RETURN(value.kind, ParseCaptureKind(kind));
  ABSL_ASSIGN_OR_RETURN(value.dropped, GetOr(json, "dropped", std::uint64_t{0}));
  if (absl::Status valid = value.Validate(); !valid.ok())
    return valid;
  return value;
}

std::string_view A11SerialTag(a11::data::TypeTag<TranscriptionEvent>) {
  return kTranscriptionEventTypeTag;
}

absl::StatusOr<nlohmann::json> A11ToJson(const TranscriptionEvent& value) {
  if (absl::Status valid = value.Validate(); !valid.ok())
    return valid;
  return nlohmann::json{{"kind", std::string(ToString(value.kind))}};
}

absl::StatusOr<TranscriptionEvent> A11FromJson(
    a11::data::TypeTag<TranscriptionEvent>, const nlohmann::json& json) {
  if (!json.is_object()) {
    return absl::InvalidArgumentError(
        "TranscriptionEvent JSON must be an object");
  }
  TranscriptionEvent value;
  std::string kind;
  ABSL_ASSIGN_OR_RETURN(kind, GetOr(json, "kind", std::string("capture_started")));
  ABSL_ASSIGN_OR_RETURN(value.kind, ParseTranscriptionKind(kind));
  return value;
}

// ===================== Direct chunk encode helpers =========================

absl::StatusOr<data::Chunk> EncodeAudioBufferChunk(const AudioBuffer& value) {
  absl::StatusOr<std::string> bytes = A11ToMsgpackBytes(value);
  if (!bytes.ok()) {
    return bytes.status();
  }
  data::Chunk chunk;
  chunk.data = std::move(*bytes);
  chunk.metadata = data::ChunkMetadata{
      .mimetype = absl::StrCat(data::kMsgpackMimetype, ";type=",
                               kAudioBufferTypeTag)};
  return chunk;
}

absl::StatusOr<AudioBuffer> DecodeAudioBufferChunk(const data::Chunk& chunk) {
  return A11FromMsgpackBytes(a11::data::TypeTag<AudioBuffer>{}, chunk.data);
}

// ============================ Registration =================================

absl::Status RegisterAudioTypes(a11::data::SerializationRegistry& registry) {
  if (absl::Status s = a11::data::RegisterSerializable<AudioBuffer>(registry);
      !s.ok())
    return s;
  if (absl::Status s =
          a11::data::RegisterSerializable<AudioInputOptions>(registry);
      !s.ok())
    return s;
  if (absl::Status s =
          a11::data::RegisterSerializable<SpeechRecognizerOptions>(registry);
      !s.ok())
    return s;
  if (absl::Status s = a11::data::RegisterSerializable<DeviceInfo>(registry);
      !s.ok())
    return s;
  if (absl::Status s =
          a11::data::RegisterSerializable<AudioControlEvent>(registry);
      !s.ok())
    return s;
  if (absl::Status s =
          a11::data::RegisterSerializable<AudioCaptureEvent>(registry);
      !s.ok())
    return s;
  if (absl::Status s =
          a11::data::RegisterSerializable<TranscriptionEvent>(registry);
      !s.ok())
    return s;
  return absl::OkStatus();
}

}  // namespace a11::sdk::audio
