// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Lets exception_guard::Wrap wrap a callable returning a Future or Task.
 *
 * Separate from a11/internal/exception_guard_impl.h so that the core library's
 * guard needs no knowledge of futures: a11_data adopts codecs returning
 * StatusOr and must not gain a dependency on a11_concurrency to do it. Include
 * this alongside exception_guard_impl.h in a boundary translation unit whose
 * callables return Tasks.
 */

#ifndef A11_CONCURRENCY_INTERNAL_EXCEPTION_GUARD_FUTURE_H_
#define A11_CONCURRENCY_INTERNAL_EXCEPTION_GUARD_FUTURE_H_

#include <utility>

#include <absl/status/status.h>

#include "a11/concurrency/future.h"
#include "a11/internal/exception_guard_impl.h"

namespace a11::exception_guard::internal {

/// A callable that promised a Future fails it, which is what its awaiter reads.
template <typename T>
struct Failure<a11::Future<T>> {
  static a11::Future<T> From(absl::Status status) {
    return a11::FailedFuture<T>(std::move(status));
  }
};

}  // namespace a11::exception_guard::internal

#endif  // A11_CONCURRENCY_INTERNAL_EXCEPTION_GUARD_FUTURE_H_
