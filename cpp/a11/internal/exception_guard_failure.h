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
 * @brief How a wrapped callable's return type carries a failure.
 *
 * This header contains no `try`, so no-exceptions translation units can use the
 * failure traits without including exception_guard_impl.h.
 */

#ifndef A11_INTERNAL_EXCEPTION_GUARD_FAILURE_H_
#define A11_INTERNAL_EXCEPTION_GUARD_FAILURE_H_

#include <absl/status/status.h>
#include <absl/status/statusor.h>

namespace a11::exception_guard::internal {

/**
 * How a wrapped callable's return type carries a failure.
 *
 * Status values return the error, futures fail with it, and void callbacks log
 * it. Unsupported result types fail to compile.
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
  static void From(const absl::Status& status);
};

}  // namespace a11::exception_guard::internal

#endif  // A11_INTERNAL_EXCEPTION_GUARD_FAILURE_H_
