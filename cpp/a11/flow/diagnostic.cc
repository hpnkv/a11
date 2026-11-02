// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "a11/flow/diagnostic.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

#include <absl/types/span.h>

#include "a11/utf8.h"

namespace a11::flow {
namespace {

// Every code the language publishes, sorted by code so a listing is stable and
// a lookup can binary-search.
constexpr std::array kCodes = {
    CodeInfo{"flow.barrier.after-order", Family::kBarrier, Severity::kWarning,
             "'after' references a later step or the current statement."},
    CodeInfo{"flow.barrier.after-reads-subject", Family::kBarrier,
             Severity::kError,
             "A call argument reads an output of that call, creating a "
             "dependency cycle."},
    CodeInfo{"flow.barrier.cancel-after-wait", Family::kBarrier,
             Severity::kWarning,
             "A call is cancelled after its completion has been awaited."},
    CodeInfo{"flow.barrier.carry-outside-repeat", Family::kBarrier,
             Severity::kError, "'<-' requires an enclosing 'repeat'."},
    CodeInfo{"flow.barrier.continuation-cycle", Family::kBarrier,
             Severity::kError,
             "A continuation completion depends on a writer it prevents."},
    CodeInfo{"flow.barrier.duplicate", Family::kBarrier, Severity::kWeakWarning,
             "A second 'wait' or 'close' repeats an existing barrier."},
    CodeInfo{"flow.barrier.duplicate-carry", Family::kBarrier, Severity::kError,
             "A repeat carries the same name twice."},
    CodeInfo{"flow.barrier.duplicate-until", Family::kBarrier, Severity::kError,
             "A loop has multiple stop conditions."},
    CodeInfo{"flow.barrier.expression-cycle", Family::kBarrier,
             Severity::kError, "Expression cycle."},
    CodeInfo{"flow.barrier.scope-cycle", Family::kBarrier, Severity::kError,
             "A preceding producer needs a consumer after a scope barrier."},
    CodeInfo{"flow.barrier.unordered-clock", Family::kBarrier,
             Severity::kWarning,
             "A clock read and a flow-produced value have no defined order; "
             "add an 'after' dependency."},
    CodeInfo{"flow.barrier.until-outside-repeat", Family::kBarrier,
             Severity::kError,
             "'until' or 'while' requires an enclosing loop."},
    CodeInfo{"flow.barrier.until-parallel", Family::kBarrier, Severity::kError,
             "'until' cannot determine a stop condition for parallel passes."},
    CodeInfo{"flow.barrier.value-read-twice", Family::kBarrier,
             Severity::kWarning,
             "Two scalar reads in one statement consume separate stream "
             "values."},
    CodeInfo{"flow.barrier.wait-lends-node", Family::kBarrier,
             Severity::kWarning,
             "'wait' or 'close' targets a node with no writers."},
    CodeInfo{"flow.barrier.wrong-carry", Family::kBarrier, Severity::kError,
             "A carry references a value outside its repeat state."},
    CodeInfo{"flow.form.bad-number", Family::kForm, Severity::kError,
             "Invalid numeric literal."},
    CodeInfo{"flow.form.bad-pattern", Family::kForm, Severity::kError,
             "Invalid 'match' pattern: unclosed hole, unknown kind, or "
             "duplicate hole name."},
    CodeInfo{"flow.form.continuation-grouping", Family::kForm, Severity::kError,
             "A transfer and broadcast need explicit unambiguous grouping."},
    CodeInfo{"flow.form.count-not-positive", Family::kForm, Severity::kWarning,
             "A count must be positive, as in 'parallel 1' or 'skip 1'."},
    CodeInfo{"flow.form.declaration-attribute", Family::kForm, Severity::kError,
             "A declaration attribute is invalid or out of place."},
    CodeInfo{"flow.form.declaration-body", Family::kForm, Severity::kError,
             "A declaration body violates its module or flow form."},
    CodeInfo{"flow.form.declaration-conflict", Family::kForm, Severity::kError,
             "Callable declarations have incompatible contracts."},
    CodeInfo{"flow.form.declaration-schema-mismatch", Family::kForm,
             Severity::kError,
             "A callable implementation does not satisfy its declaration."},
    CodeInfo{"flow.form.default-on-required", Family::kForm, Severity::kWarning,
             "A required field has an unreachable default."},
    CodeInfo{"flow.form.duplicate-field", Family::kForm, Severity::kError,
             "A field is declared twice in one shape."},
    CodeInfo{"flow.form.duplicate-flow", Family::kForm, Severity::kError,
             "Two flows in one file are declared with the same name."},
    CodeInfo{"flow.form.duplicate-header", Family::kForm, Severity::kError,
             "A normalized header name is declared more than once."},
    CodeInfo{"flow.form.duplicate-port", Family::kForm, Severity::kError,
             "A port is declared twice in one direction."},
    CodeInfo{"flow.form.duplicate-struct", Family::kForm, Severity::kError,
             "Two structs in one file are declared with the same name."},
    CodeInfo{"flow.form.duration-unit", Family::kForm, Severity::kError,
             "Invalid duration unit suffix."},
    CodeInfo{"flow.form.empty-range", Family::kForm, Severity::kError,
             "An empty range or reversed range bounds."},
    CodeInfo{"flow.form.field-constraint", Family::kForm, Severity::kError,
             "A field constraint is incompatible with its type, such as "
             "'unique' on a scalar or 'matching' on a number."},
    CodeInfo{"flow.form.field-modifier-order", Family::kForm,
             Severity::kWarning, "Field modifiers are out of canonical order."},
    CodeInfo{"flow.form.field-type-mismatch", Family::kForm, Severity::kError,
             "A field default or 'one of' value does not match its type."},
    CodeInfo{"flow.form.forward-headers", Family::kForm, Severity::kError,
             "'headers' without the 'forward' that owns it."},
    CodeInfo{"flow.form.handle-escape", Family::kForm, Severity::kError,
             "A call handle cannot be embedded as data."},
    CodeInfo{"flow.form.import-context", Family::kForm, Severity::kError,
             "An import has invalid context or module identity."},
    CodeInfo{"flow.form.import-scope", Family::kForm, Severity::kError,
             "An import appears outside an allowed scope."},
    CodeInfo{"flow.form.invalid-action-target", Family::kForm, Severity::kError,
             "An action target has an invalid form."},
    CodeInfo{"flow.form.log-format", Family::kForm, Severity::kError,
             "'logf' requires a format string."},
    CodeInfo{"flow.form.log-value", Family::kForm, Severity::kError,
             "'log' requires a value."},
    CodeInfo{"flow.form.missing-field", Family::kForm, Severity::kError,
             "A value omits a required field with no default."},
    CodeInfo{"flow.form.multiple-implementations", Family::kForm,
             Severity::kWarning,
             "A callable has multiple executable implementations."},
    CodeInfo{"flow.form.node-parentheses", Family::kForm, Severity::kError,
             "Node construction requires 'node()' or 'node(id)'."},
    CodeInfo{"flow.form.not-json-representable", Family::kForm,
             Severity::kError,
             "JSON cannot represent raw bytes; use 'packb' for binary data."},
    CodeInfo{"flow.form.not-waitable", Family::kForm, Severity::kError,
             "'wait' requires a waitable call, node, or stream."},
    CodeInfo{"flow.form.one-of-empty", Family::kForm, Severity::kError,
             "'one of' requires at least one allowed value."},
    CodeInfo{"flow.form.one-of-not-a-list", Family::kForm, Severity::kError,
             "'one of' requires a list of constants."},
    CodeInfo{
        "flow.form.optional-binding", Family::kForm, Severity::kError,
        "An optional binding requires an option with validated provenance."},
    CodeInfo{"flow.form.pipeline-call", Family::kForm, Severity::kError,
             "A pipeline invocation has an unknown or incompatible interface."},
    CodeInfo{"flow.form.port-modifier-order", Family::kForm, Severity::kError,
             "'stream' and 'required' follow the port type."},
    CodeInfo{"flow.form.private-type-exposed", Family::kForm, Severity::kError,
             "A public interface exposes a private type."},
    CodeInfo{"flow.form.repeated-modifier", Family::kForm, Severity::kError,
             "A modifier appears more than once."},
    CodeInfo{"flow.form.resource-limit", Family::kForm, Severity::kError,
             "A host-configured parsing or compilation limit was reached."},
    CodeInfo{"flow.form.scope-barrier", Family::kForm, Severity::kError,
             "A bare scope barrier cannot be modified or used as a value."},
    CodeInfo{"flow.form.sink-as-source", Family::kForm, Severity::kError,
             "A continuation sink cannot be read or carried as payload."},
    CodeInfo{"flow.form.stage-argument", Family::kForm, Severity::kError,
             "A stage argument is missing or has the wrong form."},
    CodeInfo{"flow.form.statement-as-value", Family::kForm, Severity::kError,
             "A statement is used where an expression is required."},
    CodeInfo{"flow.form.struct-shadows-builtin", Family::kForm,
             Severity::kError,
             "A struct is named after a built-in type, which nothing could "
             "then write."},
    CodeInfo{"flow.form.unbounded-repeat", Family::kForm, Severity::kError,
             "'repeat' requires 'until', 'while', or 'max'."},
    CodeInfo{"flow.form.unconditional-abort", Family::kForm, Severity::kError,
             "A top-level 'abort' has no condition, 'after', or data-dependent "
             "argument, so it can run immediately."},
    CodeInfo{"flow.form.unconditional-cancel", Family::kForm, Severity::kError,
             "A 'cancel' at the top of a flow's body with no 'after': it runs "
             "at once rather than where it is written."},
    CodeInfo{"flow.form.unconditional-fail", Family::kForm, Severity::kError,
             "A top-level 'fail' has no condition, 'after', or data-dependent "
             "argument, so it can run immediately."},
    CodeInfo{"flow.form.unconditional-log", Family::kForm, Severity::kError,
             "A top-level 'log' has no condition, 'after', or data-dependent "
             "argument, so it can run immediately."},
    CodeInfo{"flow.form.unconditional-logf", Family::kForm, Severity::kError,
             "A top-level 'logf' has no condition, 'after', or data-dependent "
             "argument, so it can run immediately."},
    CodeInfo{"flow.form.unknown-builtin", Family::kForm, Severity::kError,
             "A function name does not match a Flow built-in."},
    CodeInfo{"flow.form.unknown-field", Family::kForm, Severity::kError,
             "A value names a field absent from its declared shape."},
    CodeInfo{"flow.form.unknown-log-level", Family::kForm, Severity::kError,
             "Unknown 'log' level."},
    CodeInfo{"flow.form.unknown-stage", Family::kForm, Severity::kError,
             "Unknown pipeline stage."},
    CodeInfo{
        "flow.form.unknown-status-code", Family::kForm, Severity::kError,
        "'fail' requires a canonical status code, number, or bound value."},
    CodeInfo{"flow.form.unknown-type", Family::kForm, Severity::kError,
             "A type that is neither built in, nor a shape the file declares, "
             "nor written as a registry tag or a quoted mimetype."},
    CodeInfo{"flow.form.zip-empty", Family::kForm, Severity::kError,
             "'zip' requires at least one input stream."},
    CodeInfo{"flow.name.ambiguous-implementation", Family::kName,
             Severity::kError,
             "A callable name resolves to multiple implementations."},
    CodeInfo{"flow.name.call-as-stream", Family::kName, Severity::kError,
             "A call is used as a stream; select an output port."},
    CodeInfo{"flow.name.discard-bound", Family::kName, Severity::kError,
             "'_' is a discard target and cannot bind a name."},
    CodeInfo{"flow.name.discard-read", Family::kName, Severity::kError,
             "'_' discards its value and cannot be read."},
    CodeInfo{"flow.name.import-alias-conflict", Family::kName, Severity::kError,
             "An import alias conflicts with another binding."},
    CodeInfo{"flow.name.import-shadowing", Family::kName, Severity::kWarning,
             "An import shadows an existing binding."},
    CodeInfo{"flow.name.it-outside-stage", Family::kName, Severity::kError,
             "'it' requires a stage that binds the current value."},
    CodeInfo{"flow.name.module-not-found", Family::kName, Severity::kError,
             "An imported module cannot be resolved."},
    CodeInfo{"flow.name.no-status", Family::kName, Severity::kError,
             "A status is requested from a value without status."},
    CodeInfo{"flow.name.not-a-call", Family::kName, Severity::kError,
             "'cancel' requires a call handle."},
    CodeInfo{"flow.name.not-a-stream", Family::kName, Severity::kError,
             "A pipeline source must be readable."},
    CodeInfo{"flow.name.not-writable", Family::kName, Severity::kError,
             "A destination is not writable, such as an 'in' port, call "
             "output, or barrier."},
    CodeInfo{"flow.name.private-member", Family::kName, Severity::kError,
             "A member is private to its module."},
    CodeInfo{"flow.name.taken", Family::kName, Severity::kError,
             "A name is bound twice in one scope."},
    CodeInfo{"flow.name.unknown", Family::kName, Severity::kError,
             "A name has no preceding binding in this scope."},
    CodeInfo{"flow.name.unknown-call", Family::kName, Severity::kError,
             "A qualified invocation has no matching callable contract."},
    CodeInfo{"flow.name.unknown-node-map", Family::kName, Severity::kError,
             "A node map is referenced before its 'nodes' declaration."},
    CodeInfo{"flow.name.unknown-port", Family::kName, Severity::kError,
             "A called action does not declare this port."},
    CodeInfo{"flow.name.unresolved-declaration", Family::kName,
             Severity::kError,
             "A declared callable has no matching implementation."},
    CodeInfo{"flow.schema.not-a-record", Family::kForm, Severity::kWarning,
             "A JSON Schema cannot be represented as a Flow struct."},
    CodeInfo{"flow.schema.unsupported-keyword", Family::kForm,
             Severity::kWarning,
             "A JSON Schema contains a constraint without a Flow spelling."},
    CodeInfo{"flow.sequence.conflicting-access", Family::kSequence,
             Severity::kError,
             "Parallel accesses require a deterministic order."},
    CodeInfo{"flow.sequence.impossible", Family::kSequence, Severity::kWarning,
             "A stage sequence cannot produce the requested result, such as "
             "'first 0' or dropping from a single value."},
    CodeInfo{"flow.sequence.lossy-conversion", Family::kSequence,
             Severity::kWarning,
             "A constant conversion loses precision or falls back to zero."},
    CodeInfo{"flow.sequence.redundant-declaration", Family::kSequence,
             Severity::kInformation, "Redundant declaration."},
    CodeInfo{"flow.sequence.redundant-definition", Family::kSequence,
             Severity::kInformation, "Redundant definition."},
    CodeInfo{"flow.sequence.redundant-import", Family::kSequence,
             Severity::kInformation, "Redundant import."},
    CodeInfo{"flow.sequence.redundant-parentheses", Family::kSequence,
             Severity::kInformation, "Redundant parentheses."},
    CodeInfo{"flow.sequence.redundant-stage", Family::kSequence,
             Severity::kWeakWarning,
             "A stage has no effect after a reduction or repeated reshape."},
    CodeInfo{"flow.sequence.rename-graph-boundary", Family::kSequence,
             Severity::kInformation,
             "A source rename covers only the supplied module graph."},
    CodeInfo{"flow.sequence.scalar-rvalue-read", Family::kSequence,
             Severity::kInformation,
             "A scalar read consumes one value from a stream."},
    CodeInfo{"flow.sequence.schema-refined", Family::kSequence,
             Severity::kInformation,
             "A declaration refines the callable schema."},
    CodeInfo{"flow.sequence.schema-refinement-unproven", Family::kSequence,
             Severity::kInformation,
             "Schema refinement cannot be established statically."},
    CodeInfo{"flow.sequence.skip-count-target", Family::kSequence,
             Severity::kError,
             "Counted 'skip' requires a port or node; use '| drop n' for a "
             "pipeline."},
    CodeInfo{"flow.syntax.constant-required", Family::kSyntax, Severity::kError,
             "A constant is required in this position."},
    CodeInfo{"flow.syntax.nesting-too-deep", Family::kSyntax, Severity::kError,
             "Expression or block nesting exceeds the parser limit."},
    CodeInfo{"flow.syntax.statement-end", Family::kSyntax, Severity::kError,
             "Unexpected text after a complete statement."},
    CodeInfo{"flow.syntax.unclosed", Family::kSyntax, Severity::kError,
             "Unclosed flow, block, call, list, or object."},
    CodeInfo{"flow.syntax.unexpected", Family::kSyntax, Severity::kError,
             "Unexpected token in this grammatical position."},
    CodeInfo{"flow.syntax.unexpected-character", Family::kSyntax,
             Severity::kError, "Invalid character in Flow source."},
    CodeInfo{"flow.syntax.unterminated-string", Family::kSyntax,
             Severity::kError, "Unterminated string literal."},
    CodeInfo{"flow.unused.barrier-name", Family::kUnused,
             Severity::kWeakWarning,
             "An unread barrier name; the barrier still executes."},
    CodeInfo{"flow.unused.header", Family::kUnused, Severity::kWeakWarning,
             "A header alias has no reads."},
    CodeInfo{"flow.unused.import", Family::kUnused, Severity::kWeakWarning,
             "An import has no references."},
    CodeInfo{"flow.unused.loop-variable", Family::kUnused,
             Severity::kWeakWarning, "A 'for' variable has no reads."},
    CodeInfo{"flow.unused.node-map", Family::kUnused, Severity::kWeakWarning,
             "A node map has no placements."},
    CodeInfo{"flow.unused.output-port", Family::kUnused, Severity::kWarning,
             "An 'out' port has no writes and produces no values."},
    CodeInfo{"flow.unused.struct", Family::kUnused, Severity::kWeakWarning,
             "A struct has no references from ports, casts, or other structs."},
    CodeInfo{"flow.unused.try-pipe", Family::kUnused, Severity::kWeakWarning,
             "A 'try' pipe whose outcome nothing reads: a failure truncates "
             "its destination and every reader sees an ordinary end."},
    CodeInfo{"flow.unused.try-status", Family::kUnused, Severity::kWeakWarning,
             "A 'try' whose status nothing reads: a failure leaves the ports "
             "it feeds silently empty."},
    CodeInfo{"flow.unused.value", Family::kUnused, Severity::kWarning,
             "An initialized value has no reads."},

};

}  // namespace

std::string_view SeverityName(Severity severity) {
  switch (severity) {
    case Severity::kError:
      return "error";
    case Severity::kWarning:
      return "warning";
    case Severity::kWeakWarning:
      return "weak-warning";
    case Severity::kInformation:
      return "information";
  }
  return "error";
}

std::string_view FamilyName(Family family) {
  switch (family) {
    case Family::kSyntax:
      return "syntax";
    case Family::kForm:
      return "form";
    case Family::kName:
      return "name";
    case Family::kSequence:
      return "sequence";
    case Family::kBarrier:
      return "barrier";
    case Family::kUnused:
      return "unused";
  }
  return "syntax";
}

Severity SeverityFromName(std::string_view name) {
  if (name == "warning") {
    return Severity::kWarning;
  }
  if (name == "weak-warning") {
    return Severity::kWeakWarning;
  }
  if (name == "information") {
    return Severity::kInformation;
  }
  return Severity::kError;
}

Family FamilyFromName(std::string_view name) {
  if (name == "form") {
    return Family::kForm;
  }
  if (name == "name") {
    return Family::kName;
  }
  if (name == "sequence") {
    return Family::kSequence;
  }
  if (name == "barrier") {
    return Family::kBarrier;
  }
  if (name == "unused") {
    return Family::kUnused;
  }
  return Family::kSyntax;
}

const Diagnostic* absl_nullable FirstError(
    absl::Span<const Diagnostic> diagnostics) {
  for (const Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == Severity::kError) {
      return &diagnostic;
    }
  }
  return nullptr;
}

absl::Span<const CodeInfo> KnownCodes() {
  return absl::MakeConstSpan(kCodes.data(), kCodes.size());
}

const CodeInfo* absl_nullable FindCode(std::string_view code) {
  const auto found =
      std::lower_bound(kCodes.begin(), kCodes.end(), code,
                       [](const CodeInfo& entry, std::string_view key) {
                         return entry.code < key;
                       });
  if (found == kCodes.end() || found->code != code) {
    return nullptr;
  }
  return &*found;
}

LineIndex::LineIndex(std::string_view source)
    : length_(source.size()), source_(source) {
  line_starts_.push_back(0);
  for (size_t index = 0; index < source.size(); ++index) {
    if (source[index] == '\n') {
      line_starts_.push_back(index + 1);
    }
  }
}

Position LineIndex::At(size_t offset) const {
  const size_t clamped = std::min(offset, length_);
  // The last line whose first byte is at or before the offset.
  const auto after =
      std::upper_bound(line_starts_.begin(), line_starts_.end(), clamped);
  const size_t index =
      static_cast<size_t>(std::distance(line_starts_.begin(), after)) - 1;
  Position position;
  position.offset = clamped;
  position.line = static_cast<int>(index) + 1;
  // Columns count characters because `a11.flow.lexer` reports positions in a
  // Python string. UTF-8 continuation bytes do not advance the column.
  int column = 1;
  for (size_t at = line_starts_[index]; at < clamped; ++at) {
    if (!utf8::IsContinuation(source_[at])) {
      ++column;
    }
  }
  position.column = column;
  return position;
}

Range LineIndex::Between(size_t start, size_t end) const {
  Range range;
  range.start = At(start);
  range.end = At(std::max(start, end));
  return range;
}

size_t LineIndex::LineStart(int line) const {
  if (line <= 1) {
    return 0;
  }
  const size_t index = static_cast<size_t>(line) - 1;
  if (index >= line_starts_.size()) {
    return length_;
  }
  return line_starts_[index];
}

void SortDiagnostics(std::vector<Diagnostic>& diagnostics) {
  std::stable_sort(diagnostics.begin(), diagnostics.end(),
                   [](const Diagnostic& left, const Diagnostic& right) {
                     if (left.range.start.offset != right.range.start.offset) {
                       return left.range.start.offset <
                              right.range.start.offset;
                     }
                     if (left.code != right.code) {
                       return left.code < right.code;
                     }
                     return left.message < right.message;
                   });
}

}  // namespace a11::flow
