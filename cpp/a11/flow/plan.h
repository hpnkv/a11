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

/// One field of a `struct`, resolved.
///
/// The syntax node's constraints, with the type worked out the way a port's is:
/// `declared` is what was written and `type` is what it *is*. A field naming
/// another `struct` keeps that name in both, and `dto_name` says so, which is what
/// saves every reader from deciding whether a bare name is a shape again.
struct FieldPlan {
  std::string name;
  std::string declared;
  std::string type;
  /// The element type of a `list[T]`, resolved, or empty where it is not a list.
  std::string element;
  /// Whether the field, or a list's element, names a declared shape.
  std::string dto_name;
  std::string element_dto_name;
  bool required = false;
  bool unique = false;
  syntax::FieldRange range;
  std::string pattern;
  bool has_pattern = false;
  std::vector<syntax::Constant> enumeration;
  bool has_enumeration = false;
  syntax::Constant default_value;
  bool has_default = false;
  std::string description;
  syntax::Location location;
};

/// One compiled `struct`: a shape a port may be typed with.
struct DtoPlan {
  std::string name;
  std::string description;
  std::vector<FieldPlan> fields;
  /// Whether the shape holds bytes anywhere in it, directly or through another
  /// shape it names.
  ///
  /// Worked out once, at resolution, because it is what decides whether `| json`
  /// on a value of this shape can mean anything -- and working it out on demand
  /// would mean walking a graph of shapes that may refer to each other.
  bool binary = false;
  syntax::Location location;

  /// The field of this name, or `nullptr`.
  const FieldPlan* absl_nullable Field(std::string_view field_name) const;

  /// Every field name, in declaration order, for a message that lists them.
  std::vector<std::string> FieldNames() const;
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
  /// Empty for the entry flow, which is the one flow that has no name.
  std::string name;
  /**
   * Whether this is the file's entry point, declared `flow { ... }`.
   *
   * Its name is empty, and that is not an accident to be worked around: a flow
   * nothing can name is a flow nothing can `run` or `call`, which is what keeps
   * a program's entry point from being reachable as a library or from
   * recursing into itself. Reach it with Program::Entry(), not by name.
   */
  bool entry = false;
  std::string description;
  std::vector<PortPlan> ports;
  std::vector<HeaderPlan> headers;
  /// The node maps the flow declares, in declaration order.
  std::vector<std::string> node_maps;
  std::vector<StepPlan> steps;
  syntax::Location location;

  /// The port with this name and direction, or `nullptr`.
  const PortPlan* absl_nullable Port(std::string_view port_name,
                                     syntax::PortDirection direction) const;

  /// Whether a port of this name is declared in that direction.
  bool HasPort(std::string_view port_name,
               syntax::PortDirection direction) const {
    return Port(port_name, direction) != nullptr;
  }

  /// Every port name in one direction, sorted, for a message that lists them.
  std::vector<std::string> PortNames(syntax::PortDirection direction) const;
};

/// Every flow and shape one source file declares.
struct Program {
  std::string source_name;
  std::vector<FlowPlan> flows;
  /// The shapes, in declaration order.
  ///
  /// Beside the flows because a shape is not any one flow's: two flows in a file
  /// describe the same records. A name here **outranks** a serialisation
  /// registry tag of the same name, which is the point of being able to declare
  /// one -- what a file says about a shape is what the file means by it.
  std::vector<DtoPlan> dtos;

  /// The flow of this name, or `nullptr`. Never returns the entry flow: an
  /// empty name matches nothing, so a `run`/`call` cannot reach it.
  const FlowPlan* absl_nullable Flow(std::string_view name) const;

  /// The file's entry point, or `nullptr` when it declares none.
  const FlowPlan* absl_nullable Entry() const;

  /// The shape of this name, or `nullptr`.
  const DtoPlan* absl_nullable Dto(std::string_view name) const;
};

/// The `format` field of the plan envelope.
inline constexpr std::string_view kPlanFormat = "flow.plan/v1";

}  // namespace a11::flow

#endif  // A11_FLOW_PLAN_H_
