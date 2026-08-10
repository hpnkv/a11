// Copyright 2026 The A11 Authors.

#include "sdk/audio/model_registry.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <absl/base/no_destructor.h>
#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/net/http/download.h"
#include "a11/net/http/fetch.h"

namespace a11::sdk::audio {
namespace {

/// Transcription models live in the whisper.cpp repository.
constexpr std::string_view kWhisperRoot =
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main";
/// The Silero VAD model lives in whisper-vad.
constexpr std::string_view kWhisperVadRoot =
    "https://huggingface.co/ggml-org/whisper-vad/resolve/main";

AudioModelSpec MakeSpec(std::string_view name, std::string_view sha1,
                        int size_mib, std::string_view root) {
  const std::string filename = absl::StrCat("ggml-", name, ".bin");
  return AudioModelSpec{.name = std::string(name),
                        .filename = filename,
                        .url = absl::StrCat(root, "/", filename),
                        .sha1 = std::string(sha1),
                        .size_mib = size_mib};
}

using ModelTable = std::vector<AudioModelSpec>;

/**
 * The transcription models, with their upstream published SHA-1s. Restricted to
 * tiny/base so that a first run, local inference, and wheel testing all stay
 * within reasonable time and disk.
 */
const ModelTable& AsrModels() {
  static const absl::NoDestructor<ModelTable> table(ModelTable{
      MakeSpec("tiny", "bd577a113a864445d4c299885e0cb97d4ba92b5f", 75,
               kWhisperRoot),
      MakeSpec("tiny.en", "c78c86eb1a8faa21b369bcd33207cc90d64ae9df", 75,
               kWhisperRoot),
      MakeSpec("base", "465707469ff3a37a2b9b8d8f89f2f99de7299dac", 142,
               kWhisperRoot),
      MakeSpec("base.en", "137c40403d78fd54d454da0f9bd998f78703390c", 142,
               kWhisperRoot),
  });
  return *table;
}

const ModelTable& VadModels() {
  static const absl::NoDestructor<ModelTable> table(ModelTable{
      MakeSpec("silero-v5.1.2", "a372f48dcf0bd9e4330eef2802bc46e061c19634", 1,
               kWhisperVadRoot),
  });
  return *table;
}

std::vector<std::string> NamesOf(const ModelTable& table) {
  std::vector<std::string> names;
  names.reserve(table.size());
  for (const AudioModelSpec& spec : table) {
    names.push_back(spec.name);
  }
  return names;
}

absl::StatusOr<AudioModelSpec> Lookup(const ModelTable& table,
                                      std::string_view shorthand) {
  for (const AudioModelSpec& spec : table) {
    if (spec.name == shorthand) {
      return spec;
    }
  }
  return absl::NotFoundError(
      absl::StrCat("unknown model shorthand: ", shorthand));
}

/**
 * Resolves one spec on the calling fiber. Shared by both entry points: the only
 * differences between ASR and VAD are the table and what an empty spec means,
 * and both are decided by the caller.
 */
absl::StatusOr<std::string> Resolve(const ModelTable& table, std::string spec,
                                    OnModelProgress on_progress) {
  // The shorthand table wins over the filesystem, so a file that happens to be
  // named "base.en" cannot shadow the model of that name.
  const absl::StatusOr<AudioModelSpec> known = Lookup(table, spec);
  if (!known.ok()) {
    std::error_code error;
    if (std::filesystem::is_regular_file(spec, error)) {
      return spec;
    }
    return absl::InvalidArgumentError(absl::StrCat(
        "model must be a path to an existing file or one of: ",
        absl::StrJoin(NamesOf(table), ", "), "; got: ", spec));
  }

  net::DownloadOptions options;
  options.destination = ModelCacheDir() / known->filename;
  options.expected_sha1 = known->sha1;
  if (on_progress) {
    options.on_progress = std::move(on_progress);
  }
  // Models are large and served through a CDN redirect, so the default fetch
  // timeout is raised well past what a single request needs.
  options.fetch.timeout = absl::Minutes(30);

  ABSL_ASSIGN_OR_RETURN(
      const std::filesystem::path path,
      net::Download(known->url, std::move(options)).Await());
  return path.string();
}

}  // namespace

std::vector<std::string> AsrModelShorthands() {
  return NamesOf(AsrModels());
}

std::vector<std::string> VadModelShorthands() {
  return NamesOf(VadModels());
}

absl::StatusOr<AudioModelSpec> LookupAsrModel(std::string_view shorthand) {
  return Lookup(AsrModels(), shorthand);
}

absl::StatusOr<AudioModelSpec> LookupVadModel(std::string_view shorthand) {
  return Lookup(VadModels(), shorthand);
}

std::filesystem::path ModelCacheDir() {
  const char* home = std::getenv("HOME");
  const std::filesystem::path root =
      (home != nullptr && *home != '\0')
          ? std::filesystem::path(home)
          : std::filesystem::temp_directory_path();
  return root / ".cache" / "a11" / "audio";
}

namespace internal {

absl::StatusOr<std::string> ResolveAsrModelBlocking(
    std::string spec, OnModelProgress on_progress) {
  if (spec.empty()) {
    spec = std::string(kDefaultAsrModel);
  }
  return Resolve(AsrModels(), std::move(spec), std::move(on_progress));
}

absl::StatusOr<std::string> ResolveVadModelBlocking(
    std::string spec, OnModelProgress on_progress) {
  // Empty means "no VAD", not "the default VAD": whisper.cpp only runs Silero
  // when it is given a model, and that stays the caller's choice.
  if (spec.empty()) {
    return std::string();
  }
  return Resolve(VadModels(), std::move(spec), std::move(on_progress));
}

}  // namespace internal

a11::Future<std::string> ResolveAsrModel(std::string spec,
                                         OnModelProgress on_progress) {
  return a11::Submit<std::string>(
      [spec = std::move(spec), on_progress = std::move(on_progress)]() mutable
      -> absl::StatusOr<std::string> {
        return internal::ResolveAsrModelBlocking(std::move(spec),
                                                 std::move(on_progress));
      });
}

a11::Future<std::string> ResolveVadModel(std::string spec,
                                         OnModelProgress on_progress) {
  return a11::Submit<std::string>(
      [spec = std::move(spec), on_progress = std::move(on_progress)]() mutable
      -> absl::StatusOr<std::string> {
        return internal::ResolveVadModelBlocking(std::move(spec),
                                                 std::move(on_progress));
      });
}

}  // namespace a11::sdk::audio
