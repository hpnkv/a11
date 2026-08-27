// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The definition of a11::exception_guard::Wrap, for boundary units.
 *
 * Include this **only** from a translation unit compiled with exceptions -- one
 * of the `boundary.cc` files named in the exception policy block of
 * cpp/CMakeLists.txt -- and follow it with an explicit instantiation of every
 * signature that library adopts. The wrapper's body has to live where
 * exceptions
 * exist, or it protects nothing. See a11/exception_guard.h for why.
 *
 * Including it anywhere else is the `#error` below rather than a diagnostic on
 * one of the `try` blocks, because whether *that* is an error depends on the
 * compiler: a `try` in an uninstantiated template body is rejected at parse
 * time
 * by GCC and by clang up to around 17, and accepted by Apple clang 21. Relying
 * on
 * it meant a build that passed on one macOS and failed on another, which is
 * exactly what happened. If you need the `Failure` trait and not the wrappers
 * --
 * a11/exception_guard.cc does -- include
 * a11/internal/exception_guard_failure.h, which compiles anywhere.
 */

#ifndef A11_INTERNAL_EXCEPTION_GUARD_IMPL_H_
#define A11_INTERNAL_EXCEPTION_GUARD_IMPL_H_

#if !defined(__cpp_exceptions) && !defined(__EXCEPTIONS)
#error \
    "a11/internal/exception_guard_impl.h needs exceptions. Include it only from a boundary translation unit named in the exception policy block of cpp/CMakeLists.txt; for the Failure trait alone use a11/internal/exception_guard_failure.h."
#endif

#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/exception_guard.h"
#include "a11/internal/exception_guard_failure.h"

namespace a11::exception_guard {

template <typename Result, typename... Args>
std::function<Result(Args...)> Wrap(std::function<Result(Args...)> callable,
                                    std::string_view what) {
  if (!callable) {
    return callable;
  }
  return [callable = std::move(callable),
          what = std::string(what)](Args... args) -> Result {
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
  return [callable = std::move(callable),
          what = std::string(what)](Args... args) mutable -> Result {
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
  return [callable = std::move(callable),
          what = std::string(what)](Args... args) mutable -> Result {
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
