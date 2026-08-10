// Copyright 2026 The A11 Authors.

#include "sdk/audio/actions/audio_serialization.h"

#include <cstdint>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/time.h"
#include "sdk/audio/actions/audio_actions.h"
#include "sdk/audio/actions/audio_events.h"
#include "sdk/audio/audio_buffer.h"
#include "sdk/audio/audio_input.h"
#include "sdk/audio/device.h"
#include "sdk/audio/speech_recognizer.h"

namespace a11::sdk::audio {
namespace {

using ::a11::data::Chunk;
using ::a11::data::kJsonMimetype;
using ::a11::data::kMsgpackMimetype;
using ::a11::data::SerializationRegistry;

TEST(AudioSerializationTest, AudioBufferRoundTripsMsgpack) {
  SerializationRegistry registry(/*register_defaults=*/false);
  ASSERT_TRUE(RegisterAudioTypes(registry).ok());
  AudioBuffer buffer;
  buffer.num_channels = 2;
  buffer.num_frames = 3;
  buffer.sample_rate = 48000.0;
  buffer.end_time = absl::FromUnixNanos(1234567890);
  buffer.samples = {0.0f, 0.25f, -0.5f, 1.0f, -1.0f, 0.125f};

  absl::StatusOr<Chunk> chunk = registry.ToChunk<AudioBuffer>(buffer);
  ASSERT_TRUE(chunk.ok()) << chunk.status();
  ASSERT_TRUE(chunk->metadata.has_value());
  EXPECT_TRUE(absl::StrContains(chunk->metadata->mimetype,
                                "type=a11.sdk.AudioBuffer"));
  EXPECT_TRUE(absl::StartsWith(chunk->metadata->mimetype, kMsgpackMimetype));

  absl::StatusOr<AudioBuffer> decoded = registry.FromChunk<AudioBuffer>(*chunk);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(decoded->num_channels, buffer.num_channels);
  EXPECT_EQ(decoded->num_frames, buffer.num_frames);
  EXPECT_EQ(decoded->sample_rate, buffer.sample_rate);
  EXPECT_EQ(decoded->samples, buffer.samples);
  EXPECT_EQ(decoded->end_time, buffer.end_time);
}

TEST(AudioSerializationTest, AudioBufferChunkEncodeDecode) {
  AudioBuffer buffer;
  buffer.num_channels = 1;
  buffer.num_frames = 4;
  buffer.sample_rate = 16000.0;
  buffer.samples = {0.0f, 0.1f, 0.2f, 0.3f};
  absl::StatusOr<Chunk> chunk = EncodeAudioBufferChunk(buffer);
  ASSERT_TRUE(chunk.ok()) << chunk.status();
  EXPECT_TRUE(absl::StrContains(chunk->metadata->mimetype,
                                "type=a11.sdk.AudioBuffer"));
  absl::StatusOr<AudioBuffer> decoded = DecodeAudioBufferChunk(*chunk);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(decoded->num_channels, 1u);
  EXPECT_EQ(decoded->num_frames, 4u);
  EXPECT_EQ(decoded->sample_rate, 16000.0);
  EXPECT_EQ(decoded->samples, buffer.samples);
}

TEST(AudioSerializationTest, AudioBufferHasNoJsonCodec) {
  SerializationRegistry registry(/*register_defaults=*/false);
  ASSERT_TRUE(RegisterAudioTypes(registry).ok());
  AudioBuffer buffer;
  buffer.num_channels = 1;
  buffer.num_frames = 1;
  buffer.samples = {0.5f};
  EXPECT_FALSE(registry.ToChunk<AudioBuffer>(buffer, kJsonMimetype).ok());
}

TEST(AudioSerializationTest, AudioInputOptionsJsonOmitsDefaults) {
  SerializationRegistry registry(/*register_defaults=*/false);
  ASSERT_TRUE(RegisterAudioTypes(registry).ok());
  absl::StatusOr<Chunk> chunk =
      registry.ToChunk<AudioInputOptions>(AudioInputOptions{}, kJsonMimetype);
  ASSERT_TRUE(chunk.ok()) << chunk.status();
  EXPECT_EQ(chunk->data, "{}");  // every field is at its default
  EXPECT_TRUE(absl::StrContains(chunk->metadata->mimetype,
                                "type=a11.sdk.AudioInputOptions"));
}

TEST(AudioSerializationTest, AudioInputOptionsJsonRoundTripAndDefaults) {
  SerializationRegistry registry(/*register_defaults=*/false);
  ASSERT_TRUE(RegisterAudioTypes(registry).ok());
  AudioInputOptions options;
  options.device_name = "Built-in Microphone";
  options.sample_rate = 16000.0;
  options.buffer_frames = 512;

  absl::StatusOr<Chunk> chunk =
      registry.ToChunk<AudioInputOptions>(options, kJsonMimetype);
  ASSERT_TRUE(chunk.ok()) << chunk.status();

  absl::StatusOr<AudioInputOptions> decoded =
      registry.FromChunk<AudioInputOptions>(*chunk);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(decoded->device_name, "Built-in Microphone");
  EXPECT_EQ(decoded->sample_rate, 16000.0);
  EXPECT_EQ(decoded->buffer_frames, 512u);
  // Omitted fields recover their defaults.
  EXPECT_EQ(decoded->device_index, -1);
  EXPECT_EQ(decoded->block_frames, 256u);
  EXPECT_EQ(decoded->ring_blocks, 32u);
}

TEST(AudioSerializationTest, AudioInputOptionsMsgpackRoundTrip) {
  SerializationRegistry registry(/*register_defaults=*/false);
  ASSERT_TRUE(RegisterAudioTypes(registry).ok());
  AudioInputOptions options;
  options.channels = 1;
  options.buffer_frames = 256;
  absl::StatusOr<Chunk> chunk =
      registry.ToChunk<AudioInputOptions>(options, kMsgpackMimetype);
  ASSERT_TRUE(chunk.ok()) << chunk.status();
  absl::StatusOr<AudioInputOptions> decoded =
      registry.FromChunk<AudioInputOptions>(*chunk);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(decoded->channels, 1);
  EXPECT_EQ(decoded->buffer_frames, 256u);
}

TEST(AudioSerializationTest, SpeechRecognizerOptionsRoundTrip) {
  SerializationRegistry registry(/*register_defaults=*/false);
  ASSERT_TRUE(RegisterAudioTypes(registry).ok());
  SpeechRecognizerOptions options;
  options.model = "/models/ggml-base.bin";
  options.language = "en";
  options.translate = true;

  absl::StatusOr<Chunk> json =
      registry.ToChunk<SpeechRecognizerOptions>(options, kJsonMimetype);
  ASSERT_TRUE(json.ok()) << json.status();
  absl::StatusOr<SpeechRecognizerOptions> from_json =
      registry.FromChunk<SpeechRecognizerOptions>(*json);
  ASSERT_TRUE(from_json.ok()) << from_json.status();
  EXPECT_EQ(from_json->model, "/models/ggml-base.bin");
  EXPECT_EQ(from_json->language, "en");
  EXPECT_TRUE(from_json->translate);
  EXPECT_EQ(from_json->use_gpu, SpeechRecognizerOptions{}.use_gpu);
}

TEST(AudioSerializationTest, DeviceInfoRoundTrip) {
  SerializationRegistry registry(/*register_defaults=*/false);
  ASSERT_TRUE(RegisterAudioTypes(registry).ok());
  DeviceInfo device;
  device.index = 2;
  device.name = "USB Mic";
  device.host_api = "Core Audio";
  device.max_input_channels = 1;
  device.default_sample_rate = 44100.0;
  device.default_low_input_latency = absl::Milliseconds(10);
  device.is_default_input = true;

  absl::StatusOr<Chunk> chunk =
      registry.ToChunk<DeviceInfo>(device, kJsonMimetype);
  ASSERT_TRUE(chunk.ok()) << chunk.status();
  EXPECT_TRUE(absl::StrContains(chunk->metadata->mimetype,
                                "type=a11.sdk.AudioDeviceInfo"));
  absl::StatusOr<DeviceInfo> decoded = registry.FromChunk<DeviceInfo>(*chunk);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(decoded->index, 2);
  EXPECT_EQ(decoded->name, "USB Mic");
  EXPECT_EQ(decoded->host_api, "Core Audio");
  EXPECT_EQ(decoded->max_input_channels, 1);
  EXPECT_EQ(decoded->default_sample_rate, 44100.0);
  EXPECT_EQ(decoded->default_low_input_latency, absl::Milliseconds(10));
  EXPECT_TRUE(decoded->is_default_input);
}

TEST(AudioSerializationTest, ControlEventRoundTrip) {
  SerializationRegistry registry(/*register_defaults=*/false);
  ASSERT_TRUE(RegisterAudioTypes(registry).ok());
  absl::StatusOr<Chunk> chunk =
      registry.ToChunk<AudioControlEvent>(AudioControlEvent::Stop());
  ASSERT_TRUE(chunk.ok()) << chunk.status();
  EXPECT_TRUE(absl::StrContains(chunk->metadata->mimetype,
                                "type=a11.sdk.AudioControlEvent"));
  absl::StatusOr<AudioControlEvent> decoded =
      registry.FromChunk<AudioControlEvent>(*chunk);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(*decoded, AudioControlEvent::Stop());
}

TEST(AudioSerializationTest, CaptureEventRoundTrip) {
  SerializationRegistry registry(/*register_defaults=*/false);
  ASSERT_TRUE(RegisterAudioTypes(registry).ok());
  absl::StatusOr<Chunk> chunk = registry.ToChunk<AudioCaptureEvent>(
      AudioCaptureEvent::BuffersDropped(5));
  ASSERT_TRUE(chunk.ok()) << chunk.status();
  absl::StatusOr<AudioCaptureEvent> decoded =
      registry.FromChunk<AudioCaptureEvent>(*chunk);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(*decoded, AudioCaptureEvent::BuffersDropped(5));
}

TEST(AudioSerializationTest, CaptureEventRejectsDropOnNonDropKind) {
  SerializationRegistry registry(/*register_defaults=*/false);
  ASSERT_TRUE(RegisterAudioTypes(registry).ok());
  // A started event carrying a drop count is invalid on decode.
  Chunk chunk;
  chunk.metadata = data::ChunkMetadata{
      .mimetype = std::string(kJsonMimetype) +
                  ";type=a11.sdk.AudioCaptureEvent"};
  chunk.data = R"({"kind":"started","dropped":5})";
  EXPECT_FALSE(registry.FromChunk<AudioCaptureEvent>(chunk).ok());
}

TEST(AudioSerializationTest, PythonStyleJsonDecodesOnGlobalRegistry) {
  ASSERT_TRUE(EnsureAudioTypesRegistered().ok());
  data::SerializationRegistry& reg = data::GlobalSerializationRegistry();
  Chunk chunk;
  chunk.metadata = data::ChunkMetadata{
      .mimetype =
          "application/json;type=a11.sdk.AudioInputOptions"};
  chunk.data =
      R"({"device_index": -1, "device_name": "", "sample_rate": 0.0, )"
      R"("channels": 0, "block_frames": 256, "ring_blocks": 32, )"
      R"("buffer_frames": 512})";
  absl::StatusOr<AudioInputOptions> decoded =
      reg.FromChunk<AudioInputOptions>(chunk);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(decoded->buffer_frames, 512u);
}

TEST(AudioSerializationTest, TranscriptionEventRoundTrip) {
  SerializationRegistry registry(/*register_defaults=*/false);
  ASSERT_TRUE(RegisterAudioTypes(registry).ok());
  absl::StatusOr<Chunk> chunk = registry.ToChunk<TranscriptionEvent>(
      TranscriptionEvent::InferenceStarted());
  ASSERT_TRUE(chunk.ok()) << chunk.status();
  absl::StatusOr<TranscriptionEvent> decoded =
      registry.FromChunk<TranscriptionEvent>(*chunk);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(*decoded, TranscriptionEvent::InferenceStarted());
}

}  // namespace
}  // namespace a11::sdk::audio
