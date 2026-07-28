// Copyright 2026 The A11 Authors.

#ifndef A11_TIME_H_
#define A11_TIME_H_

#include <cstdint>

#include <absl/status/statusor.h>
#include <absl/time/time.h>

namespace a11 {

// A11 time values intentionally use Abseil's nanosecond-aware infinities.
using Duration = absl::Duration;
using Time = absl::Time;

Duration ZeroDuration();
Duration InfiniteDuration();
Time Now();
Time InfiniteFuture();
Time InfinitePast();

absl::StatusOr<std::int64_t> DurationNanoseconds(Duration duration);
absl::StatusOr<std::int64_t> TimeNanosecondsSinceEpoch(Time time);

}  // namespace a11

#endif  // A11_TIME_H_
