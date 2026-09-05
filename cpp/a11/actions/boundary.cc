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
 * @brief The actions library's exception boundary.
 *
 * Compiled with exceptions (see the exception policy block in
 * cpp/CMakeLists.txt). Nothing here but the wrappers; a11/exception_guard.h
 * explains why they cannot live at the call sites.
 */

#include <memory>
#include <regex>
#include <string>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>

#include "a11/actions/action.h"
#include "a11/actions/internal/exception_guarded_handlers.h"
#include "a11/actions/internal/pattern.h"
#include "a11/concurrency/internal/exception_guard_future.h"
#include "a11/internal/exception_guard_impl.h"

namespace a11::actions::internal {

absl::StatusOr<std::regex> CompilePattern(const std::string& pattern) {
  try {
    return std::regex(pattern, std::regex::ECMAScript);
  } catch (const std::regex_error& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Not a regular expression: ", error.what()));
  }
}

ActionHandler GuardHandler(ActionHandler handler) {
  return exception_guard::Wrap<a11::Task, std::shared_ptr<Action>>(
      std::move(handler), "Action handler");
}

SyncActionHandler GuardSyncHandler(SyncActionHandler handler) {
  return exception_guard::Wrap<absl::Status, std::shared_ptr<Action>>(
      std::move(handler), "Action handler");
}

OnActionCancelled GuardOnCancelled(OnActionCancelled callback) {
  return exception_guard::Wrap<absl::Status, std::shared_ptr<Action>>(
      std::move(callback), "Action cancel callback");
}

}  // namespace a11::actions::internal
