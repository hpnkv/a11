// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief How a wrapped callable's return type carries a failure.
 *
 * Split out of a11/internal/exception_guard_impl.h because this half contains no
 * `try` and so compiles anywhere, while that one may only be included from a
 * translation unit built with exceptions. Two kinds of file need it:
 *
 *  - the boundary translation units, which get it through the impl header;
 *  - a11/exception_guard.cc, which defines `Failure<void>::From` and is compiled
 *    `-fno-exceptions` like the rest of A11.
 *
 * Without the split, that second file had to include the impl header and so
 * parse the `try` blocks in it. Whether that is an error depends on the compiler:
 * a `try` in an uninstantiated template body is rejected at parse time by
 * clang 17 or so and by GCC, and accepted by Apple clang 21 -- which is a build
 * that passes on one macOS and fails on another. Nothing here relies on that.
 */

#ifndef A11_INTERNAL_EXCEPTION_GUARD_FAILURE_H_
#define A11_INTERNAL_EXCEPTION_GUARD_FAILURE_H_

#include <absl/status/status.h>
#include <absl/status/statusor.h>

namespace a11::exception_guard::internal {

/**
 * How a wrapped callable's return type carries a failure.
 *
 * Specialised per return kind rather than assumed, because the kinds differ in
 * what they can even express: a Status returns the error, a Future is failed
 * with it (see a11/concurrency/internal/exception_guard_future.h), and a void
 * callback has nowhere to put it and so is logged. A return type with no
 * specialisation fails to compile, which is the right outcome -- silently
 * dropping an exception is exactly what this file exists to prevent.
 */
template <typename Result>
struct Failure;

template <>
struct Failure<absl::Status> {
  static absl::Status From(absl::Status status) { return status; }
};

template <typename T>
struct Failure<absl::StatusOr<T>> {
  static absl::StatusOr<T> From(absl::Status status) { return status; }
};

template <>
struct Failure<void> {
  static void From(absl::Status status);
};

}  // namespace a11::exception_guard::internal

#endif  // A11_INTERNAL_EXCEPTION_GUARD_FAILURE_H_
