// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The actions library's exception boundary.
 *
 * Compiled with exceptions (see the exception policy block in
 * cpp/CMakeLists.txt). Nothing here but the wrappers; a11/exception_guard.h
 * explains why they cannot live at the call sites.
 */

#include "a11/actions/internal/exception_guarded_handlers.h"

#include <memory>
#include <utility>

#include <absl/status/status.h>

#include "a11/actions/action.h"
#include "a11/concurrency/internal/exception_guard_future.h"
#include "a11/internal/exception_guard_impl.h"

namespace a11::actions::internal {

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
