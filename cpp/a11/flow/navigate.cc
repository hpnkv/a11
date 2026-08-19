// Copyright 2026 The A11 Authors.

#include "a11/flow/navigate.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/types/span.h>

#include "a11/flow/highlight.h"
#include "a11/flow/lexer.h"
#include "a11/flow/parser.h"
#include "a11/flow/resolve.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow {
namespace {

/// The class a resolved symbol belongs to.
SymbolClass ClassOf(SymbolKind kind) {
  switch (kind) {
    case SymbolKind::kInputPort:
    case SymbolKind::kOutputPort:
      return SymbolClass::kPort;
    case SymbolKind::kHeader:
      return SymbolClass::kHeader;
    case SymbolKind::kCall:
      return SymbolClass::kCall;
    case SymbolKind::kNode:
      return SymbolClass::kNode;
    case SymbolKind::kNodeMap:
      return SymbolClass::kNodeMap;
    case SymbolKind::kBarrier:
      return SymbolClass::kBarrier;
    case SymbolKind::kLoopVariable:
    case SymbolKind::kCarry:
    case SymbolKind::kValue:
      return SymbolClass::kVariable;
  }
  return SymbolClass::kExternal;
}

/// What an editor shows beside a symbol's name.
std::string DetailOf(const Symbol& symbol) {
  switch (symbol.kind) {
    case SymbolKind::kCall:
      return symbol.action;
    case SymbolKind::kInputPort:
      return "in";
    case SymbolKind::kOutputPort:
      return "out";
    default:
      return std::string(SymbolKindName(symbol.kind));
  }
}

Range RangeOf(const LineIndex& lines, const syntax::Location& location) {
  return lines.Between(location.start, location.end);
}

/// A port's line as a symbol.
DocumentSymbol PortSymbol(const LineIndex& lines, const PortPlan& port) {
  DocumentSymbol symbol;
  symbol.name = port.name;
  symbol.kind = SymbolClass::kPort;
  symbol.detail = absl::StrCat(
      port.direction == syntax::PortDirection::kInput ? "in " : "out ",
      port.declared.empty() ? port.type : port.declared,
      port.unary ? "" : " stream", port.required ? " required" : "");
  symbol.range = RangeOf(lines, port.location);
  symbol.selection = symbol.range;
  return symbol;
}

/// The ports of an action, as one block of Markdown.
///
/// What a hover over a call's name is *for*: `run make_http_request(` is eight
/// output ports and nine inputs, and the whole reason to hover it is to see them
/// without leaving the file.
std::string PortTable(const catalogue::ActionInfo& action) {
  std::string out;
  for (const auto& [heading, ports] :
       {std::pair<std::string_view, const std::vector<catalogue::PortInfo>*>{
            "Inputs", &action.inputs},
        std::pair<std::string_view, const std::vector<catalogue::PortInfo>*>{
            "Outputs", &action.outputs}}) {
    if (ports->empty()) continue;
    absl::StrAppend(&out, "\n**", heading, "**\n\n");
    for (const catalogue::PortInfo& port : *ports) {
      absl::StrAppend(&out, "- `", port.name, "`: ", port.type,
                      port.unary ? "" : " stream",
                      port.required ? " *(required)*" : "");
      if (!port.description.empty()) {
        absl::StrAppend(&out, " — ", port.description);
      }
      absl::StrAppend(&out, "\n");
    }
  }
  return out;
}

/// The fields of a shape, as one block of Markdown.
std::string FieldTable(const DtoPlan& shape) {
  if (shape.fields.empty()) return "";
  std::string out = "\n**Fields**\n\n";
  for (const FieldPlan& field : shape.fields) {
    absl::StrAppend(&out, "- `", field.name, "`: ",
                    field.declared.empty() ? field.type : field.declared,
                    field.required ? " *(required)*" : "");
    if (!field.description.empty()) {
      absl::StrAppend(&out, " — ", field.description);
    }
    absl::StrAppend(&out, "\n");
  }
  return out;
}

/// Where each top-level declaration begins, in order.
///
/// A flow owns the text from its own `flow` to whatever is declared next, which
/// is the only way to say which flow an offset is in: a declaration records
/// where it starts, and asking whether an offset is merely *after* one would put
/// every name in the file in the first flow that has a name like it.
std::vector<size_t> DeclarationStarts(const ParseResult& parsed) {
  std::vector<size_t> starts;
  starts.reserve(parsed.flows.size() + parsed.dtos.size());
  for (const syntax::FlowDeclarationPtr& flow : parsed.flows) {
    starts.push_back(flow->location.start);
  }
  for (const syntax::DtoDeclarationPtr& shape : parsed.dtos) {
    starts.push_back(shape->location.start);
  }
  std::sort(starts.begin(), starts.end());
  return starts;
}

/// Whether `offset` falls in the declaration that starts at `start`.
bool Within(const std::vector<size_t>& starts, size_t start, size_t offset,
            size_t size) {
  if (offset < start) return false;
  size_t end = size;
  for (const size_t boundary : starts) {
    if (boundary > start) {
      end = boundary;
      break;
    }
  }
  return offset < end;
}

/// Where a name was written, rather than where its declaration began.
///
/// A definition is what "go to declaration" jumps to and what a hover offers as
/// the place this came from, and both want the name: landing on the `struct`
/// keyword is landing one word to the left of the thing that was asked about.
const syntax::Word* absl_nullable NameOfFlow(const ParseResult& parsed,
                                              std::string_view name) {
  for (const syntax::FlowDeclarationPtr& flow : parsed.flows) {
    if (flow->name.text == name) return &flow->name;
  }
  return nullptr;
}

const syntax::Word* absl_nullable NameOfShape(const ParseResult& parsed,
                                               std::string_view name) {
  for (const syntax::DtoDeclarationPtr& shape : parsed.dtos) {
    if (shape->name.text == name) return &shape->name;
  }
  return nullptr;
}

/// The semantic token covering `offset`, or nothing.
const SemanticToken* absl_nullable TokenAt(
    const std::vector<SemanticToken>& tokens, size_t offset) {
  for (const SemanticToken& token : tokens) {
    if (offset >= token.start && offset < token.end) return &token;
  }
  return nullptr;
}

/// The dotted name the token at `index` is part of, and where it starts.
///
/// `a11.sdk.AudioBuffer` is five tokens and one name, and a caret anywhere in it
/// is a caret on the type -- so the whole chain is gathered rather than the one
/// word the caret happens to be in.
std::pair<std::string, size_t> DottedAround(
    const std::vector<Token>& tokens, size_t index) {
  size_t first = index;
  while (first >= 2 && tokens[first - 1].kind == TokenKind::kDot &&
         tokens[first - 2].IsWord()) {
    first -= 2;
  }
  size_t last = index;
  while (last + 2 < tokens.size() && tokens[last + 1].kind == TokenKind::kDot &&
         tokens[last + 2].IsWord()) {
    last += 2;
  }
  std::string name;
  for (size_t at = first; at <= last; ++at) absl::StrAppend(&name, tokens[at].text);
  return {name, first};
}

/// One word of the language written out as reference.
///
/// Deliberately plain Markdown: a bold run, a code span and blank lines between
/// paragraphs. That is the subset every consumer renders, and a fenced code block
/// would come out as literal backticks in the IntelliJ popup, so the example is a
/// code span on its own line instead.
/// The tables a word at a position of this kind may be documented in, in the
/// order they are tried.
///
/// More than one per kind, because the highlighter folds several word sets into
/// one kind and is right to: `kStatementKeyword` covers the statements, the
/// source words and the clauses, and `kDeclarationKeyword` covers the
/// declarations and -- past a port's or a field's `:` -- the port and field
/// modifiers. Trying them in the highlighter's own order (`SemanticKindOf` in
/// `highlight.cc`) is what keeps the answer the one the position actually means.
///
/// Empty for the kinds that are never a word of the language: an identifier, a
/// member, a comment. Those fall through to the last resort in [Describe].
absl::Span<const vocabulary::WordRole> RolesFor(SemanticKind kind) {
  using vocabulary::WordRole;
  static constexpr WordRole kStage[] = {WordRole::kStage};
  static constexpr WordRole kBuiltin[] = {WordRole::kBuiltin};
  static constexpr WordRole kStatement[] = {
      WordRole::kStatement, WordRole::kSource, WordRole::kClause};
  static constexpr WordRole kDeclaration[] = {WordRole::kDeclaration,
                                              WordRole::kPortModifier,
                                              WordRole::kFieldModifier};
  static constexpr WordRole kModifier[] = {WordRole::kModifier};
  static constexpr WordRole kType[] = {WordRole::kType};
  static constexpr WordRole kConstant[] = {WordRole::kConstant};
  static constexpr WordRole kOperatorWord[] = {WordRole::kOperatorWord};
  static constexpr WordRole kStatusCode[] = {WordRole::kStatusCode};
  static constexpr WordRole kDuration[] = {WordRole::kDurationUnit};
  // A field of a status record is the one thing after a `.` that the language
  // itself named; every other member belongs to a shape or a step, and is
  // answered above this by name resolution.
  static constexpr WordRole kMember[] = {WordRole::kStatusField};
  static constexpr WordRole kSymbol[] = {WordRole::kSymbol};
  switch (kind) {
    case SemanticKind::kStage:
      return absl::MakeConstSpan(kStage);
    case SemanticKind::kBuiltin:
      return absl::MakeConstSpan(kBuiltin);
    case SemanticKind::kStatementKeyword:
      return absl::MakeConstSpan(kStatement);
    case SemanticKind::kDeclarationKeyword:
      return absl::MakeConstSpan(kDeclaration);
    case SemanticKind::kModifierKeyword:
      return absl::MakeConstSpan(kModifier);
    case SemanticKind::kType:
      return absl::MakeConstSpan(kType);
    case SemanticKind::kConstant:
      return absl::MakeConstSpan(kConstant);
    case SemanticKind::kWordOperator:
      return absl::MakeConstSpan(kOperatorWord);
    case SemanticKind::kStatusCode:
      return absl::MakeConstSpan(kStatusCode);
    case SemanticKind::kDuration:
      return absl::MakeConstSpan(kDuration);
    case SemanticKind::kMember:
      return absl::MakeConstSpan(kMember);
    case SemanticKind::kFlowOperator:
    case SemanticKind::kOperator:
    case SemanticKind::kBrace:
    case SemanticKind::kParenthesis:
    case SemanticKind::kBracket:
    case SemanticKind::kPunctuation:
      return absl::MakeConstSpan(kSymbol);
    default:
      return {};
  }
}

/// What a word of this role is called, for the line above the summary.
///
/// A category rather than a restatement: the summary that follows says what the
/// thing does, so this only has to place it. [WordRole::kSymbol] has no name of
/// its own here because the token's kind already distinguishes a flow operator
/// from a brace, and that is the more useful word.
std::string_view RoleName(vocabulary::WordRole role) {
  switch (role) {
    case vocabulary::WordRole::kStage:
      return "a pipeline stage";
    case vocabulary::WordRole::kBuiltin:
      return "a built-in function";
    case vocabulary::WordRole::kStatement:
      return "a statement";
    case vocabulary::WordRole::kDeclaration:
      return "a declaration";
    case vocabulary::WordRole::kClause:
      return "a clause";
    case vocabulary::WordRole::kModifier:
      return "a call modifier";
    case vocabulary::WordRole::kSource:
      return "a pipeline source";
    case vocabulary::WordRole::kPortModifier:
      return "a port modifier";
    case vocabulary::WordRole::kFieldModifier:
      return "a field modifier";
    case vocabulary::WordRole::kType:
      return "a port type";
    case vocabulary::WordRole::kConstant:
      return "a constant";
    case vocabulary::WordRole::kOperatorWord:
      return "an operator";
    case vocabulary::WordRole::kStatusCode:
      return "a status code";
    case vocabulary::WordRole::kStatusField:
      return "a field of a status";
    case vocabulary::WordRole::kDurationUnit:
      return "a duration";
    case vocabulary::WordRole::kSymbol:
      return "an operator";
  }
  return "a word of the language";
}

/// The kind as a phrase: `flow-operator` becomes `flow operator`.
std::string KindPhrase(SemanticKind kind) {
  std::string phrase(SemanticKindName(kind));
  for (char& letter : phrase) {
    if (letter == '-') letter = ' ';
  }
  return phrase;
}

/// The unit a duration token ends in: `ms` of `250ms`.
///
/// One unit per token, always: the lexer reads `1m30s` as the two tokens `1m`
/// and `30s` and the parser adds them, so a caret is always in a duration of a
/// single unit and there is no compound token to answer for.
std::string_view DurationUnitOf(std::string_view text) {
  size_t letters = text.size();
  while (letters > 0 &&
         absl::ascii_isalpha(static_cast<unsigned char>(text[letters - 1]))) {
    --letters;
  }
  return text.substr(letters);
}

std::string DocMarkdown(std::string_view name, std::string_view kind,
                        const vocabulary::WordDoc& doc) {
  std::string out = absl::StrCat("`", name, "` — ", kind, "\n\n", doc.summary,
                                 "\n");
  if (!doc.takes.empty()) {
    absl::StrAppend(&out, "\n**Takes:** ", doc.takes, ".\n");
  }
  if (!doc.detail.empty()) absl::StrAppend(&out, "\n", doc.detail, "\n");
  if (!doc.example.empty()) {
    absl::StrAppend(&out, "\n**Example:** `", doc.example, "`\n");
  }
  return out;
}

}  // namespace


std::string WordMarkdown(std::string_view name, vocabulary::WordRole role) {
  const std::string canonical = vocabulary::Canonical(name);
  const vocabulary::WordDoc* doc = vocabulary::Documentation(role, canonical);
  if (doc == nullptr) doc = vocabulary::AnyDocumentation(canonical);
  if (doc == nullptr) return "";
  return DocMarkdown(name, RoleName(role), *doc);
}

std::string StageMarkdown(std::string_view name) {
  return WordMarkdown(name, vocabulary::WordRole::kStage);
}

std::string BuiltinMarkdown(std::string_view name) {
  return WordMarkdown(name, vocabulary::WordRole::kBuiltin);
}

std::string ActionMarkdown(const catalogue::ActionInfo& action) {
  std::string out = absl::StrCat("`", action.name, "` — an action\n\n",
                                 action.description, "\n", PortTable(action));
  // Where it was written, for an action a scan of the project found. Last,
  // because it is provenance rather than reference: a reader wants to know what
  // the action does first and where it lives only if they are going there.
  if (action.origin.has_value()) {
    absl::StrAppend(&out, "\nDeclared in `", action.origin->file, ":",
                    action.origin->line, "`\n");
  }
  return out;
}

std::string PortMarkdown(std::string_view name, std::string_view type,
                         bool required, bool unary,
                         std::string_view description) {
  std::string out = absl::StrCat("`", name, "` — an input port\n\n");
  if (!description.empty()) absl::StrAppend(&out, description, "\n");
  absl::StrAppend(&out, "\n**Takes:** ", type.empty() ? "any" : type,
                  unary ? " (one value)" : " (a stream)",
                  required ? ", required" : ", optional", ".\n");
  return out;
}

std::string FlowMarkdown(const FlowPlan& flow) {
  std::string ports;
  for (const PortPlan& port : flow.ports) {
    absl::StrAppend(&ports, "- `", port.name, "`: ",
                    port.declared.empty() ? port.type : port.declared,
                    port.direction == syntax::PortDirection::kInput ? " (in)"
                                                                    : " (out)",
                    "\n");
  }
  return absl::StrCat("`", flow.name, "` — a flow of this file\n\n",
                      flow.description, ports.empty() ? "" : "\n\n**Ports**\n\n",
                      ports);
}

std::string ShapeMarkdown(const DtoPlan& shape) {
  return absl::StrCat(
      "`", shape.name, "` — a struct of ", shape.fields.size(), " field",
      shape.fields.size() == 1 ? "" : "s", "\n\n",
      shape.description.empty() ? "" : absl::StrCat(shape.description, "\n"),
      FieldTable(shape));
}

std::string_view SymbolClassName(SymbolClass kind) {
  switch (kind) {
    case SymbolClass::kFlow:
      return "flow";
    case SymbolClass::kDto:
      return "struct";
    case SymbolClass::kField:
      return "field";
    case SymbolClass::kPort:
      return "port";
    case SymbolClass::kHeader:
      return "header";
    case SymbolClass::kNodeMap:
      return "node-map";
    case SymbolClass::kNode:
      return "node";
    case SymbolClass::kCall:
      return "call";
    case SymbolClass::kBarrier:
      return "barrier";
    case SymbolClass::kVariable:
      return "variable";
    case SymbolClass::kExternal:
      return "external";
  }
  return "external";
}

/// The whole construct a declaration opens: its first token through the `}` that
/// closes its block.
///
/// **Why this is worked out here.** The parser records where a node *started* --
/// the token it began at, which is what `flow.syntax/v1` documents `at` as -- and
/// that is the wrong extent for a symbol. A `flow`'s name is the token *after* the
/// keyword, so a range of the keyword alone does not contain the name, and a
/// document symbol whose selection is outside its range is a protocol violation:
/// LSP refuses the whole answer with "selectionRange must be contained in
/// fullRange", so one bad entry cost the document its entire outline.
///
/// It is also what the format promises: `range` is the whole construct, so
/// "select symbol" takes the block.
/// `range`, widened to hold `selection`.
///
/// The invariant every document symbol has to satisfy, applied where a construct
/// has no block to match braces around: a port, a field, a bound step. Their range
/// and selection are usually the same token, and this is what makes "usually" into
/// "always".
Range Widened(const LineIndex& lines, const Range& range, const Range& selection) {
  return lines.Between(std::min(range.start.offset, selection.start.offset),
                       std::max(range.end.offset, selection.end.offset));
}

Range ConstructRange(const LineIndex& lines, absl::Span<const Token> tokens,
                     const syntax::Location& opened, const Range& selection) {
  size_t end = opened.end;
  size_t at = 0;
  while (at < tokens.size() && tokens[at].start < opened.start) ++at;
  int depth = 0;
  for (; at < tokens.size() && tokens[at].kind != TokenKind::kEnd; ++at) {
    if (tokens[at].kind == TokenKind::kLeftBrace) {
      ++depth;
    } else if (tokens[at].kind == TokenKind::kRightBrace) {
      --depth;
      if (depth <= 0) {
        end = tokens[at].end;
        break;
      }
    }
  }
  // A declaration somebody is part-way through typing has no closing brace, and a
  // range that stopped short of its own name would be the violation this exists to
  // prevent. So the name is always inside it, whatever the text looks like.
  end = std::max(end, selection.end.offset);
  return lines.Between(std::min(opened.start, selection.start.offset), end);
}

std::vector<DocumentSymbol> Symbols(std::string_view source) {
  const LineIndex lines(source);
  const ParseResult parsed = Parse(source);
  const ResolveResult resolved = Resolve(source, parsed);
  // Lexed again rather than threaded through: finding a construct's end is brace
  // matching, and the tokens are what that reads. Cheap next to the resolve above.
  const LexResult lexed = Lex(source);
  std::vector<DocumentSymbol> found;

  // The shapes first, as they are written: a file that declares one usually
  // opens with it, and the outline should read like the file.
  for (const syntax::DtoDeclarationPtr& declaration : parsed.dtos) {
    DocumentSymbol shape;
    shape.name = declaration->name.text;
    shape.kind = SymbolClass::kDto;
    shape.detail = declaration->description;
    shape.selection = RangeOf(lines, declaration->name.location);
    shape.range = ConstructRange(lines, lexed.tokens, declaration->location,
                                 shape.selection);
    for (const syntax::FieldDeclarationPtr& field : declaration->fields) {
      DocumentSymbol one;
      one.name = field->name.text;
      one.kind = SymbolClass::kField;
      one.detail = field->type.ToString();
      one.selection = RangeOf(lines, field->name.location);
      // A field is one line and has no block, so its extent is the line it is
      // written on -- but its name still has to be inside it.
      one.range = Widened(lines, RangeOf(lines, field->location), one.selection);
      shape.children.push_back(std::move(one));
    }
    found.push_back(std::move(shape));
  }

  for (const ResolvedFlow& flow : resolved.flows) {
    if (flow.declaration == nullptr) continue;
    DocumentSymbol one;
    one.name = flow.plan.name;
    one.kind = SymbolClass::kFlow;
    one.detail = flow.plan.description;
    one.selection = RangeOf(lines, flow.declaration->name.location);
    one.range = ConstructRange(lines, lexed.tokens, flow.declaration->location,
                               one.selection);
    for (const PortPlan& port : flow.plan.ports) {
      one.children.push_back(PortSymbol(lines, port));
    }
    // Everything the flow bound that a reader might want to jump to. The
    // implicit ones are the language's, not the author's, and nobody navigates
    // to `index`.
    for (const Symbol& symbol : flow.symbols) {
      if (symbol.implicit) continue;
      if (symbol.kind == SymbolKind::kInputPort ||
          symbol.kind == SymbolKind::kOutputPort) {
        continue;  // already listed, with its declared type
      }
      DocumentSymbol child;
      child.name = symbol.name;
      child.kind = ClassOf(symbol.kind);
      child.detail = DetailOf(symbol);
      child.range = RangeOf(lines, symbol.location);
      child.selection = child.range;
      one.children.push_back(std::move(child));
    }
    found.push_back(std::move(one));
  }
  return found;
}

Description Describe(std::string_view source, size_t offset,
                     const catalogue::Catalogue& known) {
  Description about;
  const LineIndex lines(source);
  const LexResult lexed = Lex(source, LexOptions{.keep_comments = true});
  const std::vector<SemanticToken> semantic = Highlight(lexed.tokens);
  const SemanticToken* here = TokenAt(semantic, offset);
  if (here == nullptr) return about;
  // A line break is a token -- it is what ends a statement -- but it is not
  // something anybody hovers, and "`\n` -- punctuation" is a worse answer than
  // no answer.
  if (source.substr(here->start, here->end - here->start)
          .find_first_not_of(" \t\r\n") == std::string_view::npos) {
    return about;
  }

  size_t index = 0;
  for (size_t at = 0; at < semantic.size(); ++at) {
    if (&semantic[at] == here) index = at;
  }
  about.found = true;
  about.text = std::string(source.substr(here->start, here->end - here->start));
  about.range = lines.Between(here->start, here->end);

  const ParseResult parsed = Parse(source);
  const ResolveResult resolved = Resolve(source, parsed);
  const std::string word = vocabulary::Canonical(about.text);

  const std::vector<size_t> starts = DeclarationStarts(parsed);

  // A name the document bound. Asked first, because a flow's own names are what
  // a reader is usually looking at, and a port called `text` is that port rather
  // than the stage of the same name.
  //
  // Only the flow the caret is *in*: two flows may each declare `in q`, and
  // answering with whichever came first in the file would describe a port the
  // reader cannot see and offer a definition in the wrong flow.
  for (const ResolvedFlow& flow : resolved.flows) {
    if (flow.declaration == nullptr) continue;
    if (!Within(starts, flow.declaration->location.start, here->start,
                source.size())) {
      continue;
    }
    for (const Symbol& symbol : flow.symbols) {
      if (symbol.name != about.text || symbol.implicit) continue;
      about.kind = ClassOf(symbol.kind);
      about.summary =
          absl::StrCat("`", about.text, "` — ", SymbolKindName(symbol.kind),
                       " of `", flow.plan.name, "`");
      about.has_definition = true;
      about.definition = RangeOf(lines, symbol.location);
      if (symbol.kind == SymbolKind::kCall) {
        absl::StrAppend(&about.summary, ", running `", symbol.action, "`");
      }
      if (const PortPlan* port = flow.plan.Port(
              symbol.name, symbol.kind == SymbolKind::kInputPort
                               ? syntax::PortDirection::kInput
                               : syntax::PortDirection::kOutput);
          port != nullptr) {
        about.detail = port->description;
        absl::StrAppend(&about.summary, ": ",
                        port->declared.empty() ? port->type : port->declared);
      }
      break;
    }
    if (about.has_definition) break;
  }

  // A shape, whether this file declared it or the host knows it. Both read the
  // same, which is the point of recording them the same way.
  if (!about.has_definition && index < lexed.tokens.size() &&
      lexed.tokens[index].IsWord()) {
    const auto [dotted, first] = DottedAround(lexed.tokens, index);
    const DtoPlan* shape = resolved.program.Dto(dotted);
    std::optional<catalogue::Origin> from_host;
    if (shape == nullptr) {
      if (const catalogue::TypeInfo* type = known.Type(dotted);
          type != nullptr) {
        shape = &type->shape;
        from_host = type->origin;
      }
    }
    if (shape != nullptr) {
      about.text = dotted;
      about.range = lines.Between(lexed.tokens[first].start, here->end);
      about.kind = SymbolClass::kDto;
      about.summary = absl::StrCat("`", dotted, "` — a struct of ",
                                   shape->fields.size(), " field",
                                   shape->fields.size() == 1 ? "" : "s");
      about.detail = shape->description;
      about.markdown = ShapeMarkdown(*shape);
      about.origin = from_host;
      if (const syntax::Word* name = NameOfShape(parsed, dotted);
          name != nullptr) {
        about.has_definition = true;
        about.definition = RangeOf(lines, name->location);
      }
      return about;
    }
  }

  // An action, whether a flow of this file or one the world has.
  if (!about.has_definition &&
      (here->kind == SemanticKind::kActionName ||
       here->kind == SemanticKind::kFlowName)) {
    if (const FlowPlan* sibling = resolved.program.Flow(about.text);
        sibling != nullptr) {
      about.kind = SymbolClass::kFlow;
      about.summary = absl::StrCat("`", about.text, "` — a flow of this file");
      about.detail = sibling->description;
      if (const syntax::Word* name = NameOfFlow(parsed, about.text);
          name != nullptr) {
        about.has_definition = true;
        about.definition = RangeOf(lines, name->location);
      }
      about.markdown = FlowMarkdown(*sibling);
      return about;
    }
    if (const catalogue::ActionInfo* action = known.Action(about.text);
        action != nullptr) {
      about.kind = SymbolClass::kExternal;
      about.summary = absl::StrCat("`", about.text, "` — an action");
      about.detail = action->description;
      about.markdown = ActionMarkdown(*action);
      // An action declared in a file something read has somewhere to go, which
      // is new: before a scan there was nothing to point at and hovering one of
      // these said "action name".
      about.origin = action->origin;
      return about;
    }
  }

  // A word or a mark of the language, with the language's own reference for it.
  //
  // Which table answers is decided by what the highlighter already said this
  // position means, so this is a lookup and not a second guess: the same `text`
  // is a stage after a `|` and a function before a `(`, and they do different
  // things. Everything here used to be one line naming the token's kind --
  // "flow operator" for a `|` -- with four hand-written paragraphs bolted on
  // beside it for the words that read as something else. The paragraphs are in
  // `vocabulary.cc` now, with the rest of the language.
  if (about.summary.empty()) {
    for (const vocabulary::WordRole role : RolesFor(here->kind)) {
      const std::string key =
          role == vocabulary::WordRole::kSymbol ? about.text
          : here->kind == SemanticKind::kDuration
              ? std::string(DurationUnitOf(about.text))
              : word;
      const vocabulary::WordDoc* doc = vocabulary::Documentation(role, key);
      if (doc == nullptr) continue;
      // A brace is not "an operator" and a `|` is not "a punctuation": for a
      // mark the token's own kind is the more useful word, and it is what the
      // reader sees the colour of.
      const std::string label = role == vocabulary::WordRole::kSymbol
                                    ? KindPhrase(here->kind)
                                    : std::string(RoleName(role));
      about.kind = SymbolClass::kExternal;
      about.summary =
          absl::StrCat("`", about.text, "` — ", label, ": ", doc->summary);
      about.detail = std::string(doc->detail);
      about.markdown = DocMarkdown(about.text, label, *doc);
      return about;
    }
  }

  // Nothing the document, the world, or the language named: an identifier the
  // resolver could not place, a comment, a string. Saying what the highlighter
  // decided is still better than saying nothing.
  if (about.summary.empty()) {
    about.summary =
        absl::StrCat("`", about.text, "` — ", KindPhrase(here->kind));
  }
  if (about.markdown.empty()) {
    about.markdown = about.detail.empty()
                         ? about.summary
                         : absl::StrCat(about.summary, "\n\n", about.detail);
  }
  return about;
}

}  // namespace a11::flow
