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
 * @brief Host capabilities for registered Flow actions.
 *
 * The host fixes this policy at registration. Call options may narrow access;
 * they cannot widen it. Unspecified capabilities default to denied.
 * ResolvePath() resolves relative paths, `..` and symlinks before checking
 * filesystem roots. flow_actions.h provides common policy builders.
 */

#ifndef A11_SDK_FLOW_ACTIONS_POLICY_H_
#define A11_SDK_FLOW_ACTIONS_POLICY_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

namespace a11::sdk::flow {

/** @brief What the filesystem actions may reach. */
struct FilesystemPolicy {
  /** Base for relative action paths. Empty uses the process directory. */
  std::string current_directory;
  /**
   * Directories the actions may work inside, resolved before use. A path
   * outside every one of them is refused with `permission_denied`.
   */
  std::vector<std::string> roots;
  /**
   * Whether to allow paths outside every root -- which is to say, the whole
   * filesystem. Named rather than spelled as an empty root list, so that a
   * misread config that drops the roots refuses everything instead of allowing
   * everything.
   */
  bool unrestricted = false;
  /** Whether the writing actions are allowed at all. */
  bool writable = false;
  /**
   * Whether a symlink is followed when reading or listing. Containment is
   * checked on the resolved path either way, so this is about what a directory
   * listing reports and what `read_file` opens, not about escaping a root.
   */
  bool follow_symlinks = false;
  /** Largest file `read_file` will read, or 0 for no limit. */
  std::uint64_t max_read_bytes = 0;
  /** Most `write_file` will write in one run, or 0 for no limit. */
  std::uint64_t max_write_bytes = 0;
  /** Most entries `list_directory` will report, or 0 for no limit. */
  std::uint64_t max_entries = 0;
};

/**
 * @brief Whether a child must be confined by the kernel as well as by policy.
 *
 * Child programs make syscalls outside A11's in-process checks. `kPreferred`
 * applies confinement when available; `kRequired` fails Prepare() if the
 * system cannot confine the child. See sandbox.h.
 */
enum class SandboxRequest {
  kNever,      ///< Do not confine. The policy is A11's own checks.
  kPreferred,  ///< Confine where the system can; carry on where it cannot.
  kRequired,   ///< Confine, or refuse to run the program.
};

/** @brief What the process actions may run. */
struct ProcessPolicy {
  /** Whether `spawn_process` is allowed at all. */
  bool enabled = false;
  /**
   * Program names or absolute paths that may be run. Empty with @c any_program
   * unset refuses everything, so enabling the action is not by itself a shell.
   */
  std::vector<std::string> programs;
  /** Read-only runtime and toolchain roots needed after the child starts. */
  std::vector<std::string> read_roots;
  /** Writable temporary and runtime roots needed while a child runs. */
  std::vector<std::string> write_roots;
  /** Whether any program may be run. */
  bool any_program = false;
  /** Whether a child inherits this process's environment. */
  bool inherit_environment = false;
  /** Longest a child may run before it is signalled, or zero for no limit. */
  std::int64_t max_seconds = 0;
  /**
   * Kernel confinement for children: Landlock on Linux, Seatbelt on macOS.
   * Defaults to `kPreferred`.
   */
  SandboxRequest sandbox = SandboxRequest::kPreferred;
};

/** @brief Where the network actions may connect, and what they may serve. */
struct NetworkPolicy {
  /** Whether the connecting actions are allowed at all. */
  bool enabled = false;
  /** Whether the listening actions are allowed. Separate: serving is not
   *  connecting, and a host often wants exactly one of them. */
  bool may_listen = false;
  /**
   * Host patterns that may be connected to; `*` matches a label. Empty with
   * @c any_host unset refuses everything.
   */
  std::vector<std::string> hosts;
  /** Whether any host may be connected to. */
  bool any_host = false;
  /**
   * Whether loopback, private (RFC 1918) and link-local addresses may be
   * reached. All default to refused; link-local includes 169.254.169.254.
   */
  bool allow_loopback = false;
  bool allow_private = false;
  bool allow_link_local = false;
  /** Ports that may be listened on, or empty for any. */
  std::vector<std::uint32_t> listen_ports;
};

/** @brief Which environment variables a flow may read. */
struct EnvironmentPolicy {
  std::vector<std::string> names;
  bool any_name = false;
};

/**
 * @brief One host's whole answer, shared by every handler it registered.
 *
 * Handlers share an immutable `Capabilities` instance across calls.
 */
struct Capabilities {
  FilesystemPolicy filesystem;
  ProcessPolicy process;
  NetworkPolicy network;
  EnvironmentPolicy environment;
};

/** @brief A shared, immutable policy. */
using CapabilitiesPtr = std::shared_ptr<const Capabilities>;

/**
 * @brief Resolves @p path and checks it against @p policy.
 *
 * @param policy The filesystem policy to check against.
 * @param path The path as the caller wrote it. Relative paths resolve against
 *        @c policy.current_directory, or the process directory when it is
 *        empty, and are refused when the result is outside every root.
 * @param for_write Whether to require write permission.
 * @return The resolved absolute path, or `permission_denied` naming the
 *         expected root without revealing whether an outside path exists.
 */
absl::StatusOr<std::filesystem::path> ResolvePath(
    const FilesystemPolicy& policy, std::string_view path, bool for_write);

/**
 * @brief Whether @p program may be run, and as what.
 * @return The program to execute, or `permission_denied`.
 */
absl::StatusOr<std::string> ResolveProgram(const ProcessPolicy& policy,
                                           std::string_view program);

/** @brief Whether @p name may be read from the environment. */
absl::Status CheckEnvironment(const EnvironmentPolicy& policy,
                              std::string_view name);

/**
 * @brief Whether @p host may be connected to.
 *
 * Checks the pattern list, and -- for a host that is already a literal address
 * -- the loopback, private and link-local rules. A name that resolves to one of
 * those is caught at connect time, where the address is known, so this is the
 * cheap half of the check rather than the whole of it.
 */
absl::Status CheckHost(const NetworkPolicy& policy, std::string_view host);

/** @brief Whether a resolved address may be connected to. */
absl::Status CheckAddress(const NetworkPolicy& policy,
                          std::string_view address);

/** @brief Whether the process may listen on @p port. */
absl::Status CheckListen(const NetworkPolicy& policy, std::uint32_t port);

}  // namespace a11::sdk::flow

#endif  // A11_SDK_FLOW_ACTIONS_POLICY_H_
