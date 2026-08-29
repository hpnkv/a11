// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Running a program, with its two output streams kept apart.
 *
 * A11 already has a shell: `shell_start` and `shell_execute` keep a persistent
 * working directory and environment, which is a different and useful thing.
 * What they do not do is keep `stdout` and `stderr` apart -- they interleave
 * both onto one `output_lines` port -- and that is exactly the collapse an A11
 * port makes unnecessary. Two streams the operating system went to the trouble
 * of separating should not be rejoined on the way in.
 *
 * So `spawn_process` gives each of them a port, in both encodings:
 *
 * @code{.a11flow}
 *   build = run spawn_process(program: "make", arguments: ["-j8"])
 *   build.stdout_lines | where contains(it, "warning:") -> warnings
 *   build.stderr_lines -> problems
 *   if build.exit_code != 0 { fail internal "the build failed" }
 * @endcode
 *
 * ### What arrives when
 *
 * `pid` is written as soon as there is one, before any output, so a flow can
 * order something else behind the process having started -- the same reason
 * `make_http_request` writes its status before its body. `exit_code` arrives
 * last, and `signal` says whether it was killed rather than exited, because
 * "exit code 143" is a thing a caller should not have to decode.
 *
 * A non-zero exit is **not** a failure of the action: the program ran and this
 * is what it said, exactly as a 404 is a response. The action fails when the
 * program could not be run at all.
 *
 * ### Stopping one
 *
 * Cancelling the action, or its deadline passing, sends `SIGTERM`, waits
 * `options.grace` for graceful shutdown, and then sends `SIGKILL`. Programs may
 * flush output during the grace period. A flow can also request a signal:
 *
 * @code{.a11flow}
 *   {"command": "signal", "signal": "HUP"} -> server.control_events
 * @endcode
 *
 * ### The parts of this that are not obvious
 *
 * `stdin` is fed by a fiber of its own, because feeding it means reading a port
 * -- which blocks -- and the same fiber cannot also be polling for output. And
 * everything the child needs is built before the fork: between fork and exec a
 * process may call almost nothing safely, and allocating there is how a
 * threaded program deadlocks in a way that reproduces once a week.
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
 * Fails when the policy does not enable processes, for the reason the
 * filesystem writers do: a host that called this meant to allow running
 * programs, and registering an action that always refuses would hide the
 * mistake until the first call.
 */
absl::Status RegisterProcessActions(actions::ActionRegistry& registry,
                                    CapabilitiesPtr capabilities);

}  // namespace a11::sdk::flow

#endif  // A11_SDK_FLOW_ACTIONS_PROCESS_ACTIONS_H_
