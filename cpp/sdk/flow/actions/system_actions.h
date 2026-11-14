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
 * @brief Process streams, environment access, and random values.
 *
 * `read_stdin`, `write_stdout` and `write_stderr` connect a Flow program to
 * its process streams. For example:
 *
 * @code{.a11flow}
 *   in pattern: string required "What to keep"
 *   stdin = run read_stdin()
 *   stdin.lines | where contains(it, pattern) | map strformat("%s\n", it)
 *     -> out.content
 *   out = run write_stdout()
 * @endcode
 *
 * Port writes wait for the store to accept each chunk, propagating backpressure
 * to standard input.
 *
 * These actions use the host process's streams. `spawn_process` exposes child
 * streams through its own ports.
 *
 * ### Environment
 *
 * `env_get` reads only named variables allowed by the host policy.
 *
 * ### Randomness
 *
 * `random_bytes` and `new_uuid` supply random values as explicit actions.
 *
 * `random_bytes` uses the system generator for tokens and nonces. `new_uuid`
 * creates A11 identifiers.
 */

#ifndef A11_SDK_FLOW_ACTIONS_SYSTEM_ACTIONS_H_
#define A11_SDK_FLOW_ACTIONS_SYSTEM_ACTIONS_H_

#include <string_view>

#include <absl/status/status.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "sdk/flow/actions/policy.h"

namespace a11::sdk::flow {

/** @brief Registered name of the standard-input reader. */
inline constexpr std::string_view kReadStdinAction = "read_stdin";
/** @brief Registered name of the standard-output writer. */
inline constexpr std::string_view kWriteStdoutAction = "write_stdout";
/** @brief Registered name of the standard-error writer. */
inline constexpr std::string_view kWriteStderrAction = "write_stderr";
/** @brief Registered name of the environment reader. */
inline constexpr std::string_view kEnvGetAction = "env_get";
/** @brief Registered name of the random-byte source. */
inline constexpr std::string_view kRandomBytesAction = "random_bytes";
/** @brief Registered name of the identifier source. */
inline constexpr std::string_view kNewUuidAction = "new_uuid";

/** @brief Schema for @c read_stdin. */
actions::ActionSchema ReadStdinSchema();
/** @brief Schema for @c write_stdout. */
actions::ActionSchema WriteStdoutSchema();
/** @brief Schema for @c write_stderr. */
actions::ActionSchema WriteStderrSchema();
/** @brief Schema for @c env_get. */
actions::ActionSchema EnvGetSchema();
/** @brief Schema for @c random_bytes. */
actions::ActionSchema RandomBytesSchema();
/** @brief Schema for @c new_uuid. */
actions::ActionSchema NewUuidSchema();

/** @brief Handler for @c read_stdin. */
actions::ActionHandler ReadStdinHandler();
/** @brief Handler for @c write_stdout. */
actions::ActionHandler WriteStdoutHandler();
/** @brief Handler for @c write_stderr. */
actions::ActionHandler WriteStderrHandler();
/** @brief Handler for @c env_get, closed over what it may read. */
actions::ActionHandler EnvGetHandler(CapabilitiesPtr capabilities);
/** @brief Handler for @c random_bytes. */
actions::ActionHandler RandomBytesHandler();
/** @brief Handler for @c new_uuid. */
actions::ActionHandler NewUuidHandler();

/**
 * @brief Registers the standard-stream actions on @p registry.
 *
 * Hosts running Flow as a command-line program register these separately.
 */
absl::Status RegisterStandardStreamActions(actions::ActionRegistry& registry);

/**
 * @brief Registers @c random_bytes and @c new_uuid on @p registry.
 *
 * Registration takes no capability policy.
 */
absl::Status RegisterRandomActions(actions::ActionRegistry& registry);

/**
 * @brief Registers @c env_get on @p registry.
 *
 * Returns an error when the policy allows no environment variables.
 */
absl::Status RegisterEnvironmentActions(actions::ActionRegistry& registry,
                                        CapabilitiesPtr capabilities);

}  // namespace a11::sdk::flow

#endif  // A11_SDK_FLOW_ACTIONS_SYSTEM_ACTIONS_H_
