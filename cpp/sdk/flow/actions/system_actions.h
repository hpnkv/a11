// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The process's own streams, its environment, and randomness.
 *
 * Three small groups that share one property: each of them is a thing the
 * language does not provide.
 *
 * ### Standard input and output
 *
 * `read_stdin`, `write_stdout` and `write_stderr` are what make a flow a Unix
 * filter. With them, this is a working `grep`:
 *
 * @code{.a11flow}
 *   in pattern: string required "What to keep"
 *   stdin = run read_stdin()
 *   stdin.lines | where contains(it, pattern) | map strformat("%s\n", it)
 *     -> out.content
 *   out = run write_stdout()
 * @endcode
 *
 * and the pipe holds, both ways: a port write does not return until the store
 * has taken the chunk, so a slow consumer downstream is felt by whoever is
 * writing into this process's standard input.
 *
 * These are the *process's* streams and not a child's -- `spawn_process` has
 * those, on ports of its own. The distinction matters because in a gateway
 * there is nothing useful on standard input at all, which is why they are
 * registered separately from everything else.
 *
 * ### Environment
 *
 * `env_get` reads named variables, and only the ones the host allowed. Note
 * what it does *not* do: there is no "give me the environment", because a flow
 * that can ask for everything can exfiltrate credentials it was never told
 * about, and a flow that has to name what it wants is a flow a reader can
 * audit.
 *
 * ### Randomness
 *
 * `random_bytes` and `new_uuid` exist because Flow has no random number
 * generator, and should not: a language whose output depends only on its input
 * is one whose failures reproduce. Keeping nondeterminism in an action keeps it
 * opt-in, visible in the source, and refusable by not registering it.
 *
 * The bytes come from the system's own generator, so they are fit for a token
 * or a nonce -- unlike `new_uuid`, which is A11's ordinary identifier and says
 * in its own documentation that it is not.
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
 * Separate, and not part of RegisterFlowActions: a gateway has nothing useful
 * on its standard input, and an action that reads it there would be a flow
 * waiting forever on a stream nobody is writing. This is for a host running
 * flows as a command-line program.
 */
absl::Status RegisterStandardStreamActions(actions::ActionRegistry& registry);

/**
 * @brief Registers @c random_bytes and @c new_uuid on @p registry.
 *
 * No policy: randomness is not a capability, it is only a thing the language
 * refuses to have.
 */
absl::Status RegisterRandomActions(actions::ActionRegistry& registry);

/**
 * @brief Registers @c env_get on @p registry.
 *
 * Fails when the policy names no variables and does not allow any, since the
 * action could then only ever refuse.
 */
absl::Status RegisterEnvironmentActions(actions::ActionRegistry& registry,
                                        CapabilitiesPtr capabilities);

}  // namespace a11::sdk::flow

#endif  // A11_SDK_FLOW_ACTIONS_SYSTEM_ACTIONS_H_
