// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Making the policy the kernel's opinion rather than only this
 * library's.
 *
 * a11::sdk::flow::Capabilities is checked before a path is opened, which is
 * exactly as strong as the checking code -- and no stronger. That is enough for
 * `read_file`, whose every syscall this library makes itself. It is not enough
 * for `spawn_process`, because the program it runs makes its own syscalls and
 * has never heard of the policy. A flow allowed to run `python` under a policy
 * rooted at `/work` can read `/etc/passwd` through it, and every check in
 * policy.cc is irrelevant to that.
 *
 * So the child asks the kernel to enforce it too:
 *
 *   * **Linux: Landlock** (5.13+). A ruleset is built in the parent, naming the
 *     directories the child may read and write; the child calls
 *     `landlock_restrict_self` before `exec`. It is unprivileged, it cannot be
 *     dropped once applied, and it is inherited by everything the child spawns.
 *   * **macOS: Seatbelt**, through `sandbox-exec`. A profile is built in
 *     the parent and the child execs `/usr/bin/sandbox-exec -p <profile>`
 *     wrapping the real program.
 *
 * ### Why macOS goes through a process and Linux does not
 *
 * `sandbox_init()` allocates, and the child of a fork in a threaded process
 * must not: it holds whatever locks the other threads held at the moment of
 * the fork, so a malloc there deadlocks it, rarely and unreproducibly.
 * Landlock has no such problem -- the allocation is the ruleset, built before
 * the fork, and the child makes two bare syscalls against a descriptor. macOS
 * has no equivalent fork-safe entry point, so rather than take a small chance
 * of an undebuggable hang, the profile is passed as `argv` and enforcement
 * begins in a process that has already exec'd. One extra process, and nothing
 * that can deadlock.
 *
 * ### What it does not claim
 *
 * These are **defence in depth, not a replacement for the policy**. Landlock
 * before kernel 6.7 does not restrict network access at all, `sandbox-exec` is
 * deprecated by Apple (it works, and has for a decade, but it is not a
 * contract), and neither confines a child that was already allowed to run
 * something dangerous. Availability() says what the running system can actually
 * do, and `Capabilities::process::require_sandbox` decides whether a host that
 * asked for confinement will accept running without it -- because "the sandbox
 * quietly did not apply" is the one outcome that would make this file worse
 * than
 * not having it.
 */

#ifndef A11_SDK_FLOW_ACTIONS_SANDBOX_H_
#define A11_SDK_FLOW_ACTIONS_SANDBOX_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "sdk/flow/actions/policy.h"

namespace a11::sdk::flow {

/** @brief What kind of confinement a system can apply. */
enum class SandboxKind {
  kNone,      ///< Nothing available; the policy is this library's checks alone.
  kLandlock,  ///< Linux LSM, applied in the child before exec.
  kSeatbelt,  ///< macOS, applied by exec'ing through sandbox-exec.
};

/**
 * @brief What the running system can do, probed once.
 *
 * The three booleans are not decoration, and a host that cares should read them
 * rather than assume that "sandboxed" means the same thing everywhere. It does
 * not, and the difference is large:
 *
 *   * **Linux/Landlock** confines reads and writes, and -- from ABI 4 -- TCP.
 *   * **macOS/Seatbelt** as this library builds it confines *writes* and the
 *     network, and **not reads**. A deny-default read profile that also lets a
 *     dynamically linked program start needs an exact list of what dyld
 *     touches, which is undocumented and changes between releases; the
 *     alternative on offer -- importing Apple's own `bsd.sb` base profile --
 *     leaves `/etc/passwd` readable. Given the choice between a sandbox that
 *     silently allows reads and one that says it allows reads, this library
 *     says it.
 *
 * So on macOS a confined child can still *read* what the policy forbids, and
 * the only thing standing between a spawned program and `/etc/passwd` there is
 * that nothing in the flow asked it to look. Reported, so it is a known
 * limitation rather than a false sense of security.
 */
struct SandboxAvailability {
  SandboxKind kind = SandboxKind::kNone;
  /// Landlock's ABI version, or 0. Version 1 has no network rules; 4 adds them.
  int abi_version = 0;
  /// Whether the child's reads are confined to the policy's roots.
  bool confines_reads = false;
  /// Whether the child's writes are confined to the policy's roots.
  bool confines_writes = false;
  /// Whether the child's network access is confined.
  bool confines_network = false;
  /// Why it is unavailable, for a diagnostic a host can act on.
  std::string why_not;
};

/** @brief Probes the running kernel. Cheap after the first call. */
const SandboxAvailability& Availability();

/**
 * @brief A prepared confinement: everything allocatable, done before the fork.
 *
 * Built by Prepare() in the parent. `Apply()` runs in the child between fork
 * and exec and calls nothing that allocates. `WrapCommand()` is the other
 * half, for the platform where confinement is reached by exec'ing something
 * else.
 */
class Sandbox {
 public:
  /**
   * @brief Prepares confinement of a child under @p capabilities.
   *
   * @param capabilities The policy to enforce. The filesystem roots become the
   *        directories the child may reach; an unrestricted filesystem policy
   *        prepares nothing, because there is nothing to confine it to.
   * @param program The program about to be run, which has to be readable and
   *        executable however narrow the rest of the policy is.
   * @return The prepared sandbox. A policy that asks for confinement on a
   *         system that cannot provide it is an error here rather than a
   *         surprise later -- see ProcessPolicy::require_sandbox.
   */
  static absl::StatusOr<std::shared_ptr<Sandbox>> Prepare(
      const Capabilities& capabilities, std::string_view program);

  ~Sandbox();
  Sandbox(const Sandbox&) = delete;
  Sandbox& operator=(const Sandbox&) = delete;

  /** @brief Whether anything will actually be enforced. */
  [[nodiscard]] bool active() const { return kind_ != SandboxKind::kNone; }

  /** @brief Which mechanism this is. */
  [[nodiscard]] SandboxKind kind() const { return kind_; }

  /**
   * @brief Applies the confinement. **Runs in the child, after fork.**
   *
   * Async-signal-safe and allocation-free: on Linux it is a `prctl` and a
   * `landlock_restrict_self` against a descriptor prepared before the fork, and
   * everywhere else it does nothing.
   *
   * @return 0, or an errno the child should report and die on. Not a Status,
   *         because building one allocates and this frame may not.
   */
  [[nodiscard]] int Apply() const;

  /**
   * @brief Rewrites the command so that exec'ing it enters the sandbox.
   *
   * The macOS half. Returns the arguments unchanged where confinement is
   * applied by Apply() instead, so a caller writes this once and does not
   * branch on the platform.
   */
  [[nodiscard]] std::vector<std::string> WrapCommand(
      std::string_view program,
      const std::vector<std::string>& arguments) const;

  /** @brief The program to exec, after WrapCommand. */
  [[nodiscard]] std::string WrapProgram(std::string_view program) const;

  /** @brief A one-line description, for the action's `sandbox` output port. */
  [[nodiscard]] std::string Describe() const;

 private:
  Sandbox() = default;

  SandboxKind kind_ = SandboxKind::kNone;
  /// Landlock: the ruleset descriptor, created and populated before the fork.
  int ruleset_fd_ = -1;
  /// Seatbelt: the profile text, built before the fork and passed as argv.
  std::string profile_;
  std::string description_;
};

}  // namespace a11::sdk::flow

#endif  // A11_SDK_FLOW_ACTIONS_SANDBOX_H_
