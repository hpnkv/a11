// Copyright 2026 The A11 Authors.

#include <filesystem>
#include <fstream>
#include <string>

#include <absl/status/status.h>
#include <gtest/gtest.h>

#include "sdk/audio/model_registry.h"

namespace a11::sdk::audio {
namespace {

TEST(AudioModelRegistryTest, ListsTheAcceptedShorthands) {
  EXPECT_EQ(AsrModelShorthands(),
            (std::vector<std::string>{"tiny", "tiny.en", "base", "base.en"}));
  EXPECT_EQ(VadModelShorthands(), (std::vector<std::string>{"silero-v5.1.2"}));
  EXPECT_TRUE(LookupAsrModel(kDefaultAsrModel).ok());
  EXPECT_TRUE(LookupVadModel(kDefaultVadModel).ok());
}

TEST(AudioModelRegistryTest, DescribesEachArtifactCompletely) {
  const auto spec = LookupAsrModel("base.en");
  ASSERT_TRUE(spec.ok()) << spec.status();
  EXPECT_EQ(spec->name, "base.en");
  EXPECT_EQ(spec->filename, "ggml-base.en.bin");
  EXPECT_EQ(spec->url,
            "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/"
            "ggml-base.en.bin");
  // A 40-hex-character SHA-1 is what makes a cache entry trustworthy.
  EXPECT_EQ(spec->sha1.size(), 40u);
  EXPECT_GT(spec->size_mib, 0);

  // The VAD model comes from a different repository.
  const auto vad = LookupVadModel("silero-v5.1.2");
  ASSERT_TRUE(vad.ok()) << vad.status();
  EXPECT_EQ(vad->url,
            "https://huggingface.co/ggml-org/whisper-vad/resolve/main/"
            "ggml-silero-v5.1.2.bin");
}

TEST(AudioModelRegistryTest, RejectsAnUnknownShorthand) {
  EXPECT_EQ(LookupAsrModel("enormous.en").status().code(),
            absl::StatusCode::kNotFound);
  // The Silero model is not a transcription model, and vice versa.
  EXPECT_FALSE(LookupAsrModel("silero-v5.1.2").ok());
  EXPECT_FALSE(LookupVadModel("tiny.en").ok());
}

TEST(AudioModelRegistryTest, CachesUnderTheDirectoryTheCliHasAlwaysUsed) {
  const std::filesystem::path directory = ModelCacheDir();
  // Not XDG-derived: a second spelling would re-download every model a user
  // already has under ~/.cache/a11/audio.
  EXPECT_EQ(directory.filename(), "audio");
  EXPECT_EQ(directory.parent_path().filename(), "a11");
  EXPECT_EQ(directory.parent_path().parent_path().filename(), ".cache");
}

TEST(AudioModelRegistryTest, AnExistingFileResolvesToItself) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "a11-model-registry-test.bin";
  {
    std::ofstream out(path, std::ios::binary);
    out << "not really a model";
  }
  const auto resolved = ResolveAsrModel(path.string()).Await();
  ASSERT_TRUE(resolved.ok()) << resolved.status();
  EXPECT_EQ(*resolved, path.string());
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

TEST(AudioModelRegistryTest, AnUnresolvableSpecNamesTheShorthands) {
  const auto resolved = ResolveAsrModel("not-a-model-or-a-file").Await();
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.status().code(), absl::StatusCode::kInvalidArgument);
  // Actionable, because the field accepts free-form paths and so cannot be
  // validated against a fixed set up front.
  EXPECT_NE(resolved.status().message().find("tiny.en"),
            std::string_view::npos);
}

TEST(AudioModelRegistryTest, AnEmptyVadSpecMeansNoVad) {
  // Empty is how a caller says "no VAD", so it must not become the default
  // model and trigger a download nobody asked for.
  const auto resolved = ResolveVadModel("").Await();
  ASSERT_TRUE(resolved.ok()) << resolved.status();
  EXPECT_EQ(*resolved, "");
}

TEST(AudioModelRegistryTest, AShorthandBeatsASameNamedFile) {
  // A file named exactly like a shorthand must not shadow the real model: the
  // table is consulted first.
  const std::filesystem::path decoy =
      std::filesystem::current_path() / "tiny.en";
  if (std::filesystem::exists(decoy)) {
    GTEST_SKIP() << "a real tiny.en exists in the working directory";
  }
  {
    std::ofstream out(decoy, std::ios::binary);
    out << "decoy";
  }
  const auto resolved = ResolveAsrModel("tiny.en").Await();
  std::error_code ignored;
  std::filesystem::remove(decoy, ignored);

  // It either downloaded/found the cached model, or failed trying to reach the
  // network -- but it never resolved to the decoy beside it.
  if (resolved.ok()) {
    EXPECT_NE(*resolved, decoy.string());
    EXPECT_NE(resolved->find("ggml-tiny.en.bin"), std::string::npos);
  }
}

}  // namespace
}  // namespace a11::sdk::audio
