/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file
 * @brief RFC 8899 path MTU discovery for datagram transports.
 *
 * usrsctp uses a fixed 1280-byte fallback when libdatachannel disables its
 * path discovery. This engine probes larger sizes and treats lost probes as
 * size limits. PathMtuProber applies sizes and sends probes, keeping the
 * search independent of WebRTC and SCTP.
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
   * All probes in one burst must be acknowledged. Concurrent probes test
   * reassembly under sustained traffic; one acknowledged datagram is
   * insufficient to confirm a usable stream size.
   */
  int confirm_burst = 4;
  /**
   * @brief How long one probe has to be acknowledged.
   *
   * Discovery pauses application traffic for each attempt. This bounds one
   * probe wait to 500 ms by default.
   */
  absl::Duration probe_timeout = absl::Milliseconds(500);
  /// How long after converging before searching upward again, which is what
  /// notices a path that *grew* -- a VPN dropped, a LAN reconnected to a
  /// jumbo-frame segment. RFC 8899's PMTU_RAISE_TIMER.
  absl::Duration raise_timer = absl::Seconds(600);
  /**
   * @brief How long to wait before retrying a search that could not start.
   *
   * Retries quickly when the association cannot yet apply an MTU. A completed
   * search uses `raise_timer` before searching upward again.
   */
  absl::Duration startup_retry = absl::Milliseconds(250);
  /// Consecutive transport send failures that count as the path having shrunk
  /// under the confirmed size. Below this, a failure is treated as ordinary
  /// loss.
  int black_hole_threshold = 3;

  absl::Status Validate() const;
};

/**
 * @brief Transport operations used by the path MTU search.
 *
 * Every callback runs on the discovery fiber and may block; none may re-enter
 * the engine.
 */
struct PathMtuProber {
  /**
   * Applies @p mtu to the association.
   *
   * A non-OK status means the transport could not apply the size. It does not
   * count as evidence of packet loss on the path.
   */
  std::function<absl::Status(size_t mtu)> apply;
  /**
   * Sends one probe whose payload is @p payload bytes and waits until it is
   * acknowledged or @p deadline passes.
   *
   * Returns true for acknowledgement, false for a sent but unanswered probe,
   * and nullopt if the probe could not be sent. Unsent probes provide no path
   * evidence. Do not retransmit probes; RFC 8899 §3 requires probe loss to
   * leave ordinary protocol traffic unaffected.
   */
  std::function<std::optional<bool>(size_t payload, absl::Time deadline)>
      probe = {};

  /**
   * @brief Sends @p count probes back to back and reports how many returned.
   *
   * Prefer this callback when the transport can keep probes in flight
   * together. `probe` supports transports without bursts and tests.
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
