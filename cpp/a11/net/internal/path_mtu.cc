// Copyright 2026 The A11 Authors.

#include "a11/net/internal/path_mtu.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "thread/boost_primitives.h"
#include "thread/fiber.h"
#include "thread/select.h"
#include "thread/selectables.h"

namespace a11::net::internal {

std::string_view PathMtuStateName(PathMtuState state) {
  switch (state) {
    case PathMtuState::kDisabled:
      return "disabled";
    case PathMtuState::kBase:
      return "base";
    case PathMtuState::kSearching:
      return "searching";
    case PathMtuState::kSearchComplete:
      return "search-complete";
    case PathMtuState::kError:
      return "error";
  }
  return "unknown";
}

absl::Status PathMtuOptions::Validate() const {
  if (min_mtu < 2 * probe_overhead) {
    return absl::InvalidArgumentError(absl::StrCat(
        "path MTU min_mtu must leave room for ", probe_overhead,
        " bytes of header, got ", min_mtu));
  }
  if (base_mtu < min_mtu) {
    return absl::InvalidArgumentError(
        "path MTU base_mtu must not be below min_mtu");
  }
  if (max_mtu < base_mtu) {
    return absl::InvalidArgumentError(
        "path MTU max_mtu must not be below base_mtu");
  }
  if (granularity == 0) {
    return absl::InvalidArgumentError("path MTU granularity must be positive");
  }
  if (max_probes <= 0) {
    return absl::InvalidArgumentError("path MTU max_probes must be positive");
  }
  if (confirm_burst <= 0) {
    return absl::InvalidArgumentError(
        "path MTU confirm_burst must be positive");
  }
  if (probe_timeout <= absl::ZeroDuration()) {
    return absl::InvalidArgumentError(
        "path MTU probe_timeout must be positive");
  }
  if (black_hole_threshold <= 0) {
    return absl::InvalidArgumentError(
        "path MTU black_hole_threshold must be positive");
  }
  return absl::OkStatus();
}

PathMtuDiscovery::PathMtuDiscovery(PathMtuOptions options, PathMtuProber prober)
    : options_(options),
      prober_(std::move(prober)),
      changed_(std::make_shared<thread::PermanentEvent>()) {}

void PathMtuDiscovery::SetState(PathMtuState state) {
  std::shared_ptr<thread::PermanentEvent> event;
  {
    thread::MutexLock lock(&mu_);
    if (state_ == state) {
      return;
    }
    state_ = state;
    event = std::exchange(changed_,
                          std::make_shared<thread::PermanentEvent>());
  }
  event->Notify();
}

PathMtuState PathMtuDiscovery::state() const {
  thread::MutexLock lock(&mu_);
  return state_;
}

size_t PathMtuDiscovery::confirmed_mtu() const {
  thread::MutexLock lock(&mu_);
  return confirmed_;
}

int PathMtuDiscovery::probes_sent() const {
  thread::MutexLock lock(&mu_);
  return probes_sent_;
}

void PathMtuDiscovery::Stop() {
  std::shared_ptr<thread::PermanentEvent> event;
  {
    thread::MutexLock lock(&mu_);
    stopped_ = true;
    event = std::exchange(changed_,
                          std::make_shared<thread::PermanentEvent>());
  }
  event->Notify();
}

void PathMtuDiscovery::ReportSendFailure() {
  std::shared_ptr<thread::PermanentEvent> event;
  {
    thread::MutexLock lock(&mu_);
    if (state_ == PathMtuState::kDisabled || state_ == PathMtuState::kError) {
      return;
    }
    if (++failures_ < options_.black_hole_threshold) {
      return;
    }
    // Latched rather than counted from here: the run loop resets it when it
    // acts, so a burst of failures arriving while a search is already underway
    // does not queue up several searches.
    failures_ = 0;
    if (black_hole_) {
      return;
    }
    black_hole_ = true;
    event = std::exchange(changed_,
                          std::make_shared<thread::PermanentEvent>());
  }
  if (event != nullptr) {
    event->Notify();
  }
}

void PathMtuDiscovery::ReportSendSuccess() {
  thread::MutexLock lock(&mu_);
  failures_ = 0;
}

void PathMtuDiscovery::RestoreConfirmed() {
  size_t confirmed = 0;
  {
    thread::MutexLock lock(&mu_);
    confirmed = confirmed_;
  }
  if (confirmed != 0) {
    (void)prober_.apply(confirmed);
  }
}

bool PathMtuDiscovery::ProbeCandidate(size_t candidate,
                                      bool* absl_nonnull indeterminate) {
  *indeterminate = false;
  bool acknowledged = false;
  for (int attempt = 0; attempt < options_.max_probes && !acknowledged;
       ++attempt) {
    const absl::Status applied = prober_.apply(candidate);
    if (!applied.ok()) {
      // The stack would not take the size. That says nothing about the path, so
      // the candidate stays untested rather than being recorded as too large --
      // treating a startup race as evidence would converge on a tiny MTU and
      // stay there.
      *indeterminate = true;
      break;
    }
    {
      thread::MutexLock lock(&mu_);
      ++probes_sent_;
    }
    // A burst, not a probe. Several probes in flight together is what stresses
    // path reassembly the way real traffic does; probes that wait for each other
    // are each isolated, and a size that only survives in isolation would be
    // confirmed and then stall the stream.
    const size_t payload = candidate - options_.probe_overhead;
    const absl::Time deadline = absl::Now() + options_.probe_timeout;
    std::optional<bool> answered;
    if (prober_.probe_burst) {
      const std::optional<int> returned =
          prober_.probe_burst(payload, options_.confirm_burst, deadline);
      if (returned.has_value()) {
        answered = *returned >= options_.confirm_burst;
      }
    } else if (prober_.probe) {
      answered = prober_.probe(payload, deadline);
    }
    if (!answered.has_value()) {
      // Could not be sent. Same reasoning as a refused apply: no evidence.
      *indeterminate = true;
      break;
    }
    if (*answered) {
      acknowledged = true;
      break;
    }
    // Failed: back to the confirmed size. That restore is also what repairs any
    // application data caught by the raise -- see the note on the prober's
    // `pause` for why nothing has to be held back.
    RestoreConfirmed();
    thread::MutexLock lock(&mu_);
    if (stopped_) {
      break;
    }
  }
  if (!acknowledged) {
    RestoreConfirmed();
  }
  return acknowledged;
}

size_t PathMtuDiscovery::Search() {
  {
    thread::MutexLock lock(&mu_);
    if (stopped_ || state_ == PathMtuState::kError) {
      return confirmed_;
    }
    black_hole_ = false;
    failures_ = 0;
    confirmed_ = 0;
  }

  // The base first, and it is not a formality. Everything above is compared
  // against it, and a base that does not arrive means the path is not carrying
  // what every conforming path must -- which is a connectivity failure and not
  // an MTU to search for.
  SetState(PathMtuState::kBase);
  bool indeterminate = false;
  if (!ProbeCandidate(options_.base_mtu, &indeterminate)) {
    if (indeterminate) {
      // Could not even be attempted. Leave the state alone so the caller retries
      // rather than declaring a dead path from a transport that was not ready.
      return 0;
    }
    // Give up probing. **Do not touch the stream.**
    //
    // The first version of this reported a base-MTU failure as a connectivity
    // failure and aborted the stream, on the reasoning that every conforming path
    // carries 1280 so failing it means the path is dead. That reasoning ignores
    // where the evidence comes from: probes ride an *unreliable* channel, and
    // under load their acknowledgements are exactly what gets dropped. It aborted
    // streams that were carrying application data at full rate, which is as wrong
    // as a transport error can be -- the stream itself is the proof the path
    // works.
    //
    // A dead association is SCTP's business and SCTP already detects it. All this
    // can honestly conclude is "cannot discover right now", and the response to
    // that is to keep the base MTU -- the safe value -- and try again later.
    LOG(INFO) << "a11 webrtc: path MTU probing gave up at the base MTU of "
              << options_.base_mtu
              << " bytes; keeping it and leaving the stream alone";
    SetState(PathMtuState::kError);
    return 0;
  }
  {
    thread::MutexLock lock(&mu_);
    confirmed_ = options_.base_mtu;
  }
  (void)prober_.apply(options_.base_mtu);

  // Binary search between a size that answered and one that has not. `ceiling`
  // is exclusive-ish: it is the smallest size known (or assumed) not to work,
  // starting one past the configured maximum so the maximum itself gets tried.
  SetState(PathMtuState::kSearching);
  size_t low = options_.base_mtu;
  size_t ceiling = options_.max_mtu + 1;
  while (ceiling - low > options_.granularity) {
    {
      thread::MutexLock lock(&mu_);
      if (stopped_) {
        break;
      }
    }
    const size_t candidate = low + (ceiling - low) / 2;
    if (candidate <= low) {
      break;
    }
    if (ProbeCandidate(candidate, &indeterminate)) {
      low = candidate;
      {
        thread::MutexLock lock(&mu_);
        confirmed_ = candidate;
      }
      // Left applied: it is confirmed, so this is where the association should
      // sit whether or not the search continues or is stopped mid-way.
      (void)prober_.apply(candidate);
      continue;
    }
    if (indeterminate) {
      // Cannot learn anything right now; stop rather than spin, keeping whatever
      // is confirmed. The raise timer will try again.
      break;
    }
    ceiling = candidate;
  }

  RestoreConfirmed();
  SetState(PathMtuState::kSearchComplete);
  size_t confirmed = 0;
  {
    thread::MutexLock lock(&mu_);
    confirmed = confirmed_;
  }
  VLOG(1) << "a11 webrtc: path MTU settled at " << confirmed << " bytes after "
          << probes_sent() << " probes";
  return confirmed;
}

void PathMtuDiscovery::Run() {
  while (true) {
    {
      thread::MutexLock lock(&mu_);
      if (stopped_) {
        return;
      }
    }
    const size_t confirmed = Search();
    {
      thread::MutexLock lock(&mu_);
      if (stopped_ || state_ == PathMtuState::kError) {
        return;
      }
    }
    // Nothing confirmed and not an error means the transport was not ready --
    // no association yet, so no size could even be applied. Come back soon
    // rather than sitting out the raise timer, which would leave the connection
    // at the base MTU for ten minutes because the probe channel happened to be
    // ready before SCTP was.
    if (confirmed == 0) {
      std::shared_ptr<thread::PermanentEvent> changed;
      {
        thread::MutexLock lock(&mu_);
        changed = changed_;
      }
      const int selected = thread::SelectUntil(
          absl::Now() + options_.startup_retry,
          {thread::OnCancel(), changed->OnEvent()});
      if (selected == 0) {
        return;
      }
      continue;
    }

    // Between searches, wait for whichever comes first: the raise timer, because
    // the path may have grown and nothing else would notice; or a black-hole
    // report, because it certainly shrank. A shrink is urgent and skips straight
    // to the base -- restoring connectivity comes before finding the best size,
    // and searching from a size that is currently black-holing would spend every
    // probe on the failure.
    const absl::Time wake = absl::Now() + options_.raise_timer;
    while (true) {
      std::shared_ptr<thread::PermanentEvent> changed;
      {
        thread::MutexLock lock(&mu_);
        if (stopped_) {
          return;
        }
        if (black_hole_) {
          break;
        }
        changed = changed_;
      }
      const int selected =
          thread::SelectUntil(wake, {thread::OnCancel(), changed->OnEvent()});
      if (selected == 0) {
        return;  // Cancelled.
      }
      if (selected < 0) {
        break;  // Raise timer expired.
      }
    }
    bool shrank = false;
    {
      thread::MutexLock lock(&mu_);
      shrank = black_hole_;
    }
    if (shrank) {
      LOG(INFO) << "a11 webrtc: path MTU black hole detected at "
                << confirmed_mtu() << " bytes; falling back to "
                << options_.base_mtu;
      (void)prober_.apply(options_.base_mtu);
      thread::MutexLock lock(&mu_);
      confirmed_ = options_.base_mtu;
    }
  }
}

}  // namespace a11::net::internal
