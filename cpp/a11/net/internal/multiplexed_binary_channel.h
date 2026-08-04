// Copyright 2026 The A11 Authors.

#ifndef A11_NET_INTERNAL_MULTIPLEXED_BINARY_CHANNEL_H_
#define A11_NET_INTERNAL_MULTIPLEXED_BINARY_CHANNEL_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/base/thread_annotations.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/net/internal/binary_channel.h"
#include "thread/boost_primitives.h"

namespace thread {
class PermanentEvent;
}  // namespace thread

namespace a11::net::internal {

/// Creates a fresh member channel for background replenishment. Returning a
/// non-OK status counts as one replenishment failure; a null channel is
/// treated the same. Left empty (server side) to disable replenishment.
using MemberChannelFactory =
    std::function<absl::StatusOr<std::shared_ptr<BinaryChannel>>()>;

/** Tuning for how many member channels a multiplexed channel maintains. */
struct MultiplexedChannelOptions {
  /// Desired number of live member channels (client) and the hard cap on
  /// members the channel will ever hold (both client replenishment and the
  /// server accept path honour it).
  size_t target_channels = 8;
  /// Consecutive replenishment failures after which replenishment gives up.
  size_t max_replenish_failures = 4;
  /// How long a replenishment channel has to open before it counts as failed.
  absl::Duration open_timeout = absl::Seconds(20);
  /// Backoff before a failed replenishment attempt is retried.
  absl::Duration replenish_backoff = absl::Milliseconds(250);
  /// Maximum out-of-order member packets buffered while awaiting a gap.
  size_t max_reorder_packets = 4096;
};

/**
 * @brief A BinaryChannel that stripes packets across several member channels.
 *
 * The upper WireStream layer sees a single ordered, reliable channel: every
 * packet handed to Send() is tagged with a monotonic sequence number and sent
 * over one of the currently live member channels chosen round-robin, so slow
 * per-channel acknowledgement round-trips overlap instead of serialising.
 * Inbound member packets are reordered by sequence number and delivered in the
 * original send order, exactly as a single data channel would. Losing one
 * member channel does not fail the stream; a background task (when a factory is
 * supplied) replenishes live members back up to the target, giving up after a
 * run of failures. Member losses and replenishment are logged at DEBUG (VLOG).
 */
class MultiplexedBinaryChannel
    : public BinaryChannel,
      public std::enable_shared_from_this<MultiplexedBinaryChannel> {
 public:
  /// Builds a channel that adopts `initial_members` and, when `factory` is
  /// set, replenishes live members toward `options.target_channels`.
  static std::shared_ptr<MultiplexedBinaryChannel> Create(
      std::vector<std::shared_ptr<BinaryChannel>> initial_members,
      MemberChannelFactory factory, MultiplexedChannelOptions options);

  ~MultiplexedBinaryChannel() override;

  absl::Status SetCallbacks(BinaryChannelCallbacks callbacks) override;
  absl::Status ResetCallbacks() override;
  absl::Status Open() override;
  absl::Status Send(std::string bytes) override;
  absl::StatusOr<size_t> BufferedAmount() const override;
  absl::StatusOr<bool> IsOpen() const override;
  absl::Status Close() override;
  [[nodiscard]] void* absl_nullable GetImpl() const override;

  /**
   * @brief Adopts an already-negotiated member channel (server accept path).
   *
   * @return false without adopting when the target cap is already reached, so
   *     the caller can close the surplus channel.
   */
  bool AddMember(std::shared_ptr<BinaryChannel> channel);

  /// Number of member channels currently open for sending and receiving.
  [[nodiscard]] size_t LiveCount() const;

 private:
  struct Member {
    std::uint64_t id = 0;
    std::shared_ptr<BinaryChannel> channel;
    bool open = false;
  };

  MultiplexedBinaryChannel(
      std::vector<std::shared_ptr<BinaryChannel>> initial_members,
      MemberChannelFactory factory, MultiplexedChannelOptions options);

  void NotifyLocked() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  // Appends a member under the lock and returns it; the caller wires its
  // transport callbacks with WireMember() after releasing the lock.
  std::shared_ptr<Member> AppendMemberLocked(
      std::shared_ptr<BinaryChannel> channel)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  // Installs the member's transport callbacks and adopts an already-open
  // channel. Must run without mu_ held: some transports flush buffered
  // messages or fire open synchronously while callbacks are being set.
  void WireMember(const std::shared_ptr<Member>& member);
  size_t LiveCountLocked() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  void DropMember(std::uint64_t id, std::string_view reason);
  void OnMemberOpen(std::uint64_t id);
  void OnMemberMessage(std::string framed);
  void FlushPending();
  bool WaitMemberOpen(std::uint64_t id);
  // Schedules a replenishment fiber if one is warranted and not already
  // running. The fiber runs Replenish() until the target is met or it gives
  // up, then exits -- so no fiber lingers in steady state.
  void MaybeReplenish();
  void Replenish();

  const MemberChannelFactory factory_;
  const MultiplexedChannelOptions options_;

  mutable thread::Mutex mu_;
  std::vector<std::shared_ptr<Member>> members_ ABSL_GUARDED_BY(mu_);
  std::uint64_t next_member_id_ ABSL_GUARDED_BY(mu_) = 1;
  std::uint64_t round_robin_ ABSL_GUARDED_BY(mu_) = 0;
  std::uint64_t next_send_seq_ ABSL_GUARDED_BY(mu_) = 0;
  std::uint64_t next_deliver_seq_ ABSL_GUARDED_BY(mu_) = 0;
  std::deque<std::string> pending_out_ ABSL_GUARDED_BY(mu_);
  size_t pending_out_bytes_ ABSL_GUARDED_BY(mu_) = 0;
  bool flushing_ ABSL_GUARDED_BY(mu_) = false;
  bool flush_again_ ABSL_GUARDED_BY(mu_) = false;
  std::map<std::uint64_t, std::string> reorder_ ABSL_GUARDED_BY(mu_);
  bool delivering_ ABSL_GUARDED_BY(mu_) = false;
  bool callbacks_set_ ABSL_GUARDED_BY(mu_) = false;
  bool any_open_ ABSL_GUARDED_BY(mu_) = false;
  bool closed_ ABSL_GUARDED_BY(mu_) = false;
  bool replenishing_ ABSL_GUARDED_BY(mu_) = false;
  bool replenish_gave_up_ ABSL_GUARDED_BY(mu_) = false;
  size_t replenish_failures_ ABSL_GUARDED_BY(mu_) = 0;
  BinaryChannelCallbacks callbacks_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<thread::PermanentEvent> changed_ ABSL_GUARDED_BY(mu_);
};

}  // namespace a11::net::internal

#endif  // A11_NET_INTERNAL_MULTIPLEXED_BINARY_CHANNEL_H_
