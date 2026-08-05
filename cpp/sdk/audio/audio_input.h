// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Continuous capture from an audio input device with fan-out
 *   subscriptions.
 */

#ifndef A11_SDK_AUDIO_AUDIO_INPUT_H_
#define A11_SDK_AUDIO_AUDIO_INPUT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <absl/base/thread_annotations.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/concurrency/future.h"
#include "sdk/audio/audio_buffer.h"
#include "sdk/audio/device.h"
#include "thread/boost_primitives.h"

namespace a11::sdk::audio {

/// Smallest buffer size, in frames, a subscription may request.
inline constexpr size_t kMinBufferSize = 32;

/**
 * @brief How an @ref AudioInput opens its capture stream.
 *
 * Fields left at their sentinel defaults are resolved from the chosen device:
 * a negative @c device_index selects the host's default input, a zero
 * @c sample_rate uses the device's default rate, and a zero @c channels uses
 * every input channel the device exposes.
 */
struct AudioInputOptions {
  /// Device index to capture from, or negative for the default input device.
  /// Ignored when @c device_name is non-empty.
  int device_index = -1;
  /// Human-readable input device name to capture from. When non-empty it takes
  /// precedence over @c device_index and is resolved to an index in Open();
  /// empty selects by @c device_index (or the default input device).
  std::string device_name;
  /// Requested sample rate in hertz, or 0 to use the device default.
  double sample_rate = 0.0;
  /// Requested channel count, or 0 to use the device's input channel count.
  int channels = 0;
  /// Frames per PortAudio callback block (internal capture granularity).
  size_t block_frames = 256;
  /// Depth of the internal callback-to-fiber ring, in blocks.
  size_t ring_blocks = 32;
  /// Frames per channel delivered in each subscription buffer, or 0 to fall
  /// back to @c block_frames. Used by the action layer's Subscribe() call.
  size_t buffer_frames = 0;

  /// Validate every field and return a descriptive status on failure.
  absl::Status Validate() const;

  /// The delivered subscription buffer size implied by these options: the
  /// requested @c buffer_frames, or @c block_frames when left at 0.
  [[nodiscard]] size_t ResolvedBufferFrames() const {
    return buffer_frames > 0 ? buffer_frames : block_frames;
  }
};

class AudioInput;

namespace internal {
class PortAudioSession;
struct CaptureContext;
struct SubscriptionState;
}  // namespace internal

/**
 * @brief A live subscription delivering fixed-size buffers from an input.
 *
 * Each subscription owns an independent queue: it receives every captured
 * sample reassembled into buffers of its own requested size, regardless of how
 * other subscriptions are configured or how quickly they read. A subscription
 * that falls behind has buffers dropped (counted by dropped()) rather than
 * stalling capture for everyone else. Capture runs while at least one
 * subscription is alive; closing the last one stops the device stream.
 */
class AudioSubscription {
 public:
  AudioSubscription(const AudioSubscription&) = delete;
  AudioSubscription& operator=(const AudioSubscription&) = delete;
  ~AudioSubscription();

  /// Frames-per-channel in every buffer this subscription delivers.
  [[nodiscard]] size_t buffer_size() const;
  /// Number of channels in every delivered buffer.
  [[nodiscard]] size_t channels() const;
  /// Sample rate, in hertz, of every delivered buffer.
  [[nodiscard]] double sample_rate() const;
  /// Buffers discarded because this subscription was not read fast enough.
  [[nodiscard]] std::uint64_t dropped() const;

  /**
   * @brief Resolve with the next captured buffer.
   *
   * The returned future completes with an @c OutOfRange status when the
   * subscription is closed (its stream ended) and with @c Cancelled when the
   * future itself is cancelled. Awaiting from a fiber yields cooperatively.
   */
  a11::Future<AudioBuffer> Read();

  /// Stop delivering; idempotent. Stops capture if this was the last one.
  void Close();

 private:
  friend class AudioInput;
  AudioSubscription(std::shared_ptr<AudioInput> input,
                    std::shared_ptr<internal::SubscriptionState> state);

  std::shared_ptr<AudioInput> input_;
  std::shared_ptr<internal::SubscriptionState> state_;
};

/**
 * @brief A capturable audio input device.
 *
 * Constructing an AudioInput resolves and exposes the device's metadata
 * (name(), sample_rate(), channels(), ...) without opening a stream. Capture
 * begins lazily when the first subscription is created and stops when the last
 * is closed, so the device is only held while something is listening.
 */
class AudioInput : public std::enable_shared_from_this<AudioInput> {
 public:
  /// Resolve the device and validate options without starting capture.
  static absl::StatusOr<std::shared_ptr<AudioInput>> Open(
      AudioInputOptions options);

  AudioInput(const AudioInput&) = delete;
  AudioInput& operator=(const AudioInput&) = delete;
  ~AudioInput();

  /// The captured device's metadata.
  [[nodiscard]] const DeviceInfo& device() const { return device_; }

  [[nodiscard]] const std::string& name() const { return device_.name; }

  [[nodiscard]] int device_index() const { return device_.index; }

  /// Sample rate, in hertz, capture runs at.
  [[nodiscard]] double sample_rate() const { return sample_rate_; }

  /// Number of channels every subscription receives.
  [[nodiscard]] int channels() const { return channels_; }

  /// Whether a capture stream is currently open.
  [[nodiscard]] bool capturing() const;

  /**
   * @brief Start receiving buffers of @p buffer_size frames per channel.
   * @param buffer_size Frames per channel per delivered buffer; at least
   *   @ref kMinBufferSize.
   */
  absl::StatusOr<std::shared_ptr<AudioSubscription>> Subscribe(
      size_t buffer_size);

 private:
  AudioInput(DeviceInfo device, double sample_rate, int channels,
             AudioInputOptions options,
             std::shared_ptr<internal::PortAudioSession> session,
             std::shared_ptr<internal::CaptureContext> context);

  friend class AudioSubscription;
  void Unsubscribe(const std::shared_ptr<internal::SubscriptionState>& state);
  absl::Status StartCaptureLocked() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  void StopCapture();
  void StopCaptureLocked() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  const DeviceInfo device_;
  const double sample_rate_;
  const int channels_;
  const AudioInputOptions options_;
  std::shared_ptr<internal::PortAudioSession> session_;
  std::shared_ptr<internal::CaptureContext> context_;

  // Guards the capture lifecycle (stream_, pump_, capturing_). Deliberately
  // separate from the context mutex the pump fiber holds, so StopCapture can
  // join the pump without deadlocking against it.
  mutable thread::Mutex mu_;
  bool capturing_ ABSL_GUARDED_BY(mu_) = false;
  // PaStream*, kept as void* to keep PortAudio out of this public header.
  void* stream_ ABSL_GUARDED_BY(mu_) = nullptr;
  a11::Task pump_ ABSL_GUARDED_BY(mu_);
};

}  // namespace a11::sdk::audio

#endif  // A11_SDK_AUDIO_AUDIO_INPUT_H_
