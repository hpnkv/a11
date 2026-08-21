// Copyright 2026 The A11 Authors.

#include "a11/flow/interpreter/interpreter.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/flow/plan.h"
#include "a11/flow/runtime.h"
#include "a11/flow/syntax.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/service/session.h"
#include "sdk/flow/actions/flow_actions.h"
#include "sdk/flow/actions/policy.h"
#include "sdk/flow/actions/system_actions.h"

namespace a11::flow::interpreter {
namespace {

/// Feeds the entry flow its arguments.
///
/// Written as chunks rather than through the typed Put<T>: what these ports
/// carry is what a port of type `integer` and a port of type `string` carry
/// everywhere else -- JSON for the number, text/plain for the strings.
absl::Status WriteArguments(const std::shared_ptr<actions::Action>& action,
                            const std::vector<std::string>& arguments) {
  const auto chunk_of = [](std::string_view mimetype,
                           std::string payload) -> data::Chunk {
    data::Chunk chunk;
    chunk.metadata = data::ChunkMetadata{.mimetype = std::string(mimetype)};
    chunk.data = std::move(payload);
    return chunk;
  };

  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<nodes::AsyncNode> argc_node,
                        action->GetInput("argc"));
  ABSL_RETURN_IF_ERROR(
      argc_node
          ->PutChunk(chunk_of("application/json",
                              absl::StrCat(arguments.size())),
                     std::nullopt, /*final=*/true)
          .Await()
          .status());

  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<nodes::AsyncNode> argv_node,
                        action->GetInput("argv"));
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const bool last = index + 1 == arguments.size();
    ABSL_RETURN_IF_ERROR(
        argv_node
            ->PutChunk(chunk_of("text/plain", arguments[index]), std::nullopt,
                       last)
            .Await()
            .status());
  }
  if (arguments.empty()) {
    // An empty argv still has to end, or a flow reading it waits for a value
    // that is never coming.
    ABSL_RETURN_IF_ERROR(
        argv_node->Finalize({.wait = true, .close = false}).Await().status());
  }
  return absl::OkStatus();
}

/**
 * Drains every output the entry flow declared, and reads `exit_code`.
 *
 * Draining is not optional: an output nobody reads stalls whatever is producing
 * it, so a program whose author added an `out` port for debugging must not hang
 * because the interpreter was not interested in it.
 */
int DrainOutputs(const std::shared_ptr<actions::Action>& action,
                 const FlowPlan& plan) {
  int exit_code = 0;
  for (const PortPlan& port : plan.ports) {
    if (port.direction != syntax::PortDirection::kOutput) continue;
    absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> node =
        action->GetOutput(port.name);
    if (!node.ok()) continue;
    while (true) {
      absl::StatusOr<std::optional<data::Chunk>> chunk =
          (*node)->NextChunk().Await();
      if (!chunk.ok() || !chunk->has_value()) break;
      if ((*chunk)->IsNull()) continue;
      if (port.name == "exit_code") {
        std::int64_t requested = 0;
        if (absl::SimpleAtoi((*chunk)->data, &requested)) {
          exit_code = static_cast<int>(requested);
        }
      }
    }
  }
  return exit_code;
}

/// The warnings, and nothing else: an error is the status this returns instead.
std::vector<Diagnostic> WarningsOf(const CompiledProgram& program) {
  std::vector<Diagnostic> kept;
  for (const Diagnostic& diagnostic : program.diagnostics()) {
    if (diagnostic.severity != Severity::kError) kept.push_back(diagnostic);
  }
  return kept;
}

absl::StatusOr<std::shared_ptr<CompiledProgram>> CompileWithEntry(
    const Source& source) {
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<CompiledProgram> program,
                        CompiledProgram::Compile(source.text, source.name));
  if (program->Entry() == nullptr) {
    return absl::NotFoundError(absl::StrCat(
        source.name.empty() ? "this program" : source.name,
        " declares no entry flow. A file that is meant to be run declares one "
        "as `flow { ... }` -- with no name, because an entry point is not "
        "something anything else calls."));
  }
  return program;
}

}  // namespace

absl::Status RegisterStandardLibrary(
    actions::ActionRegistry& registry,
    const sdk::flow::CapabilitiesPtr& capabilities, bool standard_streams) {
  if (capabilities == nullptr) {
    return absl::InvalidArgumentError(
        "a policy is required; see a11::sdk::flow::ReadOnlyCapabilities and its "
        "neighbours");
  }
  // What the host already registered stays registered. A host that put its own
  // `interact_with_llm` -- or its own `read_file` -- in this registry meant it,
  // and this library replacing it would be the interpreter overruling the
  // program's owner. So the standard library goes into a registry of its own
  // and is copied across only where there is nothing in the way.
  actions::ActionRegistry standard;
  ABSL_RETURN_IF_ERROR(
      sdk::flow::RegisterFlowActions(standard, capabilities));
  if (standard_streams) {
    ABSL_RETURN_IF_ERROR(
        sdk::flow::RegisterStandardStreamActions(standard));
  }
  for (const std::string& name : standard.ListRegisteredActions()) {
    if (registry.IsRegistered(name)) continue;
    ABSL_ASSIGN_OR_RETURN(actions::ActionSchema schema,
                          standard.GetSchema(name));
    ABSL_ASSIGN_OR_RETURN(actions::ActionHandler handler,
                          standard.GetHandler(name));
    ABSL_RETURN_IF_ERROR(
        registry.Register(name, std::move(schema), std::move(handler)));
  }
  return absl::OkStatus();
}

absl::StatusOr<RunOutcome> Check(const Source& source) {
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<CompiledProgram> program,
                        CompileWithEntry(source));
  return RunOutcome{.exit_code = 0, .diagnostics = WarningsOf(*program)};
}

absl::StatusOr<std::string> DescribeEntry(const Source& source) {
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<CompiledProgram> program,
                        CompileWithEntry(source));
  const FlowPlan& plan = program->Entry()->plan;
  std::string written = "entry flow";
  if (!plan.description.empty()) {
    absl::StrAppend(&written, ": ", plan.description);
  }
  absl::StrAppend(&written, "\n");
  for (const PortPlan& port : plan.ports) {
    absl::StrAppend(
        &written, "  ",
        port.direction == syntax::PortDirection::kInput ? "in " : "out", " ",
        port.name, ": ", port.declared, port.unary ? "" : " stream", "\n");
  }
  return written;
}

absl::StatusOr<RunOutcome> Run(const Source& source,
                               const RunOptions& options) {
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<CompiledProgram> program,
                        CompileWithEntry(source));
  const ResolvedFlow* entry = program->Entry();

  std::shared_ptr<actions::ActionRegistry> registry = options.registry;
  if (registry == nullptr) {
    registry = std::make_shared<actions::ActionRegistry>();
  }
  ABSL_RETURN_IF_ERROR(RegisterStandardLibrary(*registry, options.capabilities,
                                              options.standard_streams));

  // The bridge, when the host gave one. Left null otherwise, which is what
  // makes the runtime fall back to A11's own registry -- the right answer for a
  // command line, and the wrong one for a host whose types live in Python.
  // Qualified: inside this namespace an unqualified `RunOptions` is the
  // interpreter's, and the runtime's is a different type with the same name.
  ::a11::flow::RunOptions how;
  how.bridge = options.bridge;
  // Where `call` steps go. Given to the handler rather than to the action
  // below, because a locally-run action that holds a stream ends that stream
  // when it finishes -- so the stream belongs to the calls, not to the flow.
  how.dispatch_stream = options.dispatch_stream;
  ABSL_ASSIGN_OR_RETURN(const actions::ActionHandler handler,
                        MakeEntryHandler(program, std::move(how)));

  // The entry flow has no name and an action must have one, so it is named here
  // on a copy of the plan. Only this process sees it: the name is what an action
  // needs to derive its port node ids, not something anything can call.
  FlowPlan named = entry->plan;
  named.name = "main";
  ABSL_ASSIGN_OR_RETURN(const actions::ActionSchema schema, FlowSchema(named));
  // The session's node map when there is a session, because that is what routes
  // a dispatched call's reply fragments back. A program with no peer keeps a map
  // of its own, which is all it ever needed.
  std::shared_ptr<nodes::NodeMap> node_map;
  if (options.session != nullptr) {
    node_map = options.session->GetNodeMap();
  }
  if (node_map == nullptr) {
    ABSL_ASSIGN_OR_RETURN(node_map, nodes::NodeMap::Create());
  }
  // The action itself is bound to the session but *not* to the stream: it is run
  // here, and an action that holds a stream ends it on finishing. Its `call`
  // steps have the stream, through the handler above.
  ABSL_ASSIGN_OR_RETURN(
      const std::shared_ptr<actions::Action> action,
      actions::Action::Create(schema, "main", handler, node_map, nullptr,
                              options.session, registry));

  if (options.timeout.has_value()) {
    ABSL_RETURN_IF_ERROR((action)->SetHeader(
        "x-a11-deadline",
        absl::StrCat(absl::ToUnixMillis(absl::Now() + *options.timeout))));
  }
  ABSL_RETURN_IF_ERROR(WriteArguments(action, options.arguments));
  ABSL_RETURN_IF_ERROR(action->Run().status());

  // Drained before the wait, because a port nobody reads holds the flow that is
  // writing it and the wait would then never finish.
  const int exit_code = DrainOutputs(action, entry->plan);
  ABSL_RETURN_IF_ERROR(
      action->Wait(absl::InfiniteDuration()).Await().status());
  return RunOutcome{.exit_code = exit_code,
                    .diagnostics = WarningsOf(*program)};
}

}  // namespace a11::flow::interpreter
