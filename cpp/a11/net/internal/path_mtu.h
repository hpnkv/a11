// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief
 *   Datagram packetization-layer path MTU discovery (RFC 8899) for a transport
 *   that cannot discover its own.
 *
 * SCTP is supposed to do this itself, and usrsctp does not
 * (https://github.com/sctplab/usrsctp/issues/205), so libdatachannel disables
 * path MTU discovery and pins the association to RFC 8261's safe fallback of
 * 1280 -- the IPv6 minimum. Every message is then fragmented into 1172-byte
 * chunks whatever the path could carry, which measured at roughly a third of
 * the achievable throughput at 64 KiB on both of A11's reference machines.
 *
 * The value cannot simply be raised: above about 4 KiB on both machines every
 * message needing more than one chunk stopped arriving while small messages
 * kept flowing at full rate, so a wrong guess produces a transport that works
 * until a payload gets big and says nothing about why. That is exactly the
 * situation RFC 8899 exists for -- probe, and let a lost probe be the
 * evidence.
 *
 * This engine is independent of WebRTC, SCTP, and libdatachannel. It
 * asks a PathMtuProber to apply a size and to send one probe of a given size,
 * and it decides what to try next. That keeps the search testable against a
 * fake
 * prober with a size threshold, which is the only way to get coverage of the
 * interesting paths (a shrinking path, a vanishing peer) without a network that
 * can be reconfigured mid-test.
 */

#ifndef A11_NET_INTERNAL_PATH_MTU_H_
#define A11_NET_INTERNAL_PATH_MTU_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <absl/base/thread_annotations.h>
#include <absl/status/status.h>
#include <absl/time/time.h>

#include "thread/boost_primitives.h"

namespace thread {
class PermanentEvent;
}  // namespace thread

namespace a11::net::internal {

/** @brief Where the search is, in RFC 8899 §5.2's terms. */
enum class PathMtuState {
  /// Never started: the peer did not advertise probe support, so the base MTU
  /// stands and nothing is ever tried. The fail-safe state.
  kDisabled,
  /// Confirming that the base MTU works at all, before searching above it.
  kBase,
  /// Binary-searching between a confirmed size and one known to be too large.
  kSearching,
  /// Converged. Waiting for the raise timer or for evidence the path shrank.
  kSearchComplete,
  /// The base MTU itself could not be carried: a connectivity failure rather
  /// than an MTU problem, and terminal.
  kError,
};

std::string_view PathMtuStateName(PathMtuState state);

/**
 * @brief Search bounds and timers, all overridable per deployment.
 *
 * Defaults follow RFC 8899 §5.1.2 where it names a value, and A11's own
 * measurements where it does not.
 */
struct PathMtuOptions {
  /// The size that needs no evidence; the search starts here and falls back
  /// here. RFC 8899's BASE_PLPMTU.
  size_t base_mtu = 1280;
  /// Floor the transport will accept. RFC 8899's MIN_PLPMTU.
  size_t min_mtu = 512;
  /**
   * @brief Ceiling on the search. RFC 8899's MAX_PLPMTU.
   *
   * Above the ~4 KiB point where both reference machines stopped
   * delivering fragmented messages: the search is *supposed* to find that edge,
   * and a ceiling below it would cap the result at an arbitrary number instead.
   * The cost of setting it too high is a slower first search (one probe timeout
   * per rejected candidate), not a wrong answer.
   */
  size_t max_mtu = 8192;
  /// Stop searching once the bracket is this narrow. A smaller bracket adds
  /// probe round trips without materially improving payload size.
  size_t granularity = 64;
  /// Bytes of SCTP/DTLS/UDP/IPv6 header between a probe's payload and the
  /// packet it becomes. Matches libdatachannel's own arithmetic.
  size_t probe_overhead = 108;
  /// Attempts at one candidate before it counts as too large. RFC 8899's
  /// MAX_PROBES.
  int max_probes = 3;
  /**
   * @brief Probes that must be acknowledged *together* to confirm a candidate.
   *
   * One acknowledged probe is not evidence that a size is usable, and this is
   * the single most important number in this struct. Measured on the bare data
   * channel: a 64 KiB stream runs at 173 MiB/s at MTU 4096 and does not run at
   * all at 4256, yet a single probe at 4256 is acknowledged. One IP-fragmented
   * datagram reassembles; a stream of them does not. A search that believed a
   * single probe converged on 4256, applied it, and stalled the stream.
   *
   * So a candidate is confirmed only when a burst of this many probes goes out
   * back to back -- in flight together, which is what stresses reassembly the
   * way
   * real traffic does -- and all of them return.
   */
  int confirm_burst = 4;
  /**
   * @brief How long one probe has to be acknowledged.
   *
   * Every rejected candidate costs this once per attempt, and the application
   * is paused for each -- so it is a latency budget, not just a patience
   * setting. A second was far too generous: it made a first search stall a
   * live stream for seconds at a time. 500 ms leaves room for a slow internet
   * path while keeping the worst-case stall to half a second, and a probe lost
   * to a short timeout costs only a retry.
   */
  absl::Duration probe_timeout = absl::Milliseconds(500);
  /// How long after converging before searching upward again, which is what
  /// notices a path that *grew* -- a VPN dropped, a LAN reconnected to a
  /// jumbo-frame segment. RFC 8899's PMTU_RAISE_TIMER.
  absl::Duration raise_timer = absl::Seconds(600);
  /**
   * @brief How long to wait before retrying a search that could not start.
   *
   * Distinct from `raise_timer`, and the distinction matters: a search that
   * confirmed something has all the time in the world before looking for more,
   * while one that could not even apply an MTU has learned nothing and must
   * come back promptly. The association is routinely not up yet when the probe
   * channel appears -- with in-process signalling it never is -- and treating
   * that like a completed search meant waiting ten minutes to discover
   * anything at all.
   */
  absl::Duration startup_retry = absl::Milliseconds(250);
  /// Consecutive transport send failures that count as the path having shrunk
  /// under the confirmed size. Below this, a failure is treated as ordinary
  /// loss.
  int black_hole_threshold = 3;

  absl::Status Validate() const;
};

/**
 * @brief What the engine needs from its transport, and nothing more.
 *
 * Every callback runs on the discovery fiber and may block; none may re-enter
 * the engine.
 */
struct PathMtuProber {
  /**
   * Applies @p mtu to the association.
   *
   * A non-OK return must mean "the stack would not take this" -- no association
   * yet, out of range -- and never "the path dropped this". The engine treats
   * the two completely differently: the first abandons the attempt without
   * drawing any conclusion about the network, the second is the evidence the
   * whole search is made of. Conflating them turns a startup race into a
   * permanent verdict that the path is tiny.
   */
  std::function<absl::Status(size_t mtu)> apply;
  /**
   * Sends one probe whose payload is @p payload bytes and waits until it is
   * acknowledged or @p deadline passes.
   *
   * `true` acknowledged, `false` sent and unanswered, and **`std::nullopt` when
   * it could not be sent at all** -- the probe channel is not open yet, or has
   * gone. That third case is not optional politeness: a probe that never left
   * is
   * no evidence about the path, and counting it as a loss at the base MTU makes
   * discovery abort a perfectly healthy stream as a connectivity failure. It
   * did, before this was three-valued.
   *
   * Must not be retransmitted: RFC 8899 §3 requires that losing a probe not
   * affect the protocol, and a reliable retransmission of an oversized packet
   * blocks its channel indefinitely instead.
   */
  std::function<std::optional<bool>(size_t payload, absl::Time deadline)>
      probe = {};

  /**
   * @brief Sends @p count probes back to back and reports how many returned.
   *
   * Set in preference to `probe`: probes that wait for each other's answer are
   * never in flight together, so they cannot detect a size that only survives
   * in
   * isolation -- which is exactly the failure this search has to avoid. `probe`
   * is
   * kept for a transport that cannot express a burst, and for tests.
   *
   * `std::nullopt` when the burst could not be sent at all (no association
   * yet).
   */
  std::function<std::optional<int>(size_t payload, int count,
                                   absl::Time deadline)>
      probe_burst;
  /**
   * @brief Optional traffic pause for transports without SCTP recovery.
   *
   * SCTP re-fragments and retransmits data sent above a failed probe size, so
   * this search leaves application traffic running. Pausing for every rejected
   * candidate would stall traffic for a complete probe timeout. A transport
   * without equivalent recovery may provide this callback.
   */
  std::function<void()> pause = {};

  /// @see pause
  std::function<void()> resume = {};

  /// The path cannot carry the base MTU. Terminal; the caller aborts the
  /// stream.
  std::function<void(absl::Status)> fail = {};
};

/**
 * @brief The search itself: a binary hunt for the largest size that arrives.
 *
 * Owned by the transport, driven by one fiber. `Search()` is synchronous and
 * public so tests can drive it without timers; `Run()` is the continuous loop
 * around it that production uses.
 */
class PathMtuDiscovery {
 public:
  PathMtuDiscovery(PathMtuOptions options, PathMtuProber prober);

  /// Runs one complete search, leaving the association at the largest size that
  /// answered. Returns the confirmed MTU, or 0 if the base itself failed.
  size_t Search();

  /**
   * @brief Search, then keep searching for as long as the stream lives.
   *
   * Returns when Stop() is called, or when the base MTU fails. Between searches
   * it waits for whichever comes first: the raise timer (the path may have
   * grown) or a black-hole report (it certainly shrank).
   */
  void Run();

  /// Ends Run() at the next opportunity and wakes it.
  void Stop();

  /**
   * @brief Reports that the transport failed to deliver at the confirmed size.
   *
   * Cheap and callable from anywhere. `black_hole_threshold` consecutive
   * reports drop the association straight back to the base MTU without a search
   * first,
   * because the priority is restoring connectivity -- and start a fresh search.
   * Any successful probe resets the count, so ordinary loss does not accumulate
   * into a false positive.
   */
  void ReportSendFailure();

  /// Cancels an accumulated run of failures; called when traffic flows again.
  void ReportSendSuccess();

  [[nodiscard]] PathMtuState state() const;
  /// Largest size confirmed to arrive, and what the association is set to.
  [[nodiscard]] size_t confirmed_mtu() const;
  /// Probes sent since construction, for observability and for tests that need
  /// to assert a search did not silently do nothing.
  [[nodiscard]] int probes_sent() const;

 private:
  /// One candidate, up to max_probes attempts. `indeterminate` is set when the
  /// transport refused to apply the size, which is not evidence either way.
  bool ProbeCandidate(size_t candidate, bool* absl_nonnull indeterminate);
  void SetState(PathMtuState state);
  /// Applies `confirmed_` and records it, ignoring a refusal: there is nothing
  /// useful to do about one on a restore path.
  void RestoreConfirmed();

  const PathMtuOptions options_;
  const PathMtuProber prober_;

  mutable thread::Mutex mu_;
  PathMtuState state_ ABSL_GUARDED_BY(mu_) = PathMtuState::kDisabled;
  size_t confirmed_ ABSL_GUARDED_BY(mu_) = 0;
  int failures_ ABSL_GUARDED_BY(mu_) = 0;
  int probes_sent_ ABSL_GUARDED_BY(mu_) = 0;
  bool stopped_ ABSL_GUARDED_BY(mu_) = false;
  bool black_hole_ ABSL_GUARDED_BY(mu_) = false;
  std::shared_ptr<thread::PermanentEvent> changed_ ABSL_GUARDED_BY(mu_);
};

}  // namespace a11::net::internal

#endif  // A11_NET_INTERNAL_PATH_MTU_H_
