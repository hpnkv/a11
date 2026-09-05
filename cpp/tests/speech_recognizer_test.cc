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

#include "sdk/audio/speech_recognizer.h"

#include <absl/status/status.h>
#include <gtest/gtest.h>

namespace a11::sdk::audio {
namespace {

TEST(SpeechRecognizerOptionsTest, AcceptsDefaults) {
  EXPECT_TRUE(SpeechRecognizerOptions{}.Validate().ok());
}

TEST(SpeechRecognizerOptionsTest, RejectsInvalidVadBounds) {
  SpeechRecognizerOptions options;
  options.vad_threshold = 0.0f;
  EXPECT_EQ(options.Validate().code(), absl::StatusCode::kInvalidArgument);

  options = SpeechRecognizerOptions{};
  options.speech_pad_millis = options.min_silence_millis + 1;
  EXPECT_EQ(options.Validate().code(), absl::StatusCode::kInvalidArgument);
}

TEST(SpeechRecognizerOptionsTest, RejectsNegativeThreadCount) {
  SpeechRecognizerOptions options;
  options.inference_threads = -1;
  EXPECT_EQ(options.Validate().code(), absl::StatusCode::kInvalidArgument);
}

TEST(SpeechRecognizerOptionsTest, SileroThresholdOnlyCheckedWithModel) {
  // Out-of-range Silero threshold is ignored while Silero VAD is disabled.
  SpeechRecognizerOptions options;
  options.silero_threshold = 5.0f;
  EXPECT_TRUE(options.Validate().ok());

  // Once a VAD model is configured, the threshold must be a valid probability.
  options.vad_model = "/tmp/does-not-need-to-exist-for-option-validation";
  EXPECT_EQ(options.Validate().code(), absl::StatusCode::kInvalidArgument);
  options.silero_threshold = 0.5f;
  EXPECT_TRUE(options.Validate().ok());
}

}  // namespace
}  // namespace a11::sdk::audio
