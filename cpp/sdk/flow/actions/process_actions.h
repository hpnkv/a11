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
 * @brief Run a child process with separate stdout and stderr ports.
 *
 * `spawn_process` emits each output stream on its own ports:
 *
 * @code{.a11flow}
 *   build = run spawn_process(program: "make", arguments: ["-j8"])
 *   build.stdout_lines | where contains(it, "warning:") -> warnings
 *   build.stderr_lines -> problems
 *   if build.exit_code != 0 { fail internal "the build failed" }
 * @endcode
 *
 * `pid` arrives before process output; `exit_code` arrives last. `signal`
 * identifies signal termination. A nonzero exit code remains an action result.
 * Failure to start the program fails the action.
 *
 * Cancellation or a deadline sends SIGTERM, waits `options.grace`, then sends
 * SIGKILL. A flow can also request a signal:
 *
 * @code{.a11flow}
 *   {"command": "signal", "signal": "HUP"} -> server.control_events
 * @endcode
 *
 * A separate fiber reads the `stdin` port while the handler polls process
 * output. Child setup allocates before fork; the fork-to-exec path does not.
 */

#ifndef A11_SDK_FLOW_ACTIONS_PROCESS_ACTIONS_H_
#define A11_SDK_FLOW_ACTIONS_PROCESS_ACTIONS_H_

#include <string_view>

#include <absl/status/status.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "sdk/flow/actions/policy.h"

namespace a11::sdk::flow {

/** @brief Registered name of the process runner. */
inline constexpr std::string_view kSpawnProcessAction = "spawn_process";

/** @brief Schema for @c spawn_process. */
actions::ActionSchema SpawnProcessSchema();
/** @brief Handler for @c spawn_process, closed over what it may run. */
actions::ActionHandler SpawnProcessHandler(CapabilitiesPtr capabilities);

/**
 * @brief Registers the process actions on @p registry.
 *
 * Returns an error when the policy does not enable processes.
 */
absl::Status RegisterProcessActions(actions::ActionRegistry& registry,
                                    CapabilitiesPtr capabilities);

}  // namespace a11::sdk::flow

#endif  // A11_SDK_FLOW_ACTIONS_PROCESS_ACTIONS_H_
