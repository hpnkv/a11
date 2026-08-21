// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief What a host is willing to let a flow do, decided where it registers.
 *
 * A flow can only call the actions that were registered for it, which is the
 * whole reason accepting one from somewhere else -- a file, a peer, a model --
 * is a reasonable thing to do. Registration is therefore the capability, and
 * this file is what makes that decision expressible at the granularity it
 * actually has: not "may it touch the filesystem" but "may it read under
 * /srv/data and write under /tmp/scratch, and nothing else".
 *
 * Two rules shape the whole file, and they are the reason it is not simply a
 * bag of options on the `options` port:
 *
 *   * **The policy is set at registration and the caller cannot widen it.**
 *     `options` may narrow what one call does; it can never reach past what the
 *     host allowed. A policy a flow could set would be a policy the flow's
 *     author sets, which for a model-authored flow is no policy at all.
 *   * **A capability the host did not name is absent.** Every field defaults to
 *     the refusing value: no roots, no programs, no hosts, not writable. A
 *     host that wants more says so, and a host that says nothing gets a
 *     library that can compute and not much else.
 *
 * The presets in flow_actions.h are the intended interface -- ReadOnly,
 * Workspace, System -- and this is what they are made of.
 *
 * ### Paths
 *
 * ResolvePath() is the one function every filesystem action calls first. It
 * makes a path absolute, resolves `..` and any symlinks along it, and only then
 * checks it against the roots -- in that order, because checking before
 * resolving is how `/srv/data/../../etc/passwd` and a symlink out of a
 * sandbox both get through. The check is on the resolved path, so a symlink
 * pointing out of a root is refused however it was spelled.
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
 * The distinction that matters is between `kPreferred` and `kRequired`, and it
 * is the difference between defence in depth and a guarantee. Every check in
 * this file happens before A11 makes a syscall -- which is all of them for
 * `read_file`, and *none* of the ones a spawned program makes for itself. A
 * flow allowed to run `python` under a policy rooted at `/work` can read
 * `/etc/passwd` through it unless something outside this library says otherwise.
 *
 * `kRequired` is how a host says it would rather not run the program at all
 * than run it unconfined, and it fails at Prepare() time on a system that cannot
 * confine it. See sandbox.h.
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
  /** Whether any program may be run. */
  bool any_program = false;
  /** Whether a child inherits this process's environment. */
  bool inherit_environment = false;
  /** Longest a child may run before it is signalled, or zero for no limit. */
  std::int64_t max_seconds = 0;
  /**
   * Whether to ask the kernel to enforce the filesystem policy on the child as
   * well -- Landlock on Linux, Seatbelt on macOS. Defaults to `kPreferred`,
   * because a sandbox that has to be asked for is one that is usually not, and
   * the cost of the default is a ruleset per spawn.
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
   * reached. All three default to refused, and link-local is the one that
   * matters most: `169.254.169.254` is a cloud instance's credentials, and a
   * flow that can fetch a URL can otherwise fetch those.
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
 * Held by `shared_ptr<const Capabilities>`: the handlers are long-lived and the
 * policy does not change under them, so there is nothing to lock and nothing to
 * copy per call.
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
 *        the process's working directory, which is only useful where a root
 *        contains it -- and refused otherwise, like any other outside path.
 * @param for_write Whether this is a write. Checked here rather than at each
 *        call site so that a read-only policy refuses `write_file` even if
 *        somebody registers it by mistake.
 * @return The resolved absolute path, or `permission_denied` naming the root it
 *         was expected to be under. Deliberately not `not_found`: which paths
 *         exist outside the sandbox is not something a caller should learn from
 *         the error message.
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
