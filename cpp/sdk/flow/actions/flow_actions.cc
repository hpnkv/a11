// Copyright 2026 The A11 Authors.

#include "sdk/flow/actions/flow_actions.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>

#include "a11/actions/registry.h"
#include "sdk/flow/actions/fs_actions.h"
#include "sdk/flow/actions/policy.h"
#include "sdk/flow/actions/process_actions.h"
#include "sdk/flow/actions/system_actions.h"
#include "sdk/flow/actions/time_actions.h"

namespace a11::sdk::flow {
namespace {

/// A size limit on a read nobody has thought about. Large enough for the files
/// a composition actually reads, small enough that a flow pointed at a disk
/// image fails instead of filling memory.
constexpr std::uint64_t kDefaultMaxReadBytes = 256 * 1024 * 1024;
/// A directory listing nobody bounded. A tree walk that reaches this has
/// almost certainly been pointed at the wrong root.
constexpr std::uint64_t kDefaultMaxEntries = 1000000;

}  // namespace

CapabilitiesBuilder ReadOnlyCapabilities(std::vector<std::string> roots) {
  auto capabilities = std::make_shared<Capabilities>();
  capabilities->filesystem.roots = std::move(roots);
  capabilities->filesystem.writable = false;
  capabilities->filesystem.follow_symlinks = false;
  capabilities->filesystem.max_read_bytes = kDefaultMaxReadBytes;
  capabilities->filesystem.max_entries = kDefaultMaxEntries;
  return capabilities;
}

CapabilitiesBuilder WorkspaceCapabilities(std::vector<std::string> roots) {
  CapabilitiesBuilder capabilities = ReadOnlyCapabilities(std::move(roots));
  capabilities->filesystem.writable = true;
  return capabilities;
}

CapabilitiesBuilder SystemCapabilities() {
  auto capabilities = std::make_shared<Capabilities>();
  capabilities->filesystem.unrestricted = true;
  capabilities->filesystem.writable = true;
  capabilities->filesystem.follow_symlinks = true;
  capabilities->process.enabled = true;
  capabilities->process.any_program = true;
  capabilities->process.inherit_environment = true;
  capabilities->network.enabled = true;
  capabilities->network.may_listen = true;
  capabilities->network.any_host = true;
  // Deliberately still refused. "I trust this flow" and "this flow may read my
  // instance credentials" are different claims, and a host that means the
  // second one can say so in one more line.
  capabilities->network.allow_loopback = false;
  capabilities->network.allow_private = false;
  capabilities->network.allow_link_local = false;
  capabilities->environment.any_name = true;
  return capabilities;
}

absl::Status RegisterUnprivilegedFlowActions(
    actions::ActionRegistry& registry) {
  ABSL_RETURN_IF_ERROR(RegisterTimeActions(registry));
  return RegisterRandomActions(registry);
}

absl::Status RegisterFlowActions(actions::ActionRegistry& registry,
                                 CapabilitiesPtr capabilities) {
  if (capabilities == nullptr) {
    return absl::InvalidArgumentError(
        "a policy is required; see ReadOnlyCapabilities and its neighbours");
  }
  const FilesystemPolicy& filesystem = capabilities->filesystem;
  if (filesystem.writable && filesystem.roots.empty() &&
      !filesystem.unrestricted) {
    // A policy that contradicts itself, and the only chance to say so is here:
    // at the first call it would look like an ordinary permission_denied.
    return absl::InvalidArgumentError(
        "a writable filesystem policy with no roots allows nothing; set roots, "
        "or set unrestricted if the whole filesystem was meant");
  }

  ABSL_RETURN_IF_ERROR(RegisterUnprivilegedFlowActions(registry));

  const bool may_read = filesystem.unrestricted || !filesystem.roots.empty();
  if (may_read) {
    ABSL_RETURN_IF_ERROR(
        RegisterFilesystemReadActions(registry, capabilities));
  }
  if (may_read && filesystem.writable) {
    ABSL_RETURN_IF_ERROR(
        RegisterFilesystemWriteActions(registry, capabilities));
  }
  if (capabilities->process.enabled) {
    ABSL_RETURN_IF_ERROR(RegisterProcessActions(registry, capabilities));
  }
  if (capabilities->environment.any_name ||
      !capabilities->environment.names.empty()) {
    ABSL_RETURN_IF_ERROR(RegisterEnvironmentActions(registry, capabilities));
  }
  // The standard streams are deliberately not here: a gateway has nothing
  // useful on its standard input, and a flow reading it there would wait
  // forever on a stream nobody is writing. A command-line host calls
  // RegisterStandardStreamActions itself.
  return absl::OkStatus();
}

}  // namespace a11::sdk::flow
