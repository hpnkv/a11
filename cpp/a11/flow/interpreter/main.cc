// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief `a11-flow-run` -- runs a `.flow` file as a program.
 *
 * Flow was a way to describe how deployed actions connect. With the standard
 * library beside it -- files, a clock, processes, this process's own standard
 * streams -- a file can also just be a program, and this is what runs one:
 *
 * @code{.sh}
 *   a11-flow-run wc.flow < some-file
 *   a11-flow-run greet.flow -- Helena
 * @endcode
 *
 * The file says which of it is the program by declaring a flow with no name:
 *
 * @code{.a11flow}
 *   flow {
 *     describe "What this program does."
 *     ...
 *   }
 * @endcode
 *
 * and everything else in the file -- named flows, `struct`s -- is what that one
 * uses. `argc` and `argv` arrive as ports nobody declared, `argv[0]` being the
 * program's own name as in C.
 *
 * ### What a program may expect to be bound
 *
 * Everything in a11::sdk::flow, plus the standard streams. That is the contract
 * this interpreter offers and the reason a file can be written against it: a
 * program that reads `read_stdin` and writes `write_stdout` will find them, and
 * one that reads a file will find `read_file` -- inside whatever roots this run
 * allows, which is the working directory unless told otherwise.
 *
 * ### What it deliberately does not do
 *
 * There is no way to widen the policy from inside the file. `--allow-write`,
 * `--root`, `--allow-run` and `--allow-net` are arguments to *this* program,
 * because a capability a script could grant itself is not a capability anybody
 * granted. A file handed to this interpreter can therefore be read for what it
 * will do, and the command line is where what it *may* do is written.
 *
 * ### The exit code
 *
 * Zero when the entry flow finished, and non-zero when it failed -- with the
 * failure on standard error as a diagnostic, positioned in the source where the
 * language could say where. A flow that wants to choose its own exit code puts
 * one on an `out exit_code: integer` port.
 */

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/time/time.h>

#include "a11/flow/diagnostic.h"
#include "a11/flow/interpreter/interpreter.h"
#include "sdk/flow/actions/flow_actions.h"
#include "sdk/flow/actions/options.h"
#include "sdk/flow/actions/policy.h"

namespace a11::flow::interpreter {
namespace {

using ::a11::sdk::flow::CapabilitiesBuilder;
using ::a11::sdk::flow::SandboxRequest;

constexpr std::string_view kUsage = R"(Run a .flow file as a program.

Usage: a11-flow-run [options] <file.flow> [-- program arguments...]

The file declares what to run as a flow with no name:

    flow {
      describe "What this program does."
      ...
    }

Its `argc` and `argv` inputs arrive without being declared, argv[0] being the
file's own path. Everything else in the file -- named flows, structs -- is what
that one uses.

What the program may do is decided here and cannot be widened from inside the
file:

  --root <dir>       A directory the program may read. Repeatable. Defaults to
                     the working directory.
  --allow-write      Let it write, inside those roots.
  --allow-run        Let it run programs (spawn_process), confined by the
                     kernel where the platform can.
  --allow-net        Let it reach the network. Loopback, private and link-local
                     addresses stay refused unless --allow-local-net.
  --allow-local-net  Also allow loopback, private and link-local addresses.
                     Say this deliberately: 169.254.169.254 is a cloud
                     instance's credentials.
  --allow-env <NAME> Let it read that environment variable. Repeatable.
  --unrestricted     No filesystem sandbox at all. For a file you wrote.
  --timeout <dur>    Give up after this long: 30s, 1m30s, 500ms.
  --check            Compile and report, run nothing.
  --print-plan       Say what the entry flow is, then run it.
  --help             This.
)";

/// What the command line asked for.
struct Options {
  std::string path;
  std::vector<std::string> arguments;  ///< argv for the program, [0] = path.
  std::vector<std::string> roots;
  std::vector<std::string> environment;
  bool writable = false;
  bool may_run = false;
  bool may_reach_network = false;
  bool may_reach_local_network = false;
  bool unrestricted = false;
  bool check_only = false;
  bool print_plan = false;
  std::optional<absl::Duration> timeout;
};

absl::StatusOr<Options> ParseCommandLine(int argc, char** argv) {
  Options options;
  int index = 1;
  const auto value_for = [&](std::string_view flag) -> absl::StatusOr<std::string> {
    if (index + 1 >= argc) {
      return absl::InvalidArgumentError(absl::StrCat(flag, " needs a value"));
    }
    return std::string(argv[++index]);
  };

  for (; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--") {
      // Everything after this belongs to the program, not to this interpreter.
      // Without it a program could not take an argument called `--check`.
      for (int rest = index + 1; rest < argc; ++rest) {
        options.arguments.emplace_back(argv[rest]);
      }
      break;
    }
    if (argument == "--help" || argument == "-h") {
      std::cout << kUsage;
      std::exit(0);
    }
    if (argument == "--check") {
      options.check_only = true;
      continue;
    }
    if (argument == "--print-plan") {
      options.print_plan = true;
      continue;
    }
    if (argument == "--allow-write") {
      options.writable = true;
      continue;
    }
    if (argument == "--allow-run") {
      options.may_run = true;
      continue;
    }
    if (argument == "--allow-net") {
      options.may_reach_network = true;
      continue;
    }
    if (argument == "--allow-local-net") {
      options.may_reach_network = true;
      options.may_reach_local_network = true;
      continue;
    }
    if (argument == "--unrestricted") {
      options.unrestricted = true;
      continue;
    }
    if (argument == "--root") {
      ABSL_ASSIGN_OR_RETURN(std::string root, value_for("--root"));
      options.roots.push_back(std::move(root));
      continue;
    }
    if (argument == "--allow-env") {
      ABSL_ASSIGN_OR_RETURN(std::string name, value_for("--allow-env"));
      options.environment.push_back(std::move(name));
      continue;
    }
    if (argument == "--timeout") {
      ABSL_ASSIGN_OR_RETURN(const std::string written, value_for("--timeout"));
      ABSL_ASSIGN_OR_RETURN(const absl::Duration parsed,
                            a11::sdk::flow::ParseDuration(written));
      options.timeout = parsed;
      continue;
    }
    if (absl::StartsWith(argument, "-")) {
      return absl::InvalidArgumentError(
          absl::StrCat("Unknown option '", argument, "'. Try --help."));
    }
    if (!options.path.empty()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Only one file can be run; got '", options.path, "' and '", argument,
          "'. Arguments for the program go after '--'."));
    }
    options.path = std::string(argument);
  }
  if (options.path.empty()) {
    return absl::InvalidArgumentError("No file to run. Try --help.");
  }
  // argv[0] is the file, the way a C program's argv[0] is itself. A flow
  // reading `argv[0]` gets what it expects, and `argc` agrees.
  options.arguments.insert(options.arguments.begin(), options.path);
  return options;
}

absl::StatusOr<std::string> ReadWholeFile(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return absl::NotFoundError(absl::StrCat("cannot open ", path));
  }
  std::string contents;
  char buffer[16384];
  while (const std::size_t got = std::fread(buffer, 1, sizeof(buffer), file)) {
    contents.append(buffer, got);
  }
  const bool bad = std::ferror(file) != 0;
  std::fclose(file);
  if (bad) {
    return absl::UnavailableError(absl::StrCat("cannot read ", path));
  }
  return contents;
}

/// The capabilities the command line asked for, and nothing beyond them.
CapabilitiesBuilder CapabilitiesFrom(const Options& options) {
  std::vector<std::string> roots = options.roots;
  if (roots.empty() && !options.unrestricted) {
    // The working directory, which is what somebody running a script in a
    // directory means by "here". Named rather than implied by an empty list,
    // because an empty root list refuses everything.
    roots.emplace_back(".");
  }
  CapabilitiesBuilder capabilities =
      options.writable ? a11::sdk::flow::WorkspaceCapabilities(std::move(roots))
                       : a11::sdk::flow::ReadOnlyCapabilities(std::move(roots));
  if (options.unrestricted) {
    capabilities->filesystem.unrestricted = true;
    capabilities->filesystem.writable = options.writable;
  }
  capabilities->process.enabled = options.may_run;
  capabilities->process.any_program = options.may_run;
  capabilities->process.inherit_environment = options.may_run;
  // Confined where the platform can, and the program is told which parts were
  // actually confined on its `sandbox` port. Not `kRequired`: a script run on a
  // kernel without Landlock should still run, and the policy checks still hold.
  capabilities->process.sandbox = SandboxRequest::kPreferred;
  capabilities->network.enabled = options.may_reach_network;
  capabilities->network.any_host = options.may_reach_network;
  capabilities->network.may_listen = options.may_reach_network;
  capabilities->network.allow_loopback = options.may_reach_local_network;
  capabilities->network.allow_private = options.may_reach_local_network;
  capabilities->network.allow_link_local = options.may_reach_local_network;
  capabilities->environment.names = options.environment;
  return capabilities;
}

/// Prints a diagnostic the way a compiler does: file, line, column, why.
void PrintDiagnostic(const Diagnostic& diagnostic, const std::string& path) {
  std::cerr << path << ":" << diagnostic.range.start.line + 1 << ":"
            << diagnostic.range.start.column + 1 << ": "
            << (diagnostic.severity == Severity::kError ? "error" : "warning")
            << ": " << diagnostic.message << " [" << diagnostic.code << "]\n";
}

int Main(int argc, char** argv) {
  absl::StatusOr<Options> options = ParseCommandLine(argc, argv);
  if (!options.ok()) {
    std::cerr << "a11-flow-run: " << options.status().message() << "\n";
    return 2;
  }
  absl::StatusOr<std::string> text = ReadWholeFile(options->path);
  if (!text.ok()) {
    std::cerr << "a11-flow-run: " << text.status().message() << "\n";
    return 2;
  }
  const Source source{.text = *std::move(text), .name = options->path};

  if (options->print_plan || options->check_only) {
    absl::StatusOr<std::string> described = DescribeEntry(source);
    if (!described.ok()) {
      std::cerr << options->path << ": " << described.status().message() << "\n";
      return 2;
    }
    std::cout << *described;
  }
  if (options->check_only) {
    const absl::StatusOr<RunOutcome> checked = Check(source);
    if (!checked.ok()) {
      std::cerr << options->path << ": " << checked.status().message() << "\n";
      return 2;
    }
    for (const Diagnostic& diagnostic : checked->diagnostics) {
      PrintDiagnostic(diagnostic, options->path);
    }
    return 0;
  }

  RunOptions how;
  how.arguments = options->arguments;
  how.capabilities = CapabilitiesFrom(*options);
  how.timeout = options->timeout;
  how.standard_streams = true;

  const absl::StatusOr<RunOutcome> outcome = Run(source, how);
  if (!outcome.ok()) {
    std::cerr << options->path << ": " << outcome.status().message() << "\n";
    // 2 for "could not run it", 1 for "ran and failed" -- the distinction a
    // shell script wants, and the same one a compiler makes.
    return outcome.status().code() == absl::StatusCode::kNotFound ||
                   outcome.status().code() == absl::StatusCode::kInvalidArgument
               ? 2
               : 1;
  }
  for (const Diagnostic& diagnostic : outcome->diagnostics) {
    PrintDiagnostic(diagnostic, options->path);
  }
  return outcome->exit_code;
}

}  // namespace
}  // namespace a11::flow::interpreter

int main(int argc, char** argv) {
  return a11::flow::interpreter::Main(argc, argv);
}
