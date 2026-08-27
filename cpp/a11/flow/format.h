// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_FORMAT_H_
#define A11_FLOW_FORMAT_H_

#include <string>
#include <string_view>
#include <vector>

#include "a11/flow/diagnostic.h"

namespace a11::flow {

/// What the formatter is allowed to decide.
///
/// The fixed style keeps formatting consistent across projects.
struct FormatOptions {
  /// Spaces per block level. Repository Flow files use two.
  int indent = 2;
  /// Whether to align consecutive `in` and `out` declaration columns.
  bool align_ports = true;
};

/// The formatted text, and what it took to get there.
struct FormatResult {
  /// The whole file as it should be. Equal to the input when nothing changed --
  /// and equal to the input, unchanged, when the file does not parse.
  std::string formatted;
  bool changed = false;
  // One edit that turns the input into `formatted`, trimmed to the part that
  // actually differs, so an editor applying it does not move the cursor or lose
  // a selection over a file that only changed at the bottom.
  /// One edit that turns the input into `formatted`, trimmed to the part that
  /// actually differs, so an editor applying it does not move the cursor or
  /// lose a selection over a file that only changed at the bottom. Empty when
  /// nothing changed.
  std::vector<Edit> edits;
  /// Why the file was left alone, where it was.
  std::vector<Diagnostic> diagnostics;
};

/// Format Flow source.
///
/// **What it decides:** indentation, the spaces between tokens, how far a
/// continued line is indented, how many blank lines are allowed and where, the
/// columns of a run of port declarations, trailing whitespace, and the newline
/// at
/// the end of the file.
///
/// **What it leaves to the author:** where the lines break. Whether a pipeline
/// is
/// written across four lines or one, whether a list literal is split a value
/// per
/// line, and where the comments are, is a judgement about what the flow *means*
/// --
/// which values belong together, which stage is the interesting one -- and a
/// formatter that overruled it would make every flow in the repository worse.
/// So a
/// break the author wrote is kept and indented properly, and a break they did
/// not
/// write is not invented.
///
/// **It refuses a file it cannot read.** With an error diagnostic, `formatted`
/// is the input and `diagnostics` says why: half-formatting a file somebody is
/// in the middle of typing is how a formatter loses somebody's work.
///
/// Two invariants, both tested over every flow in the repository:
///
/// * **Idempotent.** Formatting formatted text changes nothing.
/// * **Token-preserving.** The formatted text lexes to the same tokens as the
///   input, comments included -- the only difference being trailing whitespace
///   trimmed from inside a comment. Whitespace is all it may touch, and this is
///   what says so rather than promising it.
FormatResult Format(std::string_view source, FormatOptions options = {});

}  // namespace a11::flow

#endif  // A11_FLOW_FORMAT_H_
