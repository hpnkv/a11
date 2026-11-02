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

#include "sdk/flow/actions/stop.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/concurrency/executor.h"
#include "a11/nodes/async_node.h"
#include "sdk/flow/actions/options.h"
#include "sdk/flow/actions/ports.h"
#include "thread/concurrency.h"

namespace a11::sdk::flow {
namespace {

bool IsStopCommand(const nlohmann::json& command) {
  if (command.is_string()) return command.get<std::string>() == "stop";
  const auto found = command.is_object() ? command.find("command")
                                         : command.end();
  return found != command.end() && found->is_string() &&
         found->get<std::string>() == "stop";
}

}  // namespace

void StopSignal::Shared::Stop(StopReason next) {
  if (next == StopReason::kRunning) return;
  int expected = static_cast<int>(StopReason::kRunning);
  if (reason.compare_exchange_strong(expected, static_cast<int>(next),
                                     std::memory_order_acq_rel)) {
    stopped.Notify();
  }
  Finish();
}

void StopSignal::Shared::Finish() {
  if (!finishing.exchange(true, std::memory_order_acq_rel)) finished.Notify();
}

absl::StatusOr<std::shared_ptr<StopSignal>> StopSignal::Create(
    const std::shared_ptr<actions::Action>& action,
    std::string_view control_port, OnControlCommand on_command) {
  if (action == nullptr) return absl::InvalidArgumentError("an action is required");
  auto signal = std::shared_ptr<StopSignal>(new StopSignal());
  ABSL_ASSIGN_OR_RETURN(std::optional<data::Bytes> raw,
                        action->GetHeader(absl::StrCat(
                            actions::kActionHeaderPrefix, "deadline")));
  if (raw.has_value()) {
    ABSL_ASSIGN_OR_RETURN(signal->deadline_, ParseDeadlineHeader(*raw));
    if (signal->deadline_ <= absl::Now()) {
      return absl::DeadlineExceededError("x-a11-deadline has already passed");
    }
  }
  if (!control_port.empty()) {
    ABSL_ASSIGN_OR_RETURN(signal->control_,
                          action->GetInput(std::string(control_port)));
  }
  const auto shared = signal->shared_;
  const auto control = signal->control_;
  ABSL_RETURN_IF_ERROR(action->SetOnCancelled(
      [shared, control](const std::shared_ptr<actions::Action>&) {
        shared->Stop(StopReason::kCancelled);
        if (control != nullptr) control->CancelReader();
        return absl::OkStatus();
      }));
  if (control != nullptr) {
    signal->control_task_ = a11::SubmitTask(
        [shared, control, on_command = std::move(on_command)]() -> absl::Status {
          while (!shared->finishing.load(std::memory_order_acquire)) {
            auto command = ReadJsonInput(control);
            if (!command.ok() || !command->has_value()) return absl::OkStatus();
            if (IsStopCommand(**command)) {
              shared->Stop(StopReason::kControl);
              return absl::OkStatus();
            }
            if (on_command == nullptr) {
              shared->Stop(StopReason::kControl);
              return absl::InvalidArgumentError("unknown control command");
            }
            ABSL_RETURN_IF_ERROR(on_command(**command));
          }
          return absl::OkStatus();
        });
  }
  return signal;
}

StopSignal::~StopSignal() { Join(); }

bool StopSignal::stopped() const {
  return shared_->reason.load(std::memory_order_acquire) !=
         static_cast<int>(StopReason::kRunning);
}

StopReason StopSignal::reason() const {
  return static_cast<StopReason>(shared_->reason.load(std::memory_order_acquire));
}

absl::Status StopSignal::Check() const {
  switch (reason()) {
    case StopReason::kRunning: return absl::OkStatus();
    case StopReason::kControl: return absl::CancelledError("stopped by a control command");
    case StopReason::kDeadline: return absl::DeadlineExceededError("x-a11-deadline passed");
    case StopReason::kCancelled: return absl::CancelledError("cancelled");
  }
  return absl::UnknownError("unknown stop reason");
}

absl::Status StopSignal::ExitStatus() const {
  return reason() == StopReason::kCancelled
             ? absl::CancelledError("cancelled") : absl::OkStatus();
}

bool StopSignal::WaitFor(absl::Duration duration) {
  return WaitUntil(absl::Now() + duration);
}

bool StopSignal::WaitUntil(absl::Time target) {
  if (stopped()) return true;
  const absl::Time until = std::min(target, deadline_);
  const int ready = thread::SelectUntil(
      until, {thread::OnCancel(), shared_->stopped.OnEvent()});
  if (ready == 0) shared_->Stop(StopReason::kCancelled);
  if (deadline_ <= absl::Now() && reason() == StopReason::kRunning) {
    shared_->Stop(StopReason::kDeadline);
  }
  return stopped();
}

absl::Time StopSignal::deadline() const { return deadline_; }
bool StopSignal::has_deadline() const { return deadline_ < absl::InfiniteFuture(); }
thread::Case StopSignal::OnStop() const { return shared_->stopped.OnEvent(); }
void StopSignal::Stop(StopReason reason) { shared_->Stop(reason); }

void StopSignal::Join() {
  if (joined_.exchange(true, std::memory_order_acq_rel)) return;
  shared_->Finish();
  if (control_ != nullptr) control_->CancelReader();
  control_task_.Await().IgnoreError();
}

}  // namespace a11::sdk::flow
