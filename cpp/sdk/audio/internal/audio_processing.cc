// Copyright 2026 The A11 Authors.

#include "sdk/audio/internal/audio_processing.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>
#include <absl/types/span.h>
#include <cmath>

#include "sdk/audio/audio_buffer.h"

namespace a11::sdk::audio::internal {
namespace {

size_t SamplesForMillis(size_t millis) {
  return static_cast<size_t>(kWhisperSampleRate * static_cast<double>(millis) /
                             1000.0);
}

}  // namespace

StreamingMonoResampler::StreamingMonoResampler(double input_sample_rate,
                                               size_t input_channels,
                                               double output_sample_rate)
    : input_sample_rate_(input_sample_rate),
      input_channels_(input_channels),
      output_sample_rate_(output_sample_rate),
      input_step_(input_sample_rate / output_sample_rate) {}

absl::Status StreamingMonoResampler::Process(
    const AudioBuffer& buffer, std::vector<float>* absl_nonnull output) {
  if (input_sample_rate_ <= 0.0 || output_sample_rate_ <= 0.0 ||
      input_channels_ == 0) {
    return absl::FailedPreconditionError(
        "Resampler rates and channel count must be positive");
  }
  if (buffer.num_channels != input_channels_) {
    return absl::InvalidArgumentError(
        absl::StrCat("Audio buffer has ", buffer.num_channels,
                     " channels; expected ", input_channels_));
  }
  if (std::abs(buffer.sample_rate - input_sample_rate_) > 1e-6) {
    return absl::InvalidArgumentError(
        absl::StrCat("Audio buffer sample rate is ", buffer.sample_rate,
                     "; expected ", input_sample_rate_));
  }
  if (buffer.samples.size() != buffer.num_channels * buffer.num_frames) {
    return absl::InvalidArgumentError(
        "Audio buffer sample storage does not match its dimensions");
  }
  if (buffer.num_frames == 0) {
    return absl::OkStatus();
  }

  pending_.reserve(pending_.size() + buffer.num_frames);
  for (size_t frame = 0; frame < buffer.num_frames; ++frame) {
    double mono = 0.0;
    for (size_t channel = 0; channel < input_channels_; ++channel) {
      const float sample = buffer.samples[channel * buffer.num_frames + frame];
      mono += std::isfinite(sample) ? sample : 0.0f;
    }
    pending_.push_back(
        static_cast<float>(mono / static_cast<double>(input_channels_)));
  }

  if (pending_.size() < 2) {
    return absl::OkStatus();
  }
  const size_t estimate = static_cast<size_t>(
      static_cast<double>(buffer.num_frames) / input_step_ + 2.0);
  output->reserve(output->size() + estimate);
  while (cursor_ + 1.0 < static_cast<double>(pending_.size())) {
    const size_t left = static_cast<size_t>(cursor_);
    const float fraction =
        static_cast<float>(cursor_ - static_cast<double>(left));
    output->push_back(pending_[left] +
                      (pending_[left + 1] - pending_[left]) * fraction);
    cursor_ += input_step_;
  }

  // Retain one value before the cursor so interpolation remains continuous
  // across buffer boundaries. cursor_ may legitimately remain greater than 1
  // when downsampling and the next requested output lies in a future block.
  const size_t consumed =
      std::min(static_cast<size_t>(cursor_), pending_.size() - 1);
  if (consumed > 0) {
    pending_.erase(
        pending_.begin(),
        std::next(pending_.begin(), static_cast<std::ptrdiff_t>(consumed)));
    cursor_ -= static_cast<double>(consumed);
  }
  return absl::OkStatus();
}

void StreamingMonoResampler::Flush(std::vector<float>* absl_nonnull output) {
  if (!pending_.empty() && cursor_ < static_cast<double>(pending_.size())) {
    const size_t index =
        std::min(static_cast<size_t>(cursor_), pending_.size() - 1);
    output->push_back(pending_[index]);
  }
  pending_.clear();
  cursor_ = 0.0;
}

VoiceActivityDetector::VoiceActivityDetector(VoiceActivityOptions options)
    : options_(options),
      window_samples_(
          std::max<size_t>(1, SamplesForMillis(options.window_millis))),
      min_speech_samples_(SamplesForMillis(options.min_speech_millis)),
      min_silence_samples_(SamplesForMillis(options.min_silence_millis)),
      pad_samples_(SamplesForMillis(options.speech_pad_millis)),
      max_utterance_samples_(static_cast<size_t>(kWhisperSampleRate) *
                             options.max_speech_seconds),
      pre_roll_(pad_samples_) {
  utterance_.reserve(max_utterance_samples_ + 2 * pad_samples_);
}

void VoiceActivityDetector::Process(
    absl::Span<const float> samples,
    std::vector<std::vector<float>>* absl_nonnull completed) {
  pending_.insert(pending_.end(), samples.begin(), samples.end());
  size_t consumed = 0;
  while (pending_.size() - consumed >= window_samples_) {
    ProcessWindow(
        absl::MakeConstSpan(pending_).subspan(consumed, window_samples_),
        completed);
    consumed += window_samples_;
  }
  if (consumed > 0) {
    pending_.erase(
        pending_.begin(),
        std::next(pending_.begin(), static_cast<std::ptrdiff_t>(consumed)));
  }
}

void VoiceActivityDetector::ProcessWindow(
    absl::Span<const float> window,
    std::vector<std::vector<float>>* absl_nonnull completed) {
  double sum_squares = 0.0;
  for (float sample : window) {
    const float finite = std::isfinite(sample) ? sample : 0.0f;
    sum_squares += static_cast<double>(finite) * finite;
  }
  const float rms =
      window.empty() ? 0.0f
                     : static_cast<float>(std::sqrt(
                           sum_squares / static_cast<double>(window.size())));
  const float adaptive_threshold =
      noise_initialized_ ? noise_rms_ * options_.noise_ratio : 0.0f;
  const bool voiced =
      rms >= std::max(options_.energy_threshold, adaptive_threshold);

  if (!candidate_) {
    if (!voiced) {
      noise_rms_ = noise_initialized_ ? 0.95f * noise_rms_ + 0.05f * rms : rms;
      noise_initialized_ = true;
      AppendPreRoll(window);
      return;
    }

    candidate_ = true;
    CopyPreRoll(&utterance_);
    utterance_.insert(utterance_.end(), window.begin(), window.end());
    voiced_samples_ = window.size();
    trailing_silence_samples_ = 0;
  } else {
    utterance_.insert(utterance_.end(), window.begin(), window.end());
    if (voiced) {
      voiced_samples_ += window.size();
      trailing_silence_samples_ = 0;
    } else {
      trailing_silence_samples_ += window.size();
    }
  }

  const bool endpoint = trailing_silence_samples_ >= min_silence_samples_;
  const bool length_limit =
      utterance_.size() >= max_utterance_samples_ + 2 * pad_samples_;
  if (!endpoint && !length_limit) {
    return;
  }

  std::optional<std::vector<float>> finished = FinishCandidate();
  if (finished.has_value()) {
    completed->push_back(std::move(*finished));
  }
  // Preserve the latest analysis window as bounded context for an utterance
  // that begins immediately after this endpoint.
  AppendPreRoll(window);
}

void VoiceActivityDetector::AppendPreRoll(absl::Span<const float> samples) {
  if (pre_roll_.empty()) {
    return;
  }
  for (float sample : samples) {
    if (pre_roll_size_ < pre_roll_.size()) {
      pre_roll_[(pre_roll_start_ + pre_roll_size_) % pre_roll_.size()] = sample;
      ++pre_roll_size_;
    } else {
      pre_roll_[pre_roll_start_] = sample;
      pre_roll_start_ = (pre_roll_start_ + 1) % pre_roll_.size();
    }
  }
}

void VoiceActivityDetector::CopyPreRoll(
    std::vector<float>* absl_nonnull output) const {
  output->reserve(output->size() + pre_roll_size_);
  for (size_t index = 0; index < pre_roll_size_; ++index) {
    output->push_back(pre_roll_[(pre_roll_start_ + index) % pre_roll_.size()]);
  }
}

std::optional<std::vector<float>> VoiceActivityDetector::FinishCandidate() {
  if (!candidate_) {
    return std::nullopt;
  }
  if (trailing_silence_samples_ > pad_samples_) {
    const size_t trim = trailing_silence_samples_ - pad_samples_;
    if (trim < utterance_.size()) {
      utterance_.resize(utterance_.size() - trim);
    }
  }

  std::optional<std::vector<float>> result;
  if (voiced_samples_ >= min_speech_samples_) {
    result.emplace(std::move(utterance_));
  }
  ResetCandidate();
  return result;
}

void VoiceActivityDetector::ResetCandidate() {
  candidate_ = false;
  voiced_samples_ = 0;
  trailing_silence_samples_ = 0;
  utterance_.clear();
  utterance_.reserve(max_utterance_samples_ + 2 * pad_samples_);
}

std::optional<std::vector<float>> VoiceActivityDetector::Flush() {
  if (!pending_.empty()) {
    std::vector<std::vector<float>> completed;
    ProcessWindow(absl::MakeConstSpan(pending_), &completed);
    pending_.clear();
    if (!completed.empty()) {
      return std::move(completed.front());
    }
  }
  return FinishCandidate();
}

}  // namespace a11::sdk::audio::internal
