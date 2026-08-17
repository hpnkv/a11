// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The definition of a11::exception_guard::Wrap, for boundary units.
 *
 * Include this **only** from a translation unit compiled with exceptions -- one
 * of the `boundary.cc` files named in the exception policy block of
 * cpp/CMakeLists.txt -- and follow it with an explicit instantiation of every
 * signature that library adopts. Including it anywhere else is a compile error
 * on the `try` below, which is the point: the wrapper's body has to live where
 * exceptions exist, or it protects nothing. See a11/exception_guard.h for why.
 */

#ifndef A11_INTERNAL_EXCEPTION_GUARD_IMPL_H_
#define A11_INTERNAL_EXCEPTION_GUARD_IMPL_H_

#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/exception_guard.h"

namespace a11::exception_guard {
namespace internal {

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

}  // namespace internal

template <typename Result, typename... Args>
std::function<Result(Args...)> Wrap(std::function<Result(Args...)> callable,
                                    std::string_view what) {
  if (!callable) {
    return callable;
  }
  return [callable = std::move(callable), what = std::string(what)](
             Args... args) -> Result {
    try {
      return callable(std::forward<Args>(args)...);
    } catch (const std::exception& error) {
      return internal::Failure<Result>::From(internal::Raised(error, what));
    } catch (...) {
      return internal::Failure<Result>::From(internal::RaisedUnknown(what));
    }
  };
}

template <typename Result, typename... Args>
absl::AnyInvocable<Result(Args...)> WrapOnce(
    absl::AnyInvocable<Result(Args...)> callable, std::string_view what) {
  if (!callable) {
    return callable;
  }
  return [callable = std::move(callable), what = std::string(what)](
             Args... args) mutable -> Result {
    try {
      return callable(std::forward<Args>(args)...);
    } catch (const std::exception& error) {
      return internal::Failure<Result>::From(internal::Raised(error, what));
    } catch (...) {
      return internal::Failure<Result>::From(internal::RaisedUnknown(what));
    }
  };
}

template <typename Result, typename... Args>
absl::AnyInvocable<Result(Args...) &&> WrapConsuming(
    absl::AnyInvocable<Result(Args...) &&> callable, std::string_view what) {
  if (!callable) {
    return callable;
  }
  return [callable = std::move(callable), what = std::string(what)](
             Args... args) mutable -> Result {
    try {
      return std::move(callable)(std::forward<Args>(args)...);
    } catch (const std::exception& error) {
      return internal::Failure<Result>::From(internal::Raised(error, what));
    } catch (...) {
      return internal::Failure<Result>::From(internal::RaisedUnknown(what));
    }
  };
}

}  // namespace a11::exception_guard

#endif  // A11_INTERNAL_EXCEPTION_GUARD_IMPL_H_
