// Copyright 2026 The Action Engine Authors.
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

#ifndef THREAD_FIBER_SELECTABLES_H_
#define THREAD_FIBER_SELECTABLES_H_

#include <atomic>

#include <absl/base/thread_annotations.h>
#include <absl/log/check.h>

#include "thread/boost_primitives.h"
#include "thread/cases.h"

namespace thread {
// PermanentEvent --------------- Provides a level-triggered event which may be
// added to a Select statement.
class PermanentEvent final : public internal::Selectable {
 public:
  PermanentEvent() = default;

  ~PermanentEvent() override {
    thread::MutexLock lock(&mu_);
    DCHECK(cases_to_be_selected_ == nullptr);
  }

  PermanentEvent(const PermanentEvent&) = delete;
  PermanentEvent& operator=(const PermanentEvent&) = delete;

  // Signal that the event has occurred. Any Selectors on this event will be
  // immediately notified, future Select statements against this event will be
  // non-blocking.  May only be called once.
  void Notify();

  // Returns true if Notify() has been called. False otherwise.
  bool HasBeenNotified() const;

  // May be passed to Select. Will always evaluate immediately for an event
  // that has already been notified. Once the case has been signalled, then
  // deleting the PermanentEvent will not interfere with the caller of Notify().
  Case OnEvent() const {
    Case c = {const_cast<PermanentEvent*>(this)};
    return c;
  }

  // Implementation of Selectable interface.
  bool Handle(internal::CaseInSelectClause* absl_nonnull case_state,
              bool enqueue) override;
  void Unregister(
      internal::CaseInSelectClause* absl_nonnull case_state) override;

 private:
  friend class Fiber;

  mutable thread::Mutex mu_;
  std::atomic<bool> notified_{false};

  internal::CaseInSelectClause* cases_to_be_selected_ ABSL_GUARDED_BY(mu_) =
      nullptr;
};

// NonSelectableCase() ------------------- Provides a 'null' case which will
// never evaluate as ready by Select.
Case NonSelectableCase();

// AlwaysSelectableCase() ---------------------- Provides case which will always
// evaluate as ready by Select.
Case AlwaysSelectableCase();
}  // namespace thread

#endif  // THREAD_FIBER_SELECTABLES_H_
