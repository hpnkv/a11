// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_RUNTIME_H_
#define A11_FLOW_RUNTIME_H_

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/actions/action.h"
#include "a11/actions/schema.h"
#include "a11/flow/graph.h"
#include "a11/flow/parser.h"
#include "a11/flow/plan.h"
#include "a11/flow/resolve.h"
#include "a11/flow/values.h"

namespace a11::net {
class WireStream;
}  // namespace a11::net

namespace a11::flow {

/// How many values a pipe may run ahead of its reader.
///
/// Small on purpose: A11's own stores are the buffer, and a flow should not
/// become a second one.
inline constexpr size_t kQueueDepth = 8;

/// One flow file, compiled: the plans, the graphs, and the tree they borrow.
///
/// A graph points into the parse tree it was resolved from, so the two have one
/// owner and one lifetime. Held by `shared_ptr` because a registered flow's
/// handler outlives whatever compiled it -- register the flows and drop the
/// program, and the handlers still work.
class CompiledProgram {
 public:
  /// Compile source, or fail with the first error the way `flow.loads` does.
  ///
  /// The strict door: the parser and the resolver both recover and report
  /// everything, and this turns the first `Severity::kError` into a status with
  /// the line and column on it. Everything less than an error is kept for a
  /// caller that wants to see it.
  static absl::StatusOr<std::shared_ptr<CompiledProgram>> Compile(
      std::string source, std::string source_name = {});

  const Program& program() const { return resolved_.program; }
  const std::string& source() const { return source_; }
  const std::string& source_name() const { return source_name_; }
  const std::vector<Diagnostic>& diagnostics() const {
    return resolved_.diagnostics;
  }

  /// The flows in the order they were declared.
  const std::vector<ResolvedFlow>& flows() const { return resolved_.flows; }

  /// The flow of this name, or `nullptr`.
  const ResolvedFlow* absl_nullable Flow(std::string_view name) const;

 private:
  CompiledProgram() = default;

  std::string source_;
  std::string source_name_;
  ParseResult parsed_;
  ResolveResult resolved_;
};

/// The [actions::ActionSchema] a flow presents.
///
/// A flow is an action: it has ports, headers and a name, so anything that can
/// dispatch an action can dispatch a composition without being told it is one.
absl::StatusOr<actions::ActionSchema> FlowSchema(const FlowPlan& plan);

/// What a run needs besides the program.
struct RunOptions {
  /// Who answers the questions only the host can: coercion into a registered
  /// type, and reading and writing a chunk. The native registry's answers are
  /// used when this is null.
  std::shared_ptr<HostBridge> bridge;

  /// For a flow a **client** runs over a session it already holds: the `call`
  /// steps that belong to the peer are bound to this stream, and the flow's own
  /// action is not.
  ///
  /// Binding the flow itself would end the stream when the flow finished, and
  /// the session could then dispatch nothing -- so a client that passed its
  /// stream as the action's would find its second flow unable to reach the peer
  /// at all.
  std::shared_ptr<net::WireStream> dispatch_stream;
};

/// The action handler that runs one flow of `program`.
///
/// Registering this makes the composition an action like any other: a peer can
/// dispatch it, another flow can call it, and a model can be offered it as a
/// tool, without any of them knowing it is a composition.
absl::StatusOr<actions::ActionHandler> MakeHandler(
    std::shared_ptr<const CompiledProgram> program, std::string_view flow,
    RunOptions options = {});

}  // namespace a11::flow

#endif  // A11_FLOW_RUNTIME_H_
