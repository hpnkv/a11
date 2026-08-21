// Copyright 2026 The A11 Authors.

#include "sdk/audio/speech_recognizer.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/log/log.h>
#include <absl/log/vlog_is_on.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>
#include <cmath>
#include <whisper.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "sdk/audio/audio_buffer.h"
#include "sdk/audio/audio_input.h"
#include "sdk/audio/internal/audio_processing.h"
#include "thread/boost_primitives.h"

namespace a11::sdk::audio {
namespace {

using internal::StreamingMonoResampler;
using internal::VoiceActivityDetector;
using internal::VoiceActivityOptions;

struct WhisperContextDeleter {
  void operator()(whisper_context* context) const {
    if (context != nullptr) {
      whisper_free(context);
    }
  }
};

using WhisperContext = std::unique_ptr<whisper_context, WhisperContextDeleter>;

void WhisperLogCallback(ggml_log_level level, const char* text,
                        void* /*user_data*/) {
  if (text == nullptr) {
    return;
  }
  std::string message(text);
  while (!message.empty() &&
         (message.back() == '\n' || message.back() == '\r')) {
    message.pop_back();
  }
  if (message.empty()) {
    return;
  }
  switch (level) {
    case GGML_LOG_LEVEL_ERROR:
      LOG(ERROR) << "whisper.cpp: " << message;
      break;
    case GGML_LOG_LEVEL_WARN:
      LOG(WARNING) << "whisper.cpp: " << message;
      break;
    default:
      if (VLOG_IS_ON(1)) {
        LOG(INFO) << "whisper.cpp: " << message;
      }
      break;
  }
}

int DefaultInferenceThreads() {
  const unsigned int available = std::thread::hardware_concurrency();
  if (available == 0) {
    return 4;
  }
  return static_cast<int>(std::min(available, 4u));
}

absl::Status CallbackStatus(OnTranscription& callback,
                            std::optional<std::string> piece) {
  try {
    a11::Task task = callback(std::move(piece));
    if (!task.valid()) {
      return absl::FailedPreconditionError(
          "Transcription callback returned an invalid Task");
    }
    return task.Await().status();
  } catch (const std::exception& error) {
    return absl::UnknownError(error.what());
  } catch (...) {
    return absl::UnknownError(
        "Transcription callback raised a non-standard exception");
  }
}

absl::Status CallbackStatus(OnRecognitionDone& callback) {
  try {
    a11::Task task = callback();
    if (!task.valid()) {
      return absl::FailedPreconditionError(
          "Recognition done callback returned an invalid Task");
    }
    return task.Await().status();
  } catch (const std::exception& error) {
    return absl::UnknownError(error.what());
  } catch (...) {
    return absl::UnknownError(
        "Recognition done callback raised a non-standard exception");
  }
}

}  // namespace

namespace internal {

struct SpeechRecognizerState {
  SpeechRecognizerState(std::string model, SpeechRecognizerOptions options,
                        WhisperContext context,
                        std::shared_ptr<AudioInput> input,
                        std::shared_ptr<AudioSubscription> subscription)
      : model(std::move(model)),
        options(std::move(options)),
        context(std::move(context)),
        input(std::move(input)),
        supplied_subscription(std::move(subscription)) {}

  const std::string model;
  const SpeechRecognizerOptions options;
  WhisperContext context;
  const std::shared_ptr<AudioInput> input;
  const std::shared_ptr<AudioSubscription> supplied_subscription;

  mutable thread::Mutex mu;
  bool running ABSL_GUARDED_BY(mu) = false;
  bool supplied_subscription_used ABSL_GUARDED_BY(mu) = false;
  absl::Status status ABSL_GUARDED_BY(mu);
  a11::Task worker ABSL_GUARDED_BY(mu);
  a11::Future<AudioBuffer> pending_read ABSL_GUARDED_BY(mu);
  // Releases the current run's audio source (closes the subscription, or the
  // caller's reader). Invoked by Stop() and after the run finishes.
  std::function<void()> active_close ABSL_GUARDED_BY(mu);
  std::atomic<bool> stopping = false;
};

}  // namespace internal

namespace {

bool ShouldAbortInference(void* user_data) {
  const auto* state =
      static_cast<const internal::SpeechRecognizerState*>(user_data);
  return state->stopping.load(std::memory_order_relaxed);
}

absl::StatusOr<std::shared_ptr<internal::SpeechRecognizerState>> LoadState(
    std::string model, SpeechRecognizerOptions options,
    std::shared_ptr<AudioInput> input,
    std::shared_ptr<AudioSubscription> subscription) {
  ABSL_RETURN_IF_ERROR(options.Validate());
  if (model.empty()) {
    return absl::InvalidArgumentError("model must not be empty");
  }
  std::error_code file_error;
  if (!std::filesystem::is_regular_file(model, file_error)) {
    return absl::NotFoundError(
        absl::StrCat("Whisper model file was not found: ", model));
  }
  if (!options.vad_model.empty() &&
      !std::filesystem::is_regular_file(options.vad_model, file_error)) {
    return absl::NotFoundError(absl::StrCat(
        "Silero VAD model file was not found: ", options.vad_model));
  }
  if (options.language != "auto" && !options.language.empty() &&
      whisper_lang_id(options.language.c_str()) < 0) {
    return absl::InvalidArgumentError(
        absl::StrCat("Unsupported Whisper language: ", options.language));
  }

  whisper_log_set(&WhisperLogCallback, nullptr);
  whisper_context_params context_params = whisper_context_default_params();
  context_params.use_gpu = options.use_gpu;
  context_params.flash_attn = options.flash_attention;
  WhisperContext context(
      whisper_init_from_file_with_params(model.c_str(), context_params));
  if (context == nullptr) {
    return absl::InvalidArgumentError(
        absl::StrCat("whisper.cpp could not load model file: ", model));
  }

  return std::make_shared<internal::SpeechRecognizerState>(
      std::move(model), std::move(options), std::move(context),
      std::move(input), std::move(subscription));
}

absl::Status TranscribeUtterance(
    internal::SpeechRecognizerState* state, std::vector<float> utterance,
    OnTranscription* absl_nonnull on_transcription) {
  if (state->stopping.load(std::memory_order_relaxed)) {
    return absl::OkStatus();
  }
  // Very short endpointed phrases are valid, but whisper's encoder expects
  // enough context to form a mel window. Zero-padding is cheaper and more
  // predictable than asking callers to raise their VAD minimum.
  utterance.resize(std::max<size_t>(utterance.size(), WHISPER_SAMPLE_RATE),
                   0.0f);
  if (utterance.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return absl::ResourceExhaustedError(
        "Endpointed utterance exceeds whisper.cpp's sample limit");
  }

  whisper_full_params params =
      whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  params.n_threads = state->options.inference_threads > 0
                         ? state->options.inference_threads
                         : DefaultInferenceThreads();
  params.translate = state->options.translate;
  params.no_context = !state->options.use_context;
  params.no_timestamps = true;
  params.single_segment = false;
  params.print_special = false;
  params.print_progress = false;
  params.print_realtime = false;
  params.print_timestamps = false;
  params.suppress_blank = true;
  params.suppress_nst = true;
  params.initial_prompt = state->options.initial_prompt.empty()
                              ? nullptr
                              : state->options.initial_prompt.c_str();
  // A null language already makes whisper_full auto-detect before it decodes.
  // detect_language is the separate "detect and return" switch: it makes
  // whisper_full stop right after detection and yield zero segments, so it must
  // stay false here or transcription silently produces nothing.
  params.language = state->options.language == "auto"
                        ? nullptr
                        : state->options.language.c_str();
  params.detect_language = false;
  params.abort_callback = &ShouldAbortInference;
  params.abort_callback_user_data = state;

  // Silero VAD, when a model is configured, runs inside whisper_full: it filters
  // the endpointed utterance down to its speech frames before decoding, and
  // returns zero segments (hence no transcription) for a false-positive that the
  // energy gate let through. whisper.cpp loads the model once and caches it in
  // its state, so this stays cheap across utterances. The temporal options are
  // shared with the energy gate; whisper's own defaults fill the rest.
  params.vad = !state->options.vad_model.empty();
  if (params.vad) {
    params.vad_model_path = state->options.vad_model.c_str();
    params.vad_params = whisper_vad_default_params();
    params.vad_params.threshold = state->options.silero_threshold;
    params.vad_params.min_speech_duration_ms =
        static_cast<int>(state->options.min_speech_millis);
    params.vad_params.min_silence_duration_ms =
        static_cast<int>(state->options.min_silence_millis);
    params.vad_params.speech_pad_ms =
        static_cast<int>(state->options.speech_pad_millis);
    params.vad_params.max_speech_duration_s =
        static_cast<float>(state->options.max_speech_seconds);
  }

  const int result =
      whisper_full(state->context.get(), params, utterance.data(),
                   static_cast<int>(utterance.size()));
  if (result != 0) {
    if (state->stopping.load(std::memory_order_relaxed)) {
      return absl::OkStatus();
    }
    return absl::InternalError(
        absl::StrCat("whisper_full failed with code ", result));
  }
  if (state->stopping.load(std::memory_order_relaxed)) {
    return absl::OkStatus();
  }

  const int segment_count = whisper_full_n_segments(state->context.get());
  for (int index = 0; index < segment_count; ++index) {
    const char* raw_text =
        whisper_full_get_segment_text(state->context.get(), index);
    if (raw_text == nullptr) {
      continue;
    }
    const std::string_view trimmed =
        absl::StripAsciiWhitespace(std::string_view(raw_text));
    if (trimmed.empty()) {
      continue;
    }
    if (state->stopping.load(std::memory_order_relaxed)) {
      return absl::OkStatus();
    }
    ABSL_RETURN_IF_ERROR(
        CallbackStatus(*on_transcription, std::string(trimmed)));
  }
  return absl::OkStatus();
}

absl::Status RecognitionLoop(internal::SpeechRecognizerState* state,
                             const AudioBufferReader& reader,
                             absl::Duration pause_after,
                             OnTranscription* absl_nonnull on_transcription) {
  // The resampler is built from the first block, so a caller-supplied stream
  // does not need to declare its rate and channel count up front (a device
  // subscription's blocks simply carry its fixed values).
  std::optional<StreamingMonoResampler> resampler;
  VoiceActivityDetector vad(VoiceActivityOptions{
      .energy_threshold = state->options.vad_threshold,
      .noise_ratio = state->options.vad_noise_ratio,
      .window_millis = state->options.vad_window_millis,
      .min_speech_millis = state->options.min_speech_millis,
      .min_silence_millis = state->options.min_silence_millis,
      .speech_pad_millis = state->options.speech_pad_millis,
      .max_speech_seconds = state->options.max_speech_seconds,
  });
  const bool finite_pause = pause_after < absl::InfiniteDuration();

  // Endpoints and transcribes whatever speech the VAD currently holds; used
  // both to drain a stalled stream (pause) and to finish the stream.
  const auto flush_pending = [&]() -> absl::Status {
    std::optional<std::vector<float>> utterance = vad.Flush();
    if (utterance.has_value()) {
      return TranscribeUtterance(state, std::move(*utterance),
                                 on_transcription);
    }
    return absl::OkStatus();
  };

  bool natural_end = false;
  a11::Future<AudioBuffer> read = reader();
  while (!state->stopping.load(std::memory_order_relaxed)) {
    {
      thread::MutexLock lock(&state->mu);
      state->pending_read = read;
    }
    if (state->stopping.load(std::memory_order_relaxed)) {
      (void)read.Cancel();
    }
    absl::StatusOr<AudioBuffer> buffer =
        finite_pause ? read.Await(absl::Now() + pause_after) : read.Await();
    if (!buffer.ok()) {
      const absl::StatusCode code = buffer.status().code();
      // A pause timeout does not end the run: endpoint what we have and keep
      // awaiting the same (still-pending) read for more audio.
      if (finite_pause && code == absl::StatusCode::kDeadlineExceeded &&
          !state->stopping.load(std::memory_order_relaxed)) {
        ABSL_RETURN_IF_ERROR(flush_pending());
        continue;
      }
      {
        thread::MutexLock lock(&state->mu);
        state->pending_read = a11::Future<AudioBuffer>();
      }
      if (state->stopping.load(std::memory_order_relaxed) ||
          code == absl::StatusCode::kCancelled) {
        break;
      }
      if (code == absl::StatusCode::kOutOfRange) {
        natural_end = true;
        break;
      }
      return buffer.status();
    }
    {
      thread::MutexLock lock(&state->mu);
      state->pending_read = a11::Future<AudioBuffer>();
    }

    if (!resampler.has_value()) {
      resampler.emplace(buffer->sample_rate,
                        static_cast<size_t>(buffer->num_channels));
    }
    std::vector<float> mono;
    ABSL_RETURN_IF_ERROR(resampler->Process(*buffer, &mono));
    std::vector<std::vector<float>> utterances;
    vad.Process(mono, &utterances);
    for (std::vector<float>& utterance : utterances) {
      ABSL_RETURN_IF_ERROR(
          TranscribeUtterance(state, std::move(utterance), on_transcription));
    }
    read = reader();
  }

  if (natural_end && !state->stopping.load(std::memory_order_relaxed)) {
    if (resampler.has_value()) {
      std::vector<float> tail;
      resampler->Flush(&tail);
      std::vector<std::vector<float>> utterances;
      vad.Process(tail, &utterances);
      for (std::vector<float>& utterance : utterances) {
        ABSL_RETURN_IF_ERROR(
            TranscribeUtterance(state, std::move(utterance), on_transcription));
      }
    }
    ABSL_RETURN_IF_ERROR(flush_pending());
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status SpeechRecognizerOptions::Validate() const {
  if (language.empty()) {
    return absl::InvalidArgumentError(
        "language must be a Whisper language code or 'auto'");
  }
  if (inference_threads < 0) {
    return absl::InvalidArgumentError("inference_threads must not be negative");
  }
  if (subscription_buffer_millis < 20 || subscription_buffer_millis > 1000) {
    return absl::InvalidArgumentError(
        "subscription_buffer_millis must be in [20, 1000]");
  }
  if (!std::isfinite(vad_threshold) || vad_threshold <= 0.0f ||
      vad_threshold > 1.0f) {
    return absl::InvalidArgumentError("vad_threshold must be in (0, 1]");
  }
  if (!std::isfinite(vad_noise_ratio) || vad_noise_ratio < 1.0f ||
      vad_noise_ratio > 100.0f) {
    return absl::InvalidArgumentError("vad_noise_ratio must be in [1, 100]");
  }
  if (vad_window_millis < 10 || vad_window_millis > 100) {
    return absl::InvalidArgumentError("vad_window_millis must be in [10, 100]");
  }
  if (min_speech_millis < vad_window_millis) {
    return absl::InvalidArgumentError(
        "min_speech_millis must be at least vad_window_millis");
  }
  if (min_silence_millis < vad_window_millis) {
    return absl::InvalidArgumentError(
        "min_silence_millis must be at least vad_window_millis");
  }
  if (speech_pad_millis > min_silence_millis) {
    return absl::InvalidArgumentError(
        "speech_pad_millis must not exceed min_silence_millis");
  }
  if (max_speech_seconds == 0 || max_speech_seconds > 60) {
    return absl::InvalidArgumentError("max_speech_seconds must be in [1, 60]");
  }
  const size_t max_speech_millis = max_speech_seconds * 1000;
  if (min_speech_millis > max_speech_millis) {
    return absl::InvalidArgumentError(
        "min_speech_millis must not exceed max_speech_seconds");
  }
  if (min_silence_millis > max_speech_millis) {
    return absl::InvalidArgumentError(
        "min_silence_millis must not exceed max_speech_seconds");
  }
  if (!vad_model.empty() &&
      (!std::isfinite(silero_threshold) || silero_threshold <= 0.0f ||
       silero_threshold > 1.0f)) {
    return absl::InvalidArgumentError("silero_threshold must be in (0, 1]");
  }
  return absl::OkStatus();
}

SpeechRecognizer::SpeechRecognizer(
    std::shared_ptr<internal::SpeechRecognizerState> state)
    : state_(std::move(state)) {}

SpeechRecognizer::~SpeechRecognizer() {
  a11::Task stopped = Stop();
  if (stopped.valid()) {
    (void)stopped.Await();
  }
}

absl::StatusOr<std::shared_ptr<SpeechRecognizer>> SpeechRecognizer::Create(
    std::string model, SpeechRecognizerOptions options) {
  AudioInputOptions input_options{
      .device_index = -1,
      .sample_rate = 0.0,
      .channels = 1,
      .block_frames = 256,
      .ring_blocks = 32,
  };
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AudioInput> input,
                        AudioInput::Open(input_options));
  return Create(std::move(model), std::move(input), std::move(options));
}

absl::StatusOr<std::shared_ptr<SpeechRecognizer>> SpeechRecognizer::Create(
    std::string model, std::shared_ptr<AudioInput> input,
    SpeechRecognizerOptions options) {
  if (input == nullptr) {
    return absl::InvalidArgumentError("input must not be null");
  }
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<internal::SpeechRecognizerState> state,
                        LoadState(std::move(model), std::move(options),
                                  std::move(input), nullptr));
  return std::shared_ptr<SpeechRecognizer>(
      new SpeechRecognizer(std::move(state)));
}

absl::StatusOr<std::shared_ptr<SpeechRecognizer>> SpeechRecognizer::Create(
    std::string model, std::shared_ptr<AudioSubscription> subscription,
    SpeechRecognizerOptions options) {
  if (subscription == nullptr) {
    return absl::InvalidArgumentError("subscription must not be null");
  }
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<internal::SpeechRecognizerState> state,
                        LoadState(std::move(model), std::move(options), nullptr,
                                  std::move(subscription)));
  return std::shared_ptr<SpeechRecognizer>(
      new SpeechRecognizer(std::move(state)));
}

absl::StatusOr<std::shared_ptr<SpeechRecognizer>>
SpeechRecognizer::CreateForStream(std::string model,
                                  SpeechRecognizerOptions options) {
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<internal::SpeechRecognizerState> state,
      LoadState(std::move(model), std::move(options), nullptr, nullptr));
  return std::shared_ptr<SpeechRecognizer>(
      new SpeechRecognizer(std::move(state)));
}

a11::Task SpeechRecognizer::Start(OnTranscription on_transcription,
                                  OnRecognitionDone on_done) {
  if (on_transcription == nullptr || on_done == nullptr) {
    return a11::FailedTask(absl::InvalidArgumentError(
        "on_transcription and on_done must both be callable"));
  }

  thread::MutexLock lock(&state_->mu);
  if (state_->running) {
    return a11::FailedTask(
        absl::FailedPreconditionError("SpeechRecognizer is already running"));
  }

  std::shared_ptr<AudioSubscription> subscription;
  if (state_->supplied_subscription != nullptr) {
    if (state_->supplied_subscription_used) {
      return a11::FailedTask(absl::FailedPreconditionError(
          "A supplied AudioSubscription can only be recognised once"));
    }
    state_->supplied_subscription_used = true;
    subscription = state_->supplied_subscription;
  } else if (state_->input == nullptr) {
    return a11::FailedTask(absl::FailedPreconditionError(
        "This recognizer has no audio device; use StartStream"));
  } else {
    const double requested_frames =
        state_->input->sample_rate() *
        static_cast<double>(state_->options.subscription_buffer_millis) /
        1000.0;
    const size_t buffer_size = std::max(
        kMinBufferSize, static_cast<size_t>(std::llround(requested_frames)));
    absl::StatusOr<std::shared_ptr<AudioSubscription>> created =
        state_->input->Subscribe(buffer_size);
    if (!created.ok()) {
      state_->status = created.status();
      return a11::FailedTask(created.status());
    }
    subscription = std::move(*created);
  }

  // A subscription is itself an AudioBufferReader; a device stream never pauses.
  AudioBufferReader reader = [subscription]() {
    return subscription->Read();
  };
  std::function<void()> close_source = [subscription]() {
    subscription->Close();
  };
  return StartWithReaderLocked(std::move(reader), absl::InfiniteDuration(),
                               std::move(close_source),
                               std::move(on_transcription), std::move(on_done));
}

a11::Task SpeechRecognizer::StartStream(AudioBufferReader reader,
                                        OnTranscription on_transcription,
                                        OnRecognitionDone on_done,
                                        absl::Duration pause_after,
                                        std::function<void()> on_close) {
  if (reader == nullptr) {
    return a11::FailedTask(
        absl::InvalidArgumentError("reader must be callable"));
  }
  if (on_transcription == nullptr || on_done == nullptr) {
    return a11::FailedTask(absl::InvalidArgumentError(
        "on_transcription and on_done must both be callable"));
  }
  thread::MutexLock lock(&state_->mu);
  if (state_->running) {
    return a11::FailedTask(
        absl::FailedPreconditionError("SpeechRecognizer is already running"));
  }
  return StartWithReaderLocked(std::move(reader), pause_after,
                               std::move(on_close), std::move(on_transcription),
                               std::move(on_done));
}

a11::Task SpeechRecognizer::StartWithReaderLocked(
    AudioBufferReader reader, absl::Duration pause_after,
    std::function<void()> close_source, OnTranscription on_transcription,
    OnRecognitionDone on_done) ABSL_NO_THREAD_SAFETY_ANALYSIS {
  state_->stopping.store(false, std::memory_order_relaxed);
  state_->status = absl::OkStatus();
  state_->running = true;
  state_->active_close = close_source;
  std::shared_ptr<SpeechRecognizer> self = shared_from_this();
  state_->worker = a11::SubmitTask(
      [self = std::move(self), reader = std::move(reader), pause_after,
       close_source = std::move(close_source),
       on_transcription = std::move(on_transcription),
       on_done = std::move(on_done)]() mutable {
        return self->Run(reader, pause_after, close_source,
                         std::move(on_transcription), std::move(on_done));
      },
      {.stack_size = 64 * 1024});
  return a11::ReadyTask();
}

a11::Task SpeechRecognizer::Stop() {
  a11::Task worker;
  a11::Future<AudioBuffer> pending_read;
  std::function<void()> close_source;
  {
    thread::MutexLock lock(&state_->mu);
    if (!state_->running) {
      return a11::ReadyTask();
    }
    state_->stopping.store(true, std::memory_order_relaxed);
    worker = state_->worker;
    pending_read = state_->pending_read;
    close_source = state_->active_close;
  }
  if (close_source) {
    close_source();
  }
  if (pending_read.valid()) {
    (void)pending_read.Cancel();
  }
  return worker.valid() ? worker : a11::ReadyTask();
}

a11::Task SpeechRecognizer::Wait() {
  thread::MutexLock lock(&state_->mu);
  return state_->worker.valid() ? state_->worker : a11::ReadyTask();
}

bool SpeechRecognizer::running() const {
  thread::MutexLock lock(&state_->mu);
  return state_->running;
}

absl::Status SpeechRecognizer::GetStatus() const {
  thread::MutexLock lock(&state_->mu);
  return state_->status;
}

const std::string& SpeechRecognizer::model() const {
  return state_->model;
}

const SpeechRecognizerOptions& SpeechRecognizer::options() const {
  return state_->options;
}

absl::Status SpeechRecognizer::Run(const AudioBufferReader& reader,
                                   absl::Duration pause_after,
                                   const std::function<void()>& close_source,
                                   OnTranscription on_transcription,
                                   OnRecognitionDone on_done) {
  absl::Status status;
  try {
    status =
        RecognitionLoop(state_.get(), reader, pause_after, &on_transcription);
  } catch (const std::exception& error) {
    status = absl::UnknownError(error.what());
  } catch (...) {
    status = absl::UnknownError(
        "Speech recognition raised a non-standard exception");
  }

  if (close_source) {
    close_source();
  }
  const absl::Status terminal = CallbackStatus(on_transcription, std::nullopt);
  if (status.ok() && !terminal.ok()) {
    status = terminal;
  }
  {
    thread::MutexLock lock(&state_->mu);
    state_->status = status;
  }

  const absl::Status done = CallbackStatus(on_done);
  if (status.ok() && !done.ok()) {
    status = done;
  }
  {
    thread::MutexLock lock(&state_->mu);
    state_->status = status;
    state_->pending_read = a11::Future<AudioBuffer>();
    state_->active_close = nullptr;
    state_->running = false;
  }
  return status;
}

}  // namespace a11::sdk::audio
