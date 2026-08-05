// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Lock-free single-producer/single-consumer ring of sample blocks.
 */

#ifndef A11_SDK_AUDIO_INTERNAL_SAMPLE_RING_H_
#define A11_SDK_AUDIO_INTERNAL_SAMPLE_RING_H_

#include <atomic>
#include <cstddef>
#include <vector>

#include <absl/base/nullability.h>

#include "a11/time.h"

namespace a11::sdk::audio::internal {

/// One PortAudio callback's worth of interleaved samples plus its timing.
struct SampleBlock {
  /// Interleaved samples, frame-major: `frames * channels` valid values.
  std::vector<float> interleaved;
  /// Number of frames currently held (0 <= frames <= capacity frames).
  size_t frames = 0;
  /// Best-effort wall-clock instant the last frame in this block was taken.
  a11::Time capture_time = a11::InfinitePast();
};

/**
 * @brief A bounded lock-free ring buffer handing sample blocks from the
 *   PortAudio callback thread to the reassembly fiber.
 *
 * Exactly one producer (the realtime callback) calls Push(); exactly one
 * consumer (the pump fiber) calls Peek()/Pop(). Blocks are preallocated at
 * construction so the producer never allocates or locks. When the ring is full
 * the producer drops the incoming block and counts an overrun rather than
 * blocking the realtime thread.
 */
class SampleRing {
 public:
  SampleRing(size_t block_count, size_t frames_per_block, size_t channels)
      : channels_(channels),
        frames_per_block_(frames_per_block),
        blocks_(block_count < 2 ? 2 : block_count) {
    for (SampleBlock& block : blocks_) {
      block.interleaved.assign(frames_per_block_ * channels_, 0.0f);
    }
  }

  SampleRing(const SampleRing&) = delete;
  SampleRing& operator=(const SampleRing&) = delete;

  /**
   * @brief Copy @p frames interleaved frames into the ring (producer side).
   * @return False when the ring was full and the block was dropped.
   */
  bool Push(const float* absl_nonnull interleaved, size_t frames,
            a11::Time capture_time) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    if (tail - head >= blocks_.size()) {
      overruns_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    SampleBlock& block = blocks_[tail % blocks_.size()];
    const size_t clamped =
        frames > frames_per_block_ ? frames_per_block_ : frames;
    const size_t values = clamped * channels_;
    for (size_t i = 0; i < values; ++i) {
      block.interleaved[i] = interleaved[i];
    }
    block.frames = clamped;
    block.capture_time = capture_time;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  /// Borrow the oldest unread block, or nullptr when empty (consumer side).
  const SampleBlock* absl_nullable Peek() const {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    if (head == tail) {
      return nullptr;
    }
    return &blocks_[head % blocks_.size()];
  }

  /// Release the block previously returned by Peek() (consumer side).
  void Pop() {
    head_.store(head_.load(std::memory_order_relaxed) + 1,
                std::memory_order_release);
  }

  /// Number of blocks dropped because the ring was full.
  [[nodiscard]] size_t overruns() const {
    return overruns_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] size_t channels() const { return channels_; }

  [[nodiscard]] size_t frames_per_block() const { return frames_per_block_; }

 private:
  const size_t channels_;
  const size_t frames_per_block_;
  std::vector<SampleBlock> blocks_;
  std::atomic<size_t> head_ = 0;  // consumer cursor (monotonic)
  std::atomic<size_t> tail_ = 0;  // producer cursor (monotonic)
  std::atomic<size_t> overruns_ = 0;
};

}  // namespace a11::sdk::audio::internal

#endif  // A11_SDK_AUDIO_INTERNAL_SAMPLE_RING_H_
