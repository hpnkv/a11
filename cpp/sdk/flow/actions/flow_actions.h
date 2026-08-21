// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The Flow standard library, and the three sizes it comes in.
 *
 * A flow can only call what was registered for it. That is the sentence the
 * whole library rests on, and it means the interesting decision is not which
 * action to write but which set of them a host hands over. This file is that
 * decision, made coarse enough to be legible: three bundles and one policy
 * object, so that "what may this flow do" is answerable by reading one call.
 *
 * @code
 *   // A gateway that composes documents and nothing else.
 *   auto capabilities = ReadOnlyCapabilities({"/srv/corpus"});
 *   ABSL_RETURN_IF_ERROR(RegisterFlowActions(registry, capabilities));
 *
 *   // A build agent, which needs its scratch directory and its compiler.
 *   auto capabilities = WorkspaceCapabilities({"/work"});
 *   capabilities->process.enabled = true;
 *   capabilities->process.programs = {"cc", "make"};
 *   ABSL_RETURN_IF_ERROR(RegisterFlowActions(registry, capabilities));
 * @endcode
 *
 * ### Why bundles rather than one call with flags
 *
 * The unit people register is a bundle, and the failure mode worth designing
 * against is reaching `spawn_process` while meaning to reach `parse_csv`. A
 * flag list makes those two adjacent; a named function makes them different
 * decisions. So `RegisterFlowActions` registers exactly what the policy allows
 * and nothing on the strength of being nearby.
 *
 * ### What is always registered
 *
 * The actions that need no capability at all -- `ticker`, `sleep_for`, and the
 * pure data ones -- because a clock and a CSV parser are not privileges. Every
 * other group is present exactly when the policy says so.
 */

#ifndef A11_SDK_FLOW_ACTIONS_FLOW_ACTIONS_H_
#define A11_SDK_FLOW_ACTIONS_FLOW_ACTIONS_H_

#include <memory>
#include <string>
#include <vector>

#include <absl/status/status.h>

#include "a11/actions/registry.h"
#include "sdk/flow/actions/policy.h"

namespace a11::sdk::flow {

/** @brief A mutable policy, for building one up before sharing it. */
using CapabilitiesBuilder = std::shared_ptr<Capabilities>;

/**
 * @brief A policy that can read under @p roots and do nothing else.
 *
 * Symlinks are not followed, and a size limit is set: the defaults of a
 * capability nobody has thought about should be the ones that cannot surprise
 * anybody.
 */
CapabilitiesBuilder ReadOnlyCapabilities(std::vector<std::string> roots);

/**
 * @brief A policy that can read and write under @p roots.
 *
 * Still no processes, no network, no environment. A workspace is a place to put
 * files, not a shell.
 */
CapabilitiesBuilder WorkspaceCapabilities(std::vector<std::string> roots);

/**
 * @brief A policy with the filesystem, processes and the network open.
 *
 * For a host running flows it wrote itself. Loopback and private addresses stay
 * refused even here -- turning those on is a separate sentence, because
 * "trusted flow" and "may reach the metadata endpoint" are separate claims.
 */
CapabilitiesBuilder SystemCapabilities();

/**
 * @brief Registers every action @p capabilities allows.
 *
 * @param registry Registry to register on.
 * @param capabilities What the host is willing to allow. Shared with every
 *        handler registered, and not copied per call.
 * @return OK, or the first registration error. A group the policy refuses is
 *         skipped rather than failing, so tightening a policy does not turn
 *         startup into an error -- but a policy that contradicts itself (a
 *         writable filesystem with no roots, say) is reported here, where it
 *         can still be fixed.
 */
absl::Status RegisterFlowActions(actions::ActionRegistry& registry,
                                 CapabilitiesPtr capabilities);

/**
 * @brief Registers only the actions that need no capability.
 *
 * `ticker` and `sleep_for` today. What a host that wants Flow to be able to
 * wait, and nothing more, registers.
 */
absl::Status RegisterUnprivilegedFlowActions(actions::ActionRegistry& registry);

}  // namespace a11::sdk::flow

#endif  // A11_SDK_FLOW_ACTIONS_FLOW_ACTIONS_H_
