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
  absl::Status Abort(absl::Status status) override;
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

  /**
   * @brief Holds packets in the unassigned queue instead of handing them over.
   *
   * For path MTU probing, which raises the association's MTU to a size nothing
   * has confirmed yet. The association has one MTU, so an application packet
   * emitted during that window would go out at the unconfirmed size too --
   * pausing is what makes "no application packet is ever sent above a confirmed
   * MTU" true rather than merely likely.
   *
   * Nesting counts, so overlapping pauses do not release early. Nothing is
   * dropped or reordered: packets keep their sequence numbers and are assigned in
   * order when sends resume.
   */
  /**
   * @brief Reports member send outcomes to path MTU discovery.
   *
   * The search can confirm a size that later stops working -- a path changes, or a
   * burst of probes was luckier than a stream of data. Its fall-back-to-base logic
   * needs a signal, and this is where the transport has one: a member's send
   * failing, or a member being lost. Without it a bad size is never walked back.
   */
  void SetSendOutcomeObserver(std::function<void(bool succeeded)> observer);

  void PauseSends();
  /// Releases one PauseSends() and flushes if that was the last one.
  void ResumeSends();

 private:
  struct Member {
    std::uint64_t id = 0;
    std::shared_ptr<BinaryChannel> channel;
    bool open = false;
    /// Packets assigned to this member and not yet handed to it, and whether a
    /// caller is currently handing them over. The claim is per member, so a send
    /// on one channel does not wait behind a send on another -- which is the
    /// point of striping in the first place.
    std::deque<std::string> pending;
    size_t pending_bytes = 0;
    bool flushing = false;
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
  // Assigns every packet still in pending_out_ to a live member, round-robin.
  void AssignPendingLocked() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  // Assigns queued packets, then hands each member's queue to it. Members with
  // work and no owner are claimed here: the first is flushed on the caller's
  // thread and the rest on fibers of their own, so several channels take bytes at
  // once instead of queueing behind one flusher.
  void FlushPending();
  // Hands one member its queue. The caller must hold member->flushing.
  void FlushMember(const std::shared_ptr<Member>& member);
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
  /// Packets with a sequence number but no member yet, either because none is
  /// live or because the member they were on was dropped mid-flight.
  std::deque<std::string> pending_out_ ABSL_GUARDED_BY(mu_);
  size_t pending_out_bytes_ ABSL_GUARDED_BY(mu_) = 0;
  std::map<std::uint64_t, std::string> reorder_ ABSL_GUARDED_BY(mu_);
  bool delivering_ ABSL_GUARDED_BY(mu_) = false;
  bool callbacks_set_ ABSL_GUARDED_BY(mu_) = false;
  bool any_open_ ABSL_GUARDED_BY(mu_) = false;
  bool closed_ ABSL_GUARDED_BY(mu_) = false;
  size_t send_pauses_ ABSL_GUARDED_BY(mu_) = 0;
  std::function<void(bool)> send_outcome_ ABSL_GUARDED_BY(mu_);
  bool replenishing_ ABSL_GUARDED_BY(mu_) = false;
  bool replenish_gave_up_ ABSL_GUARDED_BY(mu_) = false;
  size_t replenish_failures_ ABSL_GUARDED_BY(mu_) = 0;
  BinaryChannelCallbacks callbacks_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<thread::PermanentEvent> changed_ ABSL_GUARDED_BY(mu_);
};

}  // namespace a11::net::internal

#endif  // A11_NET_INTERNAL_MULTIPLEXED_BINARY_CHANNEL_H_
