// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_PLAN_H_
#define A11_FLOW_PLAN_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/time/time.h>

#include "a11/flow/syntax.h"

namespace a11::flow {

/// One declared port of a flow, resolved.
struct PortPlan {
  std::string name;
  syntax::PortDirection direction = syntax::PortDirection::kInput;
  /// The type as the flow wrote it, parameters and all: `list[string]`.
  std::string declared;
  /// What the type *is*, for the schema a caller sees: a built-in's mimetype or
  /// Python type name, a registry tag, or a quoted mimetype, exactly as
  /// `a11.flow.plan.TYPE_NAMES` resolves it.
  std::string type;
  bool unary = true;
  bool required = false;
  std::string description;
  syntax::Location location;
};

/// One declared header.
struct HeaderPlan {
  std::string name;
  std::string alias;
  syntax::Constant default_value;
  bool has_default = false;
  std::string description;
  syntax::Location location;
};

/// One resolved statement, as the plan format describes it.
///
/// Flat on purpose: this is what `a11 flow describe` prints and what a reader
/// diffs to see whether a change to a flow changed what it does. The executable
/// graph -- refs with buffers, steps that can be run -- arrives with the native
/// runtime and grows out of these, rather than being a second description of them.
struct StepPlan {
  /// The kind of statement, as `a11.flow.plan`'s `Step.kind` spells it: `call`,
  /// `pipe`, `skip`, `wait`, `drain`, `cancel`, `fail`, `for`, `repeat`, `if`,
  /// `capture`.
  std::string kind;
  /// The name this step is known by: a bound name, or `action`, `for`, `if` with
  /// a `#2` after it where one label is used twice.
  std::string label;
  /// What it waits for, by label.
  std::vector<std::string> after;
  /// For a `call`: the action, the verb, and where it goes.
  std::string action;
  std::string mode;
  std::string node_map;
  std::optional<absl::Duration> timeout;
  bool tolerant = false;
  bool tee = false;
  /// For a `pipe`/`skip`/`wait`/`drain`: what it reads and what it writes, as a
  /// reader would say them -- `search.hits | truncate 200` -> `brief.pages`.
  std::string source;
  std::string destination;
  /// Bodies nested in this step, in the order the language reads them: a `for`
  /// or `repeat` has one, an `if` has two.
  std::vector<std::vector<StepPlan>> bodies;
  syntax::Location location;
};

/// One compiled flow: what a caller sees, and what the runtime will do.
struct FlowPlan {
  std::string name;
  std::string description;
  std::vector<PortPlan> ports;
  std::vector<HeaderPlan> headers;
  /// The node maps the flow declares, in declaration order.
  std::vector<std::string> node_maps;
  std::vector<StepPlan> steps;
  syntax::Location location;

  /// The port with this name and direction, or `nullptr`.
  const PortPlan* absl_nullable Port(std::string_view name,
                                     syntax::PortDirection direction) const;

  /// Whether a port of this name is declared in that direction.
  bool HasPort(std::string_view name,
               syntax::PortDirection direction) const {
    return Port(name, direction) != nullptr;
  }

  /// Every port name in one direction, sorted, for a message that lists them.
  std::vector<std::string> PortNames(syntax::PortDirection direction) const;
};

/// Every flow one source file declares.
struct Program {
  std::string source_name;
  std::vector<FlowPlan> flows;

  const FlowPlan* absl_nullable Flow(std::string_view name) const;
};

/// The `format` field of the plan envelope.
inline constexpr std::string_view kPlanFormat = "flow.plan/v1";

}  // namespace a11::flow

#endif  // A11_FLOW_PLAN_H_
