// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Streaming sample-rate conversion and lightweight speech endpointing.
 */

#ifndef A11_SDK_AUDIO_INTERNAL_AUDIO_PROCESSING_H_
#define A11_SDK_AUDIO_INTERNAL_AUDIO_PROCESSING_H_

#include <cstddef>
#include <optional>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/types/span.h>

#include "sdk/audio/audio_buffer.h"

namespace a11::sdk::audio::internal {

/// Whisper's required PCM sample rate.
inline constexpr double kWhisperSampleRate = 16000.0;

/**
 * @brief Stateful planar-to-mono linear resampler for streaming capture.
 *
 * The fractional input cursor is preserved between AudioBuffer values, which
 * avoids the repeated rounding drift caused by resampling every callback block
 * independently. The retained input is bounded to the unconsumed interpolation
 * tail plus the newest block.
 */
class StreamingMonoResampler {
 public:
  StreamingMonoResampler(double input_sample_rate, size_t input_channels,
                         double output_sample_rate = kWhisperSampleRate);

  StreamingMonoResampler(const StreamingMonoResampler&) = delete;
  StreamingMonoResampler& operator=(const StreamingMonoResampler&) = delete;

  /// Downmix and append resampled values from @p buffer to @p output.
  absl::Status Process(const AudioBuffer& buffer,
                       std::vector<float>* absl_nonnull output);

  /// Emit the final sample, when due, and clear retained interpolation state.
  void Flush(std::vector<float>* absl_nonnull output);

 private:
  const double input_sample_rate_;
  const size_t input_channels_;
  const double output_sample_rate_;
  const double input_step_;
  std::vector<float> pending_;
  double cursor_ = 0.0;
};

/// Configuration for the low-cost energy VAD used ahead of whisper inference.
struct VoiceActivityOptions {
  float energy_threshold = 0.01f;
  float noise_ratio = 2.5f;
  size_t window_millis = 20;
  size_t min_speech_millis = 250;
  size_t min_silence_millis = 600;
  size_t speech_pad_millis = 160;
  size_t max_speech_seconds = 30;
};

/**
 * @brief Adaptive energy VAD that emits bounded, endpointed utterances.
 *
 * Samples are analysed in short RMS windows. While idle, the detector learns a
 * conservative noise floor and retains a bounded pre-roll. Once speech begins,
 * it accumulates until enough silence is seen or the maximum utterance length
 * is reached. No inference is needed while the detector remains idle.
 */
class VoiceActivityDetector {
 public:
  explicit VoiceActivityDetector(VoiceActivityOptions options);

  VoiceActivityDetector(const VoiceActivityDetector&) = delete;
  VoiceActivityDetector& operator=(const VoiceActivityDetector&) = delete;

  /// Append every newly endpointed utterance to @p completed.
  void Process(absl::Span<const float> samples,
               std::vector<std::vector<float>>* absl_nonnull completed);

  /// Endpoint a final in-progress utterance, if it contains enough speech.
  std::optional<std::vector<float>> Flush();

  [[nodiscard]] bool active() const { return candidate_; }

 private:
  void ProcessWindow(absl::Span<const float> window,
                     std::vector<std::vector<float>>* absl_nonnull completed);
  void AppendPreRoll(absl::Span<const float> samples);
  void CopyPreRoll(std::vector<float>* absl_nonnull output) const;
  std::optional<std::vector<float>> FinishCandidate();
  void ResetCandidate();

  const VoiceActivityOptions options_;
  const size_t window_samples_;
  const size_t min_speech_samples_;
  const size_t min_silence_samples_;
  const size_t pad_samples_;
  const size_t max_utterance_samples_;

  std::vector<float> pending_;
  std::vector<float> pre_roll_;
  size_t pre_roll_start_ = 0;
  size_t pre_roll_size_ = 0;

  bool candidate_ = false;
  size_t voiced_samples_ = 0;
  size_t trailing_silence_samples_ = 0;
  std::vector<float> utterance_;

  bool noise_initialized_ = false;
  float noise_rms_ = 0.0f;
};

}  // namespace a11::sdk::audio::internal

#endif  // A11_SDK_AUDIO_INTERNAL_AUDIO_PROCESSING_H_
