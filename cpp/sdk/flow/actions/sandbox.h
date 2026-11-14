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
 * @brief Kernel confinement for child processes.
 *
 * In-process capability checks do not govern a spawned program's syscalls.
 * Linux uses Landlock rules prepared in the parent and applied before exec.
 * macOS uses a Seatbelt profile passed through `sandbox-exec`; applying the
 * profile after exec avoids allocation in the child between fork and exec.
 *
 * Landlock before ABI 4 does not confine network access. macOS confinement
 * permits general reads outside the workspace. Availability() reports the
 * supported boundaries; `process.sandbox = kRequired` requires enforcement.
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
 * Landlock confines reads and writes, plus TCP from ABI 4. This Seatbelt
 * profile confines writes and network access and denies selected sensitive
 * reads. General reads outside the workspace remain available on macOS;
 * consult confines_reads before relying on read confinement.
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
 * Prepare() allocates in the parent. Apply() runs between fork and exec without
 * allocating. WrapCommand() supplies the macOS `sandbox-exec` command.
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
   * @return The prepared sandbox, or an error when required confinement is
   *         unavailable. See ProcessPolicy::sandbox.
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
   * Adds `sandbox-exec` on macOS; leaves arguments unchanged elsewhere.
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
