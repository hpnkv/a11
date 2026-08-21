// Copyright 2026 The A11 Authors.

#include "a11/exception_guard.h"

#include <exception>
#include <string_view>

#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/strings/str_cat.h>

#include "a11/internal/exception_guard_failure.h"

namespace a11::exception_guard::internal {

// This no-exceptions translation unit uses the Failure trait without including
// exception_guard_impl.h, whose wrappers are instantiated at throwing boundaries.

absl::Status Raised(const std::exception& error, std::string_view what) {
  return absl::UnknownError(absl::StrCat(what, " raised: ", error.what()));
}

absl::Status RaisedUnknown(std::string_view what) {
  return absl::UnknownError(
      absl::StrCat(what, " raised a non-standard exception"));
}

/// @cond INTERNAL
void Failure<void>::From(absl::Status status) {
  LOG(ERROR) << status.message();
}
/// @endcond

}  // namespace a11::exception_guard::internal
