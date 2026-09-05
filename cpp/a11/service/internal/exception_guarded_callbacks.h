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
