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

#include "a11/flow/contract.h"
#include "a11/flow/highlight.h"
#include "a11/flow/internal/editor_documentation.h"
#include "a11/flow/lexer.h"
#include "a11/flow/module.h"
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
    case SymbolKind::kSink:
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
  symbol.port_direction =
      port.direction == syntax::PortDirection::kInput ? "in" : "out";
  symbol.port_type = port.declared.empty() ? port.type : port.declared;
  symbol.port_stream = !port.unary;
  symbol.port_required = port.required;
  symbol.range = RangeOf(lines, port.location);
  symbol.selection = symbol.range;
  return symbol;
}

void AppendType(std::string* out, std::string_view type) {
  absl::StrAppend(out, "*", type, "*");
}

/// The ports of an action, as one block of Markdown.
///
/// What a hover over a call's name is *for*: `run make_http_request(` is eight
/// output ports and nine inputs, and the whole reason to hover it is to see
/// them
/// without leaving the file.
std::string PortTable(const catalogue::ActionInfo& action) {
  std::string out;
  for (const auto& [heading, ports] :
       {std::pair<std::string_view, const std::vector<catalogue::PortInfo>*>{
            "Inputs", &action.inputs},
        std::pair<std::string_view, const std::vector<catalogue::PortInfo>*>{
            "Outputs", &action.outputs}}) {
    if (ports->empty()) {
      continue;
    }
    absl::StrAppend(&out, "\n**", heading, "**\n\n");
    for (const catalogue::PortInfo& port : *ports) {
      absl::StrAppend(&out, "- `", port.name, "`: ");
      AppendType(&out, port.type);
      absl::StrAppend(&out, port.unary ? "" : " stream",
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
  if (shape.fields.empty()) {
    return "";
  }
  std::string out = "\n**Fields**\n\n";
  for (const FieldPlan& field : shape.fields) {
    const std::string_view type =
        field.declared.empty() ? field.type : field.declared;
    absl::StrAppend(&out, "- `", field.name, "`: ");
    AppendType(&out, type);
    absl::StrAppend(&out, field.required ? " *(required)*" : "");
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
/// where it starts, and asking whether an offset is merely *after* one would
/// put
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
  if (offset < start) {
    return false;
  }
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
  const syntax::Word* declaration = nullptr;
  for (const auto& flow : parsed.flows) {
    if (flow->name.text != name) {
      continue;
    }
    if (!flow->interface_only) {
      return &flow->name;
    }
    declaration = &flow->name;
  }
  return declaration;
}

const syntax::Word* absl_nullable NameOfShape(const ParseResult& parsed,
                                              std::string_view name) {
  for (const syntax::DtoDeclarationPtr& shape : parsed.dtos) {
    if (shape->name.text == name) {
      return &shape->name;
    }
  }
  return nullptr;
}

/// The semantic token covering `offset`, or nothing.
const SemanticToken* absl_nullable TokenAt(
    const std::vector<SemanticToken>& tokens, size_t offset) {
  for (const SemanticToken& token : tokens) {
    if (offset >= token.start && offset < token.end) {
      return &token;
    }
  }
  return nullptr;
}

/// The dotted name the token at `index` is part of, and where it starts.
///
/// `a11.sdk.AudioBuffer` is five tokens and one name, and a caret anywhere in
/// it is a caret on the type -- so the whole chain is gathered rather than the
/// one word the caret happens to be in.
std::pair<std::string, size_t> DottedAround(const std::vector<Token>& tokens,
                                            size_t index) {
  size_t first = index;
  while (first >= 2 && tokens[first - 1].kind == TokenKind::kScope &&
         tokens[first - 2].IsWord()) {
    first -= 2;
  }
  size_t last = index;
  while (last + 2 < tokens.size() &&
         tokens[last + 1].kind == TokenKind::kScope &&
         tokens[last + 2].IsWord()) {
    last += 2;
  }
  std::string name;
  for (size_t at = first; at <= last; ++at) {
    absl::StrAppend(&name, tokens[at].text);
  }
  return {name, first};
}

/// Formats docstrings in Markdown using bold text, code spans, and paragraph
/// breaks for compatibility across editor renderers.
///
/// Returns the candidate word roles for a given semantic kind, in priority
/// order.
///
/// `kStatementKeyword` covers statements, source words, and clauses, while
/// `kDeclarationKeyword` covers declarations and port/field modifiers.
/// This function tries kinds in the highlighter's own order (`SemanticKindOf`
/// in
/// `highlight.cc`) to determine the position's role.
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
  static constexpr WordRole kLogLevel[] = {WordRole::kLogLevel};
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
    case SemanticKind::kLogLevel:
      return absl::MakeConstSpan(kLogLevel);
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
    case vocabulary::WordRole::kLogLevel:
      return "a log level";
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
    if (letter == '-') {
      letter = ' ';
    }
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
  std::string out =
      absl::StrCat("`", name, "` — ", kind, "\n\n", doc.summary, "\n");
  if (!doc.takes.empty()) {
    absl::StrAppend(&out, "\n**Takes:** ", doc.takes, ".\n");
  }
  if (!doc.detail.empty()) {
    absl::StrAppend(&out, "\n", doc.detail, "\n");
  }
  if (!doc.example.empty()) {
    std::string_view examples = doc.example;
    const size_t first_line = examples.find('\n');
    const bool introduced_flow =
        first_line != std::string_view::npos &&
        examples.substr(first_line + 1).starts_with("flow ");
    const std::string_view separator =
        introduced_flow || examples.starts_with("flow ") ||
                examples.find("\n\n") != std::string_view::npos
            ? "\n\n"
            : "\n";
    while (!examples.empty()) {
      const size_t end = examples.find(separator);
      const std::string_view example = examples.substr(0, end);
      if (!example.empty()) {
        const size_t line_break = example.find('\n');
        const bool has_intro = line_break != std::string_view::npos &&
                               example.substr(line_break + 1, 5) == "flow ";
        const std::string_view code =
            has_intro ? example.substr(line_break + 1) : example;
        if (has_intro) {
          absl::StrAppend(&out, "\n", example.substr(0, line_break), "\n");
        }
        if (code.find('\n') == std::string_view::npos) {
          absl::StrAppend(&out, "\n**Example:** `", example, "`\n");
        } else {
          if (!has_intro) {
            absl::StrAppend(&out, "\n**Example:**\n");
          }
          absl::StrAppend(&out, "\n```a11flow\n", code, "\n```\n");
        }
      }
      if (end == std::string_view::npos) {
        break;
      }
      examples.remove_prefix(end + separator.size());
    }
  }
  return out;
}

enum class DelimiterContext {
  kFlow,
  kConditional,
  kLoop,
  kBlock,
  kAfterBlock,
  kNodes,
  kDeclaration,
  kRecord,
  kTypedValue,
  kActionCall,
  kFunctionCall,
  kNodeCall,
  kGroup,
};

struct DelimiterDoc {
  std::string_view kind;
  std::string_view opens;
  std::string_view closes;
  std::string_view detail;
  std::string_view example;
};

DelimiterDoc DocumentationFor(DelimiterContext context) {
  switch (context) {
    case DelimiterContext::kFlow:
      return {"flow body", "Opens the flow's statement scope.",
              "Closes the flow's statement scope.",
              "Statements in a flow start concurrently within each phase. "
              "Their written order does not wait for earlier work. Use "
              "`after` or `wait all` to create a dependency; names declared "
              "inside the flow are not visible to callers.",
              "Two calls can start together; the barrier waits for both.\n"
              "flow lookup {\n"
              "  out result: string stream\n"
              "  a = call search_one()\n"
              "  b = call search_two()\n"
              "  wait all of a, b\n"
              "  a.text | then b.text -> result\n"
              "}"};
    case DelimiterContext::kConditional:
      return {"conditional branch", "Opens a conditional branch.",
              "Closes a conditional branch.",
              "Only the selected `if` or `else` branch runs. Statements "
              "inside that branch start concurrently. Statements after its "
              "closing brace remain independent unless they name a "
              "completion dependency.",
              "Select a branch after the lookup completes.\n"
              "flow classify {\n"
              "  in query: string\n"
              "  out result: string\n"
              "  found = try call search(query: query)\n"
              "  after found {\n"
              "    ready = wait found\n"
              "    if ready.ok { found.text -> result }\n"
              "    else { \"missing\" -> result }\n"
              "  }\n"
              "}"};
    case DelimiterContext::kLoop:
      return {"loop body", "Opens a loop body.", "Closes a loop body.",
              "Statements in one pass start concurrently. A `for` reads "
              "one source value per pass unless `parallel` allows several "
              "passes at once. A `repeat` carries state between passes; "
              "closing its body does not itself finish the loop.",
              "Process a bounded number of requests concurrently.\n"
              "flow fetch_all {\n"
              "  in urls: string stream\n"
              "  out pages: string stream\n"
              "  for url in urls parallel 3 {\n"
              "    page = call fetch(url: url)\n"
              "    page.text -> pages\n"
              "  }\n"
              "}"};
    case DelimiterContext::kBlock:
      return {"standalone block", "Opens a standalone block.",
              "Closes a standalone block.",
              "The block is one step whose statements start concurrently. "
              "Its completion includes all members. A bound block yields "
              "a status; `try` contains a failure instead of ending the flow. "
              "Use a dependency when a member must wait for another.",
              "Group concurrent requests under one completion subject.\n"
              "flow report {\n"
              "  batch = {\n"
              "    first = call lookup_one()\n"
              "    second = call lookup_two()\n"
              "  }\n"
              "  wait batch\n"
              "}"};
    case DelimiterContext::kAfterBlock:
      return {"dependency block", "Opens an `after` block.",
              "Closes an `after` block.",
              "The block starts after every named subject completes. Its "
              "own statements then start concurrently; their written "
              "order does not create another dependency.",
              "Run both follow-up steps after planning finishes.\n"
              "flow planned {\n"
              "  plan = call make_plan()\n"
              "  after plan {\n"
              "    call reserve()\n"
              "    call notify()\n"
              "  }\n"
              "}"};
    case DelimiterContext::kNodes:
      return {"node-map block", "Opens a local node-map scope.",
              "Closes a local node-map scope.",
              "Calls in this scope use the named node map. Its nodes stay "
              "local to the flow rather than entering the dispatched wire "
              "graph. Statements inside the scope still start concurrently.",
              "flow local_work {\n"
              "  nodes scratch {\n"
              "    call prepare()\n"
              "    call inspect()\n"
              "  }\n"
              "}"};
    case DelimiterContext::kDeclaration:
      return {"declaration body", "Opens a declaration body.",
              "Closes a declaration body.",
              "A `struct` body declares fields; a module body groups "
              "declarations. These braces delimit definitions, not "
              "concurrent executable statements.",
              "struct Result {\n  value: string required\n}"};
    case DelimiterContext::kRecord:
      return {"record literal", "Opens a record literal.",
              "Closes a record literal.",
              "Entries form a value. Later entries overwrite earlier "
              "entries with the same key, including keys introduced by a "
              "spread. A record brace does not create a statement scope.",
              "{...defaults, \"limit\": 10}"};
    case DelimiterContext::kTypedValue:
      return {"typed value", "Opens a typed value's fields.",
              "Closes a typed value's fields.",
              "The named registered type validates the fields and applies "
              "its defaults. This is a value expression, not a statement "
              "block. Parenthesize it where `{` would otherwise open a "
              "condition or loop body.",
              "a11::sdk::Interaction{role: \"user\", content: []}"};
    case DelimiterContext::kActionCall:
      return {"action inputs", "Opens an action's input arguments.",
              "Closes an action's input arguments.",
              "Each named argument feeds an input port. A pipeline can feed "
              "a streaming port. Call modifiers such as `with`, `timeout`, "
              "`via`, and `after` follow the closing parenthesis.",
              "answer = call ask_model(question: prompt, context: notes) "
              "timeout 30s"};
    case DelimiterContext::kFunctionCall:
      return {"function arguments", "Opens function arguments.",
              "Closes function arguments.",
              "Arguments are expressions in positional order. The "
              "function's own documentation specifies accepted types and "
              "results; these parentheses do not start an action.",
              "std::join([\"alpha\", \"beta\"], \", \")"};
    case DelimiterContext::kNodeCall:
      return {"node constructor", "Opens a node constructor.",
              "Closes a node constructor.",
              "`node()` creates a node with a generated id; `node(id)` "
              "uses the supplied id. A following `in` places it in a named "
              "node map.",
              "progress = node(\"progress\") in scratch"};
    case DelimiterContext::kGroup:
      return {"grouped expression", "Opens a grouped expression.",
              "Closes a grouped expression.",
              "Grouping controls precedence. It also permits a pipeline "
              "value where an expression is required. Grouping alone "
              "does not wait for a concurrently running step.",
              "if (hits | count) > 0 { \"found\" -> result }"};
  }
  return {};
}

bool IsDelimiter(TokenKind kind) {
  return kind == TokenKind::kLeftBrace || kind == TokenKind::kRightBrace ||
         kind == TokenKind::kLeftParen || kind == TokenKind::kRightParen;
}

size_t OpeningDelimiter(absl::Span<const Token> tokens, size_t index) {
  const TokenKind closing = tokens[index].kind;
  if (closing == TokenKind::kLeftBrace || closing == TokenKind::kLeftParen) {
    return index;
  }
  const TokenKind opening = closing == TokenKind::kRightBrace
                                ? TokenKind::kLeftBrace
                                : TokenKind::kLeftParen;
  int depth = 0;
  for (size_t at = index + 1; at-- > 0;) {
    if (tokens[at].kind == closing) {
      ++depth;
    }
    if (tokens[at].kind == opening && --depth == 0) {
      return at;
    }
  }
  return index;
}

DelimiterContext BraceContext(absl::Span<const Token> tokens, size_t opening,
                              const ParseResult& parsed) {
  const size_t start = tokens[opening].start;
  if (std::binary_search(parsed.tagged_braces.begin(),
                         parsed.tagged_braces.end(), start)) {
    return DelimiterContext::kTypedValue;
  }
  if (std::binary_search(parsed.value_braces.begin(), parsed.value_braces.end(),
                         start)) {
    return DelimiterContext::kRecord;
  }
  size_t first = opening;
  while (first > 0 && tokens[first - 1].kind != TokenKind::kNewline &&
         tokens[first - 1].kind != TokenKind::kSemicolon &&
         tokens[first - 1].kind != TokenKind::kLeftBrace &&
         tokens[first - 1].kind != TokenKind::kRightBrace) {
    --first;
  }
  for (size_t at = first; at < opening; ++at) {
    if (!tokens[at].IsWord()) {
      continue;
    }
    const std::string word = vocabulary::Canonical(tokens[at].text);
    if (word == "flow") {
      return DelimiterContext::kFlow;
    }
    if (word == "if" || word == "else") {
      return DelimiterContext::kConditional;
    }
    if (word == "for" || word == "repeat") {
      return DelimiterContext::kLoop;
    }
    if (word == "nodes") {
      return DelimiterContext::kNodes;
    }
    if (word == "struct" || word == "module") {
      return DelimiterContext::kDeclaration;
    }
    if (word == "after") {
      return DelimiterContext::kAfterBlock;
    }
  }
  return DelimiterContext::kBlock;
}

DelimiterContext ParenthesisContext(absl::Span<const Token> tokens,
                                    size_t opening) {
  if (opening == 0 || !tokens[opening - 1].IsWord()) {
    return DelimiterContext::kGroup;
  }
  const std::string before = vocabulary::Canonical(tokens[opening - 1].text);
  if (before == "if" || before == "while" || before == "until" ||
      before == "for" || before == "take") {
    return DelimiterContext::kGroup;
  }
  if (before == "node") {
    return DelimiterContext::kNodeCall;
  }
  size_t first = opening - 1;
  while (first > 0 && tokens[first - 1].kind != TokenKind::kNewline &&
         tokens[first - 1].kind != TokenKind::kSemicolon &&
         tokens[first - 1].kind != TokenKind::kLeftBrace &&
         tokens[first - 1].kind != TokenKind::kRightBrace) {
    --first;
  }
  for (size_t at = first; at + 1 < opening; ++at) {
    if (!tokens[at].IsWord()) {
      continue;
    }
    const std::string word = vocabulary::Canonical(tokens[at].text);
    if (word != "run" && word != "call") {
      continue;
    }
    bool inside_arguments = false;
    for (size_t between = at + 1; between < opening; ++between) {
      if (tokens[between].kind == TokenKind::kLeftParen) {
        inside_arguments = true;
        break;
      }
    }
    if (!inside_arguments) {
      return DelimiterContext::kActionCall;
    }
  }
  return DelimiterContext::kFunctionCall;
}

}  // namespace

namespace internal {

EditorDocumentation::EditorDocumentation(std::string_view source,
                                         const ParseResult& parsed,
                                         CompilationLimits limits,
                                         CompilationBudget* budget) {
  const auto lexed =
      Lex(source, {.keep_comments = true, .limits = limits, .budget = budget});
  const auto leading = [&](size_t start) {
    std::vector<std::string_view> parts;
    auto at = std::lower_bound(
        lexed.tokens.begin(), lexed.tokens.end(), start,
        [](const Token& token, size_t offset) { return token.start < offset; });
    while (at != lexed.tokens.begin()) {
      --at;
      if (at->kind == TokenKind::kNewline) {
        continue;
      }
      if (at->kind != TokenKind::kComment) {
        break;
      }
      const auto gap = source.substr(at->end, start - at->end);
      if (gap.find_first_not_of(" \t\r\n") != std::string_view::npos ||
          std::count(gap.begin(), gap.end(), '\n') != 1) {
        break;
      }
      const size_t newline = source.rfind('\n', at->start);
      const size_t line = newline == std::string_view::npos ? 0 : newline + 1;
      if (source.substr(line, at->start - line).find_first_not_of(" \t\r") !=
          std::string_view::npos) {
        break;
      }
      auto text = absl::StripTrailingAsciiWhitespace(at->text.substr(1));
      if (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
      }
      parts.push_back(text);
      start = at->start;
    }
    std::reverse(parts.begin(), parts.end());
    size_t first = 0;
    size_t last = parts.size();
    while (first < last && parts[first].empty()) {
      ++first;
    }
    while (first < last && parts[last - 1].empty()) {
      --last;
    }
    return absl::StrJoin(
        absl::MakeConstSpan(parts).subspan(first, last - first), "\n");
  };
  const auto visit = [&](const syntax::Node& node) {
    if (syntax::IsAnyOf(&node, {syntax::NodeKind::kFlowDeclaration,
                                syntax::NodeKind::kDtoDeclaration,
                                syntax::NodeKind::kFieldDeclaration,
                                syntax::NodeKind::kPortDeclaration})) {
      auto text = leading(node.extent.start);
      if (!text.empty()) {
        comments_.emplace(node.location.start, std::move(text));
      }
    }
    if (const auto* flow = syntax::As<syntax::FlowDeclaration>(&node);
        flow != nullptr) {
      const auto found = flows_.find(flow->name.text);
      if (found == flows_.end() ||
          (found->second->interface_only && !flow->interface_only)) {
        flows_.insert_or_assign(flow->name.text, flow);
      }
    }
  };
  for (const auto& flow : parsed.flows) {
    syntax::VisitSubtree(*flow, visit);
  }
  for (const auto& dto : parsed.dtos) {
    syntax::VisitSubtree(*dto, visit);
  }
}

std::string_view EditorDocumentation::Get(
    std::string_view description, const syntax::Location& location) const {
  if (!description.empty()) {
    return description;
  }
  const auto found = comments_.find(location.start);
  return found == comments_.end() ? description : found->second;
}

FlowPlan EditorDocumentation::Flow(const FlowPlan& flow) const {
  auto shown = flow;
  shown.description = Get(flow.description, flow.location);
  for (auto& port : shown.ports) {
    port.description = Get(port.description, port.location);
  }
  return shown;
}

DtoPlan EditorDocumentation::Shape(const DtoPlan& shape) const {
  auto shown = shape;
  shown.description = Get(shape.description, shape.location);
  for (auto& field : shown.fields) {
    field.description = Get(field.description, field.location);
  }
  return shown;
}

catalogue::Catalogue EditorDocumentation::CatalogueAt(
    const LinkedModule& linked, size_t offset, bool selection,
    CompilationLimits limits, CompilationBudget* budget) const {
  const auto local = linked.CatalogueAt(offset, selection, limits, budget);
  auto actions = local.actions();
  auto types = local.types();
  for (auto& action : actions) {
    for (const auto& member : linked.members) {
      if (member.name != action.name || !action.origin.has_value() ||
          action.origin->file != member.source ||
          action.origin->line != member.location.line ||
          action.origin->column != member.location.column) {
        continue;
      }
      const auto found = flows_.find(member.target);
      if (found == flows_.end()) {
        continue;
      }
      const auto& flow = *found->second;
      action.description = Get(action.description, flow.location);
      for (const auto& port : flow.ports) {
        auto& ports = port->direction == syntax::PortDirection::kInput
                          ? action.inputs
                          : action.outputs;
        for (auto& shown : ports) {
          if (shown.name == port->name.text) {
            shown.description = Get(shown.description, port->location);
          }
        }
      }
      break;
    }
  }
  for (auto& type : types) {
    type.shape = Shape(type.shape);
  }
  return catalogue::Catalogue::Of(std::move(actions), std::move(types),
                                  local.intrinsics());
}

}  // namespace internal

std::string WordMarkdown(std::string_view name, vocabulary::WordRole role) {
  const std::string canonical = vocabulary::Canonical(name);
  const vocabulary::WordDoc* doc = vocabulary::Documentation(role, canonical);
  if (doc == nullptr) {
    doc = vocabulary::AnyDocumentation(canonical);
  }
  if (doc == nullptr) {
    return "";
  }
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
  if (action.schema_origins.size() > 1) {
    absl::StrAppend(&out, "\n**Effective schema contributors**\n\n");
    for (const auto& origin : action.schema_origins) {
      absl::StrAppend(&out, "- `", origin.file, ":", origin.line, "`\n");
    }
  }
  return out;
}

std::string PortMarkdown(std::string_view name, std::string_view type,
                         bool required, bool unary,
                         std::string_view description) {
  std::string out = absl::StrCat("`", name, "` — an input port\n\n");
  if (!description.empty()) {
    absl::StrAppend(&out, description, "\n");
  }
  absl::StrAppend(&out, "\n**Takes:** ");
  AppendType(&out, type.empty() ? "any" : type);
  absl::StrAppend(&out, unary ? " (one value)" : " (a stream)",
                  required ? ", required" : ", optional", ".\n");
  return out;
}

std::string FlowMarkdown(const FlowPlan& flow) {
  std::string ports;
  for (const PortPlan& port : flow.ports) {
    absl::StrAppend(
        &ports,
        port.direction == syntax::PortDirection::kInput ? "- in `" : "- out `",
        port.name, "`: ");
    AppendType(&ports, port.declared.empty() ? port.type : port.declared);
    absl::StrAppend(&ports, port.unary ? "" : " stream",
                    port.required ? " required" : "");
    if (!port.description.empty()) {
      absl::StrAppend(&ports, " — ", port.description);
    }
    ports.push_back('\n');
  }
  return absl::StrCat("`", flow.name, "` — a flow of this file\n\n",
                      flow.description,
                      ports.empty() ? "" : "\n\n**Ports**\n\n", ports);
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
    case SymbolClass::kModule:
      return "module";
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

/// The whole construct a declaration opens: its first token through the `}`
/// that closes its block.
///
/// **Why this is worked out here.** The parser records where a node *started*
/// -- the token it began at, which is what `flow.syntax/v1` documents `at` as
/// -- and that is the wrong extent for a symbol. A `flow`'s name is the token
/// *after* the keyword, so a range of the keyword alone does not contain the
/// name, and a document symbol whose selection is outside its range is a
/// protocol violation: LSP refuses the whole answer with "selectionRange must
/// be contained in fullRange", so one bad entry cost the document its entire
/// outline.
///
/// It is also what the format promises: `range` is the whole construct, so
/// "select symbol" takes the block.
/// `range`, widened to hold `selection`.
///
/// The invariant every document symbol has to satisfy, applied where a
/// construct
/// has no block to match braces around: a port, a field, a bound step. Their
/// range
/// and selection are usually the same token, and this is what makes "usually"
/// into
/// "always".
Range Widened(const LineIndex& lines, const Range& range,
              const Range& selection) {
  return lines.Between(std::min(range.start.offset, selection.start.offset),
                       std::max(range.end.offset, selection.end.offset));
}

Range ConstructRange(const LineIndex& lines, absl::Span<const Token> tokens,
                     const syntax::Location& opened, const Range& selection) {
  size_t end = opened.end;
  size_t at = 0;
  while (at < tokens.size() && tokens[at].start < opened.start) {
    ++at;
  }
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
  // A declaration somebody is part-way through typing has no closing brace, and
  // a range that stopped short of its own name would be the violation this
  // exists to prevent.
  end = std::max(end, selection.end.offset);
  return lines.Between(std::min(opened.start, selection.start.offset), end);
}

std::vector<DocumentSymbol> Symbols(std::string_view source,
                                    CompilationLimits limits,
                                    CompilationBudget* budget) {
  CompilationBudget local_budget(limits);
  if (budget == nullptr) {
    budget = &local_budget;
  }
  const LineIndex lines(source);
  const auto lexed = Lex(
      source,
      LexOptions{.keep_comments = true, .limits = limits, .budget = budget});
  const auto parsed =
      ParseTokens(source, lexed.tokens, lexed.diagnostics, limits, budget);
  const ResolveResult resolved =
      Resolve(source, parsed, false, nullptr, limits, budget);
  const internal::EditorDocumentation documentation(source, parsed, limits,
                                                    budget);
  std::vector<DocumentSymbol> found;

  // The shapes first, as they are written: a file that declares one usually
  // opens with it, and the outline should read like the file.
  for (const syntax::DtoDeclarationPtr& declaration : parsed.dtos) {
    DocumentSymbol shape;
    shape.name = declaration->name.text;
    shape.kind = SymbolClass::kDto;
    shape.detail =
        documentation.Get(declaration->description, declaration->location);
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
      one.range =
          Widened(lines, RangeOf(lines, field->location), one.selection);
      shape.children.push_back(std::move(one));
    }
    found.push_back(std::move(shape));
  }

  for (const ResolvedFlow& flow : resolved.flows) {
    if (flow.declaration == nullptr) {
      continue;
    }
    DocumentSymbol one;
    one.name = flow.plan.name;
    one.kind = SymbolClass::kFlow;
    one.detail = documentation.Get(flow.plan.description, flow.plan.location);
    one.selection = RangeOf(lines, flow.declaration->name.location);
    one.range = ConstructRange(lines, lexed.tokens, flow.declaration->location,
                               one.selection);
    for (const PortPlan& port : flow.plan.ports) {
      one.children.push_back(PortSymbol(lines, port));
    }
    for (const auto& declaration : flow.declaration->declarations) {
      DocumentSymbol child;
      child.name = declaration->name.text;
      child.kind = SymbolClass::kFlow;
      child.detail =
          documentation.Get(declaration->description, declaration->location);
      child.selection = RangeOf(lines, declaration->name.location);
      child.range = RangeOf(lines, declaration->extent);
      for (const auto& port : declaration->ports) {
        PortPlan plan;
        plan.name = port->name.text;
        plan.direction = port->direction;
        plan.declared = port->type.ToString();
        plan.unary = port->unary;
        plan.required = port->required;
        plan.location = port->name.location;
        child.children.push_back(PortSymbol(lines, plan));
      }
      one.children.push_back(std::move(child));
    }
    for (const auto& shape : flow.declaration->structs) {
      DocumentSymbol child;
      child.name = shape->name.text;
      child.kind = SymbolClass::kDto;
      child.detail = documentation.Get(shape->description, shape->location);
      child.selection = RangeOf(lines, shape->name.location);
      child.range = RangeOf(lines, shape->extent);
      for (const auto& field : shape->fields) {
        DocumentSymbol member;
        member.name = field->name.text;
        member.kind = SymbolClass::kField;
        member.detail = field->type.ToString();
        member.selection = RangeOf(lines, field->name.location);
        member.range =
            Widened(lines, RangeOf(lines, field->location), member.selection);
        child.children.push_back(std::move(member));
      }
      one.children.push_back(std::move(child));
    }
    // Everything the flow bound that a reader might want to jump to. The
    // implicit ones are the language's, not the author's, and nobody navigates
    // to `index`.
    for (const Symbol& symbol : flow.symbols) {
      if (symbol.implicit) {
        continue;
      }
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
  std::vector<const syntax::ModuleDeclaration*> modules;
  for (const auto& module : parsed.modules) {
    const auto text = source.substr(module.extent.start,
                                    module.extent.end - module.extent.start);
    if (text.starts_with("mod") || text.starts_with("MOD")) {
      modules.push_back(&module);
    }
  }
  std::sort(modules.begin(), modules.end(),
            [](const auto* left, const auto* right) {
              return left->extent.end - left->extent.start <
                     right->extent.end - right->extent.start;
            });
  for (const auto* module : modules) {
    DocumentSymbol symbol;
    symbol.name = module->name.text;
    symbol.kind = SymbolClass::kModule;
    symbol.detail = module->interface_only ? "declared module" : "module";
    symbol.range = RangeOf(lines, module->extent);
    symbol.selection = RangeOf(lines, module->name.location);
    std::vector<DocumentSymbol> remaining;
    for (auto& child : found) {
      if (child.range.start.offset >= module->extent.start &&
          child.range.end.offset <= module->extent.end) {
        const auto prefix = absl::StrCat(module->name.text, "::");
        if (child.name.starts_with(prefix)) {
          child.name.erase(0, prefix.size());
        }
        symbol.children.push_back(std::move(child));
      } else {
        remaining.push_back(std::move(child));
      }
    }
    found = std::move(remaining);
    found.push_back(std::move(symbol));
  }
  std::sort(found.begin(), found.end(),
            [](const auto& left, const auto& right) {
              return left.range.start.offset < right.range.start.offset;
            });
  return found;
}

Description Describe(std::string_view source, size_t offset,
                     const catalogue::Catalogue& known,
                     CompilationLimits limits, CompilationBudget* budget,
                     const LinkedModule* linked) {
  CompilationBudget local_budget(limits);
  if (budget == nullptr) {
    budget = &local_budget;
  }
  Description about;
  const LineIndex lines(source);
  const LexResult lexed = Lex(
      source,
      LexOptions{.keep_comments = true, .limits = limits, .budget = budget});
  const std::vector<SemanticToken> semantic = Highlight(lexed.tokens);
  const SemanticToken* here = TokenAt(semantic, offset);
  if (here == nullptr) {
    return about;
  }
  // A line break is a token -- it is what ends a statement -- but it is not
  // something anybody hovers, and "`\n` -- punctuation" is a worse answer than
  // no answer.
  if (source.substr(here->start, here->end - here->start)
          .find_first_not_of(" \t\r\n") == std::string_view::npos) {
    return about;
  }

  size_t index = 0;
  for (size_t at = 0; at < semantic.size(); ++at) {
    if (&semantic[at] == here) {
      index = at;
    }
  }
  about.found = true;
  about.text = std::string(source.substr(here->start, here->end - here->start));
  about.range = lines.Between(here->start, here->end);

  LinkedModule owned_linked;
  if (linked == nullptr) {
    owned_linked =
        LinkModules(std::string(source), "<editor>", {}, {}, limits, budget);
    linked = &owned_linked;
  }
  const auto& parsed = linked->parsed;
  if (index < lexed.tokens.size() && IsDelimiter(lexed.tokens[index].kind)) {
    const size_t opening = OpeningDelimiter(lexed.tokens, index);
    const bool is_closing =
        lexed.tokens[index].kind == TokenKind::kRightBrace ||
        lexed.tokens[index].kind == TokenKind::kRightParen;
    if (!is_closing || opening != index) {
      const bool brace = lexed.tokens[opening].kind == TokenKind::kLeftBrace;
      const DelimiterContext context =
          brace ? BraceContext(lexed.tokens, opening, parsed)
                : ParenthesisContext(lexed.tokens, opening);
      const DelimiterDoc doc = DocumentationFor(context);
      const std::string label =
          absl::StrCat(is_closing ? "closing " : "opening ", doc.kind,
                       brace ? " brace" : " parenthesis");
      const std::string_view summary = is_closing ? doc.closes : doc.opens;
      about.kind = SymbolClass::kExternal;
      about.summary =
          absl::StrCat("`", about.text, "` — ", label, ": ", summary);
      about.detail = std::string(doc.detail);
      about.markdown = DocMarkdown(
          about.text, label,
          vocabulary::WordDoc{summary, "", doc.detail, doc.example});
      return about;
    }
  }
  const internal::EditorDocumentation documentation(linked->source, parsed,
                                                    limits, budget);
  auto local =
      documentation.CatalogueAt(*linked, offset, false, limits, budget);
  if (linked->SelectingImport(offset)) {
    local = local.MergedWith(
        documentation.CatalogueAt(*linked, offset, true, limits, budget));
  }
  const auto editor_known = known.MergedWith(local);
  const auto evidence = editor_known.SchemaProgram();
  const ResolveResult resolved =
      Resolve(linked->source, parsed, false, &evidence, limits, budget);
  const std::string word = vocabulary::Canonical(about.text);

  if (index < lexed.tokens.size() && lexed.tokens[index].IsWord()) {
    const auto [qualified, first] = DottedAround(lexed.tokens, index);
    size_t last = first;
    while (last + 2 < lexed.tokens.size() &&
           lexed.tokens[last + 1].kind == TokenKind::kScope &&
           lexed.tokens[last + 2].IsWord()) {
      last += 2;
    }
    std::string canonical;
    if (qualified.starts_with("std::")) {
      canonical = qualified.substr(5);
    } else if (const auto alias = editor_known.intrinsics().find(qualified);
               alias != editor_known.intrinsics().end()) {
      canonical = alias->second;
    }
    if (!canonical.empty() && last + 1 < lexed.tokens.size() &&
        lexed.tokens[last + 1].kind == TokenKind::kLeftParen &&
        (vocabulary::Builtins().contains(canonical) ||
         vocabulary::TypeNames().contains(canonical) ||
         vocabulary::Documentation(vocabulary::WordRole::kSource, canonical) !=
             nullptr)) {
      about.text = qualified;
      about.range =
          lines.Between(lexed.tokens[first].start, lexed.tokens[last].end);
      about.kind = SymbolClass::kExternal;
      about.markdown =
          vocabulary::Documentation(vocabulary::WordRole::kSource, canonical) !=
                  nullptr
              ? WordMarkdown(canonical, vocabulary::WordRole::kSource)
              : BuiltinMarkdown(canonical);
      about.summary = about.markdown.substr(0, about.markdown.find('\n'));
      return about;
    }
  }

  // Code examples need the same hover as a complete flow, even when the
  // fragment has no enclosing declaration for the parser to visit.
  size_t token_index = 0;
  while (token_index < lexed.tokens.size() &&
         lexed.tokens[token_index].start != here->start) {
    ++token_index;
  }
  for (size_t start = token_index > 2 ? token_index - 2 : 0;
       start <= token_index && start + 1 < lexed.tokens.size(); ++start) {
    if (!lexed.tokens[start].IsWord() ||
        vocabulary::Canonical(lexed.tokens[start].text) != "wait") {
      continue;
    }
    const auto* keyword = TokenAt(semantic, lexed.tokens[start].start);
    if (keyword == nullptr ||
        keyword->kind != SemanticKind::kStatementKeyword ||
        !lexed.tokens[start + 1].IsWord()) {
      continue;
    }
    const std::string next =
        vocabulary::Canonical(lexed.tokens[start + 1].text);
    std::string_view phrase;
    size_t last = start + 1;
    const bool has_of =
        start + 2 < lexed.tokens.size() && lexed.tokens[start + 2].IsWord() &&
        vocabulary::Canonical(lexed.tokens[start + 2].text) == "of";
    if (next == "first" && has_of) {
      phrase = "wait first of";
      last = start + 2;
    } else if (next == "all") {
      phrase = has_of ? "wait all of" : "wait all";
      last += has_of;
    }
    if (phrase.empty() || token_index > last) {
      continue;
    }
    if (const auto* doc =
            vocabulary::Documentation(vocabulary::WordRole::kStatement, phrase);
        doc != nullptr) {
      about.text = phrase;
      about.range =
          lines.Between(lexed.tokens[start].start, lexed.tokens[last].end);
      about.kind = SymbolClass::kExternal;
      about.summary =
          absl::StrCat("`", phrase, "` — a statement: ", doc->summary);
      about.detail = doc->detail;
      about.markdown = DocMarkdown(phrase, "statement", *doc);
      return about;
    }
  }

  const syntax::FieldDeclaration* field_here = nullptr;
  const auto find_field = [&](const syntax::Node& node) {
    const auto* field = syntax::As<syntax::FieldDeclaration>(&node);
    if (field != nullptr && field->name.location.start <= here->start &&
        here->end <= field->name.location.end) {
      field_here = field;
    }
  };
  for (const auto& dto : parsed.dtos) {
    syntax::VisitSubtree(*dto, find_field);
  }
  for (const auto& flow : parsed.flows) {
    syntax::VisitSubtree(*flow, find_field);
  }
  if (field_here != nullptr) {
    about.kind = SymbolClass::kField;
    about.summary = absl::StrCat("`", field_here->name.text,
                                 "` — field: ", field_here->type.ToString());
    about.detail =
        documentation.Get(field_here->description, field_here->location);
    about.markdown = about.summary + "\n\n" + about.detail;
    about.has_definition = true;
    about.definition = RangeOf(lines, field_here->name.location);
    return about;
  }

  for (const auto& flow : parsed.flows) {
    syntax::VisitSubtree(*flow, [&](const syntax::Node& node) {
      const auto* call = syntax::As<syntax::CallExpression>(&node);
      if (call == nullptr || about.kind == SymbolClass::kPort) {
        return;
      }
      for (const auto& argument : call->args) {
        if (argument.port.location.start > here->start ||
            here->end > argument.port.location.end) {
          continue;
        }
        about.kind = SymbolClass::kPort;
        std::string type;
        bool required = false;
        bool unary = true;
        if (const auto* action = editor_known.Action(call->action);
            action != nullptr) {
          for (const auto& port : action->inputs) {
            if (port.name == argument.port.text) {
              type = port.type;
              about.detail = port.description;
              required = port.required;
              unary = port.unary;
              break;
            }
          }
          about.origin = action->origin;
        } else if (const auto* target = resolved.program.Flow(call->action);
                   target != nullptr) {
          for (const auto& port : target->ports) {
            if (port.name == argument.port.text &&
                port.direction == syntax::PortDirection::kInput) {
              type = port.declared.empty() ? port.type : port.declared;
              about.detail = documentation.Get(port.description, port.location);
              required = port.required;
              unary = port.unary;
              about.has_definition = true;
              about.definition = RangeOf(lines, port.location);
              break;
            }
          }
        }
        about.summary =
            absl::StrCat("`", argument.port.text, "` — input-port of `",
                         call->action, "`", type.empty() ? "" : ": ", type);
        about.markdown = PortMarkdown(argument.port.text, type, required, unary,
                                      about.detail);
        return;
      }
    });
    if (about.kind == SymbolClass::kPort) {
      return about;
    }
  }

  size_t after_name = here->end;
  while (after_name < source.size() &&
         (source[after_name] == ' ' || source[after_name] == '\t' ||
          source[after_name] == '\r' || source[after_name] == '\n')) {
    ++after_name;
  }
  if (after_name < source.size() && source[after_name] == ':') {
    const syntax::ObjectLiteral* object_here = nullptr;
    for (const auto& flow : parsed.flows) {
      syntax::VisitSubtree(*flow, [&](const syntax::Node& node) {
        const auto* object = syntax::As<syntax::ObjectLiteral>(&node);
        if (object == nullptr || object->location.start > here->start ||
            here->end > object->extent.end) {
          return;
        }
        if (object_here == nullptr ||
            object->extent.end - object->location.start <
                object_here->extent.end - object_here->location.start) {
          object_here = object;
        }
      });
    }
    if (object_here != nullptr) {
      about.kind = SymbolClass::kField;
      std::string name = about.text;
      if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
        name = name.substr(1, name.size() - 2);
      }
      about.text = name;
      const DtoPlan* shape = nullptr;
      for (const auto& flow : parsed.flows) {
        syntax::VisitSubtree(*flow, [&](const syntax::Node& node) {
          const auto* typed = syntax::As<syntax::TypedValue>(&node);
          if (typed == nullptr || typed->value.get() != object_here) {
            return;
          }
          shape = resolved.program.Dto(typed->type.name);
          if (shape == nullptr) {
            if (const auto* type = editor_known.Type(typed->type.name);
                type != nullptr) {
              shape = &type->shape;
              about.origin = type->origin;
            }
          }
        });
      }
      std::string type;
      if (shape != nullptr) {
        if (const auto* field = shape->Field(name); field != nullptr) {
          type = field->declared.empty() ? field->type : field->declared;
          about.detail = documentation.Get(field->description, field->location);
        }
      }
      about.summary = absl::StrCat("`", name, "` — property",
                                   type.empty() ? "" : ": ", type);
      about.markdown = about.detail.empty()
                           ? about.summary
                           : absl::StrCat(about.summary, "\n\n", about.detail);
      return about;
    }
  }

  std::string pipeline_note;
  for (const auto& flow : parsed.flows) {
    syntax::VisitSubtree(*flow, [&](const syntax::Node& node) {
      const auto* stage = syntax::As<syntax::Stage>(&node);
      if (stage == nullptr || (stage->name != "run" && stage->name != "call")) {
        return;
      }
      const auto* call =
          syntax::As<syntax::CallExpression>(stage->argument.get());
      if (call == nullptr || offset < call->action_location.start ||
          offset >= call->action_location.end) {
        return;
      }
      const auto* target = resolved.program.Flow(call->action);
      if (target == nullptr) {
        return;
      }
      std::vector<std::string> supplied;
      for (const auto& argument : call->args) {
        supplied.push_back(argument.port.text);
      }
      const auto ports = SelectPipelinePorts(*target, supplied);
      if (!ports.ok()) {
        return;
      }
      pipeline_note =
          absl::StrCat("\n\n**Whole-stream invocation** (", call->mode, "): `",
                       ports->input->name, "` → `", ports->output->name,
                       "`. One invocation per pipeline activation; ordinary "
                       "action failure and cleanup semantics apply.");
    });
  }

  const std::vector<size_t> starts = DeclarationStarts(parsed);

  for (const auto& flow : resolved.flows) {
    for (const auto& [node, info] : flow.expressions) {
      const auto* attr = syntax::As<syntax::Attr>(node);
      if (attr == nullptr || attr->name != about.text ||
          here->start <= attr->location.start || here->end > attr->extent.end) {
        continue;
      }
      const auto between = source.substr(
          attr->location.start + 1, here->start - attr->location.start - 1);
      if (between.find_first_not_of(" \t") != std::string_view::npos) {
        continue;
      }
      const auto base = flow.expressions.find(attr->base.get());
      if (base == flow.expressions.end()) {
        continue;
      }
      if (base->second.category == ExpressionCategory::kCall &&
          attr->name != "status") {
        about.kind = SymbolClass::kPort;
        about.summary = absl::StrCat("`", attr->name, "` — port of `",
                                     base->second.action, "`: ", info.type);
        about.detail = info.description;
        about.markdown = PortMarkdown(attr->name, info.type, false, info.unary,
                                      info.description);
        const syntax::FlowDeclaration* target = nullptr;
        for (const auto& declaration : parsed.flows) {
          if (declaration->name.text == base->second.action &&
              (target == nullptr || !declaration->interface_only)) {
            target = declaration.get();
          }
        }
        if (target != nullptr) {
          for (const auto& port : target->ports) {
            if (port->name.text == attr->name) {
              about.detail =
                  documentation.Get(info.description, port->location);
              about.markdown = PortMarkdown(attr->name, info.type, false,
                                            info.unary, about.detail);
              about.has_definition = true;
              about.definition = RangeOf(lines, port->name.location);
            }
          }
        }
        if (!about.has_definition) {
          if (const auto* action = editor_known.Action(base->second.action);
              action != nullptr) {
            about.origin = action->origin;
          }
        }
        return about;
      }
      if (base->second.shape != nullptr) {
        if (const auto* field = base->second.shape->Field(attr->name);
            field != nullptr) {
          about.kind = SymbolClass::kField;
          about.summary =
              absl::StrCat("`", attr->name, "` — field: ", info.type);
          about.detail = documentation.Get(field->description, field->location);
          about.markdown = about.summary + "\n\n" + about.detail;
          if (NameOfShape(parsed, base->second.type) != nullptr &&
              field->location.end > field->location.start) {
            about.has_definition = true;
            about.definition = RangeOf(lines, field->location);
          } else if (const auto* type = editor_known.Type(base->second.type);
                     type != nullptr) {
            about.origin = type->origin;
          }
          return about;
        }
      }
    }
  }

  // A name the document bound.
  for (const ResolvedFlow& flow : resolved.flows) {
    if (flow.declaration == nullptr) {
      continue;
    }
    if (!Within(starts, flow.declaration->location.start, here->start,
                source.size())) {
      continue;
    }
    for (const Symbol& symbol : flow.symbols) {
      if (symbol.name != about.text || symbol.implicit ||
          (!symbol.VisibleAt(here->start) &&
           !(here->start >= symbol.location.start &&
             here->start < symbol.location.end))) {
        continue;
      }
      about.kind = ClassOf(symbol.kind);
      about.summary =
          absl::StrCat("`", about.text, "` — ", SymbolKindName(symbol.kind),
                       " of `", flow.plan.name, "`");
      about.has_definition = true;
      about.definition = RangeOf(lines, symbol.location);
      if (symbol.kind == SymbolKind::kBarrier) {
        bool race = false;
        bool closing = false;
        syntax::VisitSubtree(*flow.declaration, [&](const syntax::Node& node) {
          const auto* bind = syntax::As<syntax::Bind>(&node);
          if (bind == nullptr ||
              bind->name.location.start != symbol.location.start) {
            return;
          }
          if (const auto* wait = syntax::As<syntax::Wait>(bind->value.get());
              wait != nullptr) {
            race = wait->race;
          }
          closing = syntax::As<syntax::Close>(bind->value.get()) != nullptr;
        });
        if (race) {
          about.summary = absl::StrCat("`", about.text, "` — winner index");
          about.detail =
              "A `wait first of` completes when one named subject finishes. "
              "This value is that subject's zero-based position in the list; "
              "the remaining subjects continue running.";
        } else {
          about.summary = absl::StrCat("`", about.text, "` — status");
          about.detail =
              closing
                  ? "A bound `close` waits for the node's writers and buffered "
                    "values, then exposes their completion status."
                  : "A bound `wait` is a completion barrier. It waits for the "
                    "named subject, then exposes that subject's status. Read "
                    "the subject's output ports separately for payload values.";
        }
      }
      if (symbol.kind == SymbolKind::kCall) {
        absl::StrAppend(&about.summary, ", running `", symbol.action, "`");
      }
      if (symbol.kind == SymbolKind::kValue && symbol.value_type.has_value()) {
        absl::StrAppend(&about.summary, ": ", symbol.value_type->ToString());
      }
      if (symbol.kind == SymbolKind::kSink) {
        about.detail =
            "Appending waits for this operation, then independent writers "
            "interleave. Completion waits for the operation and all attached "
            "writers. Aliases share the same sink; it cannot carry payload.";
      }
      if (const PortPlan* port =
              flow.plan.Port(symbol.name, symbol.kind == SymbolKind::kInputPort
                                              ? syntax::PortDirection::kInput
                                              : syntax::PortDirection::kOutput);
          port != nullptr) {
        about.detail = documentation.Get(port->description, port->location);
        absl::StrAppend(&about.summary, ": ",
                        port->declared.empty() ? port->type : port->declared);
      }
      break;
    }
    if (about.has_definition) {
      break;
    }
  }

  // File-declared and host-provided shapes share the same representation.
  if (!about.has_definition && index < lexed.tokens.size() &&
      lexed.tokens[index].IsWord()) {
    const auto [dotted, first] = DottedAround(lexed.tokens, index);
    const DtoPlan* shape = resolved.program.Dto(dotted);
    std::optional<catalogue::Origin> from_host;
    if (NameOfShape(parsed, dotted) == nullptr) {
      if (const auto* type = editor_known.Type(dotted); type != nullptr) {
        from_host = type->origin;
      }
    }
    if (shape == nullptr) {
      if (const catalogue::TypeInfo* type = editor_known.Type(dotted);
          type != nullptr) {
        shape = &type->shape;
        from_host = type->origin;
      }
    }
    if (shape != nullptr) {
      about.text = dotted;
      about.range = lines.Between(lexed.tokens[first].start, here->end);
      about.kind = SymbolClass::kDto;
      about.summary =
          absl::StrCat("`", dotted, "` — a struct of ", shape->fields.size(),
                       " field", shape->fields.size() == 1 ? "" : "s");
      about.detail = documentation.Get(shape->description, shape->location);
      about.markdown = ShapeMarkdown(documentation.Shape(*shape));
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
  if (!about.has_definition && index < lexed.tokens.size() &&
      lexed.tokens[index].IsWord()) {
    const auto [dotted, first] = DottedAround(lexed.tokens, index);
    if (editor_known.Action(dotted) != nullptr ||
        resolved.program.Flow(dotted) != nullptr) {
      about.text = dotted;
      about.range = lines.Between(lexed.tokens[first].start,
                                  lexed.tokens[first].start + dotted.size());
    }
    if (const FlowPlan* sibling = resolved.program.Flow(about.text);
        sibling != nullptr && NameOfFlow(parsed, about.text) != nullptr) {
      about.kind = SymbolClass::kFlow;
      about.summary = absl::StrCat("`", about.text, "` — a flow of this file");
      about.detail = documentation.Get(sibling->description, sibling->location);
      if (const syntax::Word* name = NameOfFlow(parsed, about.text);
          name != nullptr) {
        about.has_definition = true;
        about.definition = RangeOf(lines, name->location);
      }
      if (!about.has_definition) {
        if (const auto* action = editor_known.Action(about.text);
            action != nullptr) {
          about.origin = action->origin;
        }
      }
      about.markdown =
          FlowMarkdown(documentation.Flow(*sibling)) + pipeline_note;
      size_t count = 0;
      for (const auto& declaration : parsed.flows) {
        if (declaration->name.text == sibling->name) {
          ++count;
        }
      }
      if (count > 1) {
        absl::StrAppend(&about.markdown,
                        "\n**Effective schema contributors**\n\n");
        for (const auto& declaration : parsed.flows) {
          if (declaration->name.text == sibling->name) {
            absl::StrAppend(&about.markdown, "- Line ",
                            declaration->name.location.line,
                            declaration->interface_only ? " (declaration)\n"
                                                        : " (definition)\n");
          }
        }
      }
      return about;
    }
    if (const catalogue::ActionInfo* action = editor_known.Action(about.text);
        action != nullptr) {
      about.kind = SymbolClass::kExternal;
      about.summary = absl::StrCat("`", about.text, "` — an action");
      about.detail = action->description;
      about.markdown = ActionMarkdown(*action) + pipeline_note;
      // An action declared in a file something read has somewhere to go, which
      // is new: before a scan there was nothing to point at and hovering one of
      // these said "action name".
      about.origin = action->origin;
      if (about.origin.has_value() && !linked->documents.empty() &&
          about.origin->file == linked->documents.front().identity) {
        about.kind = SymbolClass::kFlow;
        for (const auto& member : linked->members) {
          if (member.name == about.text &&
              member.source == about.origin->file &&
              member.scope_start <= offset && offset <= member.scope_end) {
            about.has_definition = true;
            about.definition = RangeOf(lines, member.location);
            about.origin.reset();
            break;
          }
        }
      }
      return about;
    }
  }

  if (here->kind == SemanticKind::kStage && index < lexed.tokens.size()) {
    for (const int shift : {-1, 0}) {
      if (shift < 0 && index == 0) {
        continue;
      }
      const size_t first = shift < 0 ? index - 1 : index;
      if (first + 1 >= lexed.tokens.size() || !lexed.tokens[first].IsWord() ||
          !lexed.tokens[first + 1].IsWord()) {
        continue;
      }
      const auto phrase =
          absl::StrCat(vocabulary::Canonical(lexed.tokens[first].text), " ",
                       vocabulary::Canonical(lexed.tokens[first + 1].text));
      if (const auto* doc = vocabulary::StageDocumentation(phrase);
          doc != nullptr) {
        about.text = phrase;
        about.range = lines.Between(lexed.tokens[first].start,
                                    lexed.tokens[first + 1].end);
        about.kind = SymbolClass::kExternal;
        about.summary =
            absl::StrCat("`", phrase, "` — a stage: ", doc->summary);
        about.detail = doc->detail;
        about.markdown = DocMarkdown(phrase, "stage", *doc);
        return about;
      }
    }
  }

  // A word or a mark of the language, with the language's own reference for it.
  if (about.summary.empty()) {
    for (const vocabulary::WordRole role : RolesFor(here->kind)) {
      const std::string key = role == vocabulary::WordRole::kSymbol ? about.text
                              : here->kind == SemanticKind::kDuration
                                  ? std::string(DurationUnitOf(about.text))
                                  : word;
      const vocabulary::WordDoc* doc = vocabulary::Documentation(role, key);
      if (doc == nullptr) {
        continue;
      }
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
