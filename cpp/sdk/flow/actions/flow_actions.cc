// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sdk/flow/actions/flow_actions.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
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

namespace fs = std::filesystem;

/// A size limit on a read nobody has thought about. Large enough for the files
/// a composition actually reads, small enough that a flow pointed at a disk
/// image fails instead of filling memory.
constexpr std::uint64_t kDefaultMaxReadBytes = 256 * 1024 * 1024;
/// A directory listing nobody bounded. A tree walk that reaches this has
/// almost certainly been pointed at the wrong root.
constexpr std::uint64_t kDefaultMaxEntries = 1000000;

/** Existing platform scratch locations a confined child may write. */
std::vector<std::string> DefaultProcessWriteRoots() {
  std::vector<std::string> roots;
  const auto add = [&roots](const char* raw) {
    if (raw == nullptr || *raw == '\0')
      return;
    std::error_code error;
    const fs::path absolute = fs::absolute(raw, error).lexically_normal();
    if (error || !fs::is_directory(absolute, error) || error)
      return;
    roots.push_back(absolute.string());
    const fs::path canonical = fs::canonical(absolute, error);
    if (!error)
      roots.push_back(canonical.string());
  };
  std::error_code error;
  const fs::path temporary = fs::temp_directory_path(error);
  if (!error) {
    const std::string temporary_string = temporary.string();
    add(temporary_string.c_str());
  }
  for (const char* name : {"TMPDIR", "TMP", "TEMP", "XDG_RUNTIME_DIR"}) {
    add(std::getenv(name));
  }
  add("/tmp");
  add("/var/tmp");
  std::sort(roots.begin(), roots.end());
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  return roots;
}

}  // namespace

CapabilitiesBuilder ReadOnlyCapabilities(std::vector<std::string> roots) {
  auto capabilities = std::make_shared<Capabilities>();
  capabilities->filesystem.roots = std::move(roots);
  capabilities->filesystem.writable = false;
  capabilities->filesystem.follow_symlinks = false;
  capabilities->filesystem.max_read_bytes = kDefaultMaxReadBytes;
  capabilities->filesystem.max_entries = kDefaultMaxEntries;
  capabilities->process.write_roots = DefaultProcessWriteRoots();
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
  // Continue to refuse this case. "I trust this flow" and "this flow may read
  // my instance credentials" are different claims, and a host that means the
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
                                 const CapabilitiesPtr& capabilities) {
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
    ABSL_RETURN_IF_ERROR(RegisterFilesystemReadActions(registry, capabilities));
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
  // Standard streams are unavailable here: a gateway has no source or sink
  // useful on its standard input, and a flow reading it there would wait
  // forever on a stream nobody is writing.
  return absl::OkStatus();
}

}  // namespace a11::sdk::flow
