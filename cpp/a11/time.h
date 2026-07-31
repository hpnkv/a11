// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief A11's time primitives: nanosecond-aware durations and instants.
 *
 * A11 reuses Abseil's @c absl::Duration and @c absl::Time directly, so its
 * time values carry Abseil's nanosecond resolution and its special
 * infinite-past / infinite-future / infinite-duration sentinels. The free
 * functions here provide named constructors for those sentinels and the
 * current wall-clock time, plus lossless conversions to integer nanoseconds
 * that fail (rather than truncate) on non-finite values.
 */

#ifndef A11_TIME_H_
#define A11_TIME_H_

#include <cstdint>

#include <absl/status/statusor.h>
#include <absl/time/time.h>

namespace a11 {

/** @brief A11 duration; an alias of @c absl::Duration (nanosecond-aware). */
using Duration = absl::Duration;
/** @brief A11 instant; an alias of @c absl::Time (nanosecond-aware). */
using Time = absl::Time;

/** @brief Returns the zero duration. */
Duration ZeroDuration();
/** @brief Returns the duration greater than every finite duration. */
Duration InfiniteDuration();
/** @brief Returns the current wall-clock time from Abseil's clock. */
Time Now();
/** @brief Returns the instant later than every finite time. */
Time InfiniteFuture();
/** @brief Returns the instant earlier than every finite time. */
Time InfinitePast();

/**
 * @brief Converts a duration to whole nanoseconds.
 * @param duration Duration to convert.
 * @return The nanosecond count, or an error status when @p duration is
 *         infinite and therefore not representable.
 */
absl::StatusOr<std::int64_t> DurationNanoseconds(Duration duration);
/**
 * @brief Converts an instant to nanoseconds since the Unix epoch.
 * @param time Instant to convert.
 * @return The nanoseconds since epoch, or an error status when @p time is
 *         infinite and therefore not representable.
 */
absl::StatusOr<std::int64_t> TimeNanosecondsSinceEpoch(Time time);

}  // namespace a11

#endif  // A11_TIME_H_
