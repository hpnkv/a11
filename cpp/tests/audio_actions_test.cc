// Copyright 2026 The A11 Authors.

#include "sdk/audio/actions/audio_actions.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/future.h"
#include "a11/nodes/async_node.h"
#include "sdk/audio/actions/audio_events.h"
#include "sdk/audio/audio_input.h"
#include "sdk/audio/device.h"
#include "sdk/audio/speech_recognizer.h"

namespace a11::sdk::audio {
namespace {

using ::a11::actions::Action;
using ::a11::actions::ActionRegistry;

TEST(AudioActionsTest, SchemasValidate) {
  EXPECT_TRUE(ListAudioInputsSchema().Validate().ok());
  EXPECT_TRUE(CaptureAudioSchema().Validate().ok());
  EXPECT_TRUE(CaptureTranscriptionSchema().Validate().ok());
  EXPECT_TRUE(TranscribeAudioSchema().Validate().ok());
}

TEST(AudioActionsTest, RegistersAllActions) {
  ActionRegistry registry;
  ASSERT_TRUE(RegisterAudioActions(registry).ok());
  EXPECT_TRUE(registry.IsRegistered(kListAudioInputsAction));
  EXPECT_TRUE(registry.IsRegistered(kCaptureAudioAction));
  EXPECT_TRUE(registry.IsRegistered(kCaptureTranscriptionAction));
  EXPECT_TRUE(registry.IsRegistered(kTranscribeAudioAction));
  // Registration overwrites, so registering again is idempotent.
  EXPECT_TRUE(RegisterAudioActions(registry).ok());
}

TEST(AudioActionsTest, ParseDeadlineHeaderSemantics) {
  // Absent value: no deadline.
  EXPECT_EQ(*ParseDeadlineHeader(""), absl::InfiniteFuture());
  // Bare value is milliseconds; an 'ns' suffix is nanoseconds.
  EXPECT_EQ(*ParseDeadlineHeader("1500"), absl::FromUnixMillis(1500));
  EXPECT_EQ(*ParseDeadlineHeader("1500ns"), absl::FromUnixNanos(1500));
  // The same digits mean a far larger instant as ms than as ns.
  EXPECT_GT(*ParseDeadlineHeader("9999999999999"),
            *ParseDeadlineHeader("9999999999999ns"));
  // Malformed values are rejected.
  EXPECT_EQ(ParseDeadlineHeader("nope").status().code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(ParseDeadlineHeader("-5").status().code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(AudioActionsTest, DeadlineAlreadyPassedIsRejected) {
  ASSERT_TRUE(EnsureAudioTypesRegistered().ok());
  // The deadline is checked before any device is touched, so this is
  // deterministic without audio hardware. "1" ms since epoch is long past.
  auto action =
      *Action::Create(CaptureAudioSchema(), "past", CaptureAudioHandler());
  ASSERT_TRUE(action->SetHeader(std::string(kDeadlineHeader), "1").ok());
  ASSERT_TRUE(action->Run().ok());
  EXPECT_EQ(action->Wait(absl::Seconds(10)).Await().status().code(),
            absl::StatusCode::kDeadlineExceeded);
}

TEST(AudioActionsTest, SchemasDeclareDeadlineHeader) {
  for (const a11::actions::ActionSchema& schema :
       {ListAudioInputsSchema(), CaptureAudioSchema(),
        CaptureTranscriptionSchema(), TranscribeAudioSchema()}) {
    EXPECT_TRUE(schema.headers.contains(std::string(kDeadlineHeader)))
        << schema.name;
  }
}

TEST(AudioActionsTest, TranscribeAudioRejectsAnUnresolvableModel) {
  ASSERT_TRUE(EnsureAudioTypesRegistered().ok());
  auto action =
      *Action::Create(TranscribeAudioSchema(), "tr", TranscribeAudioHandler());
  ASSERT_TRUE((*action->GetInput("audio", false))->PutNullFinal().Await().ok());
  // An *absent* model is no longer an error -- it means the default shorthand,
  // which would download. A model that is neither a shorthand nor a file still
  // is, and needs no network to reject.
  // Raw tagged JSON rather than the registry's typed path, as elsewhere in
  // this file: the registry's std::any round trip does not survive crossing a
  // translation unit.
  data::Chunk asr_options;
  asr_options.metadata = data::ChunkMetadata{
      .mimetype = "application/json;type=a11.sdk.SpeechRecognizerOptions"};
  asr_options.data = R"({"model": "no-such-model-shorthand"})";
  ASSERT_TRUE((*action->GetInput("asr_options", false))
                  ->PutChunk(asr_options, std::nullopt, /*final=*/true)
                  .Await()
                  .ok());
  ASSERT_TRUE(action->Run().ok());
  const absl::Status status = action->Wait(absl::Seconds(10)).Await().status();
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  // The message lists the shorthands that would have worked.
  EXPECT_NE(status.message().find("tiny.en"), std::string_view::npos);
}

TEST(AudioActionsTest, CaptureTranscriptionRejectsAnUnresolvableModel) {
  ASSERT_TRUE(EnsureAudioTypesRegistered().ok());
  auto action = *Action::Create(CaptureTranscriptionSchema(), "t",
                                CaptureTranscriptionHandler());
  ASSERT_TRUE((*action->GetInput("capture_options", false))
                  ->PutNullFinal()
                  .Await()
                  .ok());
  // Raw tagged JSON rather than the registry's typed path, as elsewhere in
  // this file: the registry's std::any round trip does not survive crossing a
  // translation unit.
  data::Chunk asr_options;
  asr_options.metadata = data::ChunkMetadata{
      .mimetype = "application/json;type=a11.sdk.SpeechRecognizerOptions"};
  asr_options.data = R"({"model": "no-such-model-shorthand"})";
  ASSERT_TRUE((*action->GetInput("asr_options", false))
                  ->PutChunk(asr_options, std::nullopt, /*final=*/true)
                  .Await()
                  .ok());
  ASSERT_TRUE((*action->GetInput("control_events", false))
                  ->PutNullFinal()
                  .Await()
                  .ok());
  ASSERT_TRUE(action->Run().ok());
  absl::Status status = action->Wait(absl::Seconds(10)).Await().status();
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(AudioActionsTest, ListAudioInputsStreamsDevices) {
  if (!ListDevices().ok()) {
    GTEST_SKIP() << "No audio backend available";
  }
  ASSERT_TRUE(EnsureAudioTypesRegistered().ok());
  auto action = *Action::Create(ListAudioInputsSchema(), "list",
                                ListAudioInputsHandler());
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(absl::Seconds(10)).Await().ok())
      << action->GetStatus();

  auto out = *action->GetOutput("inputs", false);
  std::vector<DeviceInfo> devices;
  while (true) {
    auto next =
        out->NextObject<DeviceInfo>().Await(absl::Now() + absl::Seconds(5));
    ASSERT_TRUE(next.ok()) << next.status();
    if (!next->has_value()) {
      break;
    }
    devices.push_back(**next);
  }
  for (const DeviceInfo& device : devices) {
    EXPECT_GT(device.max_input_channels, 0);
  }
}

TEST(AudioActionsTest, CaptureAudioDecodesRawJsonOptionsNodePath) {
  if (!DefaultInputDevice().ok()) {
    GTEST_SKIP() << "No default input device available";
  }
  ASSERT_TRUE(EnsureAudioTypesRegistered().ok());
  auto action = *a11::actions::Action::Create(CaptureAudioSchema(), "capjson",
                                              CaptureAudioHandler());
  data::Chunk options;
  options.metadata = data::ChunkMetadata{
      .mimetype = "application/json;type=a11.sdk.AudioInputOptions"};
  options.data = R"({"buffer_frames": 512})";
  ASSERT_TRUE((*action->GetInput("options", false))
                  ->PutChunk(options, std::nullopt, /*final=*/true)
                  .Await()
                  .ok());
  ASSERT_TRUE((*action->GetInput("control_events", false))
                  ->Put<AudioControlEvent>(AudioControlEvent::Stop(),
                                           std::nullopt, /*final=*/true)
                  .Await()
                  .ok());
  ASSERT_TRUE(action->Run().ok());
  EXPECT_TRUE(action->Wait(absl::Seconds(15)).Await().ok())
      << action->GetStatus();
}

TEST(AudioActionsTest, CaptureAudioStopsGracefully) {
  if (!DefaultInputDevice().ok()) {
    GTEST_SKIP() << "No default input device available";
  }
  ASSERT_TRUE(EnsureAudioTypesRegistered().ok());
  auto action =
      *Action::Create(CaptureAudioSchema(), "cap", CaptureAudioHandler());

  AudioInputOptions options;
  options.buffer_frames = 512;
  ASSERT_TRUE(
      (*action->GetInput("options", false))
          ->Put<AudioInputOptions>(options, std::nullopt, /*final=*/true)
          .Await()
          .ok());
  // A stop command already queued makes the run finish promptly.
  ASSERT_TRUE((*action->GetInput("control_events", false))
                  ->Put<AudioControlEvent>(AudioControlEvent::Stop(),
                                           std::nullopt, /*final=*/true)
                  .Await()
                  .ok());

  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(absl::Seconds(15)).Await().ok())
      << action->GetStatus();

  // The events stream begins with started and ends with stopped.
  auto events = *action->GetOutput("events", false);
  std::vector<AudioCaptureEvent> seen;
  while (true) {
    auto next = events->NextObject<AudioCaptureEvent>().Await(absl::Now() +
                                                              absl::Seconds(5));
    ASSERT_TRUE(next.ok()) << next.status();
    if (!next->has_value()) {
      break;
    }
    seen.push_back(**next);
  }
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.front().kind, AudioCaptureEvent::Kind::kStarted);
  EXPECT_EQ(seen.back().kind, AudioCaptureEvent::Kind::kStopped);

  // The audio stream terminates cleanly (zero or more buffers, then end).
  auto audio = *action->GetOutput("audio", false);
  while (true) {
    auto next = audio->NextChunk().Await(absl::Now() + absl::Seconds(5));
    ASSERT_TRUE(next.ok()) << next.status();
    if (!next->has_value()) {
      break;
    }
  }
}

}  // namespace
}  // namespace a11::sdk::audio
