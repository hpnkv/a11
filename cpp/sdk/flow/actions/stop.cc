// Copyright 2026 The A11 Authors.

#include "sdk/flow/actions/stop.h"

#include <cstdint>
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
#include <uvw/timer.h>

#include "a11/actions/action.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/nodes/async_node.h"
#include "a11/uv/loop.h"
#include "sdk/flow/actions/options.h"
#include "sdk/flow/actions/ports.h"
#include "thread/concurrency.h"

namespace a11::sdk::flow {
namespace {

using ::a11::nodes::AsyncNode;

/// Reads `x-a11-deadline`, or InfiniteFuture when the caller set none.
absl::StatusOr<absl::Time> DeadlineFromAction(
    const std::shared_ptr<actions::Action>& action) {
  ABSL_ASSIGN_OR_RETURN(std::optional<data::Bytes> raw,
                        action->GetHeader(absl::StrCat(
                            actions::kActionHeaderPrefix, "deadline")));
  if (!raw.has_value()) {
    return absl::InfiniteFuture();
  }
  return ParseDeadlineHeader(*raw);
}

/// Whether a control command asked the action to stop. Both `"stop"` and
/// `{"command": "stop"}` read, because a flow writes the second and a test
/// writes the first.
bool IsStopCommand(const nlohmann::json& command) {
  if (command.is_string()) {
    return command.get<std::string>() == "stop";
  }
  if (!command.is_object() || !command.contains("command")) {
    return false;
  }
  const nlohmann::json& verb = command.at("command");
  return verb.is_string() && verb.get<std::string>() == "stop";
}

}  // namespace

void StopSignal::Shared::Stop(StopReason next) {
  if (next == StopReason::kRunning) {
    return;
  }
  int expected = static_cast<int>(StopReason::kRunning);
  // The first reason wins: a deadline firing while a cancellation is being
  // applied must not turn `cancelled` into a graceful finish.
  if (reason.compare_exchange_strong(expected, static_cast<int>(next),
                                     std::memory_order_acq_rel)) {
    stopped.Notify();
  }
  Finish();
}

void StopSignal::Shared::Finish() {
  if (!finishing.exchange(true, std::memory_order_acq_rel)) {
    finished.Notify();
  }
}

absl::StatusOr<std::shared_ptr<StopSignal>> StopSignal::Create(
    const std::shared_ptr<actions::Action>& action,
    std::string_view control_port, OnControlCommand on_command) {
  if (action == nullptr) {
    return absl::InvalidArgumentError("an action is required");
  }
  auto signal = std::shared_ptr<StopSignal>(new StopSignal());
  ABSL_ASSIGN_OR_RETURN(signal->deadline_, DeadlineFromAction(action));
  if (signal->deadline_ <= absl::Now()) {
    return absl::DeadlineExceededError("x-a11-deadline has already passed");
  }
  const std::shared_ptr<Shared> shared = signal->shared_;

  if (!control_port.empty()) {
    ABSL_ASSIGN_OR_RETURN(signal->control_,
                          action->GetInput(std::string(control_port)));
  }
  const std::shared_ptr<AsyncNode> control = signal->control_;

  // Runs on a foreign thread, before the handler's fibre is cancelled, so it
  // must not block: Stop() is an atomic exchange and a notify, and CancelReader
  // only unblocks the control watcher.
  ABSL_RETURN_IF_ERROR(action->SetOnCancelled(
      [shared,
       control](const std::shared_ptr<actions::Action>&) -> absl::Status {
        shared->Stop(StopReason::kCancelled);
        if (control != nullptr) {
          control->CancelReader();
        }
        return absl::OkStatus();
      }));

  // A deadline nobody watches only fires when the action next asks, which for
  // an action parked in a read is never. The timer exists exactly when there is
  // a deadline to watch, and Join() disarms it.
  if (signal->has_deadline()) {
    ABSL_RETURN_IF_ERROR(signal->ArmDeadlineTimer());
  }

  if (control != nullptr) {
    signal->control_task_ =
        a11::SubmitTask([shared, control,
                         on_command = std::move(on_command)]() -> absl::Status {
          while (!shared->finishing.load(std::memory_order_acquire)) {
            absl::StatusOr<std::optional<nlohmann::json>> command =
                ReadJsonInput(control);
            if (!command.ok()) {
              // The reader was cancelled during teardown, which is how this
              // fibre is meant to end.
              return absl::OkStatus();
            }
            if (!command->has_value()) {
              // The control stream ended without a stop. This is not a
              // reason to stop: a caller that closes a port it is not using
              // has not asked for anything.
              return absl::OkStatus();
            }
            if (IsStopCommand(**command)) {
              shared->Stop(StopReason::kControl);
              return absl::OkStatus();
            }
            if (on_command == nullptr) {
              // An instruction that cannot be carried out. Dropping it
              // silently would leave the caller believing it was.
              shared->Stop(StopReason::kControl);
              return absl::InvalidArgumentError(absl::StrCat(
                  "this action takes no control command but was sent ",
                  (*command)->dump()));
            }
            if (const absl::Status handled = on_command(**command);
                !handled.ok()) {
              shared->Stop(StopReason::kControl);
              return handled;
            }
          }
          return absl::OkStatus();
        });
  }

  return signal;
}

absl::Status StopSignal::ArmDeadlineTimer() {
  const std::shared_ptr<Shared> shared = shared_;
  // Create() has already refused a deadline in the past, so this is positive --
  // except for the sliver between that check and this line, which rounds to a
  // zero-millisecond timer.
  const absl::Duration remaining = deadline_ - absl::Now();
  const uvw::timer_handle::time delay{static_cast<std::uint64_t>(
      remaining > absl::ZeroDuration() ? absl::ToInt64Milliseconds(remaining)
                                       : 0)};
  ABSL_ASSIGN_OR_RETURN(
      deadline_timer_,
      uv::RunOnUv<std::shared_ptr<uvw::timer_handle>>(
          [delay,
           shared]() -> absl::StatusOr<std::shared_ptr<uvw::timer_handle>> {
            std::shared_ptr<uvw::timer_handle> timer =
                uv::UvExecutor::Instance()
                    .loop()
                    ->resource<uvw::timer_handle>();
            if (timer == nullptr) {
              return absl::UnavailableError(
                  "could not create a deadline timer on the A11 libuv loop");
            }
            timer->on<uvw::timer_event>(
                [shared](const uvw::timer_event&, uvw::timer_handle& handle) {
                  shared->Stop(StopReason::kDeadline);
                  // One-shot, so it is done: closing here rather than leaving
                  // it for Join() means an action whose deadline passed stops
                  // holding a loop handle at the moment it stops mattering.
                  handle.close();
                });
            // Zero repeat: a deadline happens once.
            if (const int started =
                    timer->start(delay, uvw::timer_handle::time{0});
                started != 0) {
              timer->close();
              return uv::UvError(started, "uv_timer_start");
            }
            return timer;
          }));
  return absl::OkStatus();
}

void StopSignal::DisarmDeadlineTimer() {
  const std::shared_ptr<uvw::timer_handle> timer = std::move(deadline_timer_);
  if (timer == nullptr) {
    return;
  }
  // Awaited, so no callback can still be in flight when this returns -- the
  // same guarantee Await()ing the control fibre gives, and needed for the same
  // reason.
  uv::RunStatusOnUv([timer]() -> absl::Status {
    if (!timer->closing()) {
      timer->stop();
      timer->close();
    }
    return absl::OkStatus();
  }).IgnoreError();
}

StopSignal::~StopSignal() {
  Join();
}

bool StopSignal::stopped() const {
  return shared_->reason.load(std::memory_order_acquire) !=
         static_cast<int>(StopReason::kRunning);
}

StopReason StopSignal::reason() const {
  return static_cast<StopReason>(
      shared_->reason.load(std::memory_order_acquire));
}

absl::Time StopSignal::deadline() const {
  return deadline_;
}

bool StopSignal::has_deadline() const {
  return deadline_ < absl::InfiniteFuture();
}

thread::Case StopSignal::OnStop() const {
  return shared_->stopped.OnEvent();
}

absl::Status StopSignal::Check() const {
  switch (reason()) {
    case StopReason::kRunning:
      return absl::OkStatus();
    case StopReason::kControl:
      return absl::CancelledError("stopped by a control command");
    case StopReason::kDeadline:
      return absl::DeadlineExceededError("x-a11-deadline passed");
    case StopReason::kCancelled:
      return absl::CancelledError("cancelled");
  }
  return absl::OkStatus();
}

absl::Status StopSignal::ExitStatus() const {
  switch (reason()) {
    case StopReason::kRunning:
    case StopReason::kControl:
    case StopReason::kDeadline:
      // It was asked to finish, and it finished. A source's stream ending is
      // not a failure of the source.
      return absl::OkStatus();
    case StopReason::kCancelled:
      return absl::CancelledError("cancelled");
  }
  return absl::OkStatus();
}

bool StopSignal::WaitFor(absl::Duration duration) {
  return WaitUntil(absl::Now() + duration);
}

bool StopSignal::WaitUntil(absl::Time deadline) {
  if (stopped()) {
    return true;
  }
  // Both cases matter: the stop for the reasons this object knows about,
  // OnCancel for the fibre itself being torn down under us.
  const int ready = thread::SelectUntil(
      deadline, {thread::OnCancel(), shared_->stopped.OnEvent()});
  if (ready == 0) {
    shared_->Stop(StopReason::kCancelled);
  }
  return stopped();
}

void StopSignal::Stop(StopReason reason) {
  shared_->Stop(reason);
}

void StopSignal::Join() {
  if (joined_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  // Finish rather than Stop: a handler that completed its own work reaches here
  // first, and its reason() has to keep saying kRunning or every successful run
  // would report having been stopped.
  shared_->Finish();
  if (control_ != nullptr) {
    control_->CancelReader();
  }
  control_task_.Await().IgnoreError();
  DisarmDeadlineTimer();
}

}  // namespace a11::sdk::flow
