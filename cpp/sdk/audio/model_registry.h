// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The catalogue of whisper.cpp and Silero models, and how a shorthand
 *        becomes a local file.
 *
 * `SpeechRecognizerOptions::model` and `::vad_model` accept either a filesystem
 * path or a shorthand -- `tiny`, `tiny.en`, `base`, `base.en`,
 * `silero-v5.1.2`. A shorthand is resolved against a shared cache directory and
 * downloaded, verified against its published SHA-1, if it is not there yet.
 *
 * This is the single definition of that table. It lives in C++ rather than in
 * the CLI because the action that needs it (`transcribe_audio`,
 * `capture_transcription`) runs wherever the gateway runs, which is not
 * necessarily where a Python CLI is; `a11.cli.voice` wraps these entry points
 * rather than keeping a second copy of the hashes.
 */

#ifndef A11_SDK_AUDIO_MODEL_REGISTRY_H_
#define A11_SDK_AUDIO_MODEL_REGISTRY_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/statusor.h>

#include "a11/concurrency/future.h"

namespace a11::sdk::audio {

/** @brief One downloadable model artifact. */
struct AudioModelSpec {
  std::string name;      ///< The shorthand it is known by.
  std::string filename;  ///< Cache filename, `ggml-<name>.bin`.
  std::string url;       ///< Where to fetch it from.
  std::string sha1;      ///< Published SHA-1, as lowercase hex.
  int size_mib = 0;      ///< Approximate size, for a progress message.
};

/// The shorthand used when a caller asks for transcription without naming a
/// model. Small enough that a first run is not a long wait.
constexpr std::string_view kDefaultAsrModel = "tiny.en";
/// whisper.cpp's Silero VAD model: the only VAD shorthand.
constexpr std::string_view kDefaultVadModel = "silero-v5.1.2";

/** @return The accepted transcription-model shorthands, in a stable order. */
std::vector<std::string> AsrModelShorthands();
/** @return The accepted VAD-model shorthands. */
std::vector<std::string> VadModelShorthands();

/** @return The spec for a transcription shorthand, or NotFound. */
absl::StatusOr<AudioModelSpec> LookupAsrModel(std::string_view shorthand);
/** @return The spec for a VAD shorthand, or NotFound. */
absl::StatusOr<AudioModelSpec> LookupVadModel(std::string_view shorthand);

/**
 * @brief The directory shorthand models are cached in.
 *
 * `$HOME/.cache/a11/audio`. Deliberately not `XDG_CACHE_HOME`-derived: this is
 * the path `a11.cli.voice` has always used, and a second spelling would
 * re-download every model a user already has.
 */
std::filesystem::path ModelCacheDir();

/** @brief Download progress, in bytes; @p total is 0 when it is not known. */
using OnModelProgress =
    std::function<void(std::uint64_t done, std::uint64_t total)>;

/**
 * @brief Resolves @p spec to a transcription-model file, downloading if needed.
 *
 * A shorthand is looked up before the filesystem is consulted, so a stray file
 * named `base.en` in the working directory cannot shadow the real model. An
 * empty @p spec means @c kDefaultAsrModel.
 *
 * @param spec A shorthand, or a path to an existing model file.
 * @param on_progress Optional progress callback for the download.
 * @return An awaitable resolving to the local path, or InvalidArgument naming
 *         the accepted shorthands when @p spec is neither.
 */
a11::Future<std::string> ResolveAsrModel(std::string spec,
                                         OnModelProgress on_progress = {});

/**
 * @brief Resolves @p spec to a VAD-model file, downloading if needed.
 *
 * As ResolveAsrModel, except that an empty @p spec resolves to the empty string
 * rather than a default: VAD is optional, and empty is how a caller says "no
 * VAD" (see @c SpeechRecognizerOptions::vad_model).
 */
a11::Future<std::string> ResolveVadModel(std::string spec,
                                         OnModelProgress on_progress = {});

namespace internal {

/**
 * @brief Blocking forms of the resolvers, for a caller already on a fiber.
 *
 * The action handlers are the callers that matter: they run inside an
 * a11::SubmitTask, and awaiting a nested a11::Submit from there does not
 * complete. Same reasoning as a11::net::internal::FetchBlocking.
 *
 * @warning Blocks. Never call these on the libuv loop thread.
 */
absl::StatusOr<std::string> ResolveAsrModelBlocking(
    std::string spec, OnModelProgress on_progress = {});
absl::StatusOr<std::string> ResolveVadModelBlocking(
    std::string spec, OnModelProgress on_progress = {});

}  // namespace internal

}  // namespace a11::sdk::audio

#endif  // A11_SDK_AUDIO_MODEL_REGISTRY_H_
