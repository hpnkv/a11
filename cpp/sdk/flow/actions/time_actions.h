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
 * @brief Time sources for Flow programs.
 *
 * `ticker` emits scheduled instants until its count, duration or stop limit.
 * For example, a flow can fetch a page every 30 seconds:
 *
 * @code{.a11flow}
 *   clock = run ticker(options: {"every": "30s", "for": "1h"})
 *   clock.ticks | map web_fetch(url) parallel 4 -> pages
 * @endcode
 *
 * `sleep_for` provides a barrier for `after`. `ticker` schedules from its start
 * time. A slow reader receives a `skipped` count for missed ticks; set
 * `catch_up` to deliver every tick.
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
