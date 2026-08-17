// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Adoption guards for the handlers an Action runs.
 *
 * An action handler is the application's, and an Action invokes it from a fibre
 * of A11's own -- so whatever it throws has to become a Status inside the
 * wrapper's frame. Implemented in a11/actions/boundary.cc, which is compiled
 * with exceptions for this purpose. See a11/exception_guard.h.
 */

#ifndef A11_ACTIONS_INTERNAL_EXCEPTION_GUARDED_HANDLERS_H_
#define A11_ACTIONS_INTERNAL_EXCEPTION_GUARDED_HANDLERS_H_

#include "a11/actions/action.h"

namespace a11::actions::internal {

/// Wraps an async handler so a raised exception fails the Task it owes.
[[nodiscard]] ActionHandler GuardHandler(ActionHandler handler);
/// Wraps a synchronous handler so a raised exception becomes its Status.
[[nodiscard]] SyncActionHandler GuardSyncHandler(SyncActionHandler handler);
/// Wraps a cancellation callback the same way.
[[nodiscard]] OnActionCancelled GuardOnCancelled(OnActionCancelled callback);

}  // namespace a11::actions::internal

#endif  // A11_ACTIONS_INTERNAL_EXCEPTION_GUARDED_HANDLERS_H_
