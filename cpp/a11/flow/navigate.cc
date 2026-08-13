// Copyright 2026 The A11 Authors.

#include "a11/flow/navigate.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>

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


std::string StageMarkdown(std::string_view name) {
  const vocabulary::WordDoc* doc =
      vocabulary::StageDocumentation(vocabulary::Canonical(name));
  if (doc == nullptr) return "";
  return DocMarkdown(name, "a pipeline stage", *doc);
}

std::string BuiltinMarkdown(std::string_view name) {
  const vocabulary::WordDoc* doc =
      vocabulary::BuiltinDocumentation(vocabulary::Canonical(name));
  if (doc == nullptr) return "";
  return DocMarkdown(name, "a built-in function", *doc);
}

std::string ActionMarkdown(const catalogue::ActionInfo& action) {
  return absl::StrCat("`", action.name, "` — an action\n\n",
                      action.description, "\n", PortTable(action));
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

std::vector<DocumentSymbol> Symbols(std::string_view source) {
  const LineIndex lines(source);
  const ParseResult parsed = Parse(source);
  const ResolveResult resolved = Resolve(source, parsed);
  std::vector<DocumentSymbol> found;

  // The shapes first, as they are written: a file that declares one usually
  // opens with it, and the outline should read like the file.
  for (const syntax::DtoDeclarationPtr& declaration : parsed.dtos) {
    DocumentSymbol shape;
    shape.name = declaration->name.text;
    shape.kind = SymbolClass::kDto;
    shape.detail = declaration->description;
    shape.range = RangeOf(lines, declaration->location);
    shape.selection = RangeOf(lines, declaration->name.location);
    for (const syntax::FieldDeclarationPtr& field : declaration->fields) {
      DocumentSymbol one;
      one.name = field->name.text;
      one.kind = SymbolClass::kField;
      one.detail = field->type.ToString();
      one.range = RangeOf(lines, field->location);
      one.selection = RangeOf(lines, field->name.location);
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
    one.range = RangeOf(lines, flow.declaration->location);
    one.selection = RangeOf(lines, flow.declaration->name.location);
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
    if (shape == nullptr) {
      if (const catalogue::TypeInfo* type = known.Type(dotted);
          type != nullptr) {
        shape = &type->shape;
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
      return about;
    }
  }

  // A stage or a function, with the language's own reference for it. Which of the
  // two a word is here is the highlighter's judgement and not a second guess: the
  // same `text` is a stage after a `|` and a function before a `(`, and they do
  // different things, so the position decides which is answered.
  if (about.summary.empty() && (here->kind == SemanticKind::kStage ||
                                here->kind == SemanticKind::kBuiltin)) {
    const bool staged = here->kind == SemanticKind::kStage;
    const vocabulary::WordDoc* doc = staged
                                         ? vocabulary::StageDocumentation(word)
                                         : vocabulary::BuiltinDocumentation(word);
    if (doc != nullptr) {
      about.kind = SymbolClass::kExternal;
      about.summary =
          absl::StrCat("`", about.text, "` — ",
                       staged ? "a pipeline stage: " : "a built-in function: ",
                       doc->summary);
      about.detail = std::string(doc->detail);
      about.markdown =
          staged ? StageMarkdown(about.text) : BuiltinMarkdown(about.text);
      return about;
    }
  }

  // Nothing the document or the world named: it is a word of the language, and
  // what it is is what the highlighter already decided.
  if (about.summary.empty()) {
    std::string what(SemanticKindName(here->kind));
    for (char& letter : what) {
      if (letter == '-') letter = ' ';
    }
    std::string extra;
    if (here->kind == SemanticKind::kStatusCode) {
      extra = ", one of Abseil's canonical status codes";
    }
    // The two words that open a pipeline source rather than naming one. Both
    // read like something else -- `status` like a port, `zip` like a function --
    // so a hover that only said "keyword" would answer the wrong question.
    if (word == "zip") {
      about.detail =
          "Reads several streams in step, as one stream of tuples: `it[0]`, "
          "`it[1]`, or `for x, y in zip(a, b)`. A source that ends well "
          "contributes a null to every tuple after it; one that ends with an "
          "error ends the iteration with that status.";
    } else if (word == "advance") {
      about.detail =
          "Rebinds a value a `let` bound to the *next* value of the same "
          "stream: `let word = words`, then `advance word`, and the name reads "
          "the second value from there on. The guarantee is positional and not "
          "an ordering, so the *k*th binding of a name is the *k*th value of its "
          "stream however the flow is scheduled. Statements written before the "
          "`advance` keep the value they resolved against. Advancing past the "
          "end binds nothing, exactly as a `let` on an empty stream does.";
    } else if (word == "let") {
      about.detail =
          "Names one value of a stream, where everything else here is a stream: "
          "`let code = http.status_code`, and the name then stands where an "
          "expression does. Lazy, so nothing is read until the name is, and it "
          "may be written where it reads best. An empty stream binds nothing, "
          "which `if not code` asks about. `advance` is how to move it on to the "
          "next value.";
    } else if (word == "status") {
      about.detail =
          "The outcome of a call, a node or a barrier, as a record: "
          "`{\"ok\": .., \"code\": .., \"number\": .., \"message\": ..}`. "
          "Reading one waits for the subject to finish.";
    }
    about.summary = absl::StrCat("`", about.text, "` — ", what, extra);
  }
  if (about.markdown.empty()) {
    about.markdown = about.detail.empty()
                         ? about.summary
                         : absl::StrCat(about.summary, "\n\n", about.detail);
  }
  return about;
}

}  // namespace a11::flow
