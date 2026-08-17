// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Adoption guards for a Session's two stream callbacks.
 *
 * Implemented in a11/service/boundary.cc, which is compiled with exceptions for
 * this purpose. See a11/exception_guard.h.
 */

#ifndef A11_SERVICE_INTERNAL_EXCEPTION_GUARDED_CALLBACKS_H_
#define A11_SERVICE_INTERNAL_EXCEPTION_GUARDED_CALLBACKS_H_

#include "a11/service/session.h"

namespace a11::service::internal {

/// Wraps a session's message callback so a raised exception fails its Task.
[[nodiscard]] OnSessionStreamMessage GuardOnStreamMessage(
    OnSessionStreamMessage callback);
/// Wraps a session's done callback so a raised exception fails its Task.
[[nodiscard]] OnSessionStreamDone GuardOnStreamDone(
    OnSessionStreamDone callback);

}  // namespace a11::service::internal

#endif  // A11_SERVICE_INTERNAL_EXCEPTION_GUARDED_CALLBACKS_H_
