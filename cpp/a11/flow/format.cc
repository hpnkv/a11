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

#include "a11/flow/format.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <nlohmann/json.hpp>

#include "a11/flow/diagnostic.h"
#include "a11/flow/emit_json.h"
#include "a11/flow/lexer.h"
#include "a11/flow/parser.h"
#include "a11/flow/token.h"
#include "a11/flow/vocabulary.h"
#include "a11/utf8.h"

namespace a11::flow {
namespace {

size_t Columns(std::string_view text);

/// A declaration, split into the columns a run of them lines up by.
///
/// Ports, headers and a shape's fields alike: `in`/`out name: type mods "why"`,
/// `header "x-name" as alias default v "why"` and `name: type mods "why"` are
/// the same four columns wearing different words, and a file reads best when
/// each run of them lines up.
struct PortColumns {
  /// `in`, `out` or `header`, as written -- a shouted `IN` stays shouted. Empty
  /// for a field, which is the one of the three that opens with its name.
  std::string direction;
  /// The name and its colon, which travel together because they always do.
  std::string name;
  /// The type and what follows it: `string required`, `list[a11.Chunk] stream`.
  std::string type;
  /// The description, quoted exactly as it was written.
  std::string description;
  /// Whether this is a `struct` field. Told apart from a port by more than its
  /// empty direction, because a run has to be all one kind and "is the
  /// direction
  /// empty" is a fact about this line rather than about what it is.
  bool field = false;
};

/// One line of output, before the columns of a run of ports are worked out.
struct OutputLine {
  int indent = 0;
  std::string text;
  /// A comment that followed code on the same line.
  std::string comment;
  std::string description_tail;
  std::optional<PortColumns> port;
  /// Whether one blank line goes above this one.
  bool blank_before = false;
  /// Whether this is a declaration's description, written on its own line.
  /// Such a line sits *inside* a run of declarations without being one of
  /// them, so the columns of the run are worked out across it.
  bool description = false;
};

bool IsOpener(TokenKind kind) {
  return kind == TokenKind::kLeftParen || kind == TokenKind::kLeftBracket ||
         kind == TokenKind::kLeftBrace;
}

bool IsCloser(TokenKind kind) {
  return kind == TokenKind::kRightParen || kind == TokenKind::kRightBracket ||
         kind == TokenKind::kRightBrace;
}

TokenKind Closes(TokenKind opener) {
  switch (opener) {
    case TokenKind::kLeftParen:
      return TokenKind::kRightParen;
    case TokenKind::kLeftBracket:
      return TokenKind::kRightBracket;
    default:
      return TokenKind::kRightBrace;
  }
}

bool ExpectsOperand(const Token& token) {
  const auto word = token.IsWord() ? vocabulary::Canonical(token.text) : "";
  return token.kind == TokenKind::kColon || token.kind == TokenKind::kEqual ||
         token.kind == TokenKind::kEqualEqual ||
         token.kind == TokenKind::kBangEqual ||
         token.kind == TokenKind::kLess ||
         token.kind == TokenKind::kLessEqual ||
         token.kind == TokenKind::kGreater ||
         token.kind == TokenKind::kGreaterEqual ||
         token.kind == TokenKind::kPlus || token.kind == TokenKind::kMinus ||
         word == "and" || word == "or" || word == "in";
}

/// Whether `word(` reads as something being called.
///
/// One decision, and it is about the space: `web-fetch(`, `len(` and `node()`
/// hug
/// their parentheses because the word and the brackets are one thing, while
/// `not (`, `if (` and `in (` are a word and then a bracketed expression. So
/// every
/// word the grammar gives meaning to takes its space -- except `node`, which is
/// the one keyword in the language that is *constructed*.
bool IsCalled(std::string_view text) {
  const std::string word = vocabulary::Canonical(text);
  // `node()` is the one keyword that is constructed, and several builtins are
  // spelled like declaration words -- `default`, `text`, `number`, `join`.
  if (word == "take") {
    return false;
  }
  if (word == "node") {
    return true;
  }
  if (vocabulary::Builtins().contains(word)) {
    return true;
  }
  return !(vocabulary::StatementWords().contains(word) ||
           vocabulary::DeclarationWords().contains(word) ||
           vocabulary::ModifierWords().contains(word) ||
           vocabulary::OperatorWords().contains(word) ||
           vocabulary::ConstantWords().contains(word));
}

/// The token stream re-emitted with the whitespace the style says.
class Formatter {
 public:
  Formatter(std::string_view source, const std::vector<Token>& tokens,
            const ParseResult& parsed, const FormatOptions& options,
            bool layout_groups = false)
      : source_(source),
        options_(options),
        value_braces_(parsed.value_braces.begin(), parsed.value_braces.end()),
        tagged_braces_(parsed.tagged_braces.begin(),
                       parsed.tagged_braces.end()) {
    const auto body = [&](const auto& statements) {
      for (const auto& statement : statements) {
        statement_starts_.insert(statement->extent.start);
      }
    };
    const auto imports = [&](const auto& declarations) {
      for (const auto& declaration : declarations) {
        statement_starts_.insert(declaration.extent.start);
      }
    };
    imports(parsed.imports);
    imports(parsed.exports);
    const auto opening_brace = [&](const syntax::Location& extent) {
      const auto begin = std::lower_bound(
          tokens.begin(), tokens.end(), extent.start,
          [](const Token& token, size_t start) { return token.start < start; });
      return std::find_if(begin, tokens.end(), [&](const Token& token) {
        return token.kind == TokenKind::kLeftBrace || token.start >= extent.end;
      });
    };
    const auto visit = [&](const syntax::Node& node) {
      for (const auto& group : node.groups) {
        expression_groups_.insert(group.start);
      }
      if (node.kind == syntax::NodeKind::kUntil) {
        loop_conditions_.insert(node.location.start);
      }
      if (syntax::IsAnyOf(&node, {syntax::NodeKind::kPortDeclaration,
                                  syntax::NodeKind::kHeaderDeclaration,
                                  syntax::NodeKind::kFieldDeclaration})) {
        statement_starts_.insert(node.extent.start);
      }
      if (const auto* flow = syntax::As<syntax::FlowDeclaration>(&node);
          flow != nullptr) {
        declaration_starts_.insert(node.extent.start);
        body(flow->body);
        imports(flow->imports);
      } else if (const auto* block = syntax::As<syntax::Block>(&node);
                 block != nullptr) {
        body(block->body);
      } else if (const auto* loop = syntax::As<syntax::ForEach>(&node);
                 loop != nullptr) {
        body(loop->body);
      } else if (const auto* repeat = syntax::As<syntax::Repeat>(&node);
                 repeat != nullptr) {
        body(repeat->body);
      } else if (const auto* branch = syntax::As<syntax::If>(&node);
                 branch != nullptr) {
        body(branch->then_body);
        body(branch->else_body);
      } else if (const auto* nodes = syntax::As<syntax::Nodes>(&node);
                 nodes != nullptr) {
        body(nodes->body);
      }
      if (node.kind == syntax::NodeKind::kDtoDeclaration) {
        declaration_starts_.insert(node.extent.start);
        const auto brace = opening_brace(node.extent);
        if (brace != tokens.end() && brace->start < node.extent.end) {
          struct_braces_.insert(brace->start);
        }
      }
    };
    for (const auto& flow : parsed.flows) {
      syntax::VisitSubtree(*flow, visit);
    }
    for (const auto& shape : parsed.dtos) {
      syntax::VisitSubtree(*shape, visit);
    }
    for (const auto& module : parsed.modules) {
      declaration_starts_.insert(module.extent.start);
      const auto brace = opening_brace(module.extent);
      if (brace != tokens.end() && brace->start < module.extent.end) {
        module_braces_.insert(brace->start);
      }
    }
    // Line breaks are not tokens here: what matters is how *many* there were
    // between two pieces of code, and the lexer folds a run of them into one.
    size_t previous_end = 0;
    for (const Token& token : tokens) {
      if (token.kind == TokenKind::kNewline ||
          token.kind == TokenKind::kSemicolon ||
          token.kind == TokenKind::kEnd) {
        continue;
      }
      int breaks = 0;
      bool separated = false;
      for (size_t at = previous_end; at < token.start; ++at) {
        if (source_[at] == '\n') {
          ++breaks;
        }
        separated |= source_[at] == ';';
      }
      code_.push_back(token);
      breaks_.push_back(std::max(breaks, separated ? 1 : 0));
      previous_end = token.end;
    }
    if (layout_groups) {
      LayoutGroups();
    } else {
      NormalizeWhitespace();
    }
  }

  std::string Run() {
    for (size_t index = 0; index < code_.size();) {
      index = Emit(index);
    }
    Flush();
    return Render();
  }

 private:
  // Statement boundaries come from the AST. Comment attachment and string
  // fragments retain their source breaks.
  void NormalizeWhitespace() {
    size_t depth = 0;
    for (size_t index = 0; index < code_.size(); ++index) {
      const auto& token = code_[index];
      const bool comment =
          token.kind == TokenKind::kComment ||
          (index > 0 && code_[index - 1].kind == TokenKind::kComment);
      const bool strings = token.kind == TokenKind::kString && index > 0 &&
                           code_[index - 1].kind == TokenKind::kString;
      if (!comment && !strings) {
        if (declaration_starts_.find(token.start) !=
            declaration_starts_.end()) {
          breaks_[index] = 2;
        } else if (statement_starts_.find(token.start) !=
                   statement_starts_.end()) {
          breaks_[index] = 1;
        } else {
          breaks_[index] = 0;
        }
        if (depth == 0 && token.IsWord() &&
            vocabulary::Canonical(token.text) == "describe" &&
            index + 1 < code_.size() &&
            code_[index + 1].kind == TokenKind::kString) {
          breaks_[index] = 1;
        }
        if (index > 0 && code_[index - 1].IsWord() && token.IsWord() &&
            vocabulary::Canonical(code_[index - 1].text) == "else" &&
            vocabulary::Canonical(token.text) == "if") {
          breaks_[index] = 0;
        }
      }
      if (IsOpener(token.kind) && !OpensBlock(token)) {
        ++depth;
      } else if (IsCloser(token.kind) && depth > 0) {
        --depth;
      }
    }
  }

  /// Whether this `{` opens a block rather than a value.
  [[nodiscard]] bool OpensBlock(const Token& token) const {
    return token.kind == TokenKind::kLeftBrace &&
           !value_braces_.contains(token.start);
  }

  /// Whether this `}` closes a block. Read off the bracket stack, which is what
  /// knows: a value brace was pushed when it opened.
  [[nodiscard]] bool ClosesBlock(const Token& token) const {
    if (token.kind != TokenKind::kRightBrace) {
      return false;
    }
    if (brackets_.empty()) {
      return true;
    }
    return brackets_.back().kind != TokenKind::kLeftBrace;
  }

  [[nodiscard]] int BlockIndent() const {
    return blocks_.empty() ? 0
                           : blocks_.back().indent +
                                 (blocks_.back().module ? 0 : options_.indent);
  }

  // Measure compact groups before expanding them. Comments and trailing commas
  // request expansion through their enclosing groups.
  void LayoutGroups() {
    struct Group {
      size_t close;
      size_t width;
      bool forced;
      bool expanded;
      bool vertical;
      bool hug;
      int indent;
    };

    std::vector<Group> groups(code_.size(),
                              Group{0, 0, false, false, false, false, 0});
    std::vector<size_t> stack;
    std::vector<size_t> widths{0};
    std::vector<size_t> hard{0};
    for (size_t index = 0; index < code_.size(); ++index) {
      const auto& token = code_[index];
      widths.push_back(widths.back() + Columns(token.text) +
                       (index > 0 && NeedsSpace(code_[index - 1], token)));
      hard.push_back(hard.back() + (token.kind == TokenKind::kComment ||
                                    absl::StrContains(token.text, '\n')));
      if (IsOpener(token.kind)) {
        stack.push_back(index);
      } else if (IsCloser(token.kind) && !stack.empty()) {
        const size_t open = stack.back();
        stack.pop_back();
        auto& group = groups[open];
        group.close = index;
        group.width =
            widths[index + 1] - widths[open + 1] + Columns(code_[open].text);
        group.forced =
            group.forced || hard[index + 1] != hard[open] ||
            (index > open + 1 && code_[index - 1].kind == TokenKind::kComma);
        if (!stack.empty() && group.forced) {
          groups[stack.back()].forced = true;
        }
      }
    }
    stack.clear();
    size_t column = 0;
    int indent = 0;
    std::optional<size_t> pipeline;
    for (size_t index = 0; index < code_.size(); ++index) {
      const auto& token = code_[index];
      if (!stack.empty() && !OpensBlock(code_[stack.back()])) {
        const size_t open = stack.back();
        const auto& parent = groups[open];
        const bool comment = token.kind == TokenKind::kComment ||
                             code_[index - 1].kind == TokenKind::kComment;
        const bool strings = parent.expanded &&
                             token.kind == TokenKind::kString &&
                             code_[index - 1].kind == TokenKind::kString;
        if (!comment && !strings) {
          const bool edge = index == open + 1 || index == parent.close;
          const bool comma = code_[index - 1].kind == TokenKind::kComma;
          if (!parent.expanded || edge || comma) {
            breaks_[index] = parent.expanded && ((edge && !parent.hug) ||
                                                 (comma && parent.vertical))
                                 ? 1
                                 : 0;
          }
        }
      }
      if (index == 0 || breaks_[index] > 0) {
        pipeline.reset();
        if (!stack.empty() && !OpensBlock(code_[stack.back()])) {
          const auto& parent = groups[stack.back()];
          indent =
              parent.indent + (index == parent.close ? 0 : options_.indent);
        } else {
          const size_t newline = source_.rfind('\n', token.start);
          const size_t line =
              newline == std::string_view::npos ? 0 : newline + 1;
          const size_t content = source_.find_first_not_of(' ', line);
          indent = static_cast<int>(content - line);
        }
        column = static_cast<size_t>(indent);
      } else if (index > 0 && NeedsSpace(code_[index - 1], token)) {
        ++column;
      }
      if (IsOpener(token.kind)) {
        auto& group = groups[index];
        group.indent = indent;
        group.expanded =
            !OpensBlock(token) && group.close > index + 1 &&
            ((!stack.empty() && groups[stack.back()].hug) || group.forced ||
             (options_.line_width != 0 &&
              column + group.width > options_.line_width));
        if (group.expanded && pipeline.has_value() &&
            (stack.empty() || OpensBlock(code_[stack.back()]))) {
          if (breaks_[*pipeline] == 0) {
            group.indent += options_.indent;
          }
          breaks_[*pipeline] = 1;
        }
        group.vertical = group.forced ||
                         (options_.line_width != 0 &&
                          static_cast<size_t>(group.indent + options_.indent) +
                                  group.width - 2 >
                              options_.line_width);
        if (group.expanded && index + 1 < code_.size()) {
          const auto& child = code_[index + 1];
          const bool literal =
              child.kind == TokenKind::kLeftBracket ||
              (child.kind == TokenKind::kLeftBrace &&
               value_braces_.find(child.start) != value_braces_.end());
          group.hug = literal && groups[index + 1].close + 1 == group.close;
        }
        stack.push_back(index);
      } else if (IsCloser(token.kind) && !stack.empty()) {
        stack.pop_back();
      }
      if (token.kind == TokenKind::kPipe &&
          (stack.empty() || OpensBlock(code_[stack.back()]))) {
        pipeline = index;
      }
      column += Columns(token.text);
    }
  }

  /// Whether the line starting at `index` continues the statement above it.
  ///
  /// The grammar's own rule, because it has to be: a line beginning with `|` or
  /// `->` is a continuation, a modifier word is one unless what follows makes
  /// it a
  /// statement of its own, and a bare `then`/`where` is one when it has an
  /// operand.
  [[nodiscard]] bool ContinuesStatement(size_t index) const {
    const Token& token = code_[index];
    const bool inside_group = !brackets_.empty();
    switch (token.kind) {
      case TokenKind::kPipe:
      case TokenKind::kArrow:
        return true;
      case TokenKind::kComma:
        return !inside_group;
      default:
        break;
    }
    // A description on a line of its own belongs to the declaration above it,
    // so it is indented under it like any other tail.
    if (!inside_group && IsDescriptionLine(index)) {
      return true;
    }
    // A line that begins where the one above left off mid-list continues it: a
    // comma at the end of the previous line says the statement is not over.
    if (!inside_group && index > 0 &&
        (code_[index - 1].kind == TokenKind::kComma ||
         ExpectsOperand(code_[index - 1]))) {
      return true;
    }
    // Inside a bracketed group a word starts an element, never a tail: `id:` is
    // a key of the object it is in, and the modifiers of a call are outside its
    // parentheses by definition.
    if (inside_group) {
      return false;
    }
    if (!token.IsWord()) {
      return false;
    }
    // A shape has no calls and no pipelines, so nothing in one is the tail of
    // the line above it.
    if (in_dto_) {
      return false;
    }
    const std::string word = vocabulary::Canonical(token.text);
    if (!vocabulary::ModifierWords().contains(word) &&
        !vocabulary::BareStages().contains(word) && word != "before" &&
        word != "parallel" && word != "unordered" && word != "into" &&
        word != "numbered" && word != "base") {
      return false;
    }
    if (index + 1 >= code_.size()) {
      return false;
    }
    switch (code_[index + 1].kind) {
      case TokenKind::kArrow:
      case TokenKind::kPipe:
      case TokenKind::kEqual:
      case TokenKind::kCarry:
        return false;
      default:
        return true;
    }
  }

  /// Whether the line at `index` is a lone string: a declaration's description,
  /// written under what it describes.
  ///
  /// The grammar's own test -- a string with nothing after it on its line -- so
  /// the formatter and the parser agree about it by construction.
  [[nodiscard]] bool IsDescriptionLine(size_t index) const {
    if (code_[index].kind != TokenKind::kString) {
      return false;
    }
    if (index + 1 >= code_.size()) {
      return true;
    }
    const Token& next = code_[index + 1];
    return next.EndsStatement() || next.kind == TokenKind::kComment ||
           breaks_[index + 1] > 0;
  }

  /// The tail of a list the line above began: the previous line ended in a `,`.
  ///
  /// Two levels for the same reason a modifier tail gets two: it is *part of*
  /// the line above rather than the next thing after it. `| scan 0 as total,`
  /// and its expression on the next line is one stage, and one level would put
  /// the expression exactly where the next `|` goes.
  [[nodiscard]] bool IsArgumentTail(size_t index) const {
    return index > 0 && code_[index - 1].kind == TokenKind::kComma;
  }

  /// A modifier tail gets two levels rather than one.
  ///
  /// `with "x": y` under a call is not the next step of a pipeline; it is part
  /// of
  /// the call above it, and indenting it further is what says so at a glance.
  [[nodiscard]] bool IsModifierTail(size_t index) const {
    const Token& token = code_[index];
    return token.IsWord() && vocabulary::ModifierWords().contains(
                                 vocabulary::Canonical(token.text));
  }

  /// Where the line starting at `index` goes, without deciding anything.
  [[nodiscard]] int IndentOf(size_t index) const {
    if (!brackets_.empty()) {
      const Bracket& bracket = brackets_.back();
      if (code_[index].kind == Closes(bracket.kind)) {
        return bracket.indent;
      }
      // A group's contents sit one level in from the line that opened it, and a
      // continuation of one of those lines sits one further -- so a pipeline
      // written down the page inside a call still reads as a pipeline.
      return bracket.indent +
             options_.indent * (ContinuesStatement(index) ? 2 : 1);
    }
    if (ContinuesStatement(index)) {
      return statement_indent_ +
             options_.indent *
                 (IsModifierTail(index) || IsArgumentTail(index) ? 2 : 1);
    }
    if (ClosesBlock(code_[index])) {
      return blocks_.empty() ? 0 : blocks_.back().indent;
    }
    return BlockIndent();
  }

  int IndentFor(size_t index) {
    const int indent = IndentOf(index);
    // A line that is not a continuation is the statement the next ones
    // continue.
    if (brackets_.empty() && !ContinuesStatement(index)) {
      statement_indent_ = BlockIndent();
    }
    return indent;
  }

  /// Where a comment line goes: with the line under it.
  ///
  /// A comment explains what follows, so it takes that line's indent --
  /// including
  /// when what follows is a continuation, which is how a note about one stage
  /// of a
  /// pipeline stays with the stage.
  [[nodiscard]] int IndentForComment(size_t index) const {
    if (!brackets_.empty()) {
      return brackets_.back().indent + options_.indent;
    }
    size_t next = index + 1;
    while (next < code_.size() && code_[next].kind == TokenKind::kComment) {
      ++next;
    }
    if (next >= code_.size()) {
      return BlockIndent();
    }
    return IndentOf(next);
  }

  /// Whether a space goes between two tokens on one line.
  [[nodiscard]] bool NeedsSpace(const Token& previous,
                                const Token& current) const {
    switch (previous.kind) {
      case TokenKind::kDot:
      case TokenKind::kScope:
      case TokenKind::kLeftParen:
      case TokenKind::kLeftBracket:
        return false;
      // `...it` and `1..200` are each one thing: nothing goes inside them.
      case TokenKind::kSpread:
      case TokenKind::kRange:
        return false;
      case TokenKind::kLeftBrace:
        // A value brace hugs its contents; a block brace never has any on its
        // line, so this only ever answers for a value.
        return false;
      default:
        break;
    }
    switch (current.kind) {
      case TokenKind::kComma:
      case TokenKind::kColon:
      case TokenKind::kScope:
      case TokenKind::kRightParen:
      case TokenKind::kRightBracket:
      case TokenKind::kDot:
      case TokenKind::kRange:
        return false;
      case TokenKind::kRightBrace:
        return false;
      case TokenKind::kLeftParen:
        // `name(` is a call; `not (` and `if (` are a word and an expression.
        return expression_groups_.find(current.start) !=
                   expression_groups_.end() ||
               !((previous.IsWord() && IsCalled(previous.text)) ||
                 previous.kind == TokenKind::kRightParen ||
                 previous.kind == TokenKind::kRightBracket);
      case TokenKind::kLeftBracket:
        // `list[` and `x[0]`; a list literal anywhere else takes its space --
        // including after `one of` and `default`, where the brackets hold a
        // value rather than a type's parameters.
        return !((previous.IsWord() &&
                  vocabulary::Canonical(previous.text) != "in" &&
                  !vocabulary::OperatorWords().contains(
                      vocabulary::Canonical(previous.text)) &&
                  !vocabulary::FieldModifierWords().contains(
                      vocabulary::Canonical(previous.text))) ||
                 previous.kind == TokenKind::kRightParen ||
                 previous.kind == TokenKind::kRightBracket);
      case TokenKind::kLeftBrace:
        // A block brace always takes its space: `flow research {`.
        if (OpensBlock(current)) {
          return true;
        }
        // `a11.sdk.Interaction{...}` hugs its tag, because the tag and the
        // braces
        // are one value. `| map {...}` is a stage and then a literal.
        return !tagged_braces_.contains(current.start);
      default:
        return true;
    }
  }

  void Append(const Token& token) {
    if (!line_.empty() && NeedsSpace(previous_, token)) {
      line_.push_back(' ');
    }
    line_.append(token.text);
    previous_ = token;
  }

  void Flush() {
    if (line_.empty() && !line_started_) {
      return;
    }
    OutputLine out;
    out.indent = indent_;
    out.text = std::move(line_);
    out.comment = std::move(comment_);
    out.port = std::move(port_);
    out.description = description_line_;
    description_line_ = false;
    // A line with nothing on it is not a line; a blank line is asked for by
    // name.
    if (!out.text.empty() || !out.comment.empty() || out.port.has_value()) {
      out.blank_before = blank_pending_;
      blank_pending_ = false;
      lines_.push_back(std::move(out));
    }
    line_.clear();
    comment_.clear();
    port_.reset();
    line_started_ = false;
  }

  void Start(int indent) {
    indent_ = indent;
    line_started_ = true;
  }

  /// One blank line, where one is wanted and allowed.
  void Blank() {
    if (lines_.empty()) {
      return;  // never at the top of the file
    }
    if (blank_pending_) {
      return;
    }
    if (opened_block_) {
      return;  // never straight after a `{`
    }
    blank_pending_ = true;
  }

  /// Whether `token` is the `else` that belongs on a closing brace's line.
  bool JoinsBlockTail(size_t index) const {
    const auto& token = code_[index];
    if (loop_conditions_.contains(token.start)) {
      return true;
    }
    return token.IsWord() && vocabulary::Canonical(token.text) == "else" &&
           (index + 1 == code_.size() ||
            code_[index + 1].kind != TokenKind::kEqual);
  }

  size_t Emit(size_t index) {
    const Token& token = code_[index];
    // A block brace ends its line whatever the author did one statement per
    // line is the language's own rule, and `{ a -> b }` is three of them.
    const bool joins_tail = after_block_close_ && JoinsBlockTail(index);
    const bool starts_line =
        index == 0 || (breaks_[index] > 0 && !joins_tail) ||
        after_block_open_ || (after_block_close_ && !joins_tail);

    if (ClosesBlock(token)) {
      last_port_direction_.clear();
      Flush();
      const int indent = blocks_.empty() ? 0 : blocks_.back().indent;
      if (!blocks_.empty()) {
        blocks_.pop_back();
      }
      in_dto_ = !blocks_.empty() && blocks_.back().dto;
      // A blank line before a `}` says nothing, so it is dropped rather than
      // indented: this is the one place a break the author wrote is thrown
      // away.
      blank_pending_ = false;
      Start(indent);
      Append(token);
      // Left open, so an `else` can join it.
      opened_block_ = false;
      after_block_open_ = false;
      after_block_close_ = true;
      return index + 1;
    }

    if (token.kind == TokenKind::kComment) {
      if (!starts_line && line_started_) {
        // A comment after code stays on its line, two spaces out.
        comment_ = std::string(absl::StripTrailingAsciiWhitespace(token.text));
        return index + 1;
      }
      Flush();
      if (breaks_[index] >= 2) {
        Blank();
      }
      Start(IndentForComment(index));
      line_.append(absl::StripTrailingAsciiWhitespace(token.text));
      Flush();
      opened_block_ = false;
      after_block_open_ = false;
      after_block_close_ = false;
      return index + 1;
    }

    if (starts_line) {
      Flush();
      if (breaks_[index] >= 2) {
        Blank();
      }
      Start(IndentFor(index));
      opened_block_ = false;
      after_block_open_ = false;
      after_block_close_ = false;
      description_line_ = brackets_.empty() && IsDescriptionLine(index);
      SeparatePortGroup(index);
      if (brackets_.empty()) {
        size_t after = EmitPort(index);
        if (after == index) {
          after = EmitHeader(index);
        }
        if (after == index && in_dto_) {
          after = EmitField(index);
        }
        if (after != index) {
          return after;
        }
      }
    }

    if (IsOpener(token.kind) && !OpensBlock(token)) {
      Append(token);
      brackets_.push_back(Bracket{token.kind, indent_});
      return index + 1;
    }
    if (IsCloser(token.kind) && !brackets_.empty() &&
        Closes(brackets_.back().kind) == token.kind) {
      brackets_.pop_back();
      Append(token);
      return index + 1;
    }

    if (OpensBlock(token)) {
      Append(token);
      Flush();
      // Which kind of body this is, so a `name: type` inside a shape is read as
      // a field and one inside a flow is not read as anything of the sort. Only
      // the outermost matters: a `struct` holds no blocks.
      blocks_.push_back(Block{
          indent_, struct_braces_.find(token.start) != struct_braces_.end(),
          module_braces_.find(token.start) != module_braces_.end()});
      in_dto_ = blocks_.back().dto;
      opened_block_ = true;
      blank_pending_ = false;
      after_block_open_ = true;
      after_block_close_ = false;
      return index + 1;
    }

    Append(token);
    after_block_close_ = false;
    return index + 1;
  }

  /// The end of the logical line starting at `index`, for a declaration.
  ///
  /// A break ends it, and so does the `}` that closes the flow around it.
  [[nodiscard]] size_t LineEnd(size_t index, bool grouped = false) const {
    size_t end = index + 1;
    int depth = 0;
    while (end < code_.size() &&
           (breaks_[end] == 0 || (grouped && depth > 0))) {
      if (IsOpener(code_[end].kind)) {
        ++depth;
      }
      if (IsCloser(code_[end].kind)) {
        if (depth == 0) {
          break;
        }
        --depth;
      }
      ++end;
    }
    return end;
  }

  /// A `header` declaration as its columns, or `index` unchanged.
  ///
  /// Everything after the name is one column: `as alias`, `default 3`, both, or
  /// neither. There is nothing to be gained from splitting it further -- what a
  /// reader is looking down is the list of names.
  size_t EmitHeader(size_t index) {
    const Token& first = code_[index];
    if (!first.IsWord() || vocabulary::Canonical(first.text) != "header") {
      return index;
    }
    const size_t stop = LineEnd(index);
    size_t end = stop;
    if (end - index < 2) {
      return index;
    }
    if (code_[index + 1].kind != TokenKind::kString) {
      return index;
    }

    std::string comment;
    if (code_[end - 1].kind == TokenKind::kComment) {
      comment =
          std::string(absl::StripTrailingAsciiWhitespace(code_[end - 1].text));
      --end;
    }

    PortColumns columns;
    columns.direction = std::string(first.text);
    columns.name = std::string(code_[index + 1].text);
    Token previous;
    for (size_t at = index + 2; at < end; ++at) {
      if (at > index + 2 && NeedsSpace(previous, code_[at])) {
        columns.type.push_back(' ');
      }
      columns.type.append(code_[at].text);
      previous = code_[at];
    }
    port_ = std::move(columns);
    comment_ = std::move(comment);
    Flush();
    opened_block_ = false;
    return stop;
  }

  /// A port declaration as its columns, or `index` unchanged if this is not
  /// one.
  size_t EmitPort(size_t index) {
    const Token& first = code_[index];
    if (!first.IsWord()) {
      return index;
    }
    const std::string direction = vocabulary::Canonical(first.text);
    if (direction != "in" && direction != "out") {
      return index;
    }

    // The whole declaration has to be on one line for its columns to mean
    // anything; anything else goes through the ordinary path.
    size_t end = LineEnd(index);
    if (end - index < 4) {
      return index;
    }
    if (!code_[index + 1].IsWord()) {
      return index;
    }
    if (code_[index + 2].kind != TokenKind::kColon) {
      return index;
    }

    const size_t stop = end;
    std::string comment;
    if (code_[end - 1].kind == TokenKind::kComment) {
      comment =
          std::string(absl::StripTrailingAsciiWhitespace(code_[end - 1].text));
      --end;
    }

    PortColumns columns;
    columns.direction = std::string(first.text);
    columns.name = absl::StrCat(code_[index + 1].text, ":");

    size_t at = index + 3;
    if (at >= end) {
      return index;
    }
    // The type: a quoted mimetype, or a qualified name with its parameters. It
    // ends
    // at the first thing that is plainly not part of a type.
    std::string type;
    Token previous;
    bool first_of_type = true;
    int depth = 0;
    while (at < end) {
      const Token& token = code_[at];
      if (depth == 0) {
        if (!first_of_type) {
          if (token.kind == TokenKind::kString) {
            break;
          }
          if (token.IsWord() && vocabulary::PortModifierWords().contains(
                                    vocabulary::Canonical(token.text))) {
            break;
          }
        }
        // What a type is made of, and nothing else: a qualified name, its
        // parameters, or a quoted mimetype.
        const bool part_of_type =
            token.kind == TokenKind::kWord || token.kind == TokenKind::kScope ||
            token.kind == TokenKind::kLeftBracket ||
            (first_of_type && token.kind == TokenKind::kString);
        if (!part_of_type) {
          break;
        }
      }
      if (IsOpener(token.kind)) {
        ++depth;
      }
      if (IsCloser(token.kind)) {
        --depth;
      }
      if (!first_of_type && NeedsSpace(previous, token)) {
        type.push_back(' ');
      }
      type.append(token.text);
      previous = token;
      first_of_type = false;
      ++at;
      if (depth == 0 && code_[at - 1].kind == TokenKind::kString) {
        break;
      }
    }
    std::vector<Token> modifiers;
    while (at < end && code_[at].IsWord() &&
           vocabulary::PortModifierWords().contains(
               vocabulary::Canonical(code_[at].text))) {
      modifiers.push_back(code_[at]);
      ++at;
    }
    const auto order = vocabulary::OrderedPortModifiers();
    std::stable_sort(modifiers.begin(), modifiers.end(),
                     [&](const auto& a, const auto& b) {
                       return std::find(order.begin(), order.end(),
                                        vocabulary::Canonical(a.text)) <
                              std::find(order.begin(), order.end(),
                                        vocabulary::Canonical(b.text));
                     });
    for (const auto& modifier : modifiers) {
      absl::StrAppend(&type, " ", modifier.text);
    }
    if (at < end && code_[at].kind == TokenKind::kString) {
      columns.description = std::string(code_[at].text);
      ++at;
    }
    // Anything left over is a shape this does not know; leave the line alone.
    if (at != end) {
      return index;
    }

    columns.type = std::move(type);
    port_ = std::move(columns);
    comment_ = std::move(comment);
    Flush();
    opened_block_ = false;
    // Past the declaration and the comment that shared its line, and no
    // further:
    // a comment on the *next* line is the next line's.
    return stop;
  }

  /// Whether a field modifier takes a value, so the thing after it is that
  /// value
  /// rather than the field's description.
  static bool TakesAnArgument(const Token& token) {
    if (!token.IsWord()) {
      return false;
    }
    const std::string word = vocabulary::Canonical(token.text);
    return word == "matching" || word == "default";
  }

  /// A `struct` field as its columns, or `index` unchanged if this is not one.
  ///
  /// The same three columns a port has, minus the direction: a field opens with
  /// its own name. Everything after the type is one column, as a header's tail
  /// is, because what a reader scans down a shape is the list of names and
  /// their
  /// types -- not which of them happens to carry a pattern.
  size_t EmitField(size_t index) {
    if (!code_[index].IsWord()) {
      return index;
    }
    if (index + 1 >= code_.size() ||
        code_[index + 1].kind != TokenKind::kColon) {
      return index;
    }
    const size_t stop = LineEnd(index, true);
    size_t end = stop;
    if (end - index < 3) {
      return index;
    }

    std::string comment;
    if (code_[end - 1].kind == TokenKind::kComment) {
      comment =
          std::string(absl::StripTrailingAsciiWhitespace(code_[end - 1].text));
      --end;
    }
    // A description ends the line; anything before it is the type and what
    // bounds it.
    std::string description;
    if (end > index + 2 && code_[end - 1].kind == TokenKind::kString &&
        code_[end - 2].kind != TokenKind::kColon &&
        !TakesAnArgument(code_[end - 2])) {
      description = std::string(code_[end - 1].text);
      --end;
    }
    if (end <= index + 2) {
      return index;
    }

    PortColumns columns;
    columns.field = true;
    columns.name = absl::StrCat(code_[index].text, ":");

    struct Modifier {
      int rank;
      size_t start;
      size_t end;
    };

    std::vector<Modifier> modifiers;
    int depth = 0;
    const auto order = vocabulary::OrderedFieldModifiers();
    for (size_t at = index + 2; at < end; ++at) {
      const auto& token = code_[at];
      if (at > index + 2 && depth == 0 &&
          code_[at - 1].kind != TokenKind::kScope &&
          code_[at - 1].kind != TokenKind::kDot) {
        const auto word = vocabulary::Canonical(token.text);
        const auto found = std::find(order.begin(), order.end(),
                                     word == "one" ? "one of" : word);
        const bool numeric = token.kind == TokenKind::kNumber ||
                             token.kind == TokenKind::kDuration;
        const bool after_default =
            at > 1 && code_[at - 2].IsWord() &&
            vocabulary::Canonical(code_[at - 2].text) == "default";
        const bool range =
            !TakesAnArgument(code_[at - 1]) &&
            ((numeric && code_[at - 1].kind != TokenKind::kRange) ||
             (token.kind == TokenKind::kRange &&
              (after_default || (code_[at - 1].kind != TokenKind::kNumber &&
                                 code_[at - 1].kind != TokenKind::kDuration))));
        if ((token.IsWord() && found != order.end()) || range) {
          if (!modifiers.empty()) {
            modifiers.back().end = at;
          }
          modifiers.push_back(
              {range ? 3 : static_cast<int>(found - order.begin()) * 2, at,
               end});
        }
      }
      if (IsOpener(token.kind)) {
        ++depth;
      }
      if (IsCloser(token.kind)) {
        --depth;
      }
    }
    std::vector<Token> ordered;
    const size_t type_end = modifiers.empty() ? end : modifiers.front().start;
    ordered.insert(ordered.end(),
                   code_.begin() + static_cast<std::ptrdiff_t>(index + 2),
                   code_.begin() + static_cast<std::ptrdiff_t>(type_end));
    std::stable_sort(
        modifiers.begin(), modifiers.end(),
        [](const auto& a, const auto& b) { return a.rank < b.rank; });
    const bool reordered = !std::is_sorted(
        modifiers.begin(), modifiers.end(),
        [](const auto& a, const auto& b) { return a.start < b.start; });
    if (!reordered) {
      for (size_t at = index + 1; at < end; ++at) {
        if (breaks_[at] > 0) {
          return index;
        }
      }
    }
    for (const auto& modifier : modifiers) {
      ordered.insert(
          ordered.end(),
          code_.begin() + static_cast<std::ptrdiff_t>(modifier.start),
          code_.begin() + static_cast<std::ptrdiff_t>(modifier.end));
    }
    Token previous;
    for (size_t at = 0; at < ordered.size(); ++at) {
      if (at > 0 && previous.kind == TokenKind::kComment) {
        columns.type.push_back('\n');
      } else if (at > 0 && NeedsSpace(previous, ordered[at])) {
        columns.type.push_back(' ');
      }
      columns.type.append(ordered[at].text);
      previous = ordered[at];
    }
    columns.description = std::move(description);
    port_ = std::move(columns);
    comment_ = std::move(comment);
    Flush();
    opened_block_ = false;
    return stop;
  }

  void SeparateGroup() {
    size_t first = lines_.size();
    while (first > 0 && lines_[first - 1].indent == indent_ &&
           lines_[first - 1].text.starts_with("#")) {
      --first;
    }
    if (first < lines_.size()) {
      lines_[first].blank_before = true;
      blank_pending_ = false;
    } else {
      Blank();
    }
  }

  void SeparatePortGroup(size_t index) {
    if (!brackets_.empty() || ContinuesStatement(index) || description_line_) {
      return;
    }
    const auto& token = code_[index];
    const std::string word =
        token.IsWord() ? vocabulary::Canonical(token.text) : "";
    const bool port = (word == "in" || word == "out") &&
                      index + 2 < code_.size() && code_[index + 1].IsWord() &&
                      code_[index + 2].kind == TokenKind::kColon;
    if (!port) {
      if (word != "header" && word != "describe") {
        if (!last_port_direction_.empty()) {
          SeparateGroup();
        }
        last_port_direction_.clear();
      }
      return;
    }
    if (!last_port_direction_.empty() && word != last_port_direction_ &&
        indent_ == last_port_indent_) {
      SeparateGroup();
    }
    last_port_direction_ = word;
    last_port_indent_ = indent_;
  }

  /// The lines, with each run of port declarations lined up.
  std::string Render() {
    // Consecutive declarations share column widths. Comments and changes of
    // port direction delimit runs.
    for (size_t index = 0; index < lines_.size();) {
      if (!lines_[index].port.has_value()) {
        ++index;
        continue;
      }
      const bool header = IsHeaderLine(lines_[index]);
      size_t run_end = index + 1;
      while (run_end < lines_.size() && !lines_[run_end].blank_before &&
             (lines_[run_end].description ||
              (lines_[run_end].port.has_value() &&
               lines_[run_end].indent == lines_[index].indent &&
               lines_[run_end].port->field == lines_[index].port->field &&
               IsHeaderLine(lines_[run_end]) == header))) {
        ++run_end;
      }
      AlignPorts(index, run_end);
      index = run_end;
    }

    std::string out;

    for (const OutputLine& line : lines_) {
      if (line.blank_before) {
        out.push_back('\n');
      }
      std::string text = absl::StrCat(
          std::string(static_cast<size_t>(line.indent), ' '), line.text);
      if (!line.comment.empty() && line.description_tail.empty()) {
        // Two spaces before a comment that shares its line with code, which is
        // the convention every language that has one uses.
        absl::StrAppend(&text, text.empty() ? "" : "  ", line.comment);
      }
      absl::StrAppend(&out, absl::StripTrailingAsciiWhitespace(text), "\n");
      if (!line.description_tail.empty()) {
        out.append(static_cast<size_t>(line.indent) +
                       static_cast<size_t>(options_.indent),
                   ' ');
        out.append(line.description_tail);
        if (!line.comment.empty()) {
          absl::StrAppend(&out, "  ", line.comment);
        }
        out.push_back('\n');
      }
    }
    return out;
  }

  /// Whether a declaration line is a `header` rather than a port.
  ///
  /// A run is one or the other: they are different lists, and lining a header
  /// up
  /// with a port above it would say they belonged together.
  static bool IsHeaderLine(const OutputLine& line) {
    return line.port.has_value() &&
           vocabulary::Canonical(line.port->direction) == "header";
  }

  void AppendDescription(OutputLine& line, std::string& text,
                         const PortColumns& port, size_t padding = 0) const {
    if (port.description.empty()) {
      return;
    }
    const size_t columns = static_cast<size_t>(line.indent) + Columns(text) +
                           1 + Columns(port.description);
    if (options_.line_width != 0 && columns > options_.line_width) {
      line.description_tail = port.description;
      return;
    }
    if (options_.line_width != 0 && columns + padding > options_.line_width) {
      padding = 0;
    }
    text.append(padding + 1, ' ');
    text.append(port.description);
  }

  void AlignPorts(size_t begin, size_t end) {
    if (!options_.align_ports) {
      for (size_t index = begin; index < end; ++index) {
        if (lines_[index].port.has_value()) {
          const auto& port = *lines_[index].port;
          lines_[index].text =
              absl::StrCat(port.direction, port.direction.empty() ? "" : " ",
                           port.name, " ", port.type);
          AppendDescription(lines_[index], lines_[index].text, port);
        }
      }
      return;
    }
    // Three for a port whatever this run holds, so `in` and `out` line up
    // across two groups of them as well as within one. A field has no such
    // word, and a header's is only ever `header`.
    const bool fields = lines_[begin].port->field;
    size_t direction_width = fields || IsHeaderLine(lines_[begin]) ? 0 : 3;
    size_t name_width = 0;
    size_t type_width = 0;
    bool any_description = false;
    for (size_t index = begin; index < end; ++index) {
      if (!lines_[index].port.has_value()) {
        continue;  // a description line
      }
      const PortColumns& port = *lines_[index].port;
      direction_width = std::max(direction_width, port.direction.size());
      name_width = std::max(name_width, port.name.size());
      // Only the lines that have a description set the column it starts at: a
      // long type on a line with nothing after it has no business pushing
      // everybody else's comment across the page.
      if (!port.description.empty()) {
        type_width = std::max(type_width, port.type.size());
        any_description = true;
      }
    }
    for (size_t index = begin; index < end; ++index) {
      if (!lines_[index].port.has_value()) {
        continue;
      }
      const PortColumns& port = *lines_[index].port;
      std::string text = port.direction;
      text.append(direction_width - port.direction.size(), ' ');
      // A field has no leading word, so it has no leading space either.
      if (direction_width > 0) {
        text.push_back(' ');
      }
      absl::StrAppend(&text, port.name);
      text.append(name_width - port.name.size(), ' ');
      text.push_back(' ');
      absl::StrAppend(&text, port.type);
      if (any_description && !port.description.empty()) {
        AppendDescription(
            lines_[index], text, port,
            type_width > port.type.size() ? type_width - port.type.size() : 0);
      }
      lines_[index].text = std::move(text);
    }
  }

  struct Bracket {
    TokenKind kind;
    int indent;
  };

  struct Block {
    int indent;
    bool dto;
    bool module;
  };

  std::string_view source_;
  FormatOptions options_;
  absl::flat_hash_set<size_t> value_braces_;
  absl::flat_hash_set<size_t> loop_conditions_;
  absl::flat_hash_set<size_t> tagged_braces_;
  absl::flat_hash_set<size_t> expression_groups_;
  absl::flat_hash_set<size_t> struct_braces_;
  absl::flat_hash_set<size_t> module_braces_;
  absl::flat_hash_set<size_t> statement_starts_;
  absl::flat_hash_set<size_t> declaration_starts_;

  std::vector<Token> code_;
  std::vector<int> breaks_;

  std::vector<OutputLine> lines_;
  std::vector<Bracket> brackets_;
  std::vector<Block> blocks_;
  /// Whether a field may be read in the innermost block.
  bool in_dto_ = false;
  std::string line_;
  std::string comment_;
  std::optional<PortColumns> port_;
  std::string last_port_direction_;
  int last_port_indent_ = 0;
  Token previous_;
  bool line_started_ = false;
  int indent_ = 0;
  int statement_indent_ = 0;
  bool opened_block_ = false;
  bool blank_pending_ = false;
  /// Whether the last token emitted was a block `{`, or a block `}`.
  bool after_block_open_ = false;
  bool after_block_close_ = false;
  /// Whether the line being built is a description on a line of its own.
  bool description_line_ = false;
};

nlohmann::json Meaning(const ParseResult& parsed) {
  auto result = SyntaxToJsonValue("-", parsed);
  result.erase("source");
  result.erase("diagnostics");
  std::vector<nlohmann::json*> pending{&result};
  while (!pending.empty()) {
    auto* node = pending.back();
    pending.pop_back();
    if (node->is_object()) {
      const bool literal =
          node->contains("kind") && (*node)["kind"] == "literal";
      node->erase("at");
      for (auto& [key, child] : node->items()) {
        if (key != "default" && key != "one_of" && key != "start" &&
            !(literal && key == "value")) {
          pending.push_back(&child);
        }
      }
    } else if (node->is_array()) {
      for (auto& child : *node) {
        pending.push_back(&child);
      }
    }
  }
  return result;
}

size_t Columns(std::string_view text) {
  return static_cast<size_t>(
      std::count_if(text.begin(), text.end(),
                    [](char c) { return !utf8::IsContinuation(c); }));
}

// Prefer unescaped spaces; split long words between complete source characters.
std::vector<std::string_view> StringParts(std::string_view text, size_t width) {
  if (width <= 2) {
    return {text};
  }
  std::vector<std::string_view> parts;
  size_t start = 0;
  size_t start_column = 0;
  size_t columns = 0;
  size_t space = std::string_view::npos;
  size_t space_column = 0;
  for (size_t at = 0; at < text.size();) {
    const bool escaped = text[at] == '\\' && at + 1 < text.size();
    const size_t atom = escaped ? 1 + utf8::SequenceWidth(text[at + 1])
                                : utf8::SequenceWidth(text[at]);
    const size_t atom_columns = escaped ? 2 : 1;
    if (!escaped && text[at] == ' ' && at > start) {
      space = at;
      space_column = columns;
    }
    if (columns + atom_columns - start_column + 2 > width && at > start) {
      const size_t split = space == std::string_view::npos ? at : space;
      parts.push_back(text.substr(start, split - start));
      start = split;
      start_column = space == std::string_view::npos ? columns : space_column;
      space = std::string_view::npos;
    }
    columns += atom_columns;
    at += std::min(atom, text.size() - at);
  }
  parts.push_back(text.substr(start));
  return parts;
}

std::string WrapStrings(std::string_view source, const LexResult& lexed,
                        const ParseResult& parsed,
                        const FormatOptions& options) {
  if (options.line_width == 0) {
    return std::string(source);
  }
  absl::flat_hash_set<size_t> expressions;
  absl::flat_hash_map<size_t, std::vector<syntax::Location>> prefixes;
  const auto protect_code = [&](const syntax::NodePtr& code,
                                const syntax::NodePtr& message) {
    if (syntax::As<syntax::Name>(code.get()) != nullptr &&
        code->groups.empty() && message != nullptr) {
      prefixes[message->location.start].push_back(code->extent);
    }
  };
  const auto protect_level = [&](const syntax::LogTail& tail) {
    if (!tail.has_format && !tail.level.Empty() && !tail.arguments.empty()) {
      prefixes[tail.arguments.front()->location.start].push_back(
          tail.level.location);
    }
  };
  const auto visit = [&](const syntax::Node& node) {
    if (const auto* literal = syntax::As<syntax::Literal>(&node);
        literal != nullptr &&
        literal->value.kind == syntax::Constant::Kind::kString) {
      auto at = std::lower_bound(
          lexed.tokens.begin(), lexed.tokens.end(), literal->location.start,
          [](const Token& token, size_t start) { return token.start < start; });
      size_t bytes = 0;
      for (; at != lexed.tokens.end(); ++at) {
        if (at->kind == TokenKind::kString) {
          expressions.insert(at->start);
          bytes += at->string_value.size();
          if (bytes >= literal->value.text.size()) {
            break;
          }
        } else if (at->kind != TokenKind::kNewline &&
                   at->kind != TokenKind::kComment) {
          break;
        }
      }
    }
    if (const auto* fail = syntax::As<syntax::Fail>(&node); fail != nullptr) {
      protect_code(fail->code, fail->message);
    } else if (const auto* abort = syntax::As<syntax::Abort>(&node);
               abort != nullptr) {
      protect_code(abort->code, abort->message);
      if (syntax::As<syntax::Name>(abort->target.get()) != nullptr &&
          abort->target->groups.empty() && abort->code != nullptr) {
        const auto code = prefixes.find(abort->message == nullptr
                                            ? abort->code->location.start
                                            : abort->message->location.start);
        if (code != prefixes.end()) {
          code->second.insert(code->second.begin(), abort->target->extent);
        } else if (syntax::As<syntax::Literal>(abort->code.get()) != nullptr) {
          prefixes[abort->code->location.start].push_back(
              abort->target->extent);
        }
      }
    } else if (const auto* log = syntax::As<syntax::Log>(&node);
               log != nullptr) {
      protect_level(log->tail);
    } else if (const auto* stage = syntax::As<syntax::Stage>(&node);
               stage != nullptr) {
      protect_level(stage->log);
    }
  };
  for (const auto& flow : parsed.flows) {
    syntax::VisitSubtree(*flow, visit);
  }
  for (const auto& dto : parsed.dtos) {
    syntax::VisitSubtree(*dto, visit);
  }
  const absl::flat_hash_set<size_t> values(parsed.value_braces.begin(),
                                           parsed.value_braces.end());

  struct Group {
    bool value;
    size_t indent;
  };

  std::vector<Group> groups;
  size_t depth = 0;
  size_t copied = 0;
  std::string output;
  const auto& tokens = lexed.tokens;
  for (size_t index = 0; index < tokens.size(); ++index) {
    const auto& token = tokens[index];
    const size_t newline = source.rfind('\n', token.start);
    const size_t line = newline == std::string_view::npos ? 0 : newline + 1;
    size_t content = line;
    while (content < source.size() && source[content] == ' ') {
      ++content;
    }
    if (IsOpener(token.kind)) {
      const bool value = token.kind != TokenKind::kLeftBrace ||
                         values.find(token.start) != values.end();
      groups.push_back({value, content - line});
      depth += value;
    } else if (IsCloser(token.kind) && !groups.empty()) {
      depth -= groups.back().value;
      groups.pop_back();
    }
    if (token.kind != TokenKind::kString || token.text.starts_with(R"(""")")) {
      continue;
    }
    size_t last = index;
    std::string text(token.text.substr(1, token.text.size() - 2));
    while (last + 1 < tokens.size() &&
           tokens[last + 1].kind == TokenKind::kString &&
           tokens[last + 1].line == token.line &&
           !tokens[last + 1].text.starts_with(R"(""")")) {
      ++last;
      text.append(tokens[last].text.substr(1, tokens[last].text.size() - 2));
    }
    const bool expression = expressions.find(token.start) != expressions.end();
    const auto& next = tokens[last + 1];
    const bool description =
        depth == 0 &&
        ((token.start == content &&
          (next.EndsStatement() || next.kind == TokenKind::kComment)) ||
         (index > 0 && tokens[index - 1].start == content &&
          vocabulary::Canonical(tokens[index - 1].text) == "describe"));
    if (!expression && !description) {
      continue;
    }
    const size_t column = Columns(source.substr(line, token.start - line));
    if (column + Columns(text) + 2 <= options.line_width) {
      index = last;
      continue;
    }
    const bool parentheses = expression && depth == 0;
    const size_t indent =
        description
            ? content - line +
                  (token.start == content ? 0
                                          : static_cast<size_t>(options.indent))
        : depth > 0 ? groups.back().indent + static_cast<size_t>(options.indent)
                    : content - line + static_cast<size_t>(options.indent);
    const size_t available =
        options.line_width > indent ? options.line_width - indent : 0;
    const auto parts = StringParts(text, available);
    if (parts.size() < 2 && !(description && token.start != content)) {
      index = last;
      continue;
    }
    const auto prefix = prefixes.find(token.start);
    if (parentheses && prefix != prefixes.end() &&
        prefix->second.front().start >= copied) {
      // A bare status code or log level followed by `(` reads as a call.
      for (const auto& at : prefix->second) {
        output.append(source.substr(copied, at.start - copied));
        absl::StrAppend(&output, "(",
                        source.substr(at.start, at.end - at.start), ")");
        copied = at.end;
      }
      output.append(source.substr(copied, token.start - copied));
    } else {
      output.append(source.substr(copied, token.start - copied));
    }
    if (parentheses) {
      output.append("(\n");
      output.append(indent, ' ');
    } else if (token.start != content) {
      output.push_back('\n');
      output.append(indent, ' ');
    }
    for (size_t part = 0; part < parts.size(); ++part) {
      if (part > 0) {
        output.push_back('\n');
        output.append(indent, ' ');
      }
      absl::StrAppend(&output, "\"", parts[part], "\"");
    }
    if (parentheses) {
      output.push_back('\n');
      output.append(content - line, ' ');
      output.push_back(')');
    }
    copied = tokens[last].end;
    index = last;
  }
  output.append(source.substr(copied));
  return output;
}

std::string WrapLines(std::string_view source, const LexResult& lexed,
                      const ParseResult& parsed, const FormatOptions& options,
                      bool modifiers) {
  if (options.line_width == 0) {
    return std::string(source);
  }
  const absl::flat_hash_set<size_t> values(parsed.value_braces.begin(),
                                           parsed.value_braces.end());
  std::vector<bool> groups;
  size_t depth = 0;
  std::vector<size_t> breaks;
  std::vector<size_t> pipeline_breaks;
  const Token* previous = nullptr;
  for (const auto& token : lexed.tokens) {
    if (token.kind == TokenKind::kNewline || token.kind == TokenKind::kEnd) {
      continue;
    }
    if (previous != nullptr && token.line == previous->line &&
        token.start >= previous->end) {
      const auto word = token.IsWord() ? vocabulary::Canonical(token.text) : "";
      const bool after_operator =
          token.start > previous->end && token.kind != TokenKind::kColon &&
          token.kind != TokenKind::kEqual && ExpectsOperand(*previous);
      const bool modifier =
          modifiers && word != "headers" && token.start > previous->end &&
          (vocabulary::ModifierWords().contains(word) || word == "parallel" ||
           word == "unordered" || word == "into" || word == "before" ||
           word == "numbered" || word == "base");
      if (after_operator || modifier || token.kind == TokenKind::kPipe ||
          token.kind == TokenKind::kArrow ||
          token.kind == TokenKind::kComment) {
        breaks.push_back(token.start);
      }
      if (depth == 0 && token.kind == TokenKind::kPipe) {
        pipeline_breaks.push_back(token.start);
      }
    }
    if (IsOpener(token.kind)) {
      const bool group =
          token.kind != TokenKind::kLeftBrace || values.contains(token.start);
      groups.push_back(group);
      if (group) {
        ++depth;
      }
    } else if (IsCloser(token.kind) && !groups.empty()) {
      if (groups.back()) {
        --depth;
      }
      groups.pop_back();
    }
    previous = &token;
  }
  std::string output;
  size_t line = 0;
  while (line < source.size()) {
    size_t end = source.find('\n', line);
    if (end == std::string_view::npos) {
      end = source.size();
    }
    size_t content = line;
    while (content < end && source[content] == ' ') {
      ++content;
    }
    size_t part = line;
    const size_t line_columns = Columns(source.substr(line, end - line));
    size_t part_column = 0;
    size_t indent = 0;
    while (line_columns - part_column + indent > options.line_width) {
      const auto first = std::upper_bound(breaks.begin(), breaks.end(),
                                          std::max(part, content));
      if (first == breaks.end() || *first >= end) {
        break;
      }
      auto chosen = first;
      std::optional<size_t> pipeline;
      for (auto candidate = first;
           candidate != breaks.end() && *candidate < end; ++candidate) {
        if (Columns(source.substr(part, *candidate - part)) + indent >
            options.line_width) {
          break;
        }
        chosen = candidate;
        if (std::binary_search(pipeline_breaks.begin(), pipeline_breaks.end(),
                               *candidate)) {
          pipeline = *candidate;
        }
      }
      if (pipeline.has_value()) {
        chosen = std::lower_bound(first, breaks.end(), *pipeline);
      }
      absl::StrAppend(&output,
                      absl::StripTrailingAsciiWhitespace(
                          source.substr(part, *chosen - part)),
                      "\n");
      indent = content - line + static_cast<size_t>(options.indent);
      output.append(indent, ' ');
      part_column += Columns(source.substr(part, *chosen - part));
      part = *chosen;
    }
    output.append(source.substr(part, end - part));
    if (end < source.size()) {
      output.push_back('\n');
    }
    line = end + 1;
  }
  return output;
}

}  // namespace

FormatResult Format(std::string_view source, FormatOptions options,
                    CompilationLimits limits, CompilationBudget* budget) {
  CompilationBudget local_budget(limits);
  if (budget == nullptr) {
    budget = &local_budget;
  }
  FormatResult result;
  LexResult lexed = Lex(
      source,
      LexOptions{.keep_comments = true, .limits = limits, .budget = budget});
  ParseResult parsed =
      ParseTokens(source, lexed.tokens, lexed.diagnostics, limits, budget);
  result.diagnostics = parsed.diagnostics;
  if (parsed.HasErrors()) {
    // A file that cannot be read is a file left exactly as it is.
    result.formatted = std::string(source);
    return result;
  }

  result.formatted = Formatter(source, lexed.tokens, parsed, options).Run();
  const auto meaning = Meaning(parsed);
  for (int pass = 0; pass < 16 && budget->Check(); ++pass) {
    auto tokens = Lex(
        result.formatted,
        LexOptions{.keep_comments = true, .limits = limits, .budget = budget});
    auto tree = ParseTokens(result.formatted, tokens.tokens, tokens.diagnostics,
                            limits, budget);
    if (tree.HasErrors() || Meaning(tree) != meaning) {
      result.formatted = std::string(source);
      break;
    }
    std::string strings = WrapStrings(result.formatted, tokens, tree, options);
    if (strings != result.formatted) {
      auto string_tokens = Lex(strings, LexOptions{.keep_comments = true,
                                                   .limits = limits,
                                                   .budget = budget});
      auto string_tree = ParseTokens(strings, string_tokens.tokens,
                                     string_tokens.diagnostics, limits, budget);
      if (!string_tree.HasErrors() && Meaning(string_tree) == meaning) {
        result.formatted = std::move(strings);
        tokens = std::move(string_tokens);
        tree = std::move(string_tree);
      }
    }
    const std::string grouped =
        Formatter(result.formatted, tokens.tokens, tree, options, true).Run();
    if (grouped != result.formatted) {
      result.formatted = grouped;
      tokens = Lex(result.formatted, LexOptions{.keep_comments = true,
                                                .limits = limits,
                                                .budget = budget});
      tree = ParseTokens(result.formatted, tokens.tokens, tokens.diagnostics,
                         limits, budget);
    }
    std::string next = WrapLines(result.formatted, tokens, tree, options, true);
    if (next != result.formatted) {
      auto checked = Parse(next, limits, budget);
      if (checked.HasErrors() || Meaning(checked) != meaning) {
        next = WrapLines(result.formatted, tokens, tree, options, false);
      }
    }
    auto next_tokens = Lex(
        next,
        LexOptions{.keep_comments = true, .limits = limits, .budget = budget});
    auto next_tree = ParseTokens(next, next_tokens.tokens,
                                 next_tokens.diagnostics, limits, budget);
    if (next_tree.HasErrors() || Meaning(next_tree) != meaning) {
      break;
    }
    next = Formatter(next, next_tokens.tokens, next_tree, options, true).Run();
    if (next == result.formatted) {
      break;
    }
    result.formatted = std::move(next);
  }
  result.changed = result.formatted != source;
  if (result.changed) {
    // Trimmed to what differs, so applying this in an editor does not touch the
    // part of the file the author is looking at.
    size_t prefix = 0;
    const size_t shortest = std::min(source.size(), result.formatted.size());
    while (prefix < shortest && source[prefix] == result.formatted[prefix]) {
      ++prefix;
    }
    size_t suffix = 0;
    while (suffix < shortest - prefix &&
           source[source.size() - 1 - suffix] ==
               result.formatted[result.formatted.size() - 1 - suffix]) {
      ++suffix;
    }
    Edit edit;
    edit.start = prefix;
    edit.end = source.size() - suffix;
    edit.text = result.formatted.substr(
        prefix, result.formatted.size() - suffix - prefix);
    result.edits.push_back(std::move(edit));
  }
  return result;
}

}  // namespace a11::flow
