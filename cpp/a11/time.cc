// Copyright 2026 The A11 Authors.

#include "a11/time.h"

#include <cstdint>

#include <absl/time/clock.h>
#include <absl/time/time.h>

namespace a11 {

Duration ZeroDuration() {
  return absl::ZeroDuration();
}

Duration InfiniteDuration() {
  return absl::InfiniteDuration();
}

Time Now() {
  return absl::Now();
}

Time InfiniteFuture() {
  return absl::InfiniteFuture();
}

Time InfinitePast() {
  return absl::InfinitePast();
}

absl::StatusOr<std::int64_t> DurationNanoseconds(Duration duration) {
  if (duration == absl::InfiniteDuration() ||
      duration == -absl::InfiniteDuration()) {
    return absl::OutOfRangeError("duration is infinite");
  }
  const std::int64_t nanoseconds = absl::ToInt64Nanoseconds(duration);
  if (absl::Nanoseconds(nanoseconds) != duration) {
    return absl::OutOfRangeError("duration does not fit in int64 nanoseconds");
  }
  return nanoseconds;
}

absl::StatusOr<std::int64_t> TimeNanosecondsSinceEpoch(Time time) {
  if (time == absl::InfiniteFuture() || time == absl::InfinitePast()) {
    return absl::OutOfRangeError("time is infinite");
  }
  const absl::Duration since_epoch = time - absl::UnixEpoch();
  return DurationNanoseconds(since_epoch);
}

}  // namespace a11
