// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief
 *   Driving a state-machine pump on whichever thread asked it for something.
 *
 * A11's pumps -- the chunk store reader and writer, and anything else built the
 * same way -- run on whichever thread asks them for something, rather than only
 * on the worker pool. Waking a worker and waiting costs a scheduler hop to do
 * work the caller could do in the frame it is already in, and through a
 * language binding an event-loop turn on top of that.
 *
 * The hazard that buys, and what this header exists to contain: completing work
 * runs the caller's continuation, that continuation may ask the pump for more,
 * so a pump can be re-entered from inside itself.
 */

#ifndef A11_CONCURRENCY_INLINE_PUMP_H_
#define A11_CONCURRENCY_INLINE_PUMP_H_

#include <cstddef>
#include <exception>
#include <string_view>
#include <utility>

#include <absl/log/log.h>
#include <absl/strings/str_cat.h>

#include "a11/exception_guard.h"
#include "thread/boost_primitives.h"

namespace a11 {

/**
 * @brief
 *   Re-entry bookkeeping for a pump that may be driven from any thread.
 *
 * Lives in the pump's own state and is read and written only under the pump's
 * mutex -- the same one passed to DriveInline(). It tracks re-entrant drive
 * depth across executing threads and fibers.
 */
struct InlinePumpState {
  /// Live DriveInline() calls for this pump, across all threads.
  size_t depth = 0;
  /// Set by a call turned away at the cap; a running turn owes it a pass.
  bool again = false;
};

/**
 * @brief
 *   Run @p once until the pump has nothing left to do without waiting.
 *
 * Recursion is bounded rather than forbidden: a call arriving over @p max_depth
 * asks the turns already running for another pass instead of adding a frame. The
 * cap counts *all* live drives, so a genuinely concurrent driver can be turned
 * away too. That costs it a pass it need not have made, which is far cheaper
 * than turning away every concurrent caller and handing its work to whoever
 * happens to be inside -- an inline drive exists precisely so that a caller
 * does its own work.
 *
 * Deciding to leave and dropping out of the count happen under one hold of
 * @p mu, and a call that is turned away sets `again` under the same lock, so one
 * of the two always observes the other and the pass asked for is never dropped.
 * @p once is wrapped so an escaping exception cannot leak the count, which would
 * strand the pump permanently.
 *
 * @param mu
 *   The pump's mutex, guarding @p state.
 * @param state
 *   The pump's re-entry bookkeeping.
 * @param name
 *   Pump name for the diagnostic when @p once raises.
 * @param once
 *   One turn of the state machine. Must not block.
 * @param max_depth
 *   How many live drives to allow before folding further calls into them.
 */
template <typename Once>
void DriveInline(thread::Mutex* absl_nonnull mu,
                 InlinePumpState* absl_nonnull state, std::string_view name,
                 Once&& once, size_t max_depth = 4) {
  {
    thread::MutexLock lock(mu);
    if (state->depth >= max_depth) {
      state->again = true;
      return;
    }
    ++state->depth;
  }
  while (true) {
    // Every pump body is A11's own -- the store writers and the session pump --
    // so inside A11 this is a plain call. It stays a guard for the benefit of a
    // build with exceptions on, where a pump reaching into a caller's code (a
    // ChunkStore implemented in Python, say) can still raise.
    const absl::Status raised =
        exception_guard::Attempt([&] { once(); }, absl::StrCat(name, " pump"));
    if (!raised.ok()) {
      LOG(ERROR) << raised.message();
    }
    thread::MutexLock lock(mu);
    if (!state->again) {
      --state->depth;
      return;
    }
    state->again = false;
  }
}

/**
 * @brief
 *   Whether a DriveInline() turn for this pump is running right now.
 *
 * For a completion that has just run inline and has more to do: handing the next
 * pass to the turn it is standing in is right, while posting one to a worker
 * would race the caller's own next drive and take the work off it. Call it with
 * @p mu held.
 */
inline bool PumpIsDriving(const InlinePumpState& state) {
  return state.depth > 0;
}

}  // namespace a11

#endif  // A11_CONCURRENCY_INLINE_PUMP_H_
