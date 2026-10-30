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
             "An 'after' names a step written further down the flow, or names "
             "the statement it is on."},
    CodeInfo{"flow.barrier.after-reads-subject", Family::kBarrier,
             Severity::kError,
             "An argument reads an output of the call its own statement waits "
             "for, so neither the argument nor the call can go first."},
    CodeInfo{"flow.barrier.cancel-after-wait", Family::kBarrier,
             Severity::kWarning,
             "A call is cancelled after this body already waited for it, so "
             "there is nothing left to stop."},
    CodeInfo{"flow.barrier.carry-outside-repeat", Family::kBarrier,
             Severity::kError,
             "A '<-' carries a value where there is no 'repeat' to carry it "
             "into."},
    CodeInfo{"flow.barrier.continuation-cycle", Family::kBarrier,
             Severity::kError,
             "A continuation completion depends on a writer it prevents."},
    CodeInfo{"flow.barrier.duplicate", Family::kBarrier, Severity::kWeakWarning,
             "A second 'wait' or 'close' on something this body already holds "
             "for."},
    CodeInfo{"flow.barrier.duplicate-carry", Family::kBarrier, Severity::kError,
             "A repeat carries the same name twice."},
    CodeInfo{"flow.barrier.duplicate-until", Family::kBarrier, Severity::kError,
             "A loop is given a second stop condition."},
    CodeInfo{"flow.barrier.expression-cycle", Family::kBarrier,
             Severity::kError, "Expression cycle."},
    CodeInfo{"flow.barrier.scope-cycle", Family::kBarrier, Severity::kError,
             "A preceding producer needs a consumer after a scope barrier."},
    CodeInfo{"flow.barrier.unordered-clock", Family::kBarrier,
             Severity::kWarning,
             "A statement reads the clock against something the flow produced, "
             "with no 'after' to say when it runs."},
    CodeInfo{"flow.barrier.until-outside-repeat", Family::kBarrier,
             Severity::kError,
             "An 'until' or 'while' ends a 'repeat' or a 'for', and there is "
             "neither here."},
    CodeInfo{"flow.barrier.until-parallel", Family::kBarrier, Severity::kError,
             "An 'until' is written on a loop whose passes run in parallel, "
             "where there is no one pass for it to ask about."},
    CodeInfo{"flow.barrier.value-read-twice", Family::kBarrier,
             Severity::kWarning,
             "One statement reads one stream twice where a value belongs, so "
             "the two reads take turns on it rather than seeing one value."},
    CodeInfo{"flow.barrier.wait-lends-node", Family::kBarrier,
             Severity::kWarning,
             "A 'wait' or 'close' on a node nothing in the flow writes: it "
             "ends the node at once rather than waiting for it."},
    CodeInfo{"flow.barrier.wrong-carry", Family::kBarrier, Severity::kError,
             "A carry names something other than what its repeat carries."},
    CodeInfo{"flow.form.bad-number", Family::kForm, Severity::kError,
             "A number the language cannot read."},
    CodeInfo{"flow.form.bad-pattern", Family::kForm, Severity::kError,
             "A 'match' pattern the language cannot read: an unclosed hole, an "
             "unknown kind, or two holes with one name."},
    CodeInfo{"flow.form.continuation-grouping", Family::kForm, Severity::kError,
             "A transfer and broadcast need explicit unambiguous grouping."},
    CodeInfo{"flow.form.count-not-positive", Family::kForm, Severity::kWarning,
             "A count that is not a number of anything: 'parallel 0', "
             "'skip -1'."},
    CodeInfo{"flow.form.declaration-attribute", Family::kForm, Severity::kError,
             "Declaration attribute."},
    CodeInfo{"flow.form.declaration-body", Family::kForm, Severity::kError,
             "Declaration body."},
    CodeInfo{"flow.form.declaration-conflict", Family::kForm, Severity::kError,
             "Declaration conflict."},
    CodeInfo{"flow.form.declaration-schema-mismatch", Family::kForm,
             Severity::kError, "Declaration schema mismatch."},
    CodeInfo{"flow.form.default-on-required", Family::kForm, Severity::kWarning,
             "A field is required and also given a default, so the default "
             "could never be used."},
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
    CodeInfo{
        "flow.form.duration-unit", Family::kForm, Severity::kError,
        "A number carrying a suffix that is not one of the duration units."},
    CodeInfo{"flow.form.empty-range", Family::kForm, Severity::kError,
             "A range that bounds nothing, or whose bounds are the wrong way "
             "round."},
    CodeInfo{"flow.form.field-constraint", Family::kForm, Severity::kError,
             "A field constraint that cannot apply to what the field holds: "
             "'unique' on one value, 'matching' on a number."},
    CodeInfo{"flow.form.field-modifier-order", Family::kForm,
             Severity::kWarning,
             "A field's modifiers are written out of the order they read in."},
    CodeInfo{"flow.form.field-type-mismatch", Family::kForm, Severity::kError,
             "A field's default or 'one of' value is not a value of the type "
             "the field holds."},
    CodeInfo{"flow.form.forward-headers", Family::kForm, Severity::kError,
             "'headers' without the 'forward' that owns it."},
    CodeInfo{"flow.form.handle-escape", Family::kForm, Severity::kError,
             "Handle escape."},
    CodeInfo{"flow.form.import-context", Family::kForm, Severity::kError,
             "Import context."},
    CodeInfo{"flow.form.import-scope", Family::kForm, Severity::kError,
             "Import scope."},
    CodeInfo{"flow.form.invalid-action-target", Family::kForm, Severity::kError,
             "Invalid action target."},
    CodeInfo{"flow.form.log-format", Family::kForm, Severity::kError,
             "A 'logf' written without a format to fill."},
    CodeInfo{"flow.form.log-value", Family::kForm, Severity::kError,
             "A 'log' written without anything to log."},
    CodeInfo{
        "flow.form.missing-field", Family::kForm, Severity::kError,
        "A value of a declared shape leaves out a field the shape requires "
        "and gives no default for."},
    CodeInfo{"flow.form.multiple-implementations", Family::kForm,
             Severity::kWarning, "Multiple implementations."},
    CodeInfo{"flow.form.node-parentheses", Family::kForm, Severity::kError,
             "Making a node takes parentheses: 'node()' or 'node(id)'."},
    CodeInfo{"flow.form.not-json-representable", Family::kForm,
             Severity::kError,
             "A value holding bytes is asked for as JSON, which has nothing to "
             "carry them in; 'packb' is the one that does."},
    CodeInfo{"flow.form.not-waitable", Family::kForm, Severity::kError,
             "Not waitable."},
    CodeInfo{"flow.form.one-of-empty", Family::kForm, Severity::kError,
             "'one of []' allows nothing, so nothing would validate."},
    CodeInfo{"flow.form.one-of-not-a-list", Family::kForm, Severity::kError,
             "'one of' is given something other than a list of constants."},
    CodeInfo{
        "flow.form.optional-binding", Family::kForm, Severity::kError,
        "An optional binding requires an option with validated provenance."},
    CodeInfo{"flow.form.pipeline-call", Family::kForm, Severity::kError,
             "A pipeline invocation has an unknown or incompatible interface."},
    CodeInfo{"flow.form.port-modifier-order", Family::kForm, Severity::kError,
             "'stream' or 'required' is written in front of the port's type "
             "instead of after it."},
    CodeInfo{"flow.form.private-type-exposed", Family::kForm, Severity::kError,
             "Private type exposed."},
    CodeInfo{"flow.form.repeated-modifier", Family::kForm, Severity::kError,
             "A modifier that says one thing is written twice."},
    CodeInfo{"flow.form.resource-limit", Family::kForm, Severity::kError,
             "A host-configured parsing or compilation limit was reached."},
    CodeInfo{"flow.form.scope-barrier", Family::kForm, Severity::kError,
             "A bare scope barrier cannot be modified or used as a value."},
    CodeInfo{"flow.form.sink-as-source", Family::kForm, Severity::kError,
             "A continuation sink cannot be read or carried as payload."},
    CodeInfo{"flow.form.stage-argument", Family::kForm, Severity::kError,
             "A stage is missing the argument it takes, or was given one of "
             "the wrong kind."},
    CodeInfo{"flow.form.statement-as-value", Family::kForm, Severity::kError,
             "Statement as value."},
    CodeInfo{"flow.form.struct-shadows-builtin", Family::kForm,
             Severity::kError,
             "A struct is named after a built-in type, which nothing could "
             "then write."},
    CodeInfo{"flow.form.unbounded-repeat", Family::kForm, Severity::kError,
             "A 'repeat' with no 'until', no 'while' and no 'max': nothing "
             "ends it."},
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
             "A function call naming something that is not one of the "
             "language's fixed functions."},
    CodeInfo{"flow.form.unknown-field", Family::kForm, Severity::kError,
             "A value of a declared shape names a field the shape does not "
             "have."},
    CodeInfo{"flow.form.unknown-log-level", Family::kForm, Severity::kError,
             "A level a 'log' named that is not one of the five."},
    CodeInfo{"flow.form.unknown-stage", Family::kForm, Severity::kError,
             "A pipeline stage the language does not have."},
    CodeInfo{"flow.form.unknown-status-code", Family::kForm, Severity::kError,
             "'fail' names something that is not a canonical status code, a "
             "number, or a value the flow read."},
    CodeInfo{"flow.form.unknown-type", Family::kForm, Severity::kError,
             "A type that is neither built in, nor a shape the file declares, "
             "nor written as a registry tag or a quoted mimetype."},
    CodeInfo{"flow.form.zip-empty", Family::kForm, Severity::kError,
             "'zip' reads streams in step, so it takes at least one."},
    CodeInfo{"flow.name.ambiguous-implementation", Family::kName,
             Severity::kError, "Ambiguous implementation."},
    CodeInfo{"flow.name.call-as-stream", Family::kName, Severity::kError,
             "A call used where one of its ports was meant."},
    CodeInfo{"flow.name.discard-bound", Family::kName, Severity::kError,
             "'_' used as a name: bound to a statement, declared as a port, or "
             "given to a loop. It is the discard, not a name."},
    CodeInfo{"flow.name.discard-read", Family::kName, Severity::kError,
             "'_' read: as a pipeline source, as something to wait on, close "
             "or abort, or inside an expression. It keeps nothing, so there is "
             "nothing there to read."},
    CodeInfo{"flow.name.import-alias-conflict", Family::kName, Severity::kError,
             "Import alias conflict."},
    CodeInfo{"flow.name.import-shadowing", Family::kName, Severity::kWarning,
             "Import shadowing."},
    CodeInfo{"flow.name.it-outside-stage", Family::kName, Severity::kError,
             "'it' outside the 'where', 'map' or 'group' stage whose value it "
             "names."},
    CodeInfo{"flow.name.module-not-found", Family::kName, Severity::kError,
             "Module not found."},
    CodeInfo{"flow.name.no-status", Family::kName, Severity::kError,
             "A status asked of something that has none."},
    CodeInfo{"flow.name.not-a-call", Family::kName, Severity::kError,
             "Something that is not a call is asked to stop."},
    CodeInfo{"flow.name.not-a-stream", Family::kName, Severity::kError,
             "Something that cannot be read is used as a pipeline source."},
    CodeInfo{"flow.name.not-writable", Family::kName, Severity::kError,
             "A stream is written somewhere that cannot be written: an 'in' "
             "port, a call's output, a barrier."},
    CodeInfo{"flow.name.private-member", Family::kName, Severity::kError,
             "Private member."},
    CodeInfo{"flow.name.taken", Family::kName, Severity::kError,
             "A name is bound twice in one scope."},
    CodeInfo{"flow.name.unknown", Family::kName, Severity::kError,
             "A name nothing has bound, including one bound further down: a "
             "flow reads in order."},
    CodeInfo{"flow.name.unknown-call", Family::kName, Severity::kError,
             "A qualified invocation has no matching callable contract."},
    CodeInfo{"flow.name.unknown-node-map", Family::kName, Severity::kError,
             "A node map used before 'nodes' declares it."},
    CodeInfo{"flow.name.unknown-port", Family::kName, Severity::kError,
             "A port the action being called does not declare."},
    CodeInfo{"flow.name.unresolved-declaration", Family::kName,
             Severity::kError, "Unresolved declaration."},
    CodeInfo{"flow.schema.not-a-record", Family::kForm, Severity::kWarning,
             "A JSON Schema cannot be represented as a Flow struct."},
    CodeInfo{"flow.schema.unsupported-keyword", Family::kForm,
             Severity::kWarning,
             "A JSON Schema contains a constraint without a Flow spelling."},
    CodeInfo{"flow.sequence.conflicting-access", Family::kSequence,
             Severity::kError,
             "Parallel accesses require a deterministic order."},
    CodeInfo{"flow.sequence.impossible", Family::kSequence, Severity::kWarning,
             "A sequence of stages that cannot produce what it appears to: "
             "dropping from one value, counting a reduced stream, 'first 0'."},
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
             "A stage with nothing to do: choosing between the single value a "
             "reducing stage left, or the same reshaping twice."},
    CodeInfo{"flow.sequence.rename-graph-boundary", Family::kSequence,
             Severity::kInformation,
             "A source rename covers only the supplied module graph."},
    CodeInfo{"flow.sequence.scalar-rvalue-read", Family::kSequence,
             Severity::kInformation, "Scalar rvalue read."},
    CodeInfo{"flow.sequence.schema-refined", Family::kSequence,
             Severity::kInformation, "Schema refined."},
    CodeInfo{"flow.sequence.schema-refinement-unproven", Family::kSequence,
             Severity::kInformation, "Schema refinement unproven."},
    CodeInfo{"flow.sequence.skip-count-target", Family::kSequence,
             Severity::kError,
             "A counted 'skip' applied to something that is not a port or a "
             "node; '| drop n' is the one that drops from a pipeline."},
    CodeInfo{"flow.syntax.constant-required", Family::kSyntax, Severity::kError,
             "An expression where the grammar requires a constant."},
    CodeInfo{"flow.syntax.nesting-too-deep", Family::kSyntax, Severity::kError,
             "Brackets, blocks or bodies nested deeper than the parser will "
             "descend."},
    CodeInfo{"flow.syntax.statement-end", Family::kSyntax, Severity::kError,
             "More text after a complete statement; one statement per line."},
    CodeInfo{"flow.syntax.unclosed", Family::kSyntax, Severity::kError,
             "A flow, block, call, list or object that is never closed."},
    CodeInfo{"flow.syntax.unexpected", Family::kSyntax, Severity::kError,
             "Text where the grammar expects something else."},
    CodeInfo{"flow.syntax.unexpected-character", Family::kSyntax,
             Severity::kError,
             "A character the language has no meaning for anywhere."},
    CodeInfo{"flow.syntax.unterminated-string", Family::kSyntax,
             Severity::kError,
             "A string that is not closed before its line "
             "ends."},
    CodeInfo{"flow.unused.barrier-name", Family::kUnused,
             Severity::kWeakWarning,
             "A barrier bound to a name nothing reads; the flow still waits "
             "there, so the name is the dead part."},
    CodeInfo{"flow.unused.header", Family::kUnused, Severity::kWeakWarning,
             "A header declared under an alias nothing uses."},
    CodeInfo{"flow.unused.import", Family::kUnused, Severity::kWeakWarning,
             "Import."},
    CodeInfo{"flow.unused.loop-variable", Family::kUnused,
             Severity::kWeakWarning,
             "A 'for' variable no pass of the loop reads."},
    CodeInfo{"flow.unused.node-map", Family::kUnused, Severity::kWeakWarning,
             "A node map nothing is placed in."},
    CodeInfo{"flow.unused.output-port", Family::kUnused, Severity::kWarning,
             "An 'out' port nothing in the flow writes, so a caller reading it "
             "gets nothing."},
    CodeInfo{"flow.unused.struct", Family::kUnused, Severity::kWeakWarning,
             "A struct no port, cast or other struct in the file names."},
    CodeInfo{"flow.unused.try-pipe", Family::kUnused, Severity::kWeakWarning,
             "A 'try' pipe whose outcome nothing reads: a failure truncates "
             "its destination and every reader sees an ordinary end."},
    CodeInfo{"flow.unused.try-status", Family::kUnused, Severity::kWeakWarning,
             "A 'try' whose status nothing reads: a failure leaves the ports "
             "it feeds silently empty."},
    CodeInfo{"flow.unused.value", Family::kUnused, Severity::kWarning,
             "An initialized value that no expression reads."},

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
