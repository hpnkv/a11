// Copyright 2026 The A11 Authors.

#include "a11/flow/parser.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/types/span.h>

#include "a11/flow/diagnostic.h"
#include "a11/flow/lexer.h"
#include "a11/flow/syntax.h"
#include "a11/flow/token.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow {
namespace {

using syntax::Location;
using syntax::Node;
using syntax::NodePtr;
using syntax::Word;

/// A piece of text as a message quotes it.
///
/// Single quotes, the way the Python reference's `repr` writes them, so the
/// sentences the two implementations produce read alike. Control characters are
/// escaped, because a message that spans two lines is a message a build log
/// mangles.
std::string Quoted(std::string_view text) {
  std::string quoted;
  quoted.reserve(text.size() + 2);
  quoted.push_back('\'');
  for (const char letter : text) {
    switch (letter) {
      case '\n':
        quoted += "\\n";
        break;
      case '\t':
        quoted += "\\t";
        break;
      case '\r':
        quoted += "\\r";
        break;
      case '\'':
        quoted += "\\'";
        break;
      case '\\':
        quoted += "\\\\";
        break;
      default:
        quoted.push_back(letter);
        break;
    }
  }
  quoted.push_back('\'');
  return quoted;
}

/// The words the grammar reads as opening a statement rather than naming one.
bool OpensStatementWord(std::string_view canonical) {
  return vocabulary::StatementWords().contains(canonical);
}

/// Recursive descent over one token stream, collecting what it cannot read.
///
/// The shape follows `a11/flow/parser.py` method for method, so the two can be
/// read side by side while the Python one is still the reference. What differs
/// is the failure path: every `raise` there is a [Report] and a recovery here.
class ParserImpl {
 public:
  ParserImpl(std::string_view source, absl::Span<const Token> tokens,
             std::vector<Diagnostic> diagnostics)
      : lines_(source) {
    result_.diagnostics = std::move(diagnostics);
    // A parser has no use for comments, and stepping over them here rather than
    // at every lookahead is what lets one lex serve a highlighter and a parser
    // both.
    tokens_.reserve(tokens.size());
    for (const Token& token : tokens) {
      if (token.kind != TokenKind::kComment) {
        tokens_.push_back(token);
      }
    }
    if (tokens_.empty() || tokens_.back().kind != TokenKind::kEnd) {
      Token end;
      end.kind = TokenKind::kEnd;
      end.start = source.size();
      end.end = source.size();
      tokens_.push_back(end);
    }
  }

  ParseResult Run() {
    ParseProgram();
    // Recorded as they were read, which is source order for everything but a
    // nested literal; sorted so a consumer can binary-search.
    std::sort(result_.value_braces.begin(), result_.value_braces.end());
    std::sort(result_.tagged_braces.begin(), result_.tagged_braces.end());
    return std::move(result_);
  }

 private:
  // -- token helpers ---------------------------------------------------------

  [[nodiscard]] const Token& Current() const { return tokens_[position_]; }

  [[nodiscard]] const Token& Peek(size_t offset = 1) const {
    return tokens_[std::min(position_ + offset, tokens_.size() - 1)];
  }

  const Token& Advance() {
    const Token& token = tokens_[position_];
    if (token.kind != TokenKind::kEnd) {
      ++position_;
    }
    return token;
  }

  [[nodiscard]] bool At(TokenKind kind) const { return Current().kind == kind; }

  // The token at `offset` as a keyword, or `""`
  /// The token at `offset` as a keyword, or `""` if it is not a word.
  ///
  /// A word written in one case throughout reads as its lower-case self, so
  /// `FOR` and `for` are the same keyword and `For` is a name.
  [[nodiscard]] std::string Keyword(size_t offset = 0) const {
    const Token& token = offset == 0 ? Current() : Peek(offset);
    if (!token.IsWord()) {
      return "";
    }
    return vocabulary::Canonical(token.text);
  }

  [[nodiscard]] bool AtWord(std::string_view word) const {
    return Current().IsWord() && Keyword() == word;
  }

  [[nodiscard]] bool AtWord(std::string_view first,
                            std::string_view second) const {
    if (!Current().IsWord()) {
      return false;
    }
    const std::string word = Keyword();
    return word == first || word == second;
  }

  void SkipNewlines() {
    while (At(TokenKind::kNewline)) {
      ++position_;
    }
  }

  /// Whether the next real token is one of `kinds`, past line breaks.
  ///
  /// This is what lets a long pipeline wrap: a line ending just before `|` or
  /// `->` is a continuation, not the end of the statement.
  [[nodiscard]] bool ContinuesWith(TokenKind kind) const {
    size_t offset = 0;
    while (Peek(offset).kind == TokenKind::kNewline) {
      ++offset;
    }
    return Peek(offset).kind == kind;
  }

  bool AcceptToken(TokenKind kind) {
    if (Current().kind != kind) {
      return false;
    }
    Advance();
    return true;
  }

  bool AcceptWord(std::string_view word) {
    if (!AtWord(word)) {
      return false;
    }
    Advance();
    return true;
  }

  /// What is actually here, for a message.
  [[nodiscard]] std::string Found() const {
    return Quoted(Current().text.empty() ? "end of file" : Current().text);
  }

  void Report(std::string_view code, std::string message, const Token& token,
              Severity severity = Severity::kError,
              Family family = Family::kSyntax) {
    Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = severity;
    diagnostic.family = family;
    diagnostic.message = std::move(message);
    diagnostic.range = lines_.Between(token.start, token.end);
    diagnostic.flow = flow_name_;
    result_.diagnostics.push_back(std::move(diagnostic));
  }

  void ReportHere(std::string_view code, std::string message,
                  Family family = Family::kSyntax) {
    Report(code, std::move(message), Current(), Severity::kError, family);
  }

  /// Require a token, or say what was wanted and leave the position alone.
  ///
  /// Not consuming on failure is deliberate: the token that is there belongs to
  /// whatever comes next, and eating it would turn one mistake into two.
  bool Expect(TokenKind kind, std::string_view what = "") {
    if (Current().kind == kind) {
      Advance();
      return true;
    }
    ReportHere("flow.syntax.unexpected",
               absl::StrCat("Expected ", what.empty() ? KindName(kind) : what,
                            ", found ", Found(), "."));
    return false;
  }

  bool ExpectWord(std::string_view word) {
    if (AtWord(word)) {
      Advance();
      return true;
    }
    ReportHere(
        "flow.syntax.unexpected",
        absl::StrCat("Expected ", Quoted(word), ", found ", Found(), "."));
    return false;
  }

  bool ExpectWord(std::string_view first, std::string_view second) {
    if (AtWord(first, second)) {
      Advance();
      return true;
    }
    ReportHere("flow.syntax.unexpected",
               absl::StrCat("Expected ", Quoted(first), " or ", Quoted(second),
                            ", found ", Found(), "."));
    return false;
  }

  /// A bare name, or an empty [Word] where there is none.
  Word ExpectName(std::string_view what = "a name") {
    if (!Current().IsWord()) {
      ReportHere("flow.syntax.unexpected",
                 absl::StrCat("Expected ", what, ", found ", Found(), "."));
      return Word{"", syntax::LocationOf(Current())};
    }
    const Token& token = Advance();
    return Word{std::string(token.text), syntax::LocationOf(token)};
  }

  /// Step to the end of the statement being read, and past it.
  ///
  /// The recovery point of the whole grammar, and it is the right one because
  /// the language is one statement per line: whatever went wrong, the next line
  /// is a statement again, so a mistake costs its own line and nothing more.
  void Recover() {
    while (!Current().EndsStatement()) {
      Advance();
    }
    if (At(TokenKind::kNewline)) {
      SkipNewlines();
    }
  }

  /// Require the end of a statement: a line break, a `}`, or the file.
  void EndStatement() {
    if (At(TokenKind::kNewline)) {
      SkipNewlines();
      return;
    }
    if (At(TokenKind::kRightBrace) || At(TokenKind::kEnd)) {
      return;
    }
    ReportHere("flow.syntax.statement-end",
               absl::StrCat("Unexpected ", Found(),
                            " after a complete statement; one statement per "
                            "line."));
    Recover();
  }

  template <typename T>
  std::unique_ptr<T> Make(const Token& token) {
    auto node = std::make_unique<T>();
    node->location = syntax::LocationOf(token);
    return node;
  }

  /**
   * @brief How deep a document may nest before the parser stops descending.
   *
   * The parse is recursive descent, so the call depth follows the input's
   * nesting -- and A11 runs its work on pooled fibres whose stacks are fixed
   * and small (see thread/thread_pool.cc). Without a bound, `[[[[[...]]]]]` in
   * a file is a stack overflow rather than a diagnostic, and a flow can arrive
   * from anywhere: an editor buffer, a `scan` of a repository, a request.
   *
   * 128 is far past anything written by hand and far short of the smallest
   * fibre stack. Every walker downstream -- the resolver, the JSON emitters,
   * the formatter -- inherits the bound, because what they descend is the tree
   * this built.
   */
  static constexpr int kMaxNesting = 128;

  /// Counts one level of descent, and gives it back on the way out.
  class Descent {
   public:
    explicit Descent(ParserImpl& parser) : parser_(&parser) {
      ++parser_->depth_;
    }

    ~Descent() { --parser_->depth_; }

    Descent(const Descent&) = delete;
    Descent& operator=(const Descent&) = delete;
    Descent(Descent&&) = delete;
    Descent& operator=(Descent&&) = delete;

   private:
    ParserImpl* parser_;
  };

  /// Whether the parser has descended as far as it will go.
  ///
  /// Reports once per document: past the bound every construct would say the
  /// same thing, and a reader needs the first one.
  bool TooDeep() {
    if (depth_ <= kMaxNesting) {
      return false;
    }
    if (!reported_too_deep_) {
      reported_too_deep_ = true;
      Report("flow.syntax.nesting-too-deep",
             absl::StrCat("This nests more than ", kMaxNesting,
                          " levels deep, which is deeper than the language "
                          "reads. Give the inner part a name and refer to it."),
             Current());
    }
    return true;
  }

  /// A stand-in for a value that could not be read.
  NodePtr MakeError(const Token& token, std::string expected) {
    auto node = Make<syntax::ErrorNode>(token);
    node->expected = std::move(expected);
    return node;
  }

  // -- program ---------------------------------------------------------------

  void ParseProgram() {
    SkipNewlines();
    while (!At(TokenKind::kEnd)) {
      const size_t before = position_;
      if (AtWord("struct") && Peek().kind == TokenKind::kWord) {
        const Token& keyword = Advance();
        result_.dtos.push_back(ParseDto(keyword));
        SkipNewlines();
        if (position_ == before) {
          Advance();
        }
        continue;
      }
      if (!ExpectWord("flow")) {
        // Not a declaration at all. The line is skipped rather than read as
        // one, so a stray statement outside a flow costs one diagnostic.
        Recover();
        if (position_ == before) {
          Advance();
        }
        continue;
      }
      const Token& keyword = tokens_[position_ - 1];
      result_.flows.push_back(ParseFlow(keyword));
      SkipNewlines();
      if (position_ == before) {
        Advance();
      }
    }
    if (result_.flows.empty() && result_.dtos.empty()) {
      ReportHere("flow.syntax.unexpected",
                 "A flow file must declare at least one flow or struct.");
    }
  }

  // `struct Name { describe ".." field: type modifiers ".." ...
  /// `struct Name { describe ".."  field: type modifiers ".." ... }`.
  ///
  /// The body is fields and nothing else: a shape has no statements, so
  /// anything that is not a field is one diagnostic and one skipped line rather
  /// than a statement parse that would go badly wrong.
  syntax::DtoDeclarationPtr ParseDto(const Token& keyword) {
    auto declaration = Make<syntax::DtoDeclaration>(keyword);
    declaration->name = ExpectName("a struct name");
    const std::string outer_flow = flow_name_;
    flow_name_ = declaration->name.text;
    if (!Expect(TokenKind::kLeftBrace)) {
      Recover();
      flow_name_ = outer_flow;
      return declaration;
    }
    SkipNewlines();
    while (!At(TokenKind::kRightBrace)) {
      if (At(TokenKind::kEnd)) {
        ReportHere("flow.syntax.unclosed",
                   absl::StrCat("Struct ", Quoted(declaration->name.text),
                                " is missing its closing '}'."));
        break;
      }
      const size_t before = position_;
      if (AtWord("describe") && DescribeFollows()) {
        Advance();
        declaration->description = ParseDescription();
      } else if (Current().IsWord() && Peek().kind == TokenKind::kColon) {
        declaration->fields.push_back(ParseField());
      } else {
        ReportHere("flow.syntax.unexpected",
                   absl::StrCat("Expected a field ('name: type'), found ",
                                Found(), "."));
        Recover();
      }
      EndStatement();
      if (position_ == before) {
        Advance();
      }
    }
    AcceptToken(TokenKind::kRightBrace);
    flow_name_ = outer_flow;
    return declaration;
  }

  // `name: type [required] [unique] [a..b] [matching ".."] [one of [..]]
  // [default v] ["description"]`.
  /// `name: type [required] [unique] [a..b] [matching ".."] [one of [..]]
  ///  [default v] ["description"]`.
  ///
  /// The modifiers are read in a loop and their order checked against the
  /// vocabulary's, so the table says what the order is and this only enforces
  /// it. Out of order is a diagnostic and not a refusal: the field still means
  /// what it plainly says, and an editor should say so while it is being typed.
  syntax::FieldDeclarationPtr ParseField() {
    const Token& start = Current();
    auto field = Make<syntax::FieldDeclaration>(start);
    field->name = ExpectName("a field name");
    Expect(TokenKind::kColon);
    field->type = ParseType();

    int reached = -1;
    const auto ordered = [&](std::string_view modifier, const Token& at) {
      const absl::Span<const std::string_view> order =
          vocabulary::OrderedFieldModifiers();
      int rank = 0;
      for (size_t index = 0; index < order.size(); ++index) {
        if (order[index] == modifier) {
          rank = static_cast<int>(index);
        }
      }
      if (rank < reached) {
        // Reached at least one modifier to be out of order with, so the index
        // is not the initial -1.
        Report("flow.form.field-modifier-order",
               absl::StrCat(Quoted(modifier), " is written before ",
                            Quoted(order[static_cast<size_t>(reached)]),
                            "; a field's modifiers read ",
                            absl::StrJoin(order, ", "), "."),
               at, Severity::kWarning, Family::kForm);
      }
      reached = std::max(reached, rank);
    };

    while (true) {
      const Token& at = Current();
      if (At(TokenKind::kRange) || At(TokenKind::kNumber) ||
          At(TokenKind::kDuration)) {
        if (!ParseFieldRange(*field)) {
          break;
        }
        continue;
      }
      if (!Current().IsWord()) {
        break;
      }
      const std::string word = Keyword();
      if (word == "required") {
        Advance();
        ordered("required", at);
        field->required = true;
        continue;
      }
      if (word == "unique") {
        Advance();
        ordered("unique", at);
        field->unique = true;
        continue;
      }
      if (word == "matching") {
        Advance();
        ordered("matching", at);
        if (At(TokenKind::kString)) {
          // One literal, not a run: `matching "re" "why"` is a pattern and a
          // description, and adjacent strings joining here would eat the
          // description instead.
          field->pattern = std::string(Advance().string_value);
          field->has_pattern = true;
        } else {
          ReportHere("flow.syntax.unexpected",
                     absl::StrCat("Expected a quoted pattern after 'matching', "
                                  "found ",
                                  Found(), "."));
        }
        continue;
      }
      if (word == "one" && Keyword(1) == "of") {
        Advance();
        Advance();
        ordered("one of", at);
        ParseFieldEnumeration(*field);
        continue;
      }
      if (word == "default") {
        Advance();
        ordered("default", at);
        const Token& value_at = Current();
        NodePtr value = ParseExpression();
        std::optional<syntax::Constant> constant =
            syntax::ConstantValue(value.get());
        if (constant.has_value()) {
          field->default_value = *std::move(constant);
          field->has_default = true;
        } else if (value->kind != syntax::NodeKind::kError) {
          Report("flow.syntax.constant-required",
                 "A field's default is a constant value.", value_at);
        }
        continue;
      }
      break;
    }
    field->description = ParseDescription();
    return field;
  }

  /// `1..200`, `1..`, `..200` -- read where a field's modifiers are.
  ///
  /// False when what is here turned out not to be a range after all, so the
  /// modifier loop can stop rather than spin.
  bool ParseFieldRange(syntax::FieldDeclaration& field) {
    const Token& start = Current();
    syntax::FieldRange range;
    if (!At(TokenKind::kRange)) {
      const Token& low = Advance();
      std::optional<syntax::Constant> minimum = BoundOf(low);
      if (!minimum.has_value()) {
        return false;
      }
      if (!At(TokenKind::kRange)) {
        Report("flow.syntax.unexpected",
               absl::StrCat("Expected '..' after ", Quoted(low.text),
                            " to make it a range."),
               low);
        return false;
      }
      range.minimum = *std::move(minimum);
      range.has_minimum = true;
    }
    Expect(TokenKind::kRange);
    if (At(TokenKind::kNumber) || At(TokenKind::kDuration)) {
      const Token& high = Advance();
      std::optional<syntax::Constant> maximum = BoundOf(high);
      if (maximum.has_value()) {
        range.maximum = *std::move(maximum);
        range.has_maximum = true;
      }
    }
    if (range.Empty()) {
      Report("flow.form.empty-range",
             "A range bounds something: write '1..', '..200' or '1..200'.",
             start, Severity::kError, Family::kForm);
      return true;
    }
    if (!field.range.Empty()) {
      Report("flow.form.repeated-modifier", "A field has one range.", start,
             Severity::kError, Family::kForm);
      return true;
    }
    field.range = std::move(range);
    return true;
  }

  /// The constant a range's bound is, or nothing where the token is not one.
  static std::optional<syntax::Constant> BoundOf(const Token& token) {
    if (token.kind == TokenKind::kDuration) {
      return syntax::Constant::Duration(token.duration);
    }
    if (token.kind != TokenKind::kNumber) {
      return std::nullopt;
    }
    return token.is_integer
               ? syntax::Constant::Integer(static_cast<long long>(token.number))
               : syntax::Constant::Double(token.number);
  }

  void ParseFieldEnumeration(syntax::FieldDeclaration& field) {
    const Token& at = Current();
    NodePtr value = ParseExpression();
    std::optional<syntax::Constant> constant =
        syntax::ConstantValue(value.get());
    if (!constant.has_value()) {
      if (value->kind != syntax::NodeKind::kError) {
        Report("flow.syntax.constant-required",
               "'one of' takes a list of constant values.", at);
      }
      return;
    }
    if (constant->kind != syntax::Constant::Kind::kList) {
      Report("flow.form.one-of-not-a-list",
             R"('one of' takes a list: write 'one of ["a", "b"]'.)", at,
             Severity::kError, Family::kForm);
      return;
    }
    field.enumeration = std::move(constant->items);
    field.has_enumeration = true;
    if (field.enumeration.empty()) {
      Report("flow.form.one-of-empty",
             "'one of []' allows nothing, so nothing would validate.", at,
             Severity::kError, Family::kForm);
    }
  }

  syntax::FlowDeclarationPtr ParseFlow(const Token& keyword) {
    auto declaration = Make<syntax::FlowDeclaration>(keyword);
    // `flow { ...
    if (At(TokenKind::kLeftBrace)) {
      declaration->entry = true;
      declaration->name.location = syntax::LocationOf(keyword);
    } else {
      declaration->name = ParseDottedName("a flow name");
    }
    // The flow a diagnostic is in, for everything reported until this one ends.
    const std::string outer_flow = flow_name_;
    flow_name_ = declaration->name.text;
    if (!Expect(TokenKind::kLeftBrace)) {
      // Nothing to read the body out of. Give up on this declaration rather
      // than read the rest of the file as its statements.
      Recover();
      flow_name_ = outer_flow;
      return declaration;
    }
    SkipNewlines();
    while (!At(TokenKind::kRightBrace)) {
      if (At(TokenKind::kEnd)) {
        ReportHere("flow.syntax.unclosed",
                   declaration->entry
                       ? std::string("The entry flow is missing its closing "
                                     "'}'.")
                       : absl::StrCat("Flow ", Quoted(declaration->name.text),
                                      " is missing its closing '}'."));
        break;
      }
      const size_t before = position_;
      if (AtWord("describe") && DescribeFollows()) {
        Advance();
        declaration->description = ParseDescription();
      } else if (AtWord("in", "out") && Peek().kind == TokenKind::kWord) {
        declaration->ports.push_back(ParsePort());
      } else if (AtWord("header") && Peek().kind == TokenKind::kString) {
        declaration->headers.push_back(ParseHeader());
      } else {
        declaration->body.push_back(ParseStatement());
      }
      EndStatement();
      if (position_ == before) {
        Advance();
      }
    }
    AcceptToken(TokenKind::kRightBrace);
    flow_name_ = outer_flow;
    return declaration;
  }

  syntax::PortDeclarationPtr ParsePort() {
    const Token& keyword = Current();
    const bool input = AtWord("in");
    Advance();
    auto port = Make<syntax::PortDeclaration>(keyword);
    port->direction =
        input ? syntax::PortDirection::kInput : syntax::PortDirection::kOutput;
    port->name = ExpectName("a port name");
    Expect(TokenKind::kColon);
    port->type = ParseType();

    // `stream` and `required` are what the port is like, not what its type is,
    // so they follow the type and may be written in either order. A port
    // carries one value unless it says otherwise, because most do.
    const std::string written = vocabulary::Canonical(port->type.name);
    if (!port->type.quoted && port->type.parameters.empty() &&
        vocabulary::PortModifierWords().contains(written)) {
      Report("flow.form.port-modifier-order",
             absl::StrCat(Quoted(port->type.name), " follows the type: write '",
                          port->name.text, ": TYPE ", port->type.name, "'."),
             TokenAt(port->type.location), Severity::kError, Family::kForm);
      if (written == "stream") {
        port->unary = false;
      } else {
        port->required = true;
      }
      if (Current().IsWord() || At(TokenKind::kString)) {
        port->type = ParseType();
      }
    }
    while (true) {
      if (AcceptWord("stream")) {
        port->unary = false;
      } else if (AcceptWord("required")) {
        port->required = true;
      } else {
        break;
      }
    }
    port->description = ParseDescription();
    return port;
  }

  /// Whether a `describe` has its string, here or on the line below.
  [[nodiscard]] bool DescribeFollows() const {
    if (Peek().kind == TokenKind::kString) {
      return true;
    }
    size_t offset = 1;
    while (Peek(offset).kind == TokenKind::kNewline) {
      ++offset;
    }
    return offset > 1 && Peek(offset).kind == TokenKind::kString &&
           Peek(offset + 1).EndsStatement();
  }

  // A declaration's description: a string after it, or a string on a line of
  // its own under it.
  /// A declaration's description: a string after it, or a string on a line of
  /// its own under it.
  ///
  /// The second spelling is there because a description is prose, and prose
  /// that says anything runs past the width of the declaration it belongs to --
  /// with `"""` all the more so. It is unambiguous because the string has to be
  /// *alone* on its line: `"hello" -> out` is a statement, since something
  /// follows the string, and a line holding nothing but a string is not a
  /// statement in this language at all.
  std::string ParseDescription() {
    std::string value;
    if (At(TokenKind::kString)) {
      value = ParseStringRun();
    }
    // Then every following line that holds nothing but strings.
    while (true) {
      size_t offset = 0;
      while (Peek(offset).kind == TokenKind::kNewline) {
        ++offset;
      }
      if (offset == 0 || Peek(offset).kind != TokenKind::kString) {
        break;
      }
      // A run of strings on the line is still one description, so what has to
      // end the statement is what follows the *last* of them.
      size_t past = offset;
      while (Peek(past).kind == TokenKind::kString) {
        ++past;
      }
      if (!Peek(past).EndsStatement()) {
        break;
      }
      SkipNewlines();
      absl::StrAppend(&value, ParseStringRun());
    }
    return value;
  }

  /// One or more string literals written next to each other, as one string.
  ///
  /// Adjacent literals concatenate, the way C and Python do it, because a
  /// description or a pattern that says anything is longer than the line it is
  /// written on and `+` at run time is the wrong tool for a constant. A run may
  /// cross line breaks only where a break means nothing anyway -- inside
  /// brackets -- so a bare string alone on the next line is still not a
  /// continuation of the statement above it.
  std::string ParseStringRun() {
    std::string value(Advance().string_value);
    while (true) {
      if (At(TokenKind::kString)) {
        absl::StrAppend(&value, Advance().string_value);
        continue;
      }
      if (brackets_ > 0 && ContinuesWith(TokenKind::kString)) {
        SkipNewlines();
        continue;
      }
      return value;
    }
  }

  /// A port's type: a name, a name with type parameters, or a string.
  ///
  /// The name may be dotted, which is how a type registered in a serialisation
  /// registry is written -- `a11.sdk.AudioBuffer` -- and the brackets are how a
  /// generic one says what it holds: `list[a11.NodeFragment]`. A quoted name is
  /// a mimetype.
  syntax::TypeExpression ParseType() {
    syntax::TypeExpression type;
    type.location = syntax::LocationOf(Current());
    if (At(TokenKind::kString)) {
      type.name = std::string(Advance().string_value);
      type.quoted = true;
      return type;
    }
    type.name = ParseDottedName("a port type").text;
    // Brackets with something inside them are the parameters a generic type is
    // given; empty ones are the `T[]` sugar, and are read by the loop below.
    if (At(TokenKind::kLeftBracket) &&
        Peek().kind != TokenKind::kRightBracket) {
      Advance();
      while (true) {
        type.parameters.push_back(ParseType());
        if (!AcceptToken(TokenKind::kComma)) {
          break;
        }
      }
      Expect(TokenKind::kRightBracket, "']' after the type parameters");
    }
    // `T[][]` is a list of lists, so the suffix is read in a loop rather than
    // once: each `[]` wraps what has been read so far.
    while (At(TokenKind::kLeftBracket) &&
           Peek().kind == TokenKind::kRightBracket) {
      type = ArrayOf(std::move(type));
    }
    return type;
  }

  /// `T[]` -- the same type `list[T]` names, having eaten the brackets.
  ///
  /// Sugar rather than a second spelling in the tree: everything downstream
  /// sees a `list`, so nothing but the formatter has to know both ways of
  /// writing it. `sugared` is what tells the formatter to write it back the way
  /// it was.
  syntax::TypeExpression ArrayOf(syntax::TypeExpression element) {
    Advance();
    const Token& closing = Advance();
    syntax::TypeExpression list;
    list.location = element.location;
    list.location.end = closing.end;
    list.name = "list";
    list.sugared = true;
    list.parameters.push_back(std::move(element));
    return list;
  }

  syntax::HeaderDeclarationPtr ParseHeader() {
    const Token& keyword = Current();
    ExpectWord("header");
    auto header = Make<syntax::HeaderDeclaration>(keyword);
    if (At(TokenKind::kString)) {
      header->name = std::string(Advance().string_value);
    } else {
      ReportHere("flow.syntax.unexpected",
                 absl::StrCat("Expected a header name, found ", Found(), "."));
    }
    std::string alias = header->name;
    for (char& letter : alias) {
      if (letter == '-' || letter == '.') {
        letter = '_';
      }
    }
    header->alias = Word{alias, syntax::LocationOf(keyword)};
    if (AcceptWord("as")) {
      header->alias = ExpectName("a header alias");
    }
    if (AcceptWord("default")) {
      const Token& at = Current();
      NodePtr value = ParseExpression();
      std::optional<syntax::Constant> constant =
          syntax::ConstantValue(value.get());
      if (constant.has_value()) {
        header->default_value = *std::move(constant);
        header->has_default = true;
      } else if (value->kind != syntax::NodeKind::kError) {
        Report("flow.syntax.constant-required", "Expected a constant value.",
               at);
      }
    }
    header->description = ParseDescription();
    return header;
  }

  // -- statements ------------------------------------------------------------

  std::vector<NodePtr> ParseBlock() {
    std::vector<NodePtr> body;
    if (!Expect(TokenKind::kLeftBrace)) {
      return body;
    }
    SkipNewlines();
    while (!At(TokenKind::kRightBrace)) {
      if (At(TokenKind::kEnd)) {
        ReportHere("flow.syntax.unclosed", "Missing '}'.");
        return body;
      }
      const size_t before = position_;
      body.push_back(ParseStatement());
      EndStatement();
      if (position_ == before) {
        Advance();
      }
    }
    Advance();
    return body;
  }

  /// Whether the `{` here opens a block of statements rather than a record.
  ///
  /// Both begin the same way and both are statements: `{"a": 1} -> out` writes
  /// a record to a port, and `{ "one" -> out }` is a block that writes a
  /// string. What tells them apart is what follows the first key: a record's
  /// keys are strings followed by `:`, and a spread is only ever a record's.
  /// Everything else opens statements. `{}` is the empty record, because
  /// somebody writes that and nobody writes an empty block.
  [[nodiscard]] bool OpensBlock() const {
    if (!At(TokenKind::kLeftBrace)) {
      return false;
    }
    switch (Peek().kind) {
      case TokenKind::kRightBrace:
      case TokenKind::kSpread:
        return false;
      case TokenKind::kString:
        return Peek(2).kind != TokenKind::kColon;
      default:
        return true;
    }
  }

  /// `{ ... }` as a statement, with `try` already read where it was written.
  NodePtr ParseBlockStatement(const Token& opener, bool tolerant) {
    auto block = Make<syntax::Block>(opener);
    block->tolerant = tolerant;
    block->body = ParseBlock();
    return block;
  }

  /// Whether a statement-opening word is used as a keyword here.
  [[nodiscard]] bool OpensStatement(std::string_view word) const {
    if (!OpensStatementWord(word)) {
      return false;
    }
    // `skip -> out` and `wait | count -> n` treat the word as a name.
    switch (Peek().kind) {
      case TokenKind::kArrow:
      case TokenKind::kPipe:
      case TokenKind::kEqual:
      case TokenKind::kCarry:
      case TokenKind::kDot:
      case TokenKind::kLeftBracket:
        return false;
      default:
        return true;
    }
  }

  NodePtr ParseStatement() {
    const Descent descent(*this);
    if (TooDeep()) {
      // Past the bound nothing more can be read, and the caller's loop needs a
      // token consumed or it spins.
      Advance();
      return MakeError(Current(), "a statement");
    }
    const Token& keyword = Current();
    if (keyword.IsWord()) {
      const std::string word = Keyword();
      if (OpensStatement(word)) {
        if (word == "run" || word == "call" || word == "try") {
          if (word == "try" && Peek().kind == TokenKind::kLeftBrace) {
            Advance();
            return ParseBlockStatement(keyword, /*tolerant=*/true);
          }
          if (word == "try" && !NextWordIsCallVerb()) {
            Advance();
            return ParsePipeStatement(keyword, /*tolerant=*/true);
          }
          auto statement = Make<syntax::CallStatement>(keyword);
          statement->call = ParseCall();
          return statement;
        }
        if (word == "let") {
          Advance();
          auto let = Make<syntax::Let>(keyword);
          let->names.push_back(ExpectName("a name for the value"));
          // Several names take the value apart, the way a `for` does.
          while (AcceptToken(TokenKind::kComma)) {
            let->names.push_back(ExpectName("another name for a part of it"));
          }
          Expect(TokenKind::kEqual, "'=' and the stream to read one value of");
          let->pipeline = ParsePipeline();
          return let;
        }
        if (word == "advance") {
          Advance();
          auto advance = Make<syntax::Advance>(keyword);
          advance->name = ExpectName("the value to advance");
          return advance;
        }
        if (word == "skip") {
          Advance();
          return ParseSkip(keyword);
        }
        if (word == "wait") {
          Advance();
          // `wait first of a, b` holds for whichever finishes first and lets
          // the others carry on; `wait all of a, b` is the plural of the plain
          // form. Anything else is one subject, which is what most waits are.
          const bool of = (AtWord("first") || AtWord("all")) &&
                          Peek().IsWord() &&
                          vocabulary::Canonical(Peek().text) == "of";
          syntax::NodePtr held;
          syntax::Wait* wait = nullptr;
          if (of) {
            held = ParseWaitOf(keyword);
            // Built by ParseWaitOf one line up, so its kind is not in
            // question.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            wait = static_cast<syntax::Wait*>(held.get());
          } else {
            auto one = Make<syntax::Wait>(keyword);
            one->subject = ParseReference();
            wait = one.get();
            held = std::move(one);
          }
          if (AcceptWord("timeout")) {
            wait->timeout = ExpectDuration();
          }
          // `wait first of a, b -> n` writes the winner's number, exactly as a
          // pipe writes a value. Only a race has one to write.
          if (ContinuesWith(TokenKind::kArrow)) {
            SkipNewlines();
          }
          if (AcceptToken(TokenKind::kArrow)) {
            wait->targets.push_back(ParseReference());
            while (AcceptToken(TokenKind::kComma)) {
              SkipNewlines();
              wait->targets.push_back(ParseReference());
            }
          }
          wait->after = ParseAfter();
          return held;
        }
        if (word == "drain") {
          Advance();
          auto drain = Make<syntax::Drain>(keyword);
          drain->target = ParseReference();
          drain->after = ParseAfter();
          return drain;
        }
        if (word == "cancel") {
          Advance();
          auto cancel = Make<syntax::Cancel>(keyword);
          cancel->name = ExpectName("a call name");
          cancel->after = ParseAfter();
          return cancel;
        }
        if (word == "fail") {
          Advance();
          return ParseFail(keyword);
        }
        if (word == "abort") {
          Advance();
          auto abort = Make<syntax::Abort>(keyword);
          abort->target = ParseReference();
          // The same tail `fail` takes, because it is the same question: which
          // status, and what to say about it.
          if (!AtStatementEnd()) {
            abort->code = ParseExpression();
          }
          if (!AtStatementEnd()) {
            abort->message = ParseExpression();
          }
          abort->after = ParseAfter();
          return abort;
        }
        if (word == "log" || word == "logf") {
          Advance();
          auto log = Make<syntax::Log>(keyword);
          log->tail = ParseLogTail(/*formatted=*/word == "logf",
                                   /*in_stage=*/false);
          log->after = ParseAfter();
          return log;
        }
        if (word == "for") {
          return ParseForEach();
        }
        if (word == "repeat") {
          return ParseRepeat();
        }
        if (word == "until" || word == "while") {
          Advance();
          auto until = Make<syntax::Until>(keyword);
          until->condition = ParseExpression();
          until->stop_when = word == "until";
          return until;
        }
        if (word == "if") {
          return ParseIf();
        }
        if (word == "nodes") {
          Advance();
          auto nodes = Make<syntax::Nodes>(keyword);
          nodes->name = ExpectName("a node map name");
          if (At(TokenKind::kLeftBrace)) {
            nodes->has_body = true;
            nodes->body = ParseBlock();
          }
          return nodes;
        }
      }

      if (Peek().kind == TokenKind::kEqual) {
        auto bind = Make<syntax::Bind>(keyword);
        bind->name =
            Word{std::string(Advance().text), syntax::LocationOf(keyword)};
        Advance();
        if (AtWord("node")) {
          bind->value = ParseNode();
        } else if (AtWord("wait", "drain") || AtWord("for", "repeat")) {
          // A loop, bound to a name, reads as its own outcome -- the same thing
          // a bound `wait`/`drain`/block does, so it takes the same route.
          bind->value = ParseStatement();
        } else if (AtWord("try") && Peek().kind != TokenKind::kLeftBrace &&
                   !NextWordIsCallVerb()) {
          // `p = try src -> dest`: the name reads how the pipe went, which is
          // the only way a tolerated failure is anything but silence.
          bind->value = ParseStatement();
        } else if (At(TokenKind::kLeftBrace) ||
                   (AtWord("try") && Peek().kind == TokenKind::kLeftBrace)) {
          // Bound, so a `{` here is a block whatever it holds: a record bound
          // to a name is not a statement this language has.
          const Token& opener = Current();
          const bool tolerant = AcceptWord("try");
          bind->value = ParseBlockStatement(opener, tolerant);
        } else {
          bind->value = ParseCall();
        }
        return bind;
      }

      if (Peek().kind == TokenKind::kCarry) {
        auto carry = Make<syntax::Carry>(keyword);
        carry->name =
            Word{std::string(Advance().text), syntax::LocationOf(keyword)};
        Advance();
        carry->pipeline = ParsePipeline();
        return carry;
      }
    }

    if (OpensBlock()) {
      return ParseBlockStatement(keyword, /*tolerant=*/false);
    }

    return ParsePipeStatement(keyword, /*tolerant=*/false);
  }

  /// `[try] source [| stage ..] -> dest[, dest ..] [after ..]`.
  ///
  /// One routine so the plain and the tolerated form cannot drift: `try` in
  /// front changes what a failure *means* and nothing about the grammar.
  NodePtr ParsePipeStatement(const Token& keyword, bool tolerant) {
    auto pipe = Make<syntax::Pipe>(keyword);
    pipe->tolerant = tolerant;
    pipe->pipeline = ParsePipeline();
    if (ContinuesWith(TokenKind::kArrow)) {
      SkipNewlines();
    }
    if (!Expect(TokenKind::kArrow, "'->' and a destination port")) {
      return pipe;
    }
    pipe->targets.push_back(ParseReference());
    while (AcceptToken(TokenKind::kComma)) {
      SkipNewlines();
      pipe->targets.push_back(ParseReference());
    }
    pipe->after = ParseAfter();
    return pipe;
  }

  /// Whether the word after this one is `run` or `call`.
  ///
  /// Distinguishes `try run foo(..)` from `try foo.out -> dest`. This is not a
  /// "word followed by `(`" test: `try zip(a, b) -> dest` is a pipe whose
  /// source is a source word, and that heuristic would read it as a call.
  [[nodiscard]] bool NextWordIsCallVerb() const {
    // From 1, because `Peek(0)` is `Current()` -- the `try` itself.
    // `NextWordIs` starts at 0 and so asks about the current token despite its
    // name, which is a trap worth not falling into twice.
    size_t offset = 1;
    while (Peek(offset).kind == TokenKind::kNewline) {
      ++offset;
    }
    const Token& token = Peek(offset);
    if (!token.IsWord()) {
      return false;
    }
    const std::string word = vocabulary::Canonical(token.text);
    return word == "run" || word == "call";
  }

  [[nodiscard]] bool NextWordIs(std::string_view word) const {
    size_t offset = 0;
    while (Peek(offset).kind == TokenKind::kNewline) {
      ++offset;
    }
    const Token& token = Peek(offset);
    return token.IsWord() && token.text == word;
  }

  // `first of a, b` / `all of a, b`, with `wait` already read. One routine
  // because a race is written in two places -- at the head of a statement, and
  // where a value is expected -- and the two must not drift.
  /// `first of a, b` / `all of a, b`, with `wait` already read.
  ///
  /// One routine because a race is written in two places -- at the head of a
  /// statement, and where a value is expected -- and the two must not drift.
  syntax::NodePtr ParseWaitOf(const Token& keyword) {
    auto wait = Make<syntax::Wait>(keyword);
    const bool first = AtWord("first");
    Advance();  // `first` or `all`
    Advance();  // `of`
    wait->race = first;
    wait->subjects.push_back(ParseReference());
    while (AcceptToken(TokenKind::kComma)) {
      SkipNewlines();
      wait->subjects.push_back(ParseReference());
    }
    if (wait->subjects.size() < 2) {
      Report("flow.form.wait-of-one",
             absl::StrCat("'wait ", first ? "first" : "all",
                          " of' waits on several subjects; with one, write "
                          "'wait ",
                          first ? "first" : "all", "'s subject on its own."),
             keyword, Severity::kWarning, Family::kForm);
    }
    return wait;
  }

  // `skip pipeline`, `skip n reference` for the first `n`, `skip a, b, c` for
  // several subjects, `skip act` for every output of a call, or `skip o1, o2 of
  // act` / `skip (o1, o2) of act` for some of them.
  /// `skip pipeline`, `skip n reference` for the first `n`, `skip a, b, c` for
  /// several subjects, `skip act` for every output of a call, or `skip o1, o2
  /// of act` / `skip (o1, o2) of act` for some of them.
  ///
  /// The counted form takes a reference rather than a pipeline because the
  /// count belongs to the node: it is the node's first `n` values that go
  /// unread, for every reader of it, which is not something a pipeline of one
  /// reader's own could say. It stays single-subject: there is no one node a
  /// count over a list of them would belong to.
  NodePtr ParseSkip(const Token& keyword) {
    auto skip = Make<syntax::Skip>(keyword);
    if (At(TokenKind::kNumber)) {
      const Token& number = Advance();
      if (!number.is_integer || number.number < 1) {
        Report("flow.form.count-not-positive",
               absl::StrCat("'skip' counts whole values, so ", number.text,
                            " is not a number of them to skip."),
               number, Severity::kError, Family::kForm);
      } else {
        skip->count = static_cast<long long>(number.number);
      }
      auto pipeline = Make<syntax::Pipeline>(Current());
      pipeline->source = ParseReference();
      syntax::SkipTarget target;
      target.pipeline = std::move(pipeline);
      skip->targets.push_back(std::move(target));
    } else if (AtOutputGroupWithoutParens()) {
      // `o1, o2 of act` with no parentheses claims the whole list: nothing
      // marks where its names end, so it cannot share the statement with
      // another target.
      skip->targets.push_back(ParseOutputGroup(/*bracketed=*/false));
    } else {
      skip->targets.push_back(ParseSkipTarget());
      while (AcceptToken(TokenKind::kComma)) {
        SkipNewlines();
        skip->targets.push_back(ParseSkipTarget());
      }
    }
    skip->after = ParseAfter();
    return skip;
  }

  /// Whether the tokens ahead are `name (, name)* of`, unparenthesized -- the
  /// one shape of an output group that has to be the entire `skip` list, since
  /// without brackets nothing else says where its names stop.
  [[nodiscard]] bool AtOutputGroupWithoutParens() const {
    if (!Current().IsWord()) {
      return false;
    }
    size_t offset = 0;
    while (true) {
      if (Peek(offset).kind != TokenKind::kWord) {
        return false;
      }
      ++offset;
      if (Peek(offset).kind != TokenKind::kComma) {
        break;
      }
      ++offset;
      while (Peek(offset).kind == TokenKind::kNewline) {
        ++offset;
      }
    }
    return Peek(offset).IsWord() && Peek(offset).text == "of";
  }

  // `(o1, o2) of act` and `(o1, o2 of act)`: which one is decided by whether
  // `of` shows up before or after the closing paren.
  /// `(o1, o2) of act` and `(o1, o2 of act)`: which one is decided by whether
  /// `of` shows up before or after the closing paren.
  ///
  /// `bracketed` says whether a leading `(` has to be consumed and a trailing
  /// `)` expected; the unparenthesized form (`o1, o2 of act`) reuses the same
  /// name-list reading with both off.
  syntax::SkipTarget ParseOutputGroup(bool bracketed) {
    syntax::SkipTarget target;
    std::optional<Bracketed> brackets;
    if (bracketed) {
      Advance();  // '('
      brackets.emplace(this);
      SkipNewlines();
    }
    target.outputs.push_back(ExpectName("an output name"));
    while (AcceptToken(TokenKind::kComma)) {
      SkipNewlines();
      target.outputs.push_back(ExpectName("an output name"));
    }
    SkipNewlines();
    if (bracketed && AtWord("of")) {
      // `(o1, o2 of act)`: the whole clause is inside the parentheses.
      Advance();
      target.call = ExpectName("the call these outputs belong to");
      SkipNewlines();
      Expect(TokenKind::kRightParen);
      return target;
    }
    if (bracketed) {
      Expect(TokenKind::kRightParen);
    }
    ExpectWord("of");
    target.call = ExpectName("the call these outputs belong to");
    return target;
  }

  /// One comma-separated item of an uncounted `skip`.
  ///
  /// A bare call name (`skip act`) parses as an ordinary pipeline, same as
  /// any other name: only the resolver knows a name is a call rather than a
  /// port, and that is where `skip act` becomes "every output of it" instead
  /// of the call-as-stream error a plain reference would otherwise be.
  syntax::SkipTarget ParseSkipTarget() {
    if (At(TokenKind::kLeftParen) && LooksLikeParenthesizedOutputGroup()) {
      return ParseOutputGroup(/*bracketed=*/true);
    }
    syntax::SkipTarget target;
    target.pipeline = ParsePipeline();
    return target;
  }

  // Whether a `(` here opens an output group (`(o1, o2) of act` or `(o1, o2 of
  // act)`) rather than an ordinary parenthesized expression or pipeline value
  // (`(a | count)`), by looking past it without consuming anything.
  /// Whether a `(` here opens an output group (`(o1, o2) of act` or
  /// `(o1, o2 of act)`) rather than an ordinary parenthesized expression or
  /// pipeline value (`(a | count)`), by looking past it without consuming
  /// anything.
  [[nodiscard]] bool LooksLikeParenthesizedOutputGroup() const {
    size_t offset = 1;  // past '('
    while (Peek(offset).kind == TokenKind::kNewline) {
      ++offset;
    }
    if (Peek(offset).kind != TokenKind::kWord) {
      return false;
    }
    while (true) {
      if (Peek(offset).kind != TokenKind::kWord) {
        return false;
      }
      ++offset;
      while (Peek(offset).kind == TokenKind::kNewline) {
        ++offset;
      }
      if (Peek(offset).IsWord() && Peek(offset).text == "of") {
        return true;
      }
      if (Peek(offset).kind != TokenKind::kComma) {
        break;
      }
      ++offset;
      while (Peek(offset).kind == TokenKind::kNewline) {
        ++offset;
      }
    }
    if (Peek(offset).kind != TokenKind::kRightParen) {
      return false;
    }
    ++offset;
    return Peek(offset).IsWord() && Peek(offset).text == "of";
  }

  /// `fail`, `fail thing`, or `fail code thing`.
  NodePtr ParseFail(const Token& keyword) {
    auto fail = Make<syntax::Fail>(keyword);
    if (!AtStatementEnd()) {
      fail->code = ParseExpression();
    }
    if (!AtStatementEnd()) {
      fail->message = ParseExpression();
    }
    fail->after = ParseAfter();
    return fail;
  }

  /// The tail `log` and `logf` share, in a statement or after a `|`.
  ///
  /// `[level] what` for a `log`, `[level] "format" [args]` for a `logf`. One
  /// routine because they are one grammar written in two places: a stage that
  /// took a subtly different tail from the statement of the same name would be
  /// a second dialect to learn for no gain.
  ///
  /// @param formatted Whether this is a `logf`, which requires a format.
  /// @param in_stage Whether the value may be left out, meaning `it`.
  syntax::LogTail ParseLogTail(bool formatted, bool in_stage) {
    syntax::LogTail tail;
    // A level is a bare word, the way a `fail` code is. `log error.message`
    // is not one: a word followed by something that continues an expression is
    // the start of a value, which is the same test OpensStatement makes.
    if (Current().IsWord() && vocabulary::IsLogLevel(Keyword())) {
      switch (Peek().kind) {
        case TokenKind::kDot:
        case TokenKind::kLeftBracket:
        case TokenKind::kLeftParen:
          break;
        default: {
          const Token& level = Advance();
          tail.level = Word{std::string(level.text), syntax::LocationOf(level)};
          break;
        }
      }
    }
    if (formatted) {
      if (At(TokenKind::kString)) {
        tail.format = std::string(Advance().string_value);
        tail.has_format = true;
      } else {
        Report("flow.form.log-format",
               absl::StrCat("A 'logf' takes a format to fill, found ", Found(),
                            "; write 'log' to log a value as it is."),
               Current(), Severity::kError, Family::kForm);
      }
      while (!AtStatementEnd() && !AtStageEnd()) {
        tail.arguments.push_back(ParseExpression());
        if (!AcceptToken(TokenKind::kComma)) {
          break;
        }
      }
      return tail;
    }
    if (!AtStatementEnd() && !AtStageEnd()) {
      tail.arguments.push_back(ParseExpression());
      // Something is still there, so the bare name just read was meant as the
      // level and is not one of the five.
      if (tail.level.Empty() && !AtStatementEnd() && !AtStageEnd()) {
        const syntax::Node* value = tail.arguments.back().get();
        if (const auto* name = syntax::As<syntax::Name>(value);
            name != nullptr) {
          tail.level = Word{name->name, value->location};
          tail.arguments.clear();
          tail.arguments.push_back(ParseExpression());
        }
      }
    } else if (!in_stage) {
      Report("flow.form.log-value",
             tail.level.Empty()
                 ? "A 'log' takes something to log."
                 : absl::StrCat("A 'log' takes something to log after the "
                                "level. (If ",
                                Quoted(tail.level.text),
                                " is the value, a level of the same name is "
                                "read first; write the level too.)"),
             Current(), Severity::kError, Family::kForm);
    }
    return tail;
  }

  /// Whether a stage's argument has run out: the next `|`, `->` or line.
  [[nodiscard]] bool AtStageEnd() const {
    return At(TokenKind::kPipe) || At(TokenKind::kArrow);
  }

  /// `node()`, `node(id)`, or either of those with `in <map>`.
  ///
  /// The parentheses are not decoration: making a node is the one thing in the
  /// language that *does* something without naming an action, and `x = node()`
  /// reads as the construction it is.
  NodePtr ParseNode() {
    const Token& keyword = Current();
    ExpectWord("node");
    auto node = Make<syntax::NodeExpression>(keyword);
    if (!At(TokenKind::kLeftParen)) {
      Report("flow.form.node-parentheses",
             "Making a node takes parentheses: 'node()', or 'node(id)' to "
             "attach to one somebody else named.",
             keyword, Severity::kError, Family::kForm);
      return node;
    }
    Advance();
    if (!At(TokenKind::kRightParen)) {
      node->id = ParseExpression();
    }
    Expect(TokenKind::kRightParen);
    if (AcceptWord("in")) {
      node->node_map = ExpectName("a node map name");
    }
    return node;
  }

  [[nodiscard]] bool AtStatementEnd() const {
    return Current().EndsStatement() || AtWord("after");
  }

  /// A trailing `after a, b` on a statement that is not a call.
  std::vector<Word> ParseAfter() {
    std::vector<Word> names;
    if (!AcceptWord("after")) {
      return names;
    }
    names.push_back(ExpectName("a step name"));
    while (At(TokenKind::kComma) && Peek().kind == TokenKind::kWord) {
      Advance();
      names.push_back(ExpectName("a step name"));
    }
    return names;
  }

  NodePtr ParseForEach() {
    const Token& keyword = Current();
    ExpectWord("for");
    auto loop = Make<syntax::ForEach>(keyword);
    // One name takes the whole value; several take it apart by position.
    loop->variables.push_back(ExpectName("a loop variable"));
    while (AcceptToken(TokenKind::kComma)) {
      loop->variables.push_back(ExpectName("a loop variable"));
    }
    ExpectWord("in");
    {
      const BlockHeader header(this);
      loop->pipeline = ParsePipeline();
    }
    if (AcceptWord("parallel")) {
      loop->parallel = ExpectCount();
    }
    loop->body = ParseBlock();
    loop->after = ParseAfter();
    return loop;
  }

  NodePtr ParseRepeat() {
    const Token& keyword = Current();
    ExpectWord("repeat");
    auto repeat = Make<syntax::Repeat>(keyword);
    if (Current().IsWord() && Peek().kind == TokenKind::kEqual) {
      const Token& name = Advance();
      repeat->variable = Word{std::string(name.text), syntax::LocationOf(name)};
      Advance();
      const BlockHeader header(this);
      repeat->start = ParseExpression();
    }
    if (AcceptWord("max")) {
      repeat->max_iterations = ExpectCount();
    }
    repeat->body = ParseBlock();
    repeat->after = ParseAfter();
    return repeat;
  }

  NodePtr ParseIf() {
    const Token& keyword = Current();
    ExpectWord("if");
    auto branch = Make<syntax::If>(keyword);
    {
      const BlockHeader header(this);
      branch->condition = ParseExpression();
    }
    branch->then_body = ParseBlock();
    if (ContinuesWith(TokenKind::kWord) && NextWordIs("else")) {
      SkipNewlines();
    }
    if (AcceptWord("else")) {
      if (AtWord("if")) {
        branch->else_body.push_back(ParseIf());
      } else {
        branch->else_body = ParseBlock();
      }
    }
    return branch;
  }

  /// A literal, where an expression must not be read.
  ///
  /// `fold 0 as total` needs the `0` without letting the expression parser
  /// reach the `as`, which it would read as a cast. So this is the one place
  /// that takes a value straight from a token.
  std::optional<syntax::Constant> AcceptLiteral() {
    switch (Current().kind) {
      case TokenKind::kNumber: {
        const Token& token = Advance();
        return token.is_integer ? syntax::Constant::Integer(
                                      static_cast<long long>(token.number))
                                : syntax::Constant::Double(token.number);
      }
      case TokenKind::kString:
        return syntax::Constant::String(std::string(Advance().string_value));
      case TokenKind::kDuration:
        return syntax::Constant::Duration(Advance().duration);
      default:
        break;
    }
    if (Current().IsWord()) {
      const std::string word = Keyword();
      if (word == "true" || word == "false") {
        Advance();
        return syntax::Constant::Bool(word == "true");
      }
      if (word == "null") {
        Advance();
        return syntax::Constant::Null();
      }
    }
    return std::nullopt;
  }

  /// A whole number of something: `parallel 2`, `max 6`.
  int ExpectCount() {
    if (!At(TokenKind::kNumber)) {
      ReportHere("flow.syntax.unexpected",
                 absl::StrCat("Expected a count, found ", Found(), "."));
      return 1;
    }
    return static_cast<int>(Advance().number);
  }

  absl::Duration ExpectDuration() {
    if (!At(TokenKind::kDuration)) {
      ReportHere("flow.syntax.unexpected",
                 absl::StrCat("Expected a duration, found ", Found(), "."));
      return absl::ZeroDuration();
    }
    return Advance().duration;
  }

  // -- calls -----------------------------------------------------------------

  syntax::CallExpressionPtr ParseCall() {
    const Token& keyword = Current();
    auto call = Make<syntax::CallExpression>(keyword);
    call->tolerant = AcceptWord("try");
    // The verb is the dispatch: `run` binds the handler registered here, `call`
    // puts the action on the stream this flow is attached to.
    const std::string mode = Keyword();
    if (!ExpectWord("run", "call")) {
      call->modifiers = Make<syntax::CallModifiers>(Current());
      return call;
    }
    call->mode = mode;
    call->action = ParseDottedName("an action name").text;
    Expect(TokenKind::kLeftParen);
    SkipNewlines();
    while (!At(TokenKind::kRightParen)) {
      if (At(TokenKind::kEnd) || At(TokenKind::kRightBrace)) {
        ReportHere("flow.syntax.unclosed",
                   absl::StrCat("Call to ", Quoted(call->action),
                                " is missing its closing ')'."));
        call->modifiers = Make<syntax::CallModifiers>(Current());
        return call;
      }
      const size_t before = position_;
      syntax::CallExpression::Argument argument;
      argument.port = ExpectName("a port name");
      Expect(TokenKind::kColon);
      argument.pipeline = ParsePipeline();
      call->args.push_back(std::move(argument));
      SkipNewlines();
      if (!AcceptToken(TokenKind::kComma)) {
        if (position_ == before) {
          Advance();
        }
        break;
      }
      SkipNewlines();
      if (position_ == before) {
        Advance();
      }
    }
    Expect(TokenKind::kRightParen);
    call->modifiers = ParseModifiers();
    return call;
  }

  /// Whether a line break is followed by a modifier for this call.
  ///
  /// Modifiers read well on a line of their own, so a break before one
  /// continues the call -- unless what follows looks like a statement in its
  /// own right, which is what a port called `timeout` left of a `->` is.
  [[nodiscard]] bool ContinuesWithModifier() const {
    size_t offset = 0;
    while (Peek(offset).kind == TokenKind::kNewline) {
      ++offset;
    }
    if (offset == 0) {
      return false;
    }
    if (!vocabulary::ModifierWords().contains(Keyword(offset))) {
      return false;
    }
    switch (Peek(offset + 1).kind) {
      case TokenKind::kArrow:
      case TokenKind::kPipe:
      case TokenKind::kEqual:
      case TokenKind::kCarry:
        return false;
      default:
        return true;
    }
  }

  syntax::CallModifiersPtr ParseModifiers() {
    auto modifiers = Make<syntax::CallModifiers>(Current());
    while (true) {
      if (ContinuesWithModifier()) {
        SkipNewlines();
      }
      if (!vocabulary::ModifierWords().contains(Keyword())) {
        break;
      }
      const Token& word = Advance();
      const std::string modifier = vocabulary::Canonical(word.text);
      if (modifier == "tee") {
        modifiers->tee = true;
      } else if (modifier == "via") {
        modifiers->node_map = ExpectName("a node map name");
      } else if (modifier == "timeout") {
        modifiers->timeout = ExpectDuration();
      } else if (modifier == "id") {
        modifiers->action_id = ParseExpression();
      } else if (modifier == "after") {
        modifiers->after.push_back(ExpectName("a call name"));
        while (At(TokenKind::kComma) && Peek().kind == TokenKind::kWord) {
          Advance();
          modifiers->after.push_back(ExpectName("a call name"));
        }
      } else if (modifier == "forward") {
        // `forward headers "a", "b"`: send the call the headers this flow was
        // given, without naming a value for each.
        if (!ExpectWord("headers")) {
          continue;
        }
        while (true) {
          if (!At(TokenKind::kString)) {
            ReportHere(
                "flow.syntax.unexpected",
                absl::StrCat("Expected a header name, found ", Found(), "."));
            break;
          }
          modifiers->forward.emplace_back(Advance().string_value);
          if (At(TokenKind::kComma) && Peek().kind == TokenKind::kString) {
            Advance();
            continue;
          }
          break;
        }
      } else if (modifier == "with") {
        while (true) {
          if (!At(TokenKind::kString)) {
            ReportHere(
                "flow.syntax.unexpected",
                absl::StrCat("Expected a header name, found ", Found(), "."));
            break;
          }
          std::string header = std::string(Advance().string_value);
          Expect(TokenKind::kColon);
          modifiers->headers.emplace_back(std::move(header), ParseExpression());
          if (At(TokenKind::kComma) && Peek().kind == TokenKind::kString) {
            Advance();
            continue;
          }
          break;
        }
      } else {  // `headers`, reached without the `forward` that owns it.
        Report("flow.form.forward-headers",
               absl::StrCat(Quoted(word.text),
                            " belongs to 'forward headers'; write 'forward "
                            "headers \"x-name\"'."),
               word, Severity::kError, Family::kForm);
        break;
      }
    }
    return modifiers;
  }

  // -- pipelines -------------------------------------------------------------

  syntax::PipelinePtr ParsePipeline() {
    const Token& keyword = Current();
    auto pipeline = Make<syntax::Pipeline>(keyword);
    pipeline->source = ParseExpression();
    while (true) {
      // `then` and `where` read as words between the things they join --
      // `history then asked`, `hits where it.ok` -- so the pipe is optional in
      // front of those two.
      if (ContinuesWith(TokenKind::kPipe) || ContinuesWithBareStage()) {
        SkipNewlines();
      }
      if (AcceptToken(TokenKind::kPipe)) {
        SkipNewlines();
        pipeline->stages.push_back(ParseStage(AcceptWord("try")));
        continue;
      }
      if (AtBareStage()) {
        pipeline->stages.push_back(ParseStage());
        continue;
      }
      break;
    }
    return pipeline;
  }

  /// Whether a stage that may go without its pipe starts here.
  [[nodiscard]] bool AtBareStage() const {
    if (!vocabulary::BareStages().contains(Keyword())) {
      return false;
    }
    // `then` and `where` take an operand, so a bare one at the end of a
    // statement is a name that happens to be spelled like a stage.
    switch (Peek().kind) {
      case TokenKind::kNewline:
      case TokenKind::kEnd:
      case TokenKind::kRightBrace:
      case TokenKind::kArrow:
      case TokenKind::kComma:
        return false;
      default:
        return true;
    }
  }

  /// Whether a line break is followed by a bare `then`/`where`.
  [[nodiscard]] bool ContinuesWithBareStage() const {
    size_t offset = 0;
    while (Peek(offset).kind == TokenKind::kNewline) {
      ++offset;
    }
    if (offset == 0) {
      return false;
    }
    if (!vocabulary::BareStages().contains(Keyword(offset))) {
      return false;
    }
    switch (Peek(offset + 1).kind) {
      case TokenKind::kArrow:
      case TokenKind::kPipe:
      case TokenKind::kEqual:
      case TokenKind::kCarry:
        return false;
      default:
        return true;
    }
  }

  /// One stage, and whether a `try` stood before its name.
  syntax::StagePtr ParseStage(bool tolerant = false) {
    const Token& keyword = Current();
    auto stage = Make<syntax::Stage>(keyword);
    stage->tolerant = tolerant;
    if (!Current().IsWord()) {
      ReportHere("flow.syntax.unexpected",
                 absl::StrCat("Expected a stage name, found ", Found(), "."));
      return stage;
    }
    Advance();
    stage->name = vocabulary::Canonical(keyword.text);
    const std::optional<vocabulary::StageArgument> takes =
        vocabulary::StageTakes(stage->name);
    if (!takes.has_value()) {
      Report("flow.form.unknown-stage",
             absl::StrCat("Unknown stage ", Quoted(stage->name), " (known: ",
                          absl::StrJoin(SortedStages(), ", "), ")."),
             keyword, Severity::kError, Family::kForm);
      return stage;
    }
    stage->takes = *takes;
    switch (*takes) {
      case vocabulary::StageArgument::kNone:
        break;
      case vocabulary::StageArgument::kNumber:
        if (At(TokenKind::kNumber)) {
          const Token& number = Advance();
          stage->number = number.number;
          stage->is_integer = number.is_integer;
        } else {
          Report("flow.form.stage-argument",
                 absl::StrCat("Expected a count for ", Quoted(stage->name),
                              ", found ", Found(), "."),
                 Current(), Severity::kError, Family::kForm);
        }
        break;
      case vocabulary::StageArgument::kString:
        if (At(TokenKind::kString)) {
          stage->text = std::string(Advance().string_value);
        } else {
          Report("flow.form.stage-argument",
                 absl::StrCat("Expected a pattern for ", Quoted(stage->name),
                              ", found ", Found(), "."),
                 Current(), Severity::kError, Family::kForm);
        }
        break;
      case vocabulary::StageArgument::kOptionalString:
        if (At(TokenKind::kString)) {
          stage->text = std::string(Advance().string_value);
        }
        break;
      case vocabulary::StageArgument::kExpression:
        stage->argument = ParseExpression();
        break;
      case vocabulary::StageArgument::kStream:
        // A stream rather than a value: `then` reads this one and then that
        // one, so its argument is whatever a pipeline may start with.
        stage->argument = ParsePostfix();
        break;
      case vocabulary::StageArgument::kOptionalExpression:
        // `| sum` is the values themselves and `| sum it.price` one field of
        // each.
        if (StageArgumentFollows()) {
          stage->argument = ParseExpression();
        }
        break;
      case vocabulary::StageArgument::kSortKey:
        if (AcceptWord("by")) {
          stage->argument = ParseExpression();
        }
        if (AcceptWord("desc")) {
          stage->descending = true;
        }
        break;
      case vocabulary::StageArgument::kFold:
        ParseFoldArgument(*stage);
        break;
      case vocabulary::StageArgument::kDuration:
        stage->duration = ExpectDuration();
        break;
      case vocabulary::StageArgument::kLog:
      case vocabulary::StageArgument::kLogFormat:
        stage->log =
            ParseLogTail(*takes == vocabulary::StageArgument::kLogFormat,
                         /*in_stage=*/true);
        break;
    }
    ParseStageTail(*stage, keyword);
    return stage;
  }

  /// Whether an optional stage argument was written, rather than the stage
  /// ending here.
  [[nodiscard]] bool StageArgumentFollows() const {
    switch (Current().kind) {
      case TokenKind::kNewline:
      case TokenKind::kEnd:
      case TokenKind::kRightBrace:
      case TokenKind::kRightParen:
      case TokenKind::kArrow:
      case TokenKind::kComma:
      case TokenKind::kPipe:
        return false;
      default:
        // A clause word belongs to the tail, not to the argument.
        return !(Current().IsWord() &&
                 vocabulary::ClauseWords().contains(Keyword()));
    }
  }

  /// `fold LITERAL as NAME, EXPRESSION`, and `scan` the same.
  ///
  /// The start is a literal rather than an expression on purpose: `0 as total`
  /// read as an expression is a cast of `0` to a type called `total`, and the
  /// language would have to decide which of the two was meant. A literal cannot
  /// be a cast, so this is unambiguous by construction.
  ///
  /// A record literal is a start too, and has to be: `scan` is how a state
  /// machine is written, and a state worth carrying is rarely one number. `{
  /// .. } as name` is not ambiguous the way `0 as name` is -- a record literal
  /// is read by its braces before `as` is looked at -- so the reason for the
  /// restriction does not apply to it. It must still fold to a constant, which
  /// is what keeps it a *starting* value and not a first pass.
  void ParseFoldArgument(syntax::Stage& stage) {
    const std::string spelled(stage.name);
    if (At(TokenKind::kLeftBrace)) {
      const Token& brace = Current();
      const syntax::NodePtr record = ParseObjectLiteral();
      std::optional<syntax::Constant> folded =
          record == nullptr ? std::nullopt
                            : syntax::ConstantValue(record.get());
      if (folded.has_value()) {
        stage.start = *std::move(folded);
      } else {
        Report("flow.form.fold-start",
               absl::StrCat("'", spelled,
                            "' starts from a value that is known before the "
                            "stream is, so the record it starts from cannot "
                            "read `it` or anything else the stream carries."),
               brace, Severity::kError, Family::kForm);
      }
    } else if (const std::optional<syntax::Constant> start = AcceptLiteral()) {
      stage.start = *start;
    } else {
      Report("flow.form.fold-start",
             absl::StrCat("'", spelled,
                          "' starts from a literal -- a number, a string, a "
                          "duration, a record, true, false or null -- and "
                          "found ",
                          Found(), "."),
             Current(), Severity::kError, Family::kForm);
    }
    if (!AcceptWord("as")) {
      Report("flow.form.fold-name",
             absl::StrCat("'", spelled, "' names what it carries: `", spelled,
                          " 0 as total, total + it`. Found ", Found(), "."),
             Current(), Severity::kError, Family::kForm);
      return;
    }
    stage.carried =
        ExpectName(absl::StrCat("a name for what the ", spelled, " carries"));
    if (!Expect(TokenKind::kComma,
                "',' and the expression that folds one value in")) {
      return;
    }
    SkipNewlines();
    stage.argument = ParseExpression();
  }

  /// `[parallel n [unordered]] [into ref]`, in either order, after a stage.
  void ParseStageTail(syntax::Stage& stage, const Token& keyword) {
    while (Current().IsWord()) {
      const std::string word = Keyword();
      if (word == "parallel") {
        Advance();
        stage.parallel = ExpectCount();
        continue;
      }
      if (word == "unordered") {
        Advance();
        stage.ordered = false;
        continue;
      }
      if (word == "into") {
        Advance();
        stage.failures = ParseReference();
        continue;
      }
      break;
    }
    if (stage.parallel > 1 &&
        !vocabulary::ParallelStages().contains(stage.name)) {
      Report("flow.form.stage-not-parallel",
             absl::StrCat("'", stage.name,
                          "' reads the whole stream, so there is nothing for "
                          "'parallel' to run at once. It goes on a stage that "
                          "reshapes each value, like 'map'."),
             keyword, Severity::kError, Family::kForm);
      stage.parallel = 1;
    }
    if (!stage.ordered && stage.parallel <= 1) {
      Report("flow.form.unordered-without-parallel",
             absl::StrCat("'unordered' says a parallel stage may finish its "
                          "values in any order, so it needs 'parallel n'. "
                          "Without it, '",
                          stage.name,
                          "' sees one value at a time and the order is the "
                          "one it was given."),
             keyword, Severity::kWarning, Family::kForm);
      stage.ordered = true;
    }
    if (stage.failures != nullptr && !stage.tolerant) {
      Report("flow.form.into-without-try",
             absl::StrCat("'into' says where a *tolerated* failure goes, so "
                          "the stage has to be a 'try': "
                          "`try ",
                          stage.name, " ... into failures`."),
             keyword, Severity::kError, Family::kForm);
    }
  }

  static std::vector<std::string> SortedStages() {
    std::vector<std::string> names;
    for (const std::string_view stage : vocabulary::Stages()) {
      names.emplace_back(stage);
    }
    std::sort(names.begin(), names.end());
    return names;
  }

  // -- expressions -----------------------------------------------------------

  /// A name, optionally with `.port` -- what a pipe writes to, and what `wait`,
  /// `drain`, `status` and a counted `skip` take.
  NodePtr ParseReference() {
    const Token& at = Current();
    NodePtr node = ParsePostfix();
    // `_` is reference-shaped and only a pipe's target may be one, so it is let
    // through here and refused by the resolver -- which knows which of the
    // several statements that read a reference this is.
    if (syntax::IsAnyOf(node.get(),
                        {syntax::NodeKind::kName, syntax::NodeKind::kAttr,
                         syntax::NodeKind::kOutcome, syntax::NodeKind::kDiscard,
                         syntax::NodeKind::kError})) {
      return node;
    }
    Report("flow.syntax.unexpected",
           "Expected a port or a node here, like 'out-port' or 'call.port'.",
           at);
    return node;
  }

  /// How many newlines sit between here and the next token that is not one.
  [[nodiscard]] size_t NewlinesAhead() const {
    size_t offset = 0;
    while (Peek(offset).kind == TokenKind::kNewline) {
      ++offset;
    }
    return offset;
  }

  /// Inside brackets, lets a line break sit in the middle of an expression.
  ///
  /// Inside `{ .. }`, `[ .. ]`, or `( .. )`, the closing bracket ends the
  /// expression, so an operator may begin the next line. Outside brackets, a
  /// line break ends the statement and prevents a following `where` from being
  /// parsed as part of the preceding pipeline.
  ///
  /// The asymmetry this removes was a real wart. A break straight after a `,`
  /// already worked, because the loops that read a comma-separated list skip
  /// newlines themselves, so `{"a": 1,\n "b": 2}` was fine while
  /// `{"a": x\n or y}` was `Expected }, found 'or'`. One rule now: inside
  /// brackets a break is whitespace.
  ///
  /// Only ever *consumes* newlines when the token after them is the one asked
  /// for, so a `}` on the next line is still a `}`.
  bool WrapsTo(TokenKind kind) {
    if (brackets_ == 0 || At(kind)) {
      return false;
    }
    const size_t ahead = NewlinesAhead();
    if (ahead == 0 || Peek(ahead).kind != kind) {
      return false;
    }
    SkipNewlines();
    return true;
  }

  bool WrapsToWord(std::string_view word) {
    if (brackets_ == 0 || AtWord(word)) {
      return false;
    }
    const size_t ahead = NewlinesAhead();
    if (ahead == 0 || Keyword(ahead) != word) {
      return false;
    }
    SkipNewlines();
    return true;
  }

  bool WrapsToComparison() {
    if (brackets_ == 0 || IsComparison(Current().kind)) {
      return false;
    }
    const size_t ahead = NewlinesAhead();
    if (ahead == 0 || !IsComparison(Peek(ahead).kind)) {
      return false;
    }
    SkipNewlines();
    return true;
  }

  NodePtr ParseExpression() {
    const Descent descent(*this);
    if (TooDeep()) {
      return MakeError(Current(), "a value");
    }
    return ParseOr();
  }

  NodePtr ParseOr() {
    NodePtr left = ParseAnd();
    while (AtWord("or") || WrapsToWord("or")) {
      const Token& op = Advance();
      auto binary = Make<syntax::Binary>(op);
      binary->op = "or";
      binary->left = std::move(left);
      binary->right = ParseAnd();
      left = std::move(binary);
    }
    return left;
  }

  NodePtr ParseAnd() {
    NodePtr left = ParseNot();
    while (AtWord("and") || WrapsToWord("and")) {
      const Token& op = Advance();
      auto binary = Make<syntax::Binary>(op);
      binary->op = "and";
      binary->left = std::move(left);
      binary->right = ParseNot();
      left = std::move(binary);
    }
    return left;
  }

  NodePtr ParseNot() {
    if (AtWord("not")) {
      const Token& op = Advance();
      auto unary = Make<syntax::Unary>(op);
      unary->op = "not";
      unary->operand = ParseNot();
      return unary;
    }
    return ParseComparison();
  }

  static bool IsComparison(TokenKind kind) {
    switch (kind) {
      case TokenKind::kEqualEqual:
      case TokenKind::kBangEqual:
      case TokenKind::kLess:
      case TokenKind::kLessEqual:
      case TokenKind::kGreater:
      case TokenKind::kGreaterEqual:
        return true;
      default:
        return false;
    }
  }

  NodePtr ParseComparison() {
    NodePtr left = ParseAdditive();
    if (IsComparison(Current().kind) || WrapsToComparison()) {
      const Token& op = Advance();
      auto binary = Make<syntax::Binary>(op);
      binary->op = std::string(KindName(op.kind));
      binary->left = std::move(left);
      binary->right = ParseAdditive();
      return binary;
    }
    if (AtWord("in")) {
      const Token& op = Advance();
      auto binary = Make<syntax::Binary>(op);
      binary->op = "in";
      binary->left = std::move(left);
      binary->right = ParseAdditive();
      return binary;
    }
    return left;
  }

  /// `a + b` and `a - b`: numbers, and times.
  ///
  /// The only arithmetic the language has, and it is here for durations -- "how
  /// long did that take", "is this older than the deadline" -- which a
  /// composition cannot express any other way. `-` needs its spaces: `a-b` is
  /// one name, because an action is called `text-upper`.
  NodePtr ParseAdditive() {
    NodePtr left = ParseCast();
    while (At(TokenKind::kPlus) || At(TokenKind::kMinus) ||
           WrapsTo(TokenKind::kPlus) || WrapsTo(TokenKind::kMinus)) {
      const Token& op = Advance();
      auto binary = Make<syntax::Binary>(op);
      binary->op = std::string(KindName(op.kind));
      binary->left = std::move(left);
      binary->right = ParseCast();
      left = std::move(binary);
    }
    return left;
  }

  /// `expr as TYPE` -- the value, made a value of that type.
  NodePtr ParseCast() {
    NodePtr node = ParsePostfix();
    while (AtWord("as")) {
      const Token& op = Advance();
      auto cast = Make<syntax::TypedValue>(op);
      cast->type = ParseType();
      cast->value = std::move(node);
      node = std::move(cast);
    }
    return node;
  }

  /// Turns off `Tag{...}` for as long as it is in scope.
  ///
  /// A `{` opens a block, not a value, in an `if` condition and a `for`'s
  /// source, for the reason Go forbids the same thing in the same places: `if
  /// x.y {` has to keep meaning what it looks like. Brackets of any kind turn
  /// it back on, so `if (x as T{a: 1}).ok { }` is still available.
  class BlockHeader {
   public:
    explicit BlockHeader(ParserImpl* parser)
        : parser_(parser), outer_(parser->brace_literals_) {
      parser_->brace_literals_ = false;
    }

    ~BlockHeader() { parser_->brace_literals_ = outer_; }

    BlockHeader(const BlockHeader&) = delete;
    BlockHeader& operator=(const BlockHeader&) = delete;

   private:
    ParserImpl* parser_;
    bool outer_;
  };

  // Turns `Tag{...}` back on for as long as it is in scope: inside brackets a
  // `{` cannot be opening a block.
  /// Turns `Tag{...}` back on for as long as it is in scope: inside brackets a
  /// `{` cannot be opening a block.
  ///
  /// It also counts the depth, which is the other thing being inside brackets
  /// means: a line break there ends nothing, so a run of strings may cross one.
  class Bracketed {
   public:
    explicit Bracketed(ParserImpl* parser)
        : parser_(parser), outer_(parser->brace_literals_) {
      parser_->brace_literals_ = true;
      ++parser_->brackets_;
    }

    ~Bracketed() {
      parser_->brace_literals_ = outer_;
      --parser_->brackets_;
    }

    Bracketed(const Bracketed&) = delete;
    Bracketed& operator=(const Bracketed&) = delete;

   private:
    ParserImpl* parser_;
    bool outer_;
  };

  NodePtr ParsePostfix() {
    NodePtr node = ParsePrimary();
    while (true) {
      if (At(TokenKind::kDot)) {
        const Token& dot = Advance();
        auto attr = Make<syntax::Attr>(dot);
        attr->name = ExpectName("a name after '.'").text;
        attr->base = std::move(node);
        node = std::move(attr);
        continue;
      }
      if (At(TokenKind::kLeftBracket)) {
        const Token& bracket = Advance();
        auto index = Make<syntax::Index>(bracket);
        index->index = ParseExpression();
        Expect(TokenKind::kRightBracket);
        index->base = std::move(node);
        node = std::move(index);
        continue;
      }
      // `a11.sdk.Interaction{...}`: a value of a named type, written the way
      // the type's own fields read. A generic one is spelled with `as`, where
      // the brackets cannot be mistaken for an index.
      if (At(TokenKind::kLeftBrace) && brace_literals_) {
        std::optional<std::string> name = syntax::DottedName(node.get());
        if (!name.has_value()) {
          return node;
        }
        auto typed = std::make_unique<syntax::TypedValue>();
        typed->location = node->location;
        typed->type.location = node->location;
        typed->type.name = *std::move(name);
        result_.tagged_braces.push_back(Current().start);
        typed->value = ParseObjectLiteral();
        node = std::move(typed);
        continue;
      }
      return node;
    }
  }

  NodePtr ParsePrimary() {
    const Token& token = Current();
    // `status` reads an outcome when something follows it to be the outcome
    // *of*; on its own it is an ordinary name, so a port may be called that.
    if (AtWord("status") && Peek().kind == TokenKind::kWord) {
      Advance();
      auto outcome = Make<syntax::Outcome>(token);
      outcome->subject = ParseReference();
      return outcome;
    }
    // `wait first of a, b` where a value is expected: the number of whichever
    // won.
    if (AtWord("wait") && Peek().IsWord() &&
        (vocabulary::Canonical(Peek().text) == "first" ||
         vocabulary::Canonical(Peek().text) == "all") &&
        Peek(2).IsWord() && vocabulary::Canonical(Peek(2).text) == "of") {
      Advance();
      return ParseWaitOf(token);
    }
    if (At(TokenKind::kString)) {
      auto literal = Make<syntax::Literal>(token);
      literal->value = syntax::Constant::String(ParseStringRun());
      return literal;
    }
    if (At(TokenKind::kNumber)) {
      Advance();
      auto literal = Make<syntax::Literal>(token);
      literal->value =
          token.is_integer
              ? syntax::Constant::Integer(static_cast<long long>(token.number))
              : syntax::Constant::Double(token.number);
      return literal;
    }
    if (At(TokenKind::kDuration)) {
      Advance();
      auto literal = Make<syntax::Literal>(token);
      literal->value = syntax::Constant::Duration(token.duration);
      return literal;
    }
    if (At(TokenKind::kLeftParen)) {
      Advance();
      const Bracketed brackets(this);
      SkipNewlines();
      NodePtr node = ParseExpression();
      if (At(TokenKind::kPipe)) {
        auto pipeline = Make<syntax::Pipeline>(token);
        while (AcceptToken(TokenKind::kPipe)) {
          SkipNewlines();
          pipeline->stages.push_back(ParseStage());
        }
        pipeline->source = std::move(node);
        SkipNewlines();
        Expect(TokenKind::kRightParen);
        auto value = Make<syntax::PipelineValue>(token);
        value->pipeline = std::move(pipeline);
        return value;
      }
      SkipNewlines();
      Expect(TokenKind::kRightParen);
      return node;
    }
    if (At(TokenKind::kLeftBracket)) {
      Advance();
      const Bracketed brackets(this);
      auto list = Make<syntax::ListLiteral>(token);
      SkipNewlines();
      while (!At(TokenKind::kRightBracket)) {
        if (At(TokenKind::kEnd) || At(TokenKind::kRightBrace)) {
          ReportHere("flow.syntax.unclosed", "Missing ']'.");
          return list;
        }
        const size_t before = position_;
        list->items.push_back(At(TokenKind::kSpread) ? ParseSpread()
                                                     : ParseExpression());
        SkipNewlines();
        if (!AcceptToken(TokenKind::kComma)) {
          if (position_ == before) {
            Advance();
          }
          break;
        }
        SkipNewlines();
        if (position_ == before) {
          Advance();
        }
      }
      Expect(TokenKind::kRightBracket);
      return list;
    }
    if (At(TokenKind::kLeftBrace)) {
      return ParseObjectLiteral();
    }
    if (Current().IsWord()) {
      const std::string spelled(Advance().text);
      const std::string word = vocabulary::Canonical(spelled);
      if (word == "true" || word == "false") {
        auto literal = Make<syntax::Literal>(token);
        literal->value = syntax::Constant::Bool(word == "true");
        return literal;
      }
      if (word == "null") {
        return Make<syntax::Literal>(token);
      }
      if (word == "it") {
        return Make<syntax::It>(token);
      }
      // `_` is the discard, and a word rather than punctuation only because
      // that is how it is spelled.
      if (word == "_") {
        return Make<syntax::Discard>(token);
      }
      // `zip(a, b)` reads several streams in step and `interleave(a, b)` reads
      // them at once. Both are spelled like a function and parsed apart from
      // one, because their arguments are streams: see [syntax::Zip].
      if ((word == "zip" || word == "interleave") &&
          At(TokenKind::kLeftParen)) {
        auto zip = Make<syntax::Zip>(token);
        zip->name = word;
        Advance();
        const Bracketed brackets(this);
        SkipNewlines();
        while (!At(TokenKind::kRightParen)) {
          if (At(TokenKind::kEnd) || At(TokenKind::kRightBrace)) {
            ReportHere("flow.syntax.unclosed",
                       absl::StrCat("Call to '", word,
                                    "' is missing its closing ')'."));
            return zip;
          }
          const size_t before = position_;
          zip->sources.push_back(ParseExpression());
          SkipNewlines();
          if (!AcceptToken(TokenKind::kComma)) {
            if (position_ == before) {
              Advance();
            }
            break;
          }
          SkipNewlines();
          if (position_ == before) {
            Advance();
          }
        }
        Expect(TokenKind::kRightParen);
        if (zip->sources.empty()) {
          Report("flow.form.zip-empty",
                 absl::StrCat("'", word,
                              "' reads several streams as one, so it takes at "
                              "least one."),
                 token, Severity::kError, Family::kForm);
        }
        return zip;
      }
      if (At(TokenKind::kLeftParen)) {
        if (!vocabulary::Builtins().contains(word)) {
          Report("flow.form.unknown-builtin",
                 absl::StrCat(Quoted(spelled),
                              " is not a built-in function (known: ",
                              absl::StrJoin(SortedBuiltins(), ", "), ")."),
                 token, Severity::kError, Family::kForm);
        }
        // Read the call either way: the arguments are still an expression, and
        // reporting one unknown function beats reporting everything after it.
        auto builtin = Make<syntax::Builtin>(token);
        builtin->name = word;
        Advance();
        SkipNewlines();
        while (!At(TokenKind::kRightParen)) {
          if (At(TokenKind::kEnd) || At(TokenKind::kRightBrace)) {
            ReportHere("flow.syntax.unclosed",
                       absl::StrCat("Call to ", Quoted(word),
                                    " is missing its closing ')'."));
            return builtin;
          }
          const size_t before = position_;
          builtin->args.push_back(ParseExpression());
          SkipNewlines();
          if (!AcceptToken(TokenKind::kComma)) {
            if (position_ == before) {
              Advance();
            }
            break;
          }
          SkipNewlines();
          if (position_ == before) {
            Advance();
          }
        }
        Expect(TokenKind::kRightParen);
        return builtin;
      }
      auto name = Make<syntax::Name>(token);
      name->name = spelled;
      return name;
    }
    ReportHere("flow.syntax.unexpected",
               absl::StrCat("Expected a value, found ", Found(), "."));
    return MakeError(token, "a value");
  }

  static std::vector<std::string> SortedBuiltins() {
    std::vector<std::string> names;
    for (const std::string_view builtin : vocabulary::Builtins()) {
      names.emplace_back(builtin);
    }
    std::sort(names.begin(), names.end());
    return names;
  }

  /// `...expr` -- what it holds, spread into the literal being written.
  ///
  /// The operand is a postfix expression rather than a whole one, so `...a.b`
  /// is the spread of `a.b` and `...a + b` is not a spread of a sum: an
  /// arithmetic expression is never a thing with parts to spread, and reading
  /// it that way only postpones the diagnostic.
  NodePtr ParseSpread() {
    const Token& dots = Advance();
    auto spread = Make<syntax::Spread>(dots);
    spread->value = ParsePostfix();
    return spread;
  }

  // `{ key: expr, ...
  /// `{ key: expr, ... }`, wherever one is allowed.
  ///
  /// Inside the braces a line break is never the end of anything, so an object
  /// with a field per line reads the way it is written.
  NodePtr ParseObjectLiteral() {
    const Token& brace = Current();
    auto object = Make<syntax::ObjectLiteral>(brace);
    if (!Expect(TokenKind::kLeftBrace)) {
      return object;
    }
    result_.value_braces.push_back(brace.start);
    const Bracketed brackets(this);
    SkipNewlines();
    while (!At(TokenKind::kRightBrace)) {
      if (At(TokenKind::kEnd)) {
        ReportHere("flow.syntax.unclosed", "Missing '}'.");
        return object;
      }
      const size_t before = position_;
      if (At(TokenKind::kSpread)) {
        // A spread has no key: the pairs it brings in keep their own.
        object->pairs.emplace_back(std::string(), ParseSpread());
        SkipNewlines();
        if (!AcceptToken(TokenKind::kComma)) {
          if (position_ == before) {
            Advance();
          }
          break;
        }
        SkipNewlines();
        if (position_ == before) {
          Advance();
        }
        continue;
      }
      std::string key;
      if (At(TokenKind::kString)) {
        key = ParseStringRun();
      } else {
        key = ExpectName("an object key").text;
      }
      Expect(TokenKind::kColon);
      object->pairs.emplace_back(std::move(key), ParseExpression());
      SkipNewlines();
      if (!AcceptToken(TokenKind::kComma)) {
        if (position_ == before) {
          Advance();
        }
        break;
      }
      SkipNewlines();
      if (position_ == before) {
        Advance();
      }
    }
    Expect(TokenKind::kRightBrace);
    return object;
  }

  Word ParseDottedName(std::string_view what) {
    if (At(TokenKind::kString)) {
      const Token& token = Advance();
      return Word{std::string(token.string_value), syntax::LocationOf(token)};
    }
    Word first = ExpectName(what);
    if (first.Empty()) {
      return first;
    }
    Word dotted = first;
    while (At(TokenKind::kDot) && Peek().kind == TokenKind::kWord) {
      Advance();
      const Token& part = Advance();
      absl::StrAppend(&dotted.text, ".", part.text);
      dotted.location.end = part.end;
    }
    return dotted;
  }

  /// The token a location came from, for a report that has only the location.
  ///
  /// Cheap enough: the range is all [Report] reads out of it.
  [[nodiscard]] Token TokenAt(const Location& location) const {
    Token token;
    token.start = location.start;
    token.end = location.end;
    token.line = location.line;
    token.column = location.column;
    return token;
  }

  LineIndex lines_;
  std::vector<Token> tokens_;
  size_t position_ = 0;
  /// Whether `Tag{...}` may start here: see [BlockHeader].
  bool brace_literals_ = true;
  /// How many brackets deep this is, where a line break ends nothing.
  int brackets_ = 0;
  /// How many levels of recursive descent are on the stack; see kMaxNesting.
  int depth_ = 0;
  /// Whether the nesting bound has already been reported for this document.
  bool reported_too_deep_ = false;
  /// The flow being read, so every diagnostic says which one it is in.
  std::string flow_name_;
  ParseResult result_;
};

}  // namespace

bool ParseResult::HasErrors() const {
  return FirstError() != nullptr;
}

const Diagnostic* absl_nullable ParseResult::FirstError() const {
  return flow::FirstError(diagnostics);
}

ParseResult Parse(std::string_view source) {
  LexResult lexed = Lex(source, LexOptions{.keep_comments = false});
  return ParseTokens(source, lexed.tokens, std::move(lexed.diagnostics));
}

ParseResult ParseTokens(std::string_view source, absl::Span<const Token> tokens,
                        std::vector<Diagnostic> diagnostics) {
  return ParserImpl(source, tokens, std::move(diagnostics)).Run();
}

}  // namespace a11::flow
