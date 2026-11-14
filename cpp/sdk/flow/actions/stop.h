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
 * @brief Cancellation, deadlines and control commands for an action.
 *
 * StopSignal watches caller cancellation, `x-a11-deadline` and the control
 * port's `{"command": "stop"}`. Sources use ExitStatus(): control stops and
 * deadlines end their streams normally. Finite operations use Check() to
 * report an early stop as an error. Cancellation always reports `cancelled`.
 *
 * stopped() is an atomic load suitable for streaming loops. A deadline uses a
 * libuv timer; a control port uses a reader fiber. Join() stops the watchers
 * before the handler releases its ports.
 */

#ifndef A11_SDK_FLOW_ACTIONS_STOP_H_
#define A11_SDK_FLOW_ACTIONS_STOP_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/concurrency/future.h"
#include "thread/selectables.h"

namespace a11::nodes {
class AsyncNode;
}  // namespace a11::nodes

namespace uvw {
class timer_handle;
}  // namespace uvw

namespace a11::sdk::flow {

/** @brief Why an action was asked to stop. */
enum class StopReason {
  kRunning,    ///< It has not been.
  kControl,    ///< A `{"command": "stop"}` arrived on the control port.
  kDeadline,   ///< `x-a11-deadline` passed.
  kCancelled,  ///< The caller cancelled the action.
};

/**
 * @brief A command that arrived on a control port and was not `stop`.
 *
 * Returning an error fails the action.
 */
using OnControlCommand = std::function<absl::Status(const nlohmann::json&)>;

/** @brief The port name every action in this library takes commands on. */
inline constexpr std::string_view kControlPort = "control_events";

/**
 * @brief Cancellation, deadline and control commands, watched together.
 *
 * Always held by `shared_ptr`, because the cancellation callback outlives the
 * frame that created it.
 */
class StopSignal {
 public:
  /**
   * @brief Starts watching for @p action being asked to stop.
   *
   * @param action The running action. Its `x-a11-deadline` header is read here,
   *        and a cancellation callback is installed on it.
   * @param control_port An input port carrying commands, or empty for an action
   *        that declares none. A `{"command": "stop"}` there stops the action;
   *        anything else goes to @p on_command.
   * @param on_command Handles commands other than `stop`. Without a handler,
   *        such commands fail the action.
   * @return The signal, or an error for a malformed deadline header.
   */
  static absl::StatusOr<std::shared_ptr<StopSignal>> Create(
      const std::shared_ptr<actions::Action>& action,
      std::string_view control_port = {}, OnControlCommand on_command = {});

  StopSignal(const StopSignal&) = delete;
  StopSignal& operator=(const StopSignal&) = delete;
  ~StopSignal();

  /** @brief Whether the action has been asked to stop. One atomic load. */
  [[nodiscard]] bool stopped() const;
  /** @brief Why it was asked to stop, or kRunning. */
  [[nodiscard]] StopReason reason() const;

  /**
   * @brief OK while the action may carry on producing a single answer.
   *
   * A deadline returns `deadline_exceeded`; cancellation returns `cancelled`.
   * Use for operations whose partial result cannot be returned as complete.
   */
  [[nodiscard]] absl::Status Check() const;

  /**
   * @brief How a source's run ends, given what stopped it.
   *
   * Returns OK for a stop command or deadline, and `cancelled` when cancelled.
   */
  [[nodiscard]] absl::Status ExitStatus() const;

  /**
   * @brief Sleeps up to @p duration, waking the moment a stop is asked for.
   * @return Whether a stop was requested during the wait.
   */
  bool WaitFor(absl::Duration duration);
  /** @brief Sleeps until @p deadline, waking early on a stop. */
  bool WaitUntil(absl::Time deadline);

  /** @brief The action's deadline, or absl::InfiniteFuture(). */
  [[nodiscard]] absl::Time deadline() const;
  /** @brief Whether a deadline was set at all. */
  [[nodiscard]] bool has_deadline() const;

  /**
   * @brief Asks the action to stop, if nothing has already.
   *
   * Idempotent and thread-safe. The first stop reason wins.
   */
  void Stop(StopReason reason);

  /**
   * @brief Stops the watchers and waits for them to finish.
   *
   * Call before returning from the handler so watcher fibers stop using its
   * ports. Idempotent. Join() does not change the stop reason or run status.
   */
  void Join();

  /**
   * @brief A case that becomes ready when a stop is asked for.
   *
   * For a handler that has its own thing to wait on and wants to wait on both:
   * `thread::Select({stop->OnStop(), channel.OnRead()})`.
   */
  [[nodiscard]] thread::Case OnStop() const;

 private:
  /**
   * The part the watcher fibers share.
   *
   * Watchers capture Shared without retaining StopSignal. This prevents its
   * destructor from joining the watcher fiber that runs it.
   */
  struct Shared {
    std::atomic<int> reason{static_cast<int>(StopReason::kRunning)};
    /// Notified once, by whichever Stop() call wins.
    thread::PermanentEvent stopped;
    /// Notified when the watchers should wind up, whether or not anything
    /// asked the action to stop.
    thread::PermanentEvent finished;
    std::atomic<bool> finishing{false};

    void Stop(StopReason next);
    void Finish();
  };

  StopSignal() = default;

  /**
   * Arms a libuv timer when the action has a deadline. The loop callback only
   * updates the stop reason and notifies waiters; it must not block.
   */
  absl::Status ArmDeadlineTimer();

  /**
   * Closes it, on the loop thread, and waits for that to have happened.
   *
   * Waits for closure so the timer cannot update the reason after Join().
   */
  void DisarmDeadlineTimer();

  std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
  absl::Time deadline_ = absl::InfiniteFuture();
  /// Held so Join() can unblock a watcher parked in a read.
  std::shared_ptr<nodes::AsyncNode> control_;
  a11::Task control_task_;
  /// The armed deadline timer, or null when there is no deadline. Only ever
  /// touched on the loop thread past construction.
  std::shared_ptr<uvw::timer_handle> deadline_timer_;
  std::atomic<bool> joined_{false};
};

}  // namespace a11::sdk::flow

#endif  // A11_SDK_FLOW_ACTIONS_STOP_H_
