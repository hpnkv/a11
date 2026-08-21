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

#include "a11/data/serial_tags.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/escaping.h>
#include <absl/strings/str_cat.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a11/actions/log.h"
#include "a11/actions/schema.h"
#include "a11/data/serializable.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/status.h"
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
    if (!section.empty() && section.front() == '_') {
      continue;
    }
    // The other half of the fixture: media types, not tags. See
    // MediaTypesMatchTheFixture below.
    if (section == "media_types") {
      continue;
    }
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

nlohmann::json SharedStatusChunk() {
  const std::filesystem::path path =
      (std::filesystem::path(A11_CPP_SOURCE_ROOT).parent_path() /
       std::filesystem::path(__FILE__))
          .parent_path()
          .parent_path()
          .parent_path() /
      "testdata" / "status_chunk.json";
  std::ifstream stream(path);
  EXPECT_TRUE(stream.is_open()) << "cannot open " << path;
  std::stringstream buffer;
  buffer << stream.rdbuf();
  return nlohmann::json::parse(buffer.str());
}

TEST(SerialTagsTest, StatusChunksMatchTheSharedShape) {
  const nlohmann::json fixture = SharedStatusChunk();
  EXPECT_EQ(fixture["mimetype"].get<std::string>(), kStatusMimetype);
  EXPECT_EQ(fixture["close_attribute"].get<std::string>(), kCloseAttribute);

  for (const nlohmann::json& test_case : fixture["cases"]) {
    const std::string name = test_case["name"].get<std::string>();
    const absl::Status status = MakeStatus(
        static_cast<absl::StatusCode>(test_case["code"].get<int>()),
        test_case["message"].get<std::string>(), test_case["details"]);

    const absl::StatusOr<Chunk> chunk = MakeStatusChunk(status);
    ASSERT_TRUE(chunk.ok()) << name << ": " << chunk.status();
    EXPECT_EQ(chunk->GetMimetype(), fixture["mimetype"].get<std::string>())
        << name;
    EXPECT_EQ(absl::Base64Escape(chunk->data),
              test_case["base64"].get<std::string>())
        << name;
    EXPECT_FALSE(IsCloseStatusChunk(*chunk)) << name;

    const absl::StatusOr<absl::Status> decoded = StatusFromStatusChunk(*chunk);
    ASSERT_TRUE(decoded.ok()) << name << ": " << decoded.status();
    EXPECT_EQ(decoded->code(), status.code()) << name;
    EXPECT_EQ(decoded->message(), status.message()) << name;
    EXPECT_EQ(StatusDetails(*decoded), StatusDetails(status)) << name;
  }
}

TEST(SerialTagsTest, AClosureMarkerOnlyAddsTheSharedAttribute) {
  const nlohmann::json fixture = SharedStatusChunk();
  const absl::StatusOr<Chunk> plain = MakeStatusChunk(absl::OkStatus());
  const absl::StatusOr<Chunk> marker = MakeStatusChunk(absl::OkStatus(), true);
  ASSERT_TRUE(plain.ok()) << plain.status();
  ASSERT_TRUE(marker.ok()) << marker.status();

  // The marker rides on the metadata, so the payload is the plain status.
  EXPECT_EQ(marker->data, plain->data);
  EXPECT_EQ(marker->GetMimetype(), plain->GetMimetype());
  EXPECT_TRUE(IsCloseStatusChunk(*marker));
  ASSERT_TRUE(marker->metadata.has_value());
  const ByteMap expected = {
      {fixture["close_attribute"].get<std::string>(), "1"}};
  EXPECT_EQ(marker->metadata->attributes, expected);
}

TEST(SerialTagsTest, AGenericPayloadCarriesNoTypeParameter) {
  SerializationRegistry registry(/*register_defaults=*/true);
  const nlohmann::json value = {{"answer", 42}};

  const absl::StatusOr<Chunk> chunk = registry.ToChunk(value);
  ASSERT_TRUE(chunk.ok()) << chunk.status();
  // JSON already says this is an object; the mimetype does not repeat it.
  EXPECT_EQ(chunk->GetMimetype(), kJsonMimetype);

  const absl::StatusOr<nlohmann::json> decoded =
      registry.FromChunk<nlohmann::json>(*chunk);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(*decoded, value);
}

TEST(SerialTagsTest, ABareMimetypeIsACompleteDescription) {
  SerializationRegistry registry(/*register_defaults=*/true);
  Chunk chunk;
  chunk.metadata = ChunkMetadata{.mimetype = std::string(kJsonMimetype)};
  chunk.data = R"({"answer":42})";

  const absl::StatusOr<nlohmann::json> decoded =
      registry.FromChunk<nlohmann::json>(chunk);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ((*decoded)["answer"].get<int>(), 42);
}

TEST(SerialTagsTest, ATaggedPayloadIsStillReadableAsPlainJson) {
  // The type parameter names what the payload *is*, not what a reader is
  // allowed to ask for. A peer that never loaded the naming module still holds
  // valid JSON, and is entitled to read it as such.
  SerializationRegistry registry(/*register_defaults=*/true);
  Chunk chunk;
  chunk.metadata = ChunkMetadata{
      .mimetype = absl::StrCat(kJsonMimetype, ";type=", kInteractionTag)};
  chunk.data = R"({"model":"golden-model"})";

  const absl::StatusOr<nlohmann::json> decoded =
      registry.FromChunk<nlohmann::json>(chunk);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ((*decoded)["model"].get<std::string>(), "golden-model");
}

TEST(SerialTagsTest, RuntimeTypesPublishTheCanonicalTags) {
  EXPECT_EQ(A11SerialTag(TypeTag<ChunkMetadata>{}), kChunkMetadataTag);
  EXPECT_EQ(A11SerialTag(TypeTag<Chunk>{}), kChunkTag);
  EXPECT_EQ(A11SerialTag(TypeTag<NodeRef>{}), kNodeRefTag);
  EXPECT_EQ(A11SerialTag(TypeTag<NodeFragment>{}), kNodeFragmentTag);
  EXPECT_EQ(A11SerialTag(TypeTag<Port>{}), kPortTag);
  EXPECT_EQ(A11SerialTag(TypeTag<ActionMessage>{}), kActionMessageTag);
  EXPECT_EQ(A11SerialTag(TypeTag<WireMessage>{}), kWireMessageTag);
}

TEST(SerialTagsTest, ARuntimeTypeIsTaggedForEveryLanguageToRead) {
  // The tag C++ writes comes from the shared table, so a registry in any
  // language can resolve it.
  SerializationRegistry registry(/*register_defaults=*/true);
  Chunk value;
  value.metadata = ChunkMetadata{.mimetype = std::string(kJsonMimetype)};
  value.data = R"({"answer":42})";

  const absl::StatusOr<Chunk> chunk = registry.ToChunk(value);
  ASSERT_TRUE(chunk.ok()) << chunk.status();
  EXPECT_EQ(chunk->GetMimetype(),
            absl::StrCat(kMsgpackMimetype, ";type=", kChunkTag));

  const absl::StatusOr<Chunk> decoded = registry.FromChunk<Chunk>(*chunk);
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(decoded->data, value.data);
  EXPECT_EQ(decoded->GetMimetype(), value.GetMimetype());
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
  EXPECT_EQ(A11SerialTag(TypeTag<TranscriptionEvent>{}),
            kTranscriptionEventTag);
}

TEST(SerialTagsTest, MediaTypesMatchTheFixture) {
  // Pinned across languages exactly as the tags are. `text` and `bytes` are the
  // ones that matter: they are the defaults for a string and a byte array in the
  // languages that tell those apart, and a chunk using either carries no `type`
  // parameter, so the media type alone is what a peer has to go on.
  const nlohmann::json table = SharedTagTable();
  const nlohmann::json& media_types = table.at("media_types");

  EXPECT_EQ(media_types.at("json").get<std::string>(), kJsonMimetype);
  EXPECT_EQ(media_types.at("msgpack").get<std::string>(), kMsgpackMimetype);
  EXPECT_EQ(media_types.at("text").get<std::string>(), kTextMimetype);
  EXPECT_EQ(media_types.at("bytes").get<std::string>(), kBytesMimetype);
}

TEST(SerialTagsTest, AStdStringDefaultsToThePinnedBytesMediaType) {
  // C++ has no text-versus-bytes distinction in its type system, so this is the
  // one language whose string type defaults to bytes; text/plain is available by
  // naming it. See cpp/tests/string_codec_test.cc.
  const nlohmann::json media_types = SharedTagTable().at("media_types");
  absl::StatusOr<Chunk> chunk =
      GlobalSerializationRegistry().ToChunk<std::string>("value");

  ASSERT_TRUE(chunk.ok()) << chunk.status();
  ASSERT_TRUE(chunk->metadata.has_value());
  EXPECT_EQ(chunk->metadata->mimetype,
            media_types.at("bytes").get<std::string>());
  EXPECT_EQ(chunk->data, "value");
}

nlohmann::json SharedLogChunk() {
  const std::filesystem::path path =
      (std::filesystem::path(A11_CPP_SOURCE_ROOT).parent_path() /
       std::filesystem::path(__FILE__))
          .parent_path()
          .parent_path()
          .parent_path() /
      "testdata" / "log_chunk.json";
  std::ifstream stream(path);
  EXPECT_TRUE(stream.is_open()) << "cannot open " << path;
  std::stringstream buffer;
  buffer << stream.rdbuf();
  return nlohmann::json::parse(buffer.str());
}

TEST(LogChunkTest, TheReservedPortAndItsMetadataMatchTheFixture) {
  // The log port and its attribute names are a cross-language contract: a peer
  // in another language reads these chunks, so the words have to be the same
  // words. Pinned here beside the status chunk for the same reason.
  const nlohmann::json fixture = SharedLogChunk();
  EXPECT_EQ(fixture.at("port").get<std::string>(), actions::kActionLogOutput);

  const nlohmann::json& attributes = fixture.at("attributes");
  EXPECT_EQ(attributes.at("level").get<std::string>(),
            actions::kLogLevelAttribute);
  EXPECT_EQ(attributes.at("internal").get<std::string>(),
            actions::kLogInternalAttribute);
  EXPECT_EQ(attributes.at("channel").get<std::string>(),
            actions::kLogChannelAttribute);
  EXPECT_EQ(attributes.at("file").get<std::string>(),
            actions::kLogFileAttribute);
  EXPECT_EQ(attributes.at("lineno").get<std::string>(),
            actions::kLogLinenoAttribute);
  EXPECT_EQ(fixture.at("internal_true").get<std::string>(),
            actions::kLogInternalTrue);
  EXPECT_EQ(fixture.at("internal_false").get<std::string>(),
            actions::kLogInternalFalse);

  std::vector<std::string> levels;
  for (const nlohmann::json& level : fixture.at("levels")) {
    levels.push_back(level.get<std::string>());
  }
  EXPECT_EQ(levels, (std::vector<std::string>{"debug", "info", "warning",
                                              "error", "critical"}));
  for (const std::string& name : levels) {
    const absl::StatusOr<actions::LogLevel> parsed =
        actions::ParseLogLevel(name);
    ASSERT_TRUE(parsed.ok()) << name << ": " << parsed.status();
    EXPECT_EQ(actions::LogLevelName(*parsed), name);
  }
  EXPECT_EQ(actions::LogLevelName(actions::kDefaultLogLevel),
            fixture.at("default_level").get<std::string>());

  // Both spellings host languages differ on resolve to the canonical name.
  for (const auto& [written, meant] : fixture.at("level_aliases").items()) {
    const absl::StatusOr<actions::LogLevel> parsed =
        actions::ParseLogLevel(written);
    ASSERT_TRUE(parsed.ok()) << written << ": " << parsed.status();
    EXPECT_EQ(actions::LogLevelName(*parsed), meant.get<std::string>());
  }
}

}  // namespace
}  // namespace a11::data
