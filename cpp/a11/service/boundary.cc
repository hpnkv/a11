// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file
 * @brief The service library's exception boundary.
 *
 * Compiled with exceptions (see the exception policy block in
 * cpp/CMakeLists.txt). A session's two stream callbacks belong to whoever
 * created the session -- in practice a Python agent through the bindings -- and
 * the session invokes them from its own fibers, so they are wrapped here where
 * a raised exception can still be caught. a11/exception_guard.h has the
 * reasoning.
 */

#include <memory>
#include <optional>
#include <utility>

#include "a11/concurrency/internal/exception_guard_future.h"
#include "a11/data/types.h"
#include "a11/internal/exception_guard_impl.h"
#include "a11/net/wire_stream.h"
#include "a11/service/internal/exception_guarded_callbacks.h"
#include "a11/service/session.h"

namespace a11::service::internal {

OnSessionStreamMessage GuardOnStreamMessage(OnSessionStreamMessage callback) {
  return exception_guard::Wrap<a11::Task, std::optional<data::WireMessage>,
                               std::shared_ptr<net::WireStream>,
                               std::shared_ptr<Session>>(
      std::move(callback), "Session message callback");
}

OnSessionStreamDone GuardOnStreamDone(OnSessionStreamDone callback) {
  return exception_guard::Wrap<a11::Task, std::shared_ptr<net::WireStream>,
                               std::shared_ptr<Session>>(
      std::move(callback), "Session done callback");
}

}  // namespace a11::service::internal
