/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef A11_FLOW_DIAGNOSTIC_H_
#define A11_FLOW_DIAGNOSTIC_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <absl/types/span.h>

namespace a11::flow {

/// Severity of a Flow diagnostic.
enum class Severity {
  /// Compilation fails. `a11 flow check` exits nonzero.
  kError,
  /// Compilation succeeds, but behavior may differ from the source's intent.
  kWarning,
  /// Compilation succeeds, but a source element has no effect.
  kWeakWarning,
  /// Non-blocking information.
  kInformation,
};

/// Diagnostic category used by editors and build gates.
enum class Family {
  /// Missing or misplaced Flow syntax.
  kSyntax,
  /// Invalid language form or form in an invalid position.
  kForm,
  /// Unresolved name or invalid use of a resolved name.
  kName,
  /// Stage or access sequence with invalid or ineffective behavior.
  kSequence,
  /// Invalid barrier, loop termination, or execution order.
  kBarrier,
  /// Unread status, unused binding, or unused declaration.
  kUnused,
};

/// Source position with a byte offset and 1-based line and column.
///
/// Byte offsets support edits. Line and column support display after the
/// diagnostic is serialized without its source text.
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

/// Source edits supplied with a diagnostic.
///
/// Frontends apply these edits without deriving another repair.
struct Fix {
  std::string label;
  std::vector<Edit> edits;
  bool automatic = true;
};

/// Another source range explaining a diagnostic.
struct RelatedDiagnostic {
  std::string source;
  Range range;
  std::string message;
};

/// Flow problem rendered by the CLI, JSON and SARIF writers, and editors.
///
/// Native analysis owns the message and any source edits.
struct Diagnostic {
  std::string source;
  /// A stable dotted identifier, e.g. `flow.stage.unknown`. Part of the
  /// contract: a toolchain may match on it, and it does not change with the
  /// wording of the message. See [KnownCodes].
  std::string code;
  Severity severity = Severity::kError;
  Family family = Family::kSyntax;
  Range range;
  /// One sentence, in the language's own terms, ending in a full stop.
  std::string message;
  /// Enclosing flow name when parsing or resolution identifies one.
  std::string flow;
  std::vector<Fix> fixes;
  std::vector<RelatedDiagnostic> related;
};

/// The spelling of a severity in the output formats.
std::string_view SeverityName(Severity severity);

/// The spelling of a family in the output formats.
std::string_view FamilyName(Family family);

/// `Severity` for a name from the output formats, or `kError` if unknown.
Severity SeverityFromName(std::string_view name);

/// `Family` for a name from the output formats, or `kSyntax` if unknown.
Family FamilyFromName(std::string_view name);

/// Published diagnostic code metadata for documentation and `a11 flow codes`.
///
/// `cpp/tests/flow_diagnostic_test.cc` checks completeness, order, and unique
/// codes.
struct CodeInfo {
  std::string_view code;
  Family family;
  Severity severity;
  /// One-line summary shown beside the code in catalogue listings.
  std::string_view summary;
};

/// The first error in @p diagnostics, or `nullptr` if none is an error.
///
/// What every strict entry point turns into its one raised failure, so that
/// "the first thing wrong with this file" means the same in each of them.
const Diagnostic* absl_nullable FirstError(
    absl::Span<const Diagnostic> diagnostics);

/// Every diagnostic code the language can produce, sorted by code.
absl::Span<const CodeInfo> KnownCodes();

/// The entry for a code, or `nullptr` if nothing publishes it.
const CodeInfo* absl_nullable FindCode(std::string_view code);

/// Line and column lookup over one source text.
///
/// Built once per file and shared by everything that reports a position, so a
/// diagnostic never costs a scan of the source to locate. Borrows the text,
/// which must outlive it.
class LineIndex {
 public:
  explicit LineIndex(std::string_view source);

  /// The borrowed document used to compute positions.
  std::string_view source() const { return source_; }

  /// The position at a byte offset, clamped to the end of the source.
  [[nodiscard]] Position At(size_t offset) const;

  /// A range from two byte offsets.
  [[nodiscard]] Range Between(size_t start, size_t end) const;

  /// How many lines the source has, counting a trailing partial line.
  [[nodiscard]] size_t LineCount() const { return line_starts_.size(); }

  /// The byte offset the 1-based `line` starts at, clamped to the source.
  [[nodiscard]] size_t LineStart(int line) const;

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
