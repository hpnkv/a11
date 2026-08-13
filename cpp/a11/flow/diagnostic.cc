// Copyright 2026 The A11 Authors.

#include "a11/flow/diagnostic.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

#include <absl/types/span.h>

namespace a11::flow {
namespace {

// Every code the language publishes, sorted by code so a listing is stable and
// a lookup can binary-search. The summary is what `a11 flow codes` prints, and
// what the documentation generates from: one line, saying what is wrong rather
// than what to do about it.
//
// Adding a check adds a row here. Removing a row is a contract change -- a
// toolchain may be matching on the code -- so a check that goes away keeps its
// code reserved rather than reusing it for something else.
constexpr std::array kCodes = {
    CodeInfo{"flow.barrier.after-order", Family::kBarrier, Severity::kWarning,
             "An 'after' names a step written further down the flow, or names "
             "the statement it is on."},
    CodeInfo{"flow.barrier.cancel-after-wait", Family::kBarrier,
             Severity::kWarning,
             "A call is cancelled after this body already waited for it, so "
             "there is nothing left to stop."},
    CodeInfo{"flow.barrier.carry-outside-repeat", Family::kBarrier,
             Severity::kError,
             "A '<-' carries a value where there is no 'repeat' to carry it "
             "into."},
    CodeInfo{"flow.barrier.duplicate", Family::kBarrier, Severity::kWeakWarning,
             "A second 'wait' or 'drain' on something this body already holds "
             "for."},
    CodeInfo{"flow.barrier.duplicate-carry", Family::kBarrier,
             Severity::kError, "A repeat carries the same name twice."},
    CodeInfo{"flow.barrier.duplicate-until", Family::kBarrier,
             Severity::kError, "A repeat is given a second stop condition."},
    CodeInfo{"flow.barrier.until-outside-repeat", Family::kBarrier,
             Severity::kError,
             "An 'until' or 'while' ends a 'repeat', and there is none here."},
    CodeInfo{"flow.barrier.wrong-carry", Family::kBarrier, Severity::kError,
             "A carry names something other than what its repeat carries."},
    CodeInfo{"flow.form.bad-number", Family::kForm, Severity::kError,
             "A number the language cannot read."},
    CodeInfo{"flow.form.count-not-positive", Family::kForm, Severity::kWarning,
             "A count that is not a number of anything: 'parallel 0', "
             "'skip -1'."},
    CodeInfo{"flow.form.duplicate-flow", Family::kForm, Severity::kError,
             "Two flows in one file are declared with the same name."},
    CodeInfo{"flow.form.duplicate-port", Family::kForm, Severity::kError,
             "A port is declared twice in one direction."},
    CodeInfo{"flow.form.duration-unit", Family::kForm, Severity::kError,
             "A number carrying a suffix that is not one of the duration units."},
    CodeInfo{"flow.form.forward-headers", Family::kForm, Severity::kError,
             "'headers' without the 'forward' that owns it."},
    CodeInfo{"flow.form.node-parentheses", Family::kForm, Severity::kError,
             "Making a node takes parentheses: 'node()' or 'node(id)'."},
    CodeInfo{"flow.form.port-modifier-order", Family::kForm, Severity::kError,
             "'stream' or 'required' is written in front of the port's type "
             "instead of after it."},
    CodeInfo{"flow.form.stage-argument", Family::kForm, Severity::kError,
             "A stage is missing the argument it takes, or was given one of "
             "the wrong kind."},
    CodeInfo{"flow.form.unknown-builtin", Family::kForm, Severity::kError,
             "A function call naming something that is not one of the "
             "language's fixed functions."},
    CodeInfo{"flow.form.unknown-stage", Family::kForm, Severity::kError,
             "A pipeline stage the language does not have."},
    CodeInfo{"flow.form.unknown-status-code", Family::kForm, Severity::kError,
             "'fail' names something that is not a canonical status code, a "
             "number, or a value the flow read."},
    CodeInfo{"flow.form.unknown-type", Family::kForm, Severity::kError,
             "A port type that is neither built in nor written as a "
             "registry tag or a quoted mimetype."},
    CodeInfo{"flow.name.call-as-stream", Family::kName, Severity::kError,
             "A call used where one of its ports was meant."},
    CodeInfo{"flow.name.it-outside-stage", Family::kName, Severity::kError,
             "'it' outside the 'where', 'map' or 'group' stage whose value it "
             "names."},
    CodeInfo{"flow.name.no-status", Family::kName, Severity::kError,
             "A status asked of something that has none."},
    CodeInfo{"flow.name.not-a-call", Family::kName, Severity::kError,
             "Something that is not a call is asked to stop."},
    CodeInfo{"flow.name.not-a-stream", Family::kName, Severity::kError,
             "Something that cannot be read is used as a pipeline source."},
    CodeInfo{"flow.name.not-writable", Family::kName, Severity::kError,
             "A stream is written somewhere that cannot be written: an 'in' "
             "port, a call's output, a barrier."},
    CodeInfo{"flow.name.taken", Family::kName, Severity::kError,
             "A name is bound twice in one scope."},
    CodeInfo{"flow.name.unknown", Family::kName, Severity::kError,
             "A name nothing has bound, including one bound further down: a "
             "flow reads in order."},
    CodeInfo{"flow.name.unknown-node-map", Family::kName, Severity::kError,
             "A node map used before 'nodes' declares it."},
    CodeInfo{"flow.name.unknown-port", Family::kName, Severity::kError,
             "A port the action being called does not declare."},
    CodeInfo{"flow.sequence.impossible", Family::kSequence, Severity::kWarning,
             "A sequence of stages that cannot produce what it appears to: "
             "dropping from one value, counting a reduced stream, 'first 0'."},
    CodeInfo{"flow.sequence.redundant-stage", Family::kSequence,
             Severity::kWeakWarning,
             "A stage with nothing to do: choosing between the single value a "
             "reducing stage left, or the same reshaping twice."},
    CodeInfo{"flow.sequence.skip-count-target", Family::kSequence,
             Severity::kError,
             "A counted 'skip' applied to something that is not a port or a "
             "node; '| drop n' is the one that drops from a pipeline."},
    CodeInfo{"flow.syntax.constant-required", Family::kSyntax,
             Severity::kError,
             "An expression where the grammar requires a constant."},
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
             Severity::kError, "A string that is not closed before its line "
                               "ends."},
    CodeInfo{"flow.unused.barrier-name", Family::kUnused,
             Severity::kWeakWarning,
             "A barrier bound to a name nothing reads; the flow still waits "
             "there, so the name is the dead part."},
    CodeInfo{"flow.unused.header", Family::kUnused, Severity::kWeakWarning,
             "A header declared under an alias nothing uses."},
    CodeInfo{"flow.unused.loop-variable", Family::kUnused,
             Severity::kWeakWarning,
             "A 'for' variable no pass of the loop reads."},
    CodeInfo{"flow.unused.node-map", Family::kUnused, Severity::kWeakWarning,
             "A node map nothing is placed in."},
    CodeInfo{"flow.unused.output-port", Family::kUnused, Severity::kWarning,
             "An 'out' port nothing in the flow writes, so a caller reading it "
             "gets nothing."},
    CodeInfo{"flow.unused.try-status", Family::kUnused, Severity::kWeakWarning,
             "A 'try' whose status nothing reads: a failure leaves the ports "
             "it feeds silently empty."},
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
  if (name == "warning") return Severity::kWarning;
  if (name == "weak-warning") return Severity::kWeakWarning;
  if (name == "information") return Severity::kInformation;
  return Severity::kError;
}

Family FamilyFromName(std::string_view name) {
  if (name == "form") return Family::kForm;
  if (name == "name") return Family::kName;
  if (name == "sequence") return Family::kSequence;
  if (name == "barrier") return Family::kBarrier;
  if (name == "unused") return Family::kUnused;
  return Family::kSyntax;
}

absl::Span<const CodeInfo> KnownCodes() {
  return absl::MakeConstSpan(kCodes.data(), kCodes.size());
}

const CodeInfo* absl_nullable FindCode(std::string_view code) {
  const auto found = std::lower_bound(
      kCodes.begin(), kCodes.end(), code,
      [](const CodeInfo& entry, std::string_view key) {
        return entry.code < key;
      });
  if (found == kCodes.end() || found->code != code) return nullptr;
  return &*found;
}

LineIndex::LineIndex(std::string_view source)
    : length_(source.size()), source_(source) {
  line_starts_.push_back(0);
  for (size_t index = 0; index < source.size(); ++index) {
    if (source[index] == '\n') line_starts_.push_back(index + 1);
  }
}

Position LineIndex::At(size_t offset) const {
  const size_t clamped = std::min(offset, length_);
  // The last line whose first byte is at or before the offset.
  const auto after = std::upper_bound(line_starts_.begin(), line_starts_.end(),
                                      clamped);
  const size_t index =
      static_cast<size_t>(std::distance(line_starts_.begin(), after)) - 1;
  Position position;
  position.offset = clamped;
  position.line = static_cast<int>(index) + 1;
  // Columns count characters, not bytes: `a11.flow.lexer` reports them over a
  // Python string, and a `§` in a prompt would otherwise put every column after
  // it in disagreement. Continuation bytes are the middles of characters.
  int column = 1;
  for (size_t at = line_starts_[index]; at < clamped; ++at) {
    if ((static_cast<unsigned char>(source_[at]) & 0xC0) != 0x80) ++column;
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
  if (line <= 1) return 0;
  const size_t index = static_cast<size_t>(line) - 1;
  if (index >= line_starts_.size()) return length_;
  return line_starts_[index];
}

void SortDiagnostics(std::vector<Diagnostic>& diagnostics) {
  std::stable_sort(diagnostics.begin(), diagnostics.end(),
                   [](const Diagnostic& left, const Diagnostic& right) {
                     if (left.range.start.offset != right.range.start.offset) {
                       return left.range.start.offset <
                              right.range.start.offset;
                     }
                     if (left.code != right.code) return left.code < right.code;
                     return left.message < right.message;
                   });
}

}  // namespace a11::flow
