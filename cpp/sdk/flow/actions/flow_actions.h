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
 * @brief Register Flow actions under a capability policy.
 *
 * A flow can call only registered actions. The capability builders provide
 * common policies, and RegisterFlowActions registers the actions they allow.
 *
 * @code
 *   // Read-only document gateway.
 *   auto capabilities = ReadOnlyCapabilities({"/srv/corpus"});
 *   ABSL_RETURN_IF_ERROR(RegisterFlowActions(registry, capabilities));
 *
 *   // Build agent with workspace and compiler access.
 *   auto capabilities = WorkspaceCapabilities({"/work"});
 *   capabilities->process.enabled = true;
 *   capabilities->process.programs = {"cc", "make"};
 *   ABSL_RETURN_IF_ERROR(RegisterFlowActions(registry, capabilities));
 * @endcode
 *
 * Time and random actions are always registered. Other groups require the
 * corresponding capability.
 */

#ifndef A11_SDK_FLOW_ACTIONS_FLOW_ACTIONS_H_
#define A11_SDK_FLOW_ACTIONS_FLOW_ACTIONS_H_

#include <memory>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>

#include "a11/actions/registry.h"
#include "sdk/flow/actions/policy.h"

namespace a11::sdk::flow {

/** @brief A mutable policy, for building one up before sharing it. */
using CapabilitiesBuilder = std::shared_ptr<Capabilities>;

/**
 * @brief A policy that can read under @p roots and do nothing else.
 *
 * Does not follow symlinks and sets a default size limit.
 */
CapabilitiesBuilder ReadOnlyCapabilities(std::vector<std::string> roots);

/**
 * @brief A policy that can read and write under @p roots.
 *
 * Does not grant process, network or environment access.
 */
CapabilitiesBuilder WorkspaceCapabilities(std::vector<std::string> roots);

/**
 * @brief A policy with the filesystem, processes and the network open.
 *
 * Intended for flows written by the host. Loopback and private network
 * addresses still require explicit permission.
 */
CapabilitiesBuilder SystemCapabilities();

/**
 * @brief Registers every action @p capabilities allows.
 *
 * @param registry Registry to register on.
 * @param capabilities What the host is willing to allow. Shared with every
 *        handler registered, and not copied per call.
 * @return OK or the first registration or invalid-policy error. Disallowed
 *         groups are skipped.
 */
absl::Status RegisterFlowActions(actions::ActionRegistry& registry,
                                 const CapabilitiesPtr& capabilities);

/**
 * @brief Registers only the actions that need no capability.
 *
 * Includes time and random actions.
 */
absl::Status RegisterUnprivilegedFlowActions(actions::ActionRegistry& registry);

}  // namespace a11::sdk::flow

#endif  // A11_SDK_FLOW_ACTIONS_FLOW_ACTIONS_H_
