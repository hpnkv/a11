// Copyright 2026 The A11 Authors.

#ifndef A11_FLOW_NAVIGATE_H_
#define A11_FLOW_NAVIGATE_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "a11/flow/catalogue.h"
#include "a11/flow/diagnostic.h"
#include "a11/flow/syntax.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow {

// Moving around a document: what is under the caret, what the file declares,
// and where a name was bound.

/// Moving around a document: what is under the caret, what the file declares,
/// and where a name was bound.
///
/// Navigation uses the parser and resolver so hover, completion, diagnostics,
/// and editor integrations assign the same role and binding to each name.
/// What a thing *is*, which is what an editor turns into an icon.
///
/// Serialized in `flow.symbols/v1` and `flow.hover/v1`.
enum class SymbolClass {
  kFlow,
  kDto,
  kField,
  kPort,
  kHeader,
  kNodeMap,
  kNode,
  kCall,
  kBarrier,
  kVariable,
  /// Something the language knows about but the document did not declare: an
  /// action, a registered type, a stage, a built-in.
  kExternal,
};

std::string_view SymbolClassName(SymbolClass kind);

/// One thing a document declares, and the things declared inside it.
struct DocumentSymbol {
  std::string name;
  SymbolClass kind = SymbolClass::kFlow;
  /// What an editor shows beside the name: a port's type, a call's action.
  std::string detail;
  /// The whole construct, for "select this symbol".
  Range range;
  /// Just the name, for "put the caret here".
  Range selection;
  std::vector<DocumentSymbol> children;
};

/// Every symbol a document declares, nested as it is written.
///
/// Top-level flows and shapes contain their ports, fields, and local bindings.
std::vector<DocumentSymbol> Symbols(std::string_view source);

/// What is at one offset, described.
struct Description {
  /// Whether anything is there at all. A caret in whitespace describes nothing,
  /// and saying so is not a failure.
  bool found = false;
  /// The word or construct itself, as written.
  std::string text;
  SymbolClass kind = SymbolClass::kExternal;
  /// One line: what it is. `an action`, `a port of 'search'`.
  std::string summary;
  /// Everything else worth reading: an action's description and its ports, a
  /// shape's fields, what a stage takes.
  std::string detail;
  /// The same as Markdown, which is what an editor's hover wants.
  std::string markdown;
  /// What the word covers, so an editor can underline exactly it.
  Range range;
  /// Where it was declared, when it was declared in this document.
  bool has_definition = false;
  Range definition;
  // Where it was declared, when it was declared in *another* file and something
  // read that file: an action or a type the catalogue carries an origin for.
  /// Where it was declared, when it was declared in *another* file and
  /// something
  /// read that file: an action or a type the catalogue carries an origin for.
  ///
  /// Separate from [definition], which always refers to the current document.
  std::optional<catalogue::Origin> origin;
};

/// What is at `offset`, and where it came from.
Description Describe(
    std::string_view source, size_t offset,
    const catalogue::Catalogue& known = catalogue::Catalogue::Builtin());

// An action, written out as the Markdown a reader wants to see: what it does,
// then every port it has.
/// An action, written out as the Markdown a reader wants to see: what it does,
/// then every port it has.
///
/// Shared by hover and completion.
std::string ActionMarkdown(const catalogue::ActionInfo& action);

/// One port, written out as the Markdown a reader wants beside its name.
///
/// Used by hover and completion to show the same full port description.
std::string PortMarkdown(std::string_view name, std::string_view type,
                         bool required, bool unary,
                         std::string_view description);

/// A flow of the document, written out the same way: what it does, then its
/// ports and which direction each runs.
std::string FlowMarkdown(const FlowPlan& flow);

/// A struct, written out the same way: how many fields, then each of them.
std::string ShapeMarkdown(const DtoPlan& shape);

// One word or mark of the language, written out as reference: what it does,
// what it takes, how it behaves, and a line of Flow using it.
/// One word or mark of the language, written out as reference: what it does,
/// what it takes, how it behaves, and a line of Flow using it.
///
/// Empty where nothing documents the name. `role` says which position the name
/// was read in, since a word can mean two things; where that role's table has
/// no
/// entry the other tables are asked, because a word set may list a word that a
/// neighbouring set documents (`vocabulary::AnyDocumentation`).
///
/// The name may be written in either case, as the language allows: `TRUNCATE`
/// is
/// documented and shown as written.
std::string WordMarkdown(std::string_view name, vocabulary::WordRole role);

/// A pipeline stage, written out as reference.
///
/// Uses the stage role when a word also has a built-in function meaning.
std::string StageMarkdown(std::string_view name);

/// A built-in function, the same way.
std::string BuiltinMarkdown(std::string_view name);

/// The `format` field of the symbols envelope.
inline constexpr std::string_view kSymbolsFormat = "flow.symbols/v1";
/// The `format` field of the hover envelope.
inline constexpr std::string_view kHoverFormat = "flow.hover/v1";
/// The `format` field of the definition envelope.
inline constexpr std::string_view kDefinitionFormat = "flow.definition/v1";

}  // namespace a11::flow

#endif  // A11_FLOW_NAVIGATE_H_
