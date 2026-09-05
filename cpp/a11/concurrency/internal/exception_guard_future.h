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
// The trait alone, not the wrappers: this header only adds a specialisation,
// and taking the `try`-bearing impl header for it would make this includable
// only where exceptions are on.
#include "a11/internal/exception_guard_failure.h"

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
