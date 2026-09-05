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

#include "a11/exception_guard.h"

#include <exception>
#include <string_view>

#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/strings/str_cat.h>

#include "a11/internal/exception_guard_failure.h"

namespace a11::exception_guard::internal {

// This no-exceptions translation unit uses the Failure trait without including
// exception_guard_impl.h, whose wrappers are instantiated at throwing
// boundaries.

absl::Status Raised(const std::exception& error, std::string_view what) {
  return absl::UnknownError(absl::StrCat(what, " raised: ", error.what()));
}

absl::Status RaisedUnknown(std::string_view what) {
  return absl::UnknownError(
      absl::StrCat(what, " raised a non-standard exception"));
}

/// @cond INTERNAL
void Failure<void>::From(const absl::Status& status) {
  LOG(ERROR) << status.message();
}

/// @endcond

}  // namespace a11::exception_guard::internal
