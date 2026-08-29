// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The concurrency library's exception boundary.
 *
 * Compiled with exceptions (see the exception policy block in
 * cpp/CMakeLists.txt) and holding nothing but the instantiations of
 * a11::exception_guard::Wrap for the callables this library adopts from a
 * caller. `Schedule` and `ScheduleCancelable` take work from anywhere,
 * including a caller built with exceptions, and run it on a fiber whose frame
 * is one of A11's -- so the wrap has to be compiled here, where the catch can
 * exist. a11/exception_guard.h has the reasoning.
 *
 * The templates that take a caller's work through a *header* -- `Submit<T>`,
 * `Future<T>::OnReady` -- are not here and cannot be: they are instantiated by
 * whoever calls them, which is where their guard belongs. They use
 * `exception_guard::Attempt` instead.
 */

#include <absl/functional/any_invocable.h>

#include "a11/internal/exception_guard_impl.h"

namespace a11::exception_guard {

/// For a11::Schedule and a11::ScheduleCancelable: work run once and discarded.
template absl::AnyInvocable<void() &&> WrapConsuming<void>(
    absl::AnyInvocable<void() &&>, std::string_view);

}  // namespace a11::exception_guard
