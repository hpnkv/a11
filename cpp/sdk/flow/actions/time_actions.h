// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Time as a source of values.
 *
 * Flow can already say a great deal about *when* a value arrives -- `| pace`
 * spaces a stream out, `| timeout` gives up on a quiet one, `after` orders one
 * statement behind another, and `now()` reads the clock. What it has no way to
 * say is that a value should arrive *because time passed*. Every flow that
 * wants to poll something therefore has to spin a `repeat` around it and hope
 * the work inside takes about the right amount of time, which is not a schedule
 * so much as a coincidence.
 *
 * `ticker` is that missing source: a stream whose values are instants, bounded
 * by a count, a duration, or being asked to stop. Once it exists, polling is
 * ordinary Flow --
 *
 * @code{.a11flow}
 *   clock = run ticker(options: {"every": "30s", "for": "1h"})
 *   clock.ticks | map web_fetch(url) parallel 4 -> pages
 * @endcode
 *
 * -- and so is a timeout that does something other than fail, a heartbeat
 * beside a long call, and a retry whose backoff is written rather than
 * simulated.
 *
 * `sleep_for` is the degenerate case, and it exists for `after`: a barrier that
 * a statement can be ordered behind, with the instant it fired as its value.
 *
 * ### Drift
 *
 * `ticker` schedules against the instant it started rather than against the end
 * of the last tick, so a slow reader does not make the interval creep. What it
 * does instead is skip: if a reader is two intervals behind, the ticks it
 * missed are counted on `skipped` and not delivered late, because a tick
 * delivered late is a lie about when it happened. A flow that would rather have
 * every tick than a truthful one sets `catch_up`.
 */

#ifndef A11_SDK_FLOW_ACTIONS_TIME_ACTIONS_H_
#define A11_SDK_FLOW_ACTIONS_TIME_ACTIONS_H_

#include <string_view>

#include <absl/status/status.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"

namespace a11::sdk::flow {

/** @brief Registered name of the interval source. */
inline constexpr std::string_view kTickerAction = "ticker";
/** @brief Registered name of the delay barrier. */
inline constexpr std::string_view kSleepAction = "sleep_for";

/** @brief Schema for @c ticker. */
actions::ActionSchema TickerSchema();
/** @brief Schema for @c sleep_for. */
actions::ActionSchema SleepSchema();

/** @brief Handler for @c ticker. */
actions::ActionHandler TickerHandler();
/** @brief Handler for @c sleep_for. */
actions::ActionHandler SleepHandler();

/**
 * @brief Registers the time actions on @p registry.
 *
 * Takes no policy: a clock is not a capability. Nothing here reads, writes,
 * spawns or connects to anything, so there is nothing for a host to withhold
 * beyond not registering them.
 */
absl::Status RegisterTimeActions(actions::ActionRegistry& registry);

}  // namespace a11::sdk::flow

#endif  // A11_SDK_FLOW_ACTIONS_TIME_ACTIONS_H_
