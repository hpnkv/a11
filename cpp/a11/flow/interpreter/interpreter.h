// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Running a `.flow` file as a program, from anywhere.
 *
 * The interpreter, as a library. `a11-flow-run` is a command line around this
 * and nothing more, and the Python binding calls the same function -- so a
 * program behaves identically whichever started it, and there is one
 * implementation to be right.
 *
 * ### Why the registry is an argument
 *
 * The interesting reason to run a flow from a host rather than from a shell is
 * that the host has actions of its own. A Python process that has registered
 * `interact_with_llm` can hand this its registry, and a program run through it
 * may call the model -- with no subprocess, no second registry, and nothing
 * about the file changed.
 *
 * So a registry passed in is **not** overwritten: RegisterStandardLibrary()
 * leaves any name the host already registered alone. A host that registered its
 * own `read_file` meant its own `read_file`, and silently replacing it with this
 * library's would be the interpreter overruling the program's owner.
 *
 * ### What stays outside the file
 *
 * The policy. a11::sdk::flow::Capabilities is an argument here for the same
 * reason it is a command-line flag in the CLI: a capability a script can grant
 * itself is not a capability anybody granted.
 */

#ifndef A11_FLOW_INTERPRETER_INTERPRETER_H_
#define A11_FLOW_INTERPRETER_INTERPRETER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/actions/registry.h"
#include "a11/flow/diagnostic.h"
#include "a11/flow/values.h"
#include "sdk/flow/actions/policy.h"

namespace a11::flow::interpreter {

/// What to run: the source, and a name for it that diagnostics will quote.
struct Source {
  std::string text;
  std::string name;
};

/// How to run it.
struct RunOptions {
  /**
   * The program's `argv`. By convention `arguments[0]` is the file, as a C
   * program's is, so what a person typed starts at index 1 -- and `argc` is the
   * size of this, counting it.
   */
  std::vector<std::string> arguments;

  /// What the program is allowed to do. Required: there is no default policy,
  /// because the default would be somebody's guess about somebody else's file.
  sdk::flow::CapabilitiesPtr capabilities;

  /// The registry to run against. One is created when this is null; when it is
  /// not, whatever the host already registered stays registered and reachable.
  std::shared_ptr<actions::ActionRegistry> registry;

  /// A bound on the whole run, applied as the `x-a11-deadline` header every
  /// action in the standard library honours -- so it covers what the program
  /// spawned as well as the program.
  std::optional<absl::Duration> timeout;

  /**
   * Whether to bind this process's own standard streams.
   *
   * True for a command line, where standard input is the program's input. Worth
   * clearing in a host that has no useful standard input -- a server, a
   * notebook -- so that a program reading it fails rather than waits forever on
   * a stream nobody is writing.
   */
  bool standard_streams = true;

  /**
   * How the program reaches types and chunks its host knows about.
   *
   * Null means A11's own registry, which is keyed by `typeid` and so can only
   * name types this binary was compiled with -- right for the command line, and
   * not enough for a host. A tagged literal like
   * `a11.sdk.Interaction{role: "user", ..}` is exactly the case: the type is
   * declared in Python, so only a bridge into Python can construct one, and
   * without this the program fails `unimplemented` while the host is holding
   * the very type it named.
   *
   * The same object the runtime takes, so a host that already has one for
   * `make_handler` passes that one here rather than growing a second.
   */
  std::shared_ptr<HostBridge> bridge;
};

/// What came of it.
struct RunOutcome {
  /// The program's exit code: what it put on an `out exit_code: integer` port,
  /// or 0.
  int exit_code = 0;
  /// Everything the compiler said that was not an error. Errors are the status.
  std::vector<Diagnostic> diagnostics;
};

/**
 * @brief Registers the standard library on @p registry, under @p capabilities.
 *
 * Every name the registry already has is left as it is. Called by Run(); exposed
 * because a host may want the actions without running anything yet.
 *
 * @param standard_streams Whether to bind `read_stdin`, `write_stdout` and
 *        `write_stderr`, which RegisterFlowActions deliberately omits.
 */
absl::Status RegisterStandardLibrary(actions::ActionRegistry& registry,
                                    const sdk::flow::CapabilitiesPtr& capabilities,
                                    bool standard_streams);

/**
 * @brief Compiles @p source and runs its entry flow to completion.
 *
 * @return What the program did, or the reason it could not: a compile error with
 *         its line and column, a missing entry flow, a policy that allows
 *         nothing, or the failure the flow itself ended with.
 */
absl::StatusOr<RunOutcome> Run(const Source& source, const RunOptions& options);

/**
 * @brief Compiles @p source and says what its entry flow is, running nothing.
 *
 * What `--check` is, and what a host wants before offering a file to somebody:
 * the ports the program expects, and anything the compiler had to say.
 */
absl::StatusOr<RunOutcome> Check(const Source& source);

/// The entry flow's ports, as text a person reads. Used by `--check`.
absl::StatusOr<std::string> DescribeEntry(const Source& source);

}  // namespace a11::flow::interpreter

#endif  // A11_FLOW_INTERPRETER_INTERPRETER_H_
