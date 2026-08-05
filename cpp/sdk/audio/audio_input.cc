// Copyright 2026 The A11 Authors.

#include "sdk/audio/audio_input.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>
#include <cmath>
#include <portaudio.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/time.h"
#include "sdk/audio/audio_buffer.h"
#include "sdk/audio/device.h"
#include "sdk/audio/internal/portaudio_session.h"
#include "sdk/audio/internal/sample_ring.h"
#include "thread/concurrency.h"

namespace a11::sdk::audio {

namespace internal {

// Per-subscription state. The channel and the counters are the cross-fiber
// surface; the reassembly fields (accum, filled) are touched only by the pump
// fiber, so they need no synchronization.
struct SubscriptionState {
  SubscriptionState(size_t buffer_size, size_t channels, double sample_rate,
                    size_t queue_depth)
      : buffer_size(buffer_size),
        channels(channels),
        sample_rate(sample_rate),
        channel(queue_depth),
        accum(buffer_size * channels, 0.0f) {}

  const size_t buffer_size;
  const size_t channels;
  const double sample_rate;
  thread::Channel<AudioBuffer> channel;

  std::vector<float> accum;  // planar [channels * buffer_size], pump-only
  size_t filled = 0;         // frames accumulated so far, pump-only
  std::atomic<std::uint64_t> dropped = 0;
  std::atomic<bool> unsubscribed = false;
};

// Shared reassembly state owned jointly by the AudioInput and the pump fiber.
// Kept separate from the AudioInput so the pump never depends on the owner's
// lifetime and its mutex is independent of the capture-lifecycle mutex.
struct CaptureContext {
  thread::Mutex mu;
  std::vector<std::shared_ptr<SubscriptionState>> subs ABSL_GUARDED_BY(mu);
  std::vector<std::shared_ptr<SubscriptionState>> to_close ABSL_GUARDED_BY(mu);
  std::unique_ptr<SampleRing> ring ABSL_GUARDED_BY(mu);

  size_t channels = 0;
  double sample_rate = 0.0;
  size_t block_frames = 0;
  absl::Duration poll_interval;
};

}  // namespace internal

namespace {

using internal::CaptureContext;
using internal::SampleBlock;
using internal::SampleRing;
using internal::SubscriptionState;

constexpr size_t kSubscriptionQueueDepth = 8;

// Realtime PortAudio callback. Runs on PortAudio's own thread, so it must never
// allocate, lock a fiber primitive, or block: it only copies the interleaved
// input into the lock-free ring passed as user data.
int PaCallback(const void* input, void* /*output*/,
               unsigned long frame_count,  // NOLINT(runtime/int) PortAudio ABI
               const PaStreamCallbackTimeInfo* /*time_info*/,
               PaStreamCallbackFlags /*flags*/, void* user_data) {
  auto* ring = static_cast<SampleRing*>(user_data);
  if (input != nullptr && frame_count > 0) {
    ring->Push(static_cast<const float*>(input),
               static_cast<size_t>(frame_count), a11::Now());
  }
  return paContinue;
}

// Split one interleaved capture block into each subscription's planar
// accumulator, emitting a buffer whenever a subscription reaches its size.
void DistributeBlock(
    const SampleBlock& block, size_t channels, double sample_rate,
    const std::vector<std::shared_ptr<SubscriptionState>>& subs) {
  for (const std::shared_ptr<SubscriptionState>& state : subs) {
    const size_t buffer_size = state->buffer_size;
    for (size_t frame = 0; frame < block.frames; ++frame) {
      for (size_t channel = 0; channel < channels; ++channel) {
        state->accum[channel * buffer_size + state->filled] =
            block.interleaved[frame * channels + channel];
      }
      ++state->filled;
      if (state->filled < buffer_size) {
        continue;
      }

      AudioBuffer buffer;
      buffer.samples = std::move(state->accum);
      buffer.num_channels = channels;
      buffer.num_frames = buffer_size;
      buffer.sample_rate = sample_rate;
      // The callback stamps a block with the instant of its final frame, so
      // frame `frame` in a block of `block.frames` was taken this far earlier.
      const double frames_from_end =
          static_cast<double>(block.frames - 1 - frame);
      buffer.end_time =
          block.capture_time - absl::Seconds(frames_from_end / sample_rate);

      // Offer without blocking: a subscription that is not keeping up drops
      // this buffer rather than stalling capture for the others.
      const int selected = thread::SelectUntil(
          a11::Now(), {state->channel.writer()->OnWrite(std::move(buffer))});
      if (selected != 0) {
        state->dropped.fetch_add(1, std::memory_order_relaxed);
      }

      state->accum.assign(buffer_size * channels, 0.0f);
      state->filled = 0;
    }
  }
}

// The reassembly fiber: drain the ring into subscriptions, close the channels
// of departed subscriptions, and exit cleanly on cancellation.
absl::Status PumpBody(std::shared_ptr<CaptureContext> context,
                      SampleRing* ring) {
  while (true) {
    std::vector<std::shared_ptr<SubscriptionState>> closing;
    std::vector<std::shared_ptr<SubscriptionState>> active;
    {
      thread::MutexLock lock(&context->mu);
      closing.swap(context->to_close);
      active = context->subs;
    }
    for (const std::shared_ptr<SubscriptionState>& state : closing) {
      state->channel.writer()->Close();
    }

    bool did_work = false;
    while (const SampleBlock* block = ring->Peek()) {
      DistributeBlock(*block, context->channels, context->sample_rate, active);
      ring->Pop();
      did_work = true;
    }

    const absl::Time deadline =
        did_work ? a11::Now() : a11::Now() + context->poll_interval;
    if (thread::SelectUntil(deadline, {thread::OnCancel()}) == 0) {
      break;
    }
  }

  // Close every channel still open so blocked readers observe the end.
  std::vector<std::shared_ptr<SubscriptionState>> remaining;
  {
    thread::MutexLock lock(&context->mu);
    remaining.swap(context->to_close);
    for (std::shared_ptr<SubscriptionState>& state : context->subs) {
      remaining.push_back(std::move(state));
    }
    context->subs.clear();
  }
  for (const std::shared_ptr<SubscriptionState>& state : remaining) {
    state->channel.writer()->Close();
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status AudioInputOptions::Validate() const {
  if (block_frames < kMinBufferSize) {
    return absl::InvalidArgumentError(
        absl::StrCat("block_frames must be at least ", kMinBufferSize));
  }
  if (ring_blocks < 2) {
    return absl::InvalidArgumentError("ring_blocks must be at least 2");
  }
  if (!std::isfinite(sample_rate) || sample_rate < 0.0) {
    return absl::InvalidArgumentError(
        "sample_rate must be finite and not negative");
  }
  if (channels < 0) {
    return absl::InvalidArgumentError("channels must not be negative");
  }
  if (buffer_frames != 0 && buffer_frames < kMinBufferSize) {
    return absl::InvalidArgumentError(
        absl::StrCat("buffer_frames must be 0 or at least ", kMinBufferSize));
  }
  return absl::OkStatus();
}

AudioInput::AudioInput(DeviceInfo device, double sample_rate, int channels,
                       AudioInputOptions options,
                       std::shared_ptr<internal::PortAudioSession> session,
                       std::shared_ptr<internal::CaptureContext> context)
    : device_(std::move(device)),
      sample_rate_(sample_rate),
      channels_(channels),
      options_(std::move(options)),
      session_(std::move(session)),
      context_(std::move(context)) {}

AudioInput::~AudioInput() {
  StopCapture();
}

absl::StatusOr<std::shared_ptr<AudioInput>> AudioInput::Open(
    AudioInputOptions options) {
  ABSL_RETURN_IF_ERROR(options.Validate());

  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<internal::PortAudioSession> session,
                        internal::PortAudioSession::Acquire());

  int index = options.device_index;
  if (!options.device_name.empty()) {
    // A requested name takes precedence and is resolved to an input device.
    ABSL_ASSIGN_OR_RETURN(std::vector<DeviceInfo> devices, ListDevices());
    index = -1;
    for (const DeviceInfo& candidate : devices) {
      if (candidate.name == options.device_name &&
          candidate.max_input_channels > 0) {
        index = candidate.index;
        break;
      }
    }
    if (index < 0) {
      return absl::NotFoundError(
          absl::StrCat("No input device named '", options.device_name, "'"));
    }
  } else if (index < 0) {
    ABSL_ASSIGN_OR_RETURN(DeviceInfo default_input, DefaultInputDevice());
    index = default_input.index;
  }

  ABSL_ASSIGN_OR_RETURN(DeviceInfo device, DeviceInfoAt(index));
  if (device.max_input_channels <= 0) {
    return absl::FailedPreconditionError(
        absl::StrCat("Device '", device.name, "' has no input channels"));
  }

  const double sample_rate = options.sample_rate > 0.0
                                 ? options.sample_rate
                                 : device.default_sample_rate;
  if (sample_rate <= 0.0) {
    return absl::InvalidArgumentError(
        "Could not determine a sample rate for the device");
  }

  const int channels =
      options.channels > 0 ? options.channels : device.max_input_channels;
  if (channels > device.max_input_channels) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Requested ", channels, " channels but device '", device.name,
        "' offers at most ", device.max_input_channels));
  }

  auto context = std::make_shared<internal::CaptureContext>();
  context->channels = static_cast<size_t>(channels);
  context->sample_rate = sample_rate;
  context->block_frames = options.block_frames;
  // Poll near the callback cadence, clamped so a tiny block does not spin and a
  // huge one does not add noticeable latency to short subscription buffers.
  absl::Duration cadence =
      absl::Seconds(static_cast<double>(options.block_frames) / sample_rate);
  cadence = std::max(cadence, absl::Milliseconds(1));
  cadence = std::min(cadence, absl::Milliseconds(20));
  context->poll_interval = cadence;

  return std::shared_ptr<AudioInput>(
      new AudioInput(std::move(device), sample_rate, channels, options,
                     std::move(session), std::move(context)));
}

bool AudioInput::capturing() const {
  thread::MutexLock lock(&mu_);
  return capturing_;
}

absl::StatusOr<std::shared_ptr<AudioSubscription>> AudioInput::Subscribe(
    size_t buffer_size) {
  if (buffer_size < kMinBufferSize) {
    return absl::InvalidArgumentError(
        absl::StrCat("buffer_size must be at least ", kMinBufferSize));
  }

  auto state = std::make_shared<internal::SubscriptionState>(
      buffer_size, static_cast<size_t>(channels_), sample_rate_,
      kSubscriptionQueueDepth);
  // Membership and capture lifecycle are one transaction. In particular, a
  // concurrent close of the previous final subscription must not stop the
  // stream after this new subscription has already been admitted.
  thread::MutexLock lifecycle_lock(&mu_);
  {
    thread::MutexLock lock(&context_->mu);
    context_->subs.push_back(state);
  }

  if (absl::Status started = StartCaptureLocked(); !started.ok()) {
    thread::MutexLock lock(&context_->mu);
    for (auto it = context_->subs.begin(); it != context_->subs.end(); ++it) {
      if (*it == state) {
        context_->subs.erase(it);
        break;
      }
    }
    return started;
  }

  return std::shared_ptr<AudioSubscription>(
      new AudioSubscription(shared_from_this(), std::move(state)));
}

absl::Status AudioInput::StartCaptureLocked() {
  if (capturing_) {
    return absl::OkStatus();
  }

  SampleRing* ring = nullptr;
  {
    thread::MutexLock context_lock(&context_->mu);
    context_->ring = std::make_unique<SampleRing>(
        options_.ring_blocks, options_.block_frames,
        static_cast<size_t>(channels_));
    ring = context_->ring.get();
  }

  PaStreamParameters input_parameters = {};
  input_parameters.device = device_.index;
  input_parameters.channelCount = channels_;
  input_parameters.sampleFormat = paFloat32;  // interleaved 32-bit float
  input_parameters.suggestedLatency =
      absl::ToDoubleSeconds(device_.default_low_input_latency);
  input_parameters.hostApiSpecificStreamInfo = nullptr;

  PaStream* stream = nullptr;
  PaError error = Pa_OpenStream(
      &stream, &input_parameters, /*outputParameters=*/nullptr, sample_rate_,
      options_.block_frames, paNoFlag, &PaCallback, ring);
  if (error != paNoError) {
    thread::MutexLock context_lock(&context_->mu);
    context_->ring.reset();
    return internal::PaErrorToStatus("Pa_OpenStream", error);
  }

  error = Pa_StartStream(stream);
  if (error != paNoError) {
    Pa_CloseStream(stream);
    thread::MutexLock context_lock(&context_->mu);
    context_->ring.reset();
    return internal::PaErrorToStatus("Pa_StartStream", error);
  }

  stream_ = stream;
  std::shared_ptr<internal::CaptureContext> context = context_;
  pump_ = a11::SubmitTask(
      [context = std::move(context), ring]() mutable {
        return PumpBody(std::move(context), ring);
      },
      {.stack_size = 16384});
  capturing_ = true;
  return absl::OkStatus();
}

void AudioInput::StopCapture() {
  thread::MutexLock lock(&mu_);
  StopCaptureLocked();
}

void AudioInput::StopCaptureLocked() {
  // Keep the lifecycle mutex through teardown. A concurrent Subscribe() must
  // not replace context_->ring while the old PortAudio callback still holds a
  // raw pointer to it.
  if (!capturing_) {
    return;
  }
  capturing_ = false;
  const a11::Task pump = pump_;
  pump_ = a11::Task();
  const auto stream = static_cast<PaStream*>(stream_);
  stream_ = nullptr;

  if (pump.valid()) {
    (void)pump.Cancel();
    (void)pump.Await();
  }
  if (stream != nullptr) {
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
  }
  {
    thread::MutexLock context_lock(&context_->mu);
    context_->ring.reset();
  }
}

void AudioInput::Unsubscribe(
    const std::shared_ptr<internal::SubscriptionState>& state) {
  if (state->unsubscribed.exchange(true)) {
    return;
  }

  thread::MutexLock lifecycle_lock(&mu_);
  bool empty = false;
  {
    thread::MutexLock lock(&context_->mu);
    for (auto it = context_->subs.begin(); it != context_->subs.end(); ++it) {
      if (*it == state) {
        context_->subs.erase(it);
        break;
      }
    }
    context_->to_close.push_back(state);
    empty = context_->subs.empty();
  }

  if (empty) {
    StopCaptureLocked();
  }
}

AudioSubscription::AudioSubscription(
    std::shared_ptr<AudioInput> input,
    std::shared_ptr<internal::SubscriptionState> state)
    : input_(std::move(input)), state_(std::move(state)) {}

AudioSubscription::~AudioSubscription() {
  Close();
}

size_t AudioSubscription::buffer_size() const {
  return state_->buffer_size;
}

size_t AudioSubscription::channels() const {
  return state_->channels;
}

double AudioSubscription::sample_rate() const {
  return state_->sample_rate;
}

std::uint64_t AudioSubscription::dropped() const {
  return state_->dropped.load(std::memory_order_relaxed);
}

a11::Future<AudioBuffer> AudioSubscription::Read() {
  std::shared_ptr<internal::SubscriptionState> state = state_;
  return a11::Submit<AudioBuffer>(
      [state = std::move(state)]() -> absl::StatusOr<AudioBuffer> {
        AudioBuffer buffer;
        bool ok = false;
        const int selected =
            thread::Select({thread::OnCancel(),
                            state->channel.reader()->OnRead(&buffer, &ok)});
        if (selected == 0) {
          return absl::CancelledError("Audio read was cancelled");
        }
        if (!ok) {
          return absl::OutOfRangeError("Audio subscription is closed");
        }
        return buffer;
      },
      {.stack_size = 512});
}

void AudioSubscription::Close() {
  input_->Unsubscribe(state_);
}

}  // namespace a11::sdk::audio
