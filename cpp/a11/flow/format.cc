// Copyright 2026 The A11 Authors.

#include "a11/flow/format.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>

#include "a11/flow/diagnostic.h"
#include "a11/flow/lexer.h"
#include "a11/flow/parser.h"
#include "a11/flow/token.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow {
namespace {

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
  /// empty direction, because a run has to be all one kind and "is the direction
  /// empty" is a fact about this line rather than about what it is.
  bool field = false;
};

/// One line of output, before the columns of a run of ports are worked out.
struct OutputLine {
  int indent = 0;
  std::string text;
  /// A comment that followed code on the same line.
  std::string comment;
  std::optional<PortColumns> port;
  /// Whether one blank line goes above this one.
  bool blank_before = false;
  /// Whether this is a declaration's description, written on its own line. Such
  /// a line sits *inside* a run of declarations without being one of them, so the
  /// columns of the run are worked out across it.
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

/// Whether `word(` reads as something being called.
///
/// One decision, and it is about the space: `web-fetch(`, `len(` and `node()` hug
/// their parentheses because the word and the brackets are one thing, while
/// `not (`, `if (` and `in (` are a word and then a bracketed expression. So every
/// word the grammar gives meaning to takes its space -- except `node`, which is
/// the one keyword in the language that is *constructed*.
bool IsCalled(std::string_view text) {
  const std::string word = vocabulary::Canonical(text);
  // `node()` is the one keyword that is constructed, and several builtins are
  // spelled like declaration words -- `default`, `text`, `number`, `join`.
  if (word == "node") return true;
  if (vocabulary::Builtins().contains(word)) return true;
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
            const std::vector<size_t>& value_braces,
            const std::vector<size_t>& tagged_braces,
            const FormatOptions& options)
      : source_(source), options_(options),
        value_braces_(value_braces.begin(), value_braces.end()),
        tagged_braces_(tagged_braces.begin(), tagged_braces.end()) {
    // Line breaks are not tokens here: what matters is how *many* there were
    // between two pieces of code, and the lexer folds a run of them into one. The
    // source gap has the number, so it is read from there and the newline tokens
    // are dropped.
    size_t previous_end = 0;
    for (const Token& token : tokens) {
      if (token.kind == TokenKind::kNewline || token.kind == TokenKind::kEnd) {
        continue;
      }
      int breaks = 0;
      for (size_t at = previous_end; at < token.start; ++at) {
        if (source_[at] == '\n') ++breaks;
      }
      code_.push_back(token);
      breaks_.push_back(breaks);
      previous_end = token.end;
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
  /// Whether this `{` opens a block rather than a value.
  bool OpensBlock(const Token& token) const {
    return token.kind == TokenKind::kLeftBrace &&
           !value_braces_.contains(token.start);
  }

  /// Whether this `}` closes a block. Read off the bracket stack, which is what
  /// knows: a value brace was pushed when it opened.
  bool ClosesBlock(const Token& token) const {
    if (token.kind != TokenKind::kRightBrace) return false;
    if (brackets_.empty()) return true;
    return brackets_.back().kind != TokenKind::kLeftBrace;
  }

  int BlockIndent() const { return block_ * options_.indent; }

  /// Whether the line starting at `index` continues the statement above it.
  ///
  /// The grammar's own rule, because it has to be: a line beginning with `|` or
  /// `->` is a continuation, a modifier word is one unless what follows makes it a
  /// statement of its own, and a bare `then`/`where` is one when it has an operand.
  bool ContinuesStatement(size_t index) const {
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
    // A description on a line of its own belongs to the declaration above it, so
    // it is indented under it like any other tail.
    if (!inside_group && IsDescriptionLine(index)) return true;
    // A line that begins where the one above left off mid-list continues it: a
    // comma at the end of the previous line says the statement is not over.
    //
    // The case that needed this is a `fold`/`scan` whose expression is on the
    // next line -- `| scan 0 as total,` then the expression. That expression is
    // the *stage's argument*, and without this it began with a `{` or a name,
    // neither of which reads as a tail, so it was pushed out to the block's own
    // indent and the construct with the language's longest argument became the
    // worst-looking thing in the file. `skip a,` and `wait first of a,` split
    // over two lines are the same shape and want the same answer.
    if (!inside_group && index > 0 &&
        code_[index - 1].kind == TokenKind::kComma) {
      return true;
    }
    // Inside a bracketed group a word starts an element, never a tail: `id:` is a
    // key of the object it is in, and the modifiers of a call are outside its
    // parentheses by definition.
    if (inside_group) return false;
    if (!token.IsWord()) return false;
    // A shape has no calls and no pipelines, so nothing in one is the tail of
    // the line above it. Without this a field named `id` or `with` -- both
    // perfectly good field names, and both call modifiers -- would be indented
    // as though it continued the field above.
    if (in_dto_) return false;
    const std::string word = vocabulary::Canonical(token.text);
    if (!vocabulary::ModifierWords().contains(word) &&
        !vocabulary::BareStages().contains(word)) {
      return false;
    }
    if (index + 1 >= code_.size()) return false;
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
  bool IsDescriptionLine(size_t index) const {
    if (code_[index].kind != TokenKind::kString) return false;
    if (index + 1 >= code_.size()) return true;
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
  bool IsArgumentTail(size_t index) const {
    return index > 0 && code_[index - 1].kind == TokenKind::kComma;
  }

  /// A modifier tail gets two levels rather than one.
  ///
  /// `with "x": y` under a call is not the next step of a pipeline; it is part of
  /// the call above it, and indenting it further is what says so at a glance.
  bool IsModifierTail(size_t index) const {
    const Token& token = code_[index];
    return token.IsWord() &&
           vocabulary::ModifierWords().contains(
               vocabulary::Canonical(token.text));
  }

  /// Where the line starting at `index` goes, without deciding anything.
  int IndentOf(size_t index) const {
    if (!brackets_.empty()) {
      const Bracket& bracket = brackets_.back();
      if (code_[index].kind == Closes(bracket.kind)) return bracket.indent;
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
      return std::max(0, block_ - 1) * options_.indent;
    }
    return BlockIndent();
  }

  int IndentFor(size_t index) {
    const int indent = IndentOf(index);
    // A line that is not a continuation is the statement the next ones continue.
    if (brackets_.empty() && !ContinuesStatement(index)) {
      statement_indent_ = BlockIndent();
    }
    return indent;
  }

  /// Where a comment line goes: with the line under it.
  ///
  /// A comment explains what follows, so it takes that line's indent -- including
  /// when what follows is a continuation, which is how a note about one stage of a
  /// pipeline stays with the stage.
  int IndentForComment(size_t index) const {
    size_t next = index + 1;
    while (next < code_.size() && code_[next].kind == TokenKind::kComment) {
      ++next;
    }
    if (next >= code_.size()) return BlockIndent();
    return IndentOf(next);
  }

  /// Whether a space goes between two tokens on one line.
  bool NeedsSpace(const Token& previous, const Token& current) const {
    switch (previous.kind) {
      case TokenKind::kDot:
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
      case TokenKind::kRightParen:
      case TokenKind::kRightBracket:
      case TokenKind::kDot:
      case TokenKind::kRange:
        return false;
      case TokenKind::kRightBrace:
        return false;
      case TokenKind::kLeftParen:
        // `name(` is a call; `not (` and `if (` are a word and an expression.
        return !((previous.IsWord() && IsCalled(previous.text)) ||
                 previous.kind == TokenKind::kRightParen ||
                 previous.kind == TokenKind::kRightBracket);
      case TokenKind::kLeftBracket:
        // `list[` and `x[0]`; a list literal anywhere else takes its space --
        // including after `one of` and `default`, where the brackets hold a
        // value rather than a type's parameters.
        return !((previous.IsWord() &&
                  !vocabulary::FieldModifierWords().contains(
                      vocabulary::Canonical(previous.text))) ||
                 previous.kind == TokenKind::kRightParen ||
                 previous.kind == TokenKind::kRightBracket);
      case TokenKind::kLeftBrace:
        // A block brace always takes its space: `flow research {`.
        if (OpensBlock(current)) return true;
        // `a11.sdk.Interaction{...}` hugs its tag, because the tag and the braces
        // are one value. `| map {...}` is a stage and then a literal.
        return !tagged_braces_.contains(current.start);
      default:
        return true;
    }
  }

  void Append(const Token& token) {
    if (!line_.empty() && NeedsSpace(previous_, token)) line_.push_back(' ');
    line_.append(token.text);
    previous_ = token;
  }

  void Flush() {
    if (line_.empty() && !line_started_) return;
    OutputLine out;
    out.indent = indent_;
    out.text = std::move(line_);
    out.comment = std::move(comment_);
    out.port = std::move(port_);
    out.description = description_line_;
    description_line_ = false;
    // A line with nothing on it is not a line; a blank line is asked for by name.
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
    if (lines_.empty()) return;          // never at the top of the file
    if (blank_pending_) return;
    if (opened_block_) return;           // never straight after a `{`
    blank_pending_ = true;
  }

  /// Whether `token` is the `else` that belongs on a closing brace's line.
  static bool IsElse(const Token& token) {
    return token.IsWord() && vocabulary::Canonical(token.text) == "else";
  }

  size_t Emit(size_t index) {
    const Token& token = code_[index];
    // A block brace ends its line whatever the author did -- one statement per
    // line is the language's own rule, and `{ a -> b }` is three of them. The one
    // thing that may share a closing brace's line is the `else` it belongs to.
    const bool starts_line = index == 0 || breaks_[index] > 0 ||
                             after_block_open_ ||
                             (after_block_close_ && !IsElse(token));

    if (ClosesBlock(token)) {
      Flush();
      block_ = std::max(0, block_ - 1);
      if (!dto_blocks_.empty()) dto_blocks_.pop_back();
      in_dto_ = !dto_blocks_.empty() && dto_blocks_.back();
      // A blank line before a `}` says nothing, so it is dropped rather than
      // indented: this is the one place a break the author wrote is thrown away.
      blank_pending_ = false;
      Start(BlockIndent());
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
      if (breaks_[index] >= 2) Blank();
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
      if (breaks_[index] >= 2) Blank();
      Start(IndentFor(index));
      opened_block_ = false;
      after_block_open_ = false;
      after_block_close_ = false;
      description_line_ = brackets_.empty() && IsDescriptionLine(index);
      if (brackets_.empty() && options_.align_ports) {
        size_t after = EmitPort(index);
        if (after == index) after = EmitHeader(index);
        if (after == index && in_dto_) after = EmitField(index);
        if (after != index) return after;
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
      dto_blocks_.push_back(block_ == 0 && opens_dto_);
      opens_dto_ = false;
      ++block_;
      in_dto_ = dto_blocks_.back();
      opened_block_ = true;
      blank_pending_ = false;
      after_block_open_ = true;
      after_block_close_ = false;
      return index + 1;
    }

    // `struct Name {`: remembered here, because by the time the `{` arrives the
    // word that said what it opens is two tokens back.
    if (block_ == 0 && token.IsWord() &&
        vocabulary::Canonical(token.text) == "struct") {
      opens_dto_ = true;
    }
    Append(token);
    after_block_close_ = false;
    return index + 1;
  }

  /// The end of the logical line starting at `index`, for a declaration.
  ///
  /// A break ends it, and so does the `}` that closes the flow around it.
  size_t LineEnd(size_t index) const {
    size_t end = index + 1;
    int depth = 0;
    while (end < code_.size() && breaks_[end] == 0) {
      if (IsOpener(code_[end].kind)) ++depth;
      if (IsCloser(code_[end].kind)) {
        if (depth == 0) break;
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
    if (end - index < 2) return index;
    if (code_[index + 1].kind != TokenKind::kString) return index;

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

  /// A port declaration as its columns, or `index` unchanged if this is not one.
  size_t EmitPort(size_t index) {
    const Token& first = code_[index];
    if (!first.IsWord()) return index;
    const std::string direction = vocabulary::Canonical(first.text);
    if (direction != "in" && direction != "out") return index;

    // The whole declaration has to be on one line for its columns to mean
    // anything; anything else goes through the ordinary path.
    size_t end = LineEnd(index);
    if (end - index < 4) return index;
    if (!code_[index + 1].IsWord()) return index;
    if (code_[index + 2].kind != TokenKind::kColon) return index;

    const size_t stop = end;
    std::string comment;
    if (code_[end - 1].kind == TokenKind::kComment) {
      comment = std::string(absl::StripTrailingAsciiWhitespace(code_[end - 1].text));
      --end;
    }

    PortColumns columns;
    columns.direction = std::string(first.text);
    columns.name = absl::StrCat(code_[index + 1].text, ":");

    size_t at = index + 3;
    if (at >= end) return index;
    // The type: a quoted mimetype, or a dotted name with its parameters. It ends
    // at the first thing that is plainly not part of a type.
    std::string type;
    Token previous;
    bool first_of_type = true;
    int depth = 0;
    while (at < end) {
      const Token& token = code_[at];
      if (depth == 0) {
        if (!first_of_type) {
          if (token.kind == TokenKind::kString) break;
          if (token.IsWord() &&
              vocabulary::PortModifierWords().contains(
                  vocabulary::Canonical(token.text))) {
            break;
          }
        }
        // What a type is made of, and nothing else: a dotted name, its
        // parameters, or a quoted mimetype.
        const bool part_of_type =
            token.kind == TokenKind::kWord || token.kind == TokenKind::kDot ||
            token.kind == TokenKind::kLeftBracket ||
            (first_of_type && token.kind == TokenKind::kString);
        if (!part_of_type) break;
      }
      if (IsOpener(token.kind)) ++depth;
      if (IsCloser(token.kind)) --depth;
      if (!first_of_type && NeedsSpace(previous, token)) type.push_back(' ');
      type.append(token.text);
      previous = token;
      first_of_type = false;
      ++at;
      if (depth == 0 && code_[at - 1].kind == TokenKind::kString) break;
    }
    // What the port is like, in the order it was written.
    while (at < end && code_[at].IsWord() &&
           vocabulary::PortModifierWords().contains(
               vocabulary::Canonical(code_[at].text))) {
      absl::StrAppend(&type, " ", code_[at].text);
      ++at;
    }
    if (at < end && code_[at].kind == TokenKind::kString) {
      columns.description = std::string(code_[at].text);
      ++at;
    }
    // Anything left over is a shape this does not know; leave the line alone.
    if (at != end) return index;

    columns.type = std::move(type);
    port_ = std::move(columns);
    comment_ = std::move(comment);
    Flush();
    opened_block_ = false;
    // Past the declaration and the comment that shared its line, and no further:
    // a comment on the *next* line is the next line's.
    return stop;
  }

  /// Whether a field modifier takes a value, so the thing after it is that value
  /// rather than the field's description.
  static bool TakesAnArgument(const Token& token) {
    if (!token.IsWord()) return false;
    const std::string word = vocabulary::Canonical(token.text);
    return word == "matching" || word == "default";
  }

  /// A `struct` field as its columns, or `index` unchanged if this is not one.
  ///
  /// The same three columns a port has, minus the direction: a field opens with
  /// its own name. Everything after the type is one column, as a header's tail
  /// is, because what a reader scans down a shape is the list of names and their
  /// types -- not which of them happens to carry a pattern.
  size_t EmitField(size_t index) {
    if (!code_[index].IsWord()) return index;
    if (index + 1 >= code_.size() ||
        code_[index + 1].kind != TokenKind::kColon) {
      return index;
    }
    const size_t stop = LineEnd(index);
    size_t end = stop;
    if (end - index < 3) return index;

    std::string comment;
    if (code_[end - 1].kind == TokenKind::kComment) {
      comment =
          std::string(absl::StripTrailingAsciiWhitespace(code_[end - 1].text));
      --end;
    }
    // A description ends the line; anything before it is the type and what
    // bounds it. Read from the right, and only where the string is not the
    // argument of the word in front of it: `matching "^x"` and `default "page"`
    // both end in a string that is not a description.
    std::string description;
    if (end > index + 2 && code_[end - 1].kind == TokenKind::kString &&
        code_[end - 2].kind != TokenKind::kColon &&
        !TakesAnArgument(code_[end - 2])) {
      description = std::string(code_[end - 1].text);
      --end;
    }
    if (end <= index + 2) return index;

    PortColumns columns;
    columns.field = true;
    columns.name = absl::StrCat(code_[index].text, ":");
    Token previous;
    for (size_t at = index + 2; at < end; ++at) {
      if (at > index + 2 && NeedsSpace(previous, code_[at])) {
        columns.type.push_back(' ');
      }
      columns.type.append(code_[at].text);
      previous = code_[at];
    }
    columns.description = std::move(description);
    port_ = std::move(columns);
    comment_ = std::move(comment);
    Flush();
    opened_block_ = false;
    return stop;
  }

  /// The lines, with each run of port declarations lined up.
  std::string Render() {
    // A run is consecutive declarations at one indent with nothing between them:
    // a blank line or a comment means the author grouped them on purpose, and two
    // groups line up their own columns.
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
      if (line.blank_before) out.push_back('\n');
      std::string text = absl::StrCat(
          std::string(static_cast<size_t>(line.indent), ' '), line.text);
      if (!line.comment.empty()) {
        // Two spaces before a comment that shares its line with code, which is
        // the convention every language that has one uses.
        absl::StrAppend(&text, text.empty() ? "" : "  ", line.comment);
      }
      absl::StrAppend(&out, absl::StripTrailingAsciiWhitespace(text), "\n");
    }
    return out;
  }

  /// Whether a declaration line is a `header` rather than a port.
  ///
  /// A run is one or the other: they are different lists, and lining a header up
  /// with a port above it would say they belonged together.
  static bool IsHeaderLine(const OutputLine& line) {
    return line.port.has_value() &&
           vocabulary::Canonical(line.port->direction) == "header";
  }

  void AlignPorts(size_t begin, size_t end) {
    // Three for a port whatever this run holds, so `in` and `out` line up across
    // two groups of them as well as within one. A field has no such word, and a
    // header's is only ever `header`.
    const bool fields = lines_[begin].port->field;
    size_t direction_width =
        fields || IsHeaderLine(lines_[begin]) ? 0 : 3;
    size_t name_width = 0;
    size_t type_width = 0;
    bool any_description = false;
    for (size_t index = begin; index < end; ++index) {
      if (!lines_[index].port.has_value()) continue;  // a description line
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
      if (!lines_[index].port.has_value()) continue;
      const PortColumns& port = *lines_[index].port;
      std::string text = port.direction;
      text.append(direction_width - port.direction.size(), ' ');
      // A field has no leading word, so it has no leading space either.
      if (direction_width > 0) text.push_back(' ');
      absl::StrAppend(&text, port.name);
      text.append(name_width - port.name.size(), ' ');
      text.push_back(' ');
      absl::StrAppend(&text, port.type);
      if (any_description && !port.description.empty()) {
        text.append(type_width > port.type.size()
                        ? type_width - port.type.size()
                        : 0,
                    ' ');
        absl::StrAppend(&text, " ", port.description);
      }
      lines_[index].text = std::move(text);
    }
  }

  struct Bracket {
    TokenKind kind;
    int indent;
  };

  std::string_view source_;
  FormatOptions options_;
  absl::flat_hash_set<size_t> value_braces_;
  absl::flat_hash_set<size_t> tagged_braces_;

  std::vector<Token> code_;
  std::vector<int> breaks_;

  std::vector<OutputLine> lines_;
  std::vector<Bracket> brackets_;
  /// Whether each open block is a `struct` body, innermost last.
  std::vector<bool> dto_blocks_;
  /// Whether the `struct` that opens the block about to start was just read.
  bool opens_dto_ = false;
  /// Whether a field may be read here: see [dto_blocks_].
  bool in_dto_ = false;
  std::string line_;
  std::string comment_;
  std::optional<PortColumns> port_;
  Token previous_;
  bool line_started_ = false;
  int indent_ = 0;
  int block_ = 0;
  int statement_indent_ = 0;
  bool opened_block_ = false;
  bool blank_pending_ = false;
  /// Whether the last token emitted was a block `{`, or a block `}`.
  bool after_block_open_ = false;
  bool after_block_close_ = false;
  /// Whether the line being built is a description on a line of its own.
  bool description_line_ = false;
};

}  // namespace

FormatResult Format(std::string_view source, FormatOptions options) {
  FormatResult result;
  LexResult lexed = Lex(source, LexOptions{.keep_comments = true});
  ParseResult parsed = ParseTokens(source, lexed.tokens, lexed.diagnostics);
  result.diagnostics = parsed.diagnostics;
  if (parsed.HasErrors()) {
    // A file that cannot be read is a file left exactly as it is.
    result.formatted = std::string(source);
    return result;
  }

  result.formatted = Formatter(source, lexed.tokens, parsed.value_braces,
                               parsed.tagged_braces, options)
                         .Run();
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
