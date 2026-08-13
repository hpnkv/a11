// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_DIAGNOSTIC_H_
#define A11_FLOW_DIAGNOSTIC_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <absl/types/span.h>

namespace a11::flow {

/// How much a diagnostic matters.
///
/// The distinction that earns its keep is between "this cannot work" and "this
/// does nothing": the first stops a flow from compiling, the second is the
/// greyed-out unused symbol every editor already knows how to show.
enum class Severity {
  /// The compiler refuses the flow. `a11 flow check` exits non-zero.
  kError,
  /// The flow compiles and does something other than what it says.
  kWarning,
  /// The flow works, and part of it is doing nothing.
  kWeakWarning,
  /// Worth knowing, never worth blocking on.
  kInformation,
};

/// The kind of problem, which is the grouping a reader thinks in.
///
/// An editor turns each of these into one switchable inspection, and a CI job
/// can gate on some and not others -- which is why the family is part of the
/// output contract rather than something a frontend infers from the code.
enum class Family {
  /// The text is not a flow: something is missing or in the wrong place.
  kSyntax,
  /// A form the language does not have, or does not have there.
  kForm,
  /// A name that cannot be resolved, or is used as the wrong thing.
  kName,
  /// A sequence of operations that cannot do what it appears to.
  kSequence,
  /// A barrier, loop tail or ordering that cannot hold.
  kBarrier,
  /// A status, wait or declaration nothing uses.
  kUnused,
};

/// One place in the source: the byte offset, and the line and column at it.
///
/// All three are carried because each consumer wants a different one. Byte
/// offsets are what an editor edits with, line and column are what a person and
/// a build log read, and computing one from the other needs the source text --
/// which a diagnostic that has travelled as JSON no longer has. Lines and
/// columns are 1-based, as `a11.flow.lexer` has always reported them.
struct Position {
  size_t offset = 0;
  int line = 1;
  int column = 1;
};

/// Half-open span of source, `[start, end)`.
struct Range {
  Position start;
  Position end;
};

/// One replacement of a span of source. An empty `text` is a deletion.
struct Edit {
  size_t start = 0;
  size_t end = 0;
  std::string text;
};

/// An edit, or set of edits, that would fix a diagnostic.
///
/// Offered only where a single edit is obviously the right one. A frontend
/// applies these blind -- it never re-derives what the fix should be -- so a fix
/// that guesses would be a fix that corrupts somebody's file.
struct Fix {
  std::string label;
  std::vector<Edit> edits;
};

/// One problem found in a flow.
///
/// This is the unit every frontend renders: the CLI prints it, the JSON and
/// SARIF writers serialise it, an editor turns it into a squiggle and an
/// Alt+Enter. Nothing downstream composes messages of its own, which is what
/// keeps the wording of a language problem in one place.
struct Diagnostic {
  /// A stable dotted identifier, e.g. `flow.stage.unknown`. Part of the
  /// contract: a toolchain may match on it, and it does not change with the
  /// wording of the message. See [KnownCodes].
  std::string code;
  Severity severity = Severity::kError;
  Family family = Family::kSyntax;
  Range range;
  /// One sentence, in the language's own terms, ending in a full stop.
  std::string message;
  /// The flow the problem is in, where the text got far enough to say.
  std::string flow;
  std::vector<Fix> fixes;
};

/// The spelling of a severity in the output formats.
std::string_view SeverityName(Severity severity);

/// The spelling of a family in the output formats.
std::string_view FamilyName(Family family);

/// `Severity` for a name from the output formats, or `kError` if unknown.
Severity SeverityFromName(std::string_view name);

/// `Family` for a name from the output formats, or `kSyntax` if unknown.
Family FamilyFromName(std::string_view name);

/// What a diagnostic code means, for documentation and for `a11 flow codes`.
///
/// The table of these *is* the published list of things the language checks; a
/// new check adds an entry, and the test in `cpp/tests/flow_diagnostic_test.cc`
/// holds it to being complete, sorted and free of duplicates.
struct CodeInfo {
  std::string_view code;
  Family family;
  Severity severity;
  /// One line, in the imperative-free "what is wrong" voice: this is what a
  /// `--help`-style listing shows next to the code.
  std::string_view summary;
};

/// Every diagnostic code the language can produce, sorted by code.
absl::Span<const CodeInfo> KnownCodes();

/// The entry for a code, or `nullptr` if nothing publishes it.
const CodeInfo* absl_nullable FindCode(std::string_view code);

/// Line and column lookup over one source text.
///
/// Built once per file and shared by everything that reports a position, so a
/// diagnostic never costs a scan of the source to locate. Borrows the text, which
/// must outlive it.
class LineIndex {
 public:
  explicit LineIndex(std::string_view source);

  /// The position at a byte offset, clamped to the end of the source.
  Position At(size_t offset) const;

  /// A range from two byte offsets.
  Range Between(size_t start, size_t end) const;

  /// How many lines the source has, counting a trailing partial line.
  size_t LineCount() const { return line_starts_.size(); }

  /// The byte offset the 1-based `line` starts at, clamped to the source.
  size_t LineStart(int line) const;

 private:
  size_t length_ = 0;
  /// The text, borrowed: a column counts characters, so it has to be read.
  std::string_view source_;
  /// Offset of the first byte of each line, `line_starts_[0] == 0`.
  std::vector<size_t> line_starts_;
};

/// Sorts diagnostics into the order every frontend presents them in: by
/// position, then by code, so two runs over the same file agree byte for byte.
void SortDiagnostics(std::vector<Diagnostic>& diagnostics);

}  // namespace a11::flow

#endif  // A11_FLOW_DIAGNOSTIC_H_
