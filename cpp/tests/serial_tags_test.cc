// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The C++ half of the cross-language tag contract.
 *
 * `testdata/serial_tags.json` at the repository root is the one table every
 * language answers to. This test asserts the C++ constants against it, and that
 * the audio types -- the ones C++ actually serializes -- publish those constants
 * through their `A11SerialTag` customization point. A tag renamed in one
 * language and forgotten here fails a test rather than a conversation.
 */

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a11/data/serial_tags.h"
#include "a11/data/serializable.h"
#include "sdk/audio/actions/audio_serialization.h"

namespace a11::data {
namespace {

nlohmann::json SharedTagTable() {
  // <repo>/cpp/tests/serial_tags_test.cc -> <repo>/testdata/serial_tags.json
  const std::filesystem::path path =
      (std::filesystem::path(A11_CPP_SOURCE_ROOT).parent_path() /
       std::filesystem::path(__FILE__))
          .parent_path()
          .parent_path()
          .parent_path() /
      "testdata" / "serial_tags.json";
  std::ifstream stream(path);
  EXPECT_TRUE(stream.is_open()) << "cannot open " << path;
  std::stringstream buffer;
  buffer << stream.rdbuf();
  return nlohmann::json::parse(buffer.str());
}

std::vector<std::string> SharedTags() {
  // Bound to a named value: `items()` is a proxy that borrows the json, so
  // iterating a temporary walks freed memory.
  const nlohmann::json table = SharedTagTable();
  std::vector<std::string> result;
  for (const auto& [section, entries] : table.items()) {
    if (!section.empty() && section.front() == '_') continue;
    for (const auto& [name, tag] : entries.items()) {
      result.push_back(tag.get<std::string>());
    }
  }
  return result;
}

std::vector<std::string> CppTags() {
  return {
      std::string(kChunkMetadataTag),
      std::string(kChunkTag),
      std::string(kNodeRefTag),
      std::string(kNodeFragmentTag),
      std::string(kPortTag),
      std::string(kActionMessageTag),
      std::string(kWireMessageTag),
      std::string(kStatusTag),
      std::string(kTimeTag),
      std::string(kDurationTag),
      std::string(kInteractionTag),
      std::string(kPeerTag),
      std::string(kActionConfigTag),
      std::string(kUsageMetadataTag),
      std::string(kInteractWithClaudeConfigTag),
      std::string(kInteractWithGeminiConfigTag),
      std::string(kInteractWithOllamaConfigTag),
      std::string(kInteractWithGemmaConfigTag),
      std::string(kAudioBufferTag),
      std::string(kAudioInputOptionsTag),
      std::string(kSpeechRecognizerOptionsTag),
      std::string(kAudioDeviceInfoTag),
      std::string(kAudioControlEventTag),
      std::string(kAudioCaptureEventTag),
      std::string(kTranscriptionEventTag),
  };
}

TEST(SerialTagsTest, CppConstantsMatchTheSharedTable) {
  std::vector<std::string> shared = SharedTags();
  std::vector<std::string> ours = CppTags();
  std::sort(shared.begin(), shared.end());
  std::sort(ours.begin(), ours.end());
  EXPECT_EQ(shared, ours);
}

TEST(SerialTagsTest, AudioTypesPublishTheCanonicalTags) {
  using ::a11::sdk::audio::AudioBuffer;
  using ::a11::sdk::audio::AudioCaptureEvent;
  using ::a11::sdk::audio::AudioControlEvent;
  using ::a11::sdk::audio::AudioInputOptions;
  using ::a11::sdk::audio::DeviceInfo;
  using ::a11::sdk::audio::SpeechRecognizerOptions;
  using ::a11::sdk::audio::TranscriptionEvent;

  EXPECT_EQ(A11SerialTag(TypeTag<AudioBuffer>{}), kAudioBufferTag);
  EXPECT_EQ(A11SerialTag(TypeTag<AudioInputOptions>{}), kAudioInputOptionsTag);
  EXPECT_EQ(A11SerialTag(TypeTag<SpeechRecognizerOptions>{}),
            kSpeechRecognizerOptionsTag);
  EXPECT_EQ(A11SerialTag(TypeTag<DeviceInfo>{}), kAudioDeviceInfoTag);
  EXPECT_EQ(A11SerialTag(TypeTag<AudioControlEvent>{}), kAudioControlEventTag);
  EXPECT_EQ(A11SerialTag(TypeTag<AudioCaptureEvent>{}), kAudioCaptureEventTag);
  EXPECT_EQ(A11SerialTag(TypeTag<TranscriptionEvent>{}), kTranscriptionEventTag);
}

}  // namespace
}  // namespace a11::data
