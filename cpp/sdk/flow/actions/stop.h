// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The three ways an action is asked to stop, as one object.
 *
 * A long-lived action is asked to stop by three different things:
 *
 *   * the caller **cancelling** it, which arrives on a foreign thread;
 *   * its **deadline** running out, which arrives from a clock nobody is
 *     watching unless somebody watches it;
 *   * a `{"command": "stop"}` on its **control port**, which arrives as data
 *     and only if something is reading that port.
 *
 * An action that honours one of them and not the others stops in some
 * circumstances and hangs in the rest, which is the worst of the available
 * behaviours because it looks like it works. So it is written once, here.
 *
 * ### Graceful, and not
 *
 * The distinction this file exists to keep straight is between an action being
 * *asked to finish* and an action being *torn down*, because they end the run
 * differently and neither answer is right for both:
 *
 *   * A **source** -- `ticker`, `watch_path`, `listen_tcp` -- is asked to
 *     finish. Its stream has simply ended: ports close normally, the run
 *     returns OK, and a flow reading it sees the end of a stream rather than a
 *     failure. StopSignal::ExitStatus() is that answer.
 *   * An action with **one answer to give** -- `read_file`, `sqlite_query` --
 *     cannot finish early, because half a file is not a shorter file. Its
 *     deadline is a failure and so is its cancellation. StopSignal::Check() is
 *     that answer, and a handler calls it where a partial result would
 *     otherwise be written.
 *
 * Cancellation is never graceful in either: a cancelled action returns
 * `cancelled` and aborts its unfinished ports, so a reader learns the stream
 * stopped early rather than being told it ended.
 *
 * ### What it costs
 *
 * stopped() is one atomic load, so a streaming loop can ask per value -- which
 * is the point, since a loop that only asks between blocking calls stops when
 * the next value arrives rather than when it was asked to. The watchers exist
 * only when there is something to watch: no deadline header means no timer, and
 * no control port means no control fibre.
 *
 * The deadline is a `uvw::timer_handle` on A11's one libuv loop rather than a
 * fibre in a timed Select. A sleeping fibre costs a stack and a scheduler slot
 * for the whole life of the action, and the loop is already running; a timer
 * costs an entry in its heap. Cancellation is a callback the framework already
 * delivers, so the control port is the only thing left that genuinely needs a
 * fibre -- it has to be parked in a read.
 *
 * ### The obligation
 *
 * Join() before returning from the handler. It stops the watchers and waits for
 * them, which is what keeps a fibre from reading a port the framework is about
 * to take apart. Every handler in this library reaches it on every path, and
 * the destructor reaches it for the paths nobody thought of.
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
 * Returning an error fails the action, which is what a control command that
 * cannot be carried out should do -- it was an instruction, not a hint.
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
   * @param on_command Handles the commands that are not `stop`. When empty, one
   *        of those fails the action, because silently ignoring an instruction
   *        is worse than refusing it.
   * @return The signal, or an error when the deadline header is malformed --
   *         worth failing on rather than treating as no deadline, since a
   *         caller who wrote one meant to bound the work.
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
   * For the action that cannot finish early: a deadline is `deadline_exceeded`
   * and a cancellation is `cancelled`, because handing back a partial result as
   * a success is the one outcome a caller cannot detect.
   */
  [[nodiscard]] absl::Status Check() const;

  /**
   * @brief How a source's run ends, given what stopped it.
   *
   * OK for a stop command and for a deadline -- both asked it to finish, and it
   * did -- and `cancelled` for a cancellation.
   */
  [[nodiscard]] absl::Status ExitStatus() const;

  /**
   * @brief Sleeps up to @p duration, waking the moment a stop is asked for.
   * @return Whether it should stop. The interval a `ticker` waits and the
   *         backoff a retry waits both go through here, so neither outlives
   *         being cancelled by more than the time it takes to wake a fibre.
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
   * Idempotent and safe on any thread: the first reason wins, so a deadline
   * that fires while a cancellation is being applied does not overwrite it.
   * Called by the watchers, and callable by a handler that has its own reason
   * to wind down.
   */
  void Stop(StopReason reason);

  /**
   * @brief Stops the watchers and waits for them to finish.
   *
   * **Call this before returning from the handler, on every path.** A watcher
   * fibre still reading a control port while the framework releases that port
   * is a use-after-free waiting for a slow enough machine.
   *
   * Idempotent, and does *not* count as a stop: a handler that finished its own
   * work reaches Join() first, and its reason() must still say kRunning
   * afterwards or every successful run would report having been stopped.
   *
   * Reports nothing: a watcher's own failure has already been turned into a
   * stop, and the run's status is the handler's to give.
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
   * The part the watcher fibres share.
   *
   * Separate from StopSignal on purpose. A watcher holding the StopSignal alive
   * would mean the last reference is dropped *by a watcher*, so the destructor
   * would run on the watcher's own fibre and Join() would wait for the fibre it
   * is running on. Watchers therefore capture this and never the handle.
   */
  struct Shared {
    std::atomic<int> reason{static_cast<int>(StopReason::kRunning)};
    /// Notified once, by whichever Stop() call wins.
    thread::PermanentEvent stopped;
    /// Notified when the watchers should wind up, whether or not anything
    /// asked the action to stop.
    thread::PermanentEvent finished;
    std::atomic<bool> finishing{false};

    void Stop(StopReason reason);
    void Finish();
  };

  StopSignal() = default;

  /**
   * Arms the deadline watcher on A11's one libuv loop. Called only when there
   * is a deadline to watch.
   *
   * A timer rather than a fibre parked in a timed Select, because a fibre
   * costs a stack and a scheduler slot for the whole life of the action while
   * doing nothing but sleeping, and the loop already exists. The callback runs
   * on the loop thread and so must not block: it is a compare-exchange and a
   * notify, which is exactly what the cancellation callback already does from a
   * foreign thread.
   */
  absl::Status ArmDeadlineTimer();

  /**
   * Closes it, on the loop thread, and waits for that to have happened.
   *
   * Waiting is the point: a timer still armed after Join() would give a run
   * that had already finished a reason it never had, so reason() would start
   * saying kDeadline about a successful run.
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
