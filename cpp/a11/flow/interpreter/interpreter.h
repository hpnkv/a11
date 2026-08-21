// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Running a `.flow` file as a program, from anywhere.
 *
 * Run a Flow program against a host's action registry and capability policy.
 * Host registrations take precedence over standard-library actions. The
 * `a11-flow-run` command and Python binding call this API.
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
#include "a11/net/wire_stream.h"
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

  /// Required policy describing what the program may do.
  sdk::flow::CapabilitiesPtr capabilities;

  /// Registry to run against. A null value creates a new registry.
  std::shared_ptr<actions::ActionRegistry> registry;

  /// Bound for the run and the standard-library actions it starts.
  std::optional<absl::Duration> timeout;

  /**
   * Whether to bind this process's own standard streams.
   *
   * Disable this in hosts such as servers and notebooks that do not provide
   * useful process streams.
   */
  bool standard_streams = true;

  /**
   * How the program reaches types and chunks its host knows about.
   *
   * A null bridge uses A11's C++ type registry. Hosts provide a bridge when
   * programs need host-defined types or chunk conversions.
   */
  std::shared_ptr<HostBridge> bridge;

  /**
   * Where the program's `call` steps go, when they are to leave this process.
   *
   * Null -- the default -- means a program composes what this process serves,
   * and a `call` naming something with no handler here fails saying so. Given a
   * stream, `call` steps are dispatched on it while `run` steps stay local,
   * which is the same split a named flow gets from
   * ::a11::flow::RunOptions::dispatch_stream.
   *
   * The peer's actions still have to be registered in @c registry for their
   * schemas, because the resolver looks a name up before it decides anything.
   */
  std::shared_ptr<net::WireStream> dispatch_stream;

  /**
   * The session the dispatch stream belongs to.
   *
   * Required alongside @c dispatch_stream and useless without it: a dispatched
   * call's reply fragments are routed by the session's node map, so a program
   * given a stream and no session would send its calls and never hear back.
   */
  std::shared_ptr<service::Session> session;
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
 * @param registry Registry to extend with standard actions.
 * @param capabilities Policy applied by capability-sensitive actions.
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
