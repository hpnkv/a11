// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Local streaming automatic speech recognition backed by whisper.cpp.
 */

#ifndef A11_SDK_AUDIO_SPEECH_RECOGNIZER_H_
#define A11_SDK_AUDIO_SPEECH_RECOGNIZER_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/concurrency/future.h"
#include "sdk/audio/audio_input.h"

namespace a11::sdk::audio {

namespace internal {
struct SpeechRecognizerState;
}  // namespace internal

/**
 * @brief Configuration for local whisper.cpp transcription and endpointing.
 *
 * The model itself is supplied separately as a path to a whisper.cpp-compatible
 * GGML/GGUF file. Sentinel defaults choose a small CPU thread count and use the
 * best compiled hardware backend (Metal/Accelerate on macOS).
 *
 * Voice activity detection runs in two complementary stages. A cheap adaptive
 * energy gate (the @c vad_* / @c min_* / @c speech_pad / @c max_speech fields)
 * runs on the live stream to endpoint utterances and keep silent microphone
 * input off the decoder entirely. When a Silero VAD model is supplied via
 * @c vad_model_path, whisper.cpp's neural Silero VAD then runs inside inference
 * on each endpointed utterance, trimming residual non-speech and rejecting
 * energy false-positives (a door slam, a keyboard tap) before the decoder sees
 * them. Silero is the accurate detector; the energy gate is the cheap trigger.
 */
struct SpeechRecognizerOptions {
  /// Whisper language code, or "auto" to detect it per utterance.
  std::string language = "auto";
  /// Translate recognised speech to English instead of transcribing it.
  bool translate = false;
  /// Decoder worker count, or zero for a bounded hardware-derived default.
  int inference_threads = 0;
  /// Use a compiled GPU backend when one is available.
  bool use_gpu = true;
  /// Use whisper.cpp flash attention where the selected backend supports it.
  bool flash_attention = true;
  /// Carry decoder text context from one endpointed utterance to the next.
  bool use_context = false;
  /// Optional text prepended to the first decoder prompt.
  std::string initial_prompt;

  /// Duration of each internally-created AudioSubscription buffer.
  size_t subscription_buffer_millis = 100;
  /// Absolute RMS floor below which a window is silence.
  float vad_threshold = 0.01f;
  /// A window must also exceed this multiple of the learned noise floor.
  float vad_noise_ratio = 2.5f;
  /// RMS analysis window duration.
  size_t vad_window_millis = 20;
  /// Minimum voiced duration accepted as an utterance.
  size_t min_speech_millis = 250;
  /// Silence required to endpoint an utterance.
  size_t min_silence_millis = 600;
  /// Audio retained before speech and after its endpoint.
  size_t speech_pad_millis = 160;
  /// Hard bound that splits continuous speech and caps memory use.
  size_t max_speech_seconds = 30;

  /// Path to a whisper.cpp Silero VAD GGML model. When non-empty, Silero VAD
  /// filters each endpointed utterance before decoding; empty disables it and
  /// leaves only the energy gate. The temporal fields above are reused as
  /// Silero's segment bounds, so both stages share one set of durations.
  std::string vad_model_path;
  /// Silero speech-probability threshold, in (0, 1]. Frames scoring below this
  /// are treated as non-speech. Ignored when @c vad_model_path is empty.
  float silero_threshold = 0.5f;

  /// Validate every option and return a descriptive status on failure.
  absl::Status Validate() const;
};

/// Receives each non-empty whisper segment; nullopt is delivered exactly once
/// when that recognition run can produce no more pieces.
using OnTranscription =
    std::function<a11::Task(std::optional<std::string> piece)>;

/// Called exactly once after the terminal transcription marker has completed.
using OnRecognitionDone = std::function<a11::Task()>;

/**
 * @brief Restartable, callback-driven local automatic speech recognizer.
 *
 * Create() loads a whisper.cpp GGML/GGUF model once. Start() then captures and
 * endpoints speech on an A11 background fiber, invoking whisper only for VAD
 * accepted utterances. This keeps silent microphone input cheap. A recognizer
 * created from an AudioInput (or from no source, which opens the default input)
 * creates a fresh, reasonably-sized subscription for every run, so callers can
 * pause recognition while an agent is speaking without accumulating stale
 * audio. A directly supplied AudioSubscription is consumed and closed by its
 * single run.
 *
 * Start() follows WireStream's callback shape: it reports startup through its
 * returned Task while the run continues in the background. Stop() requests an
 * orderly stop and returns the run's completion Task. The piece callback is
 * awaited for backpressure, receives nullopt exactly once, and is followed by
 * the awaited done callback.
 */
class SpeechRecognizer : public std::enable_shared_from_this<SpeechRecognizer> {
 public:
  /// Load @p model_path and use the system's default audio input.
  static absl::StatusOr<std::shared_ptr<SpeechRecognizer>> Create(
      std::string model_path, SpeechRecognizerOptions options = {});

  /// Load @p model_path and create a fresh subscription on @p input per run.
  static absl::StatusOr<std::shared_ptr<SpeechRecognizer>> Create(
      std::string model_path, std::shared_ptr<AudioInput> input,
      SpeechRecognizerOptions options = {});

  /// Load @p model_path and consume @p subscription for one recognition run.
  static absl::StatusOr<std::shared_ptr<SpeechRecognizer>> Create(
      std::string model_path, std::shared_ptr<AudioSubscription> subscription,
      SpeechRecognizerOptions options = {});

  SpeechRecognizer(const SpeechRecognizer&) = delete;
  SpeechRecognizer& operator=(const SpeechRecognizer&) = delete;
  ~SpeechRecognizer();

  /**
   * @brief Start recognition and return once capture has been admitted.
   * @param on_transcription Awaited for each text piece and once with nullopt.
   * @param on_done Awaited once after the terminal piece callback.
   */
  a11::Task Start(OnTranscription on_transcription, OnRecognitionDone on_done);

  /// Request stop, close capture, and return the current run's completion Task.
  a11::Task Stop();

  [[nodiscard]] bool running() const;
  /// Current/final run status (OK before the first run and after a clean stop).
  [[nodiscard]] absl::Status GetStatus() const;
  [[nodiscard]] const std::string& model_path() const;
  [[nodiscard]] const SpeechRecognizerOptions& options() const;

 private:
  explicit SpeechRecognizer(
      std::shared_ptr<internal::SpeechRecognizerState> state);
  absl::Status Run(std::shared_ptr<AudioSubscription> subscription,
                   OnTranscription on_transcription, OnRecognitionDone on_done);

  std::shared_ptr<internal::SpeechRecognizerState> state_;
};

}  // namespace a11::sdk::audio

#endif  // A11_SDK_AUDIO_SPEECH_RECOGNIZER_H_
