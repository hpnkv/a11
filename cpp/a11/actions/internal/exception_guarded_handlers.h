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
 * @brief Adoption guards for the handlers an Action runs.
 *
 * An action handler is the application's, and an Action invokes it from a fiber
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
