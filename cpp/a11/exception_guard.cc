// Copyright 2026 The A11 Authors.

#include "a11/exception_guard.h"

#include <exception>
#include <string_view>

#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/strings/str_cat.h>

#include "a11/internal/exception_guard_failure.h"

namespace a11::exception_guard::internal {

// Compiled without exceptions, like the rest of A11: naming an exception type
// and reading what() need no more than the type's declaration. Only the frame
// that *catches* needs them, and that is the wrapper in guard_impl.h, which the
// boundary translation units instantiate -- so this file takes the Failure trait
// from exception_guard_failure.h and must not include guard_impl.h, whose `try`
// blocks some compilers reject at parse time even uninstantiated.

absl::Status Raised(const std::exception& error, std::string_view what) {
  return absl::UnknownError(absl::StrCat(what, " raised: ", error.what()));
}

absl::Status RaisedUnknown(std::string_view what) {
  return absl::UnknownError(
      absl::StrCat(what, " raised a non-standard exception"));
}

void Failure<void>::From(absl::Status status) {
  // A void callback's caller had nowhere to return this to before either; the
  // log is what the sites this replaces already did.
  LOG(ERROR) << status.message();
}

}  // namespace a11::exception_guard::internal
