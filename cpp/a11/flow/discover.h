// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_DISCOVER_H_
#define A11_FLOW_DISCOVER_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/types/span.h>

#include "a11/flow/catalogue.h"

namespace a11::flow {

/// Finding the actions a *project* declares, by reading its source.
///
/// **Why this exists.** The catalogue tells the tools what the world contains,
/// and until now the world meant the SDK: a snapshot of the live registries
/// (`scripts/generate_flow_catalogue.py`), plus whatever a host pushed over
/// `setContext`. An action somebody wrote this afternoon in a file two
/// directories away was not in it, so hovering its name in a flow said
/// "identifier", completing its ports offered nothing, and there was nowhere to
/// go. That is the common case for anybody actually composing their own
/// actions, and it was the case the tools knew least about.
///
/// **Why it is here and not in each editor.** An IDE could find these with its
/// own index, and two IDEs would then be two implementations of one question,
/// each covering only the languages it knows. `ActionSchema` is declared in
/// Python, in C++ and in TypeScript in this repository alone. Reading source
/// for declarations is not knowledge of the *language* -- it is knowledge of the
/// *world*, which is what a catalogue carries -- so it belongs where the
/// catalogue is, and both editors, the CLI and CI get it from one place.
///
/// **What it is.** A tolerant textual scan, not a parser for three languages.
/// It reads string literals and same-file constants, and it is deliberately
/// happy to come away with less than everything:
///
/// * A schema written as a constructor call with literal arguments -- the
///   Python and TypeScript shape -- comes back whole: name, description, and
///   every port with its type and description.
/// * A schema assembled statement by statement -- the C++ shape, `schema.name =
///   ..; schema.outputs.emplace(..)` -- comes back with its name, its
///   description and its port names. Port types and descriptions come from the
///   literals of whatever call builds the port, read positionally, so a helper
///   whose arguments run in a different order gives a port with no type rather
///   than a port with the wrong one.
/// * A name or description computed at run time is not found at all, and a
///   schema whose *name* cannot be read is dropped: nothing can look up an
///   action with no name, so half an entry would be worse than none.
///
/// Every one of those is strictly more than the nothing there was before, and
/// the origin alone -- which is exact whenever the declaration was found --
/// makes "go to symbol" work on an action for the first time.
namespace discover {

/// Where a declaration was written.
///
/// The catalogue's own, because an origin is a thing a catalogue *entry* has
/// rather than a thing a scan produces on the side: it travels in
/// `flow.catalogue/v1` and reaches a hover and a go-to-declaration from there.
using Origin = catalogue::Origin;

/// A language an `ActionSchema` may be declared in.
enum class Language {
  kPython,
  kCpp,
  kTypeScript,
};

/// The language a path's extension says it is, or `nullopt` for a file this
/// does not read.
std::optional<Language> LanguageOf(std::string_view path);

/// What to read and what to leave alone.
struct Options {
  /// Directory names never descended into. Defaults to the ones that hold
  /// somebody else's code or a build of this one: `node_modules`, `.venv`,
  /// `build`, `cmake-build-*`, `dist`, `.git`, `__pycache__`.
  std::vector<std::string> skip_directories;

  /// The largest file read. A generated or vendored file of several megabytes
  /// is not where anybody declares an action, and reading one on every save is
  /// what makes an editor feel slow.
  size_t max_file_bytes = 1u << 20;

  /// The most files read in one scan.
  size_t max_files = 20000;

  /// Options with the defaults filled in.
  static Options Default();
};

/// What a scan found, and what it did not read.
///
/// The counts are here so that a host can *say* a scan was incomplete. A cap
/// that silently applied itself would make a half-read project look exactly
/// like a project with two actions in it.
struct Result {
  catalogue::Catalogue found;
  /// Files opened and read to the end.
  size_t files_read = 0;
  /// Files skipped for being larger than `max_file_bytes`.
  std::vector<std::string> too_large;
  /// Whether `max_files` stopped the walk before it ran out of files.
  bool reached_file_limit = false;
};

/// Every `ActionSchema` declared under `roots`, as a catalogue with origins.
///
/// A root may be a directory, which is walked, or a single file. A path that
/// cannot be read is skipped rather than failing the scan: a scan is a thing an
/// editor does in the background over a tree somebody is editing, and one
/// unreadable file is not a reason to answer nothing.
Result Discover(absl::Span<const std::string> roots,
                const Options& options = Options::Default());

/// The same, for one file whose text is already in hand.
///
/// What a host with a document open calls on a save. `path` is recorded as the
/// origin and is not opened.
catalogue::Catalogue DiscoverInSource(std::string_view source,
                                     std::string_view path, Language language);

/// The `format` field of the envelope a scan answers with, which is the
/// catalogue's own: what a scan produces *is* a catalogue, and a consumer that
/// had to tell them apart would be a consumer with two code paths for one
/// thing.
inline constexpr std::string_view kScanFormat = catalogue::kCatalogueFormat;

}  // namespace discover
}  // namespace a11::flow

#endif  // A11_FLOW_DISCOVER_H_
