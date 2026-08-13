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
/// read side by side while the Python one is still the reference. What differs is
/// the failure path: every `raise` there is a [Report] and a recovery here.
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
      if (token.kind != TokenKind::kComment) tokens_.push_back(token);
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

  const Token& Current() const { return tokens_[position_]; }

  const Token& Peek(size_t offset = 1) const {
    return tokens_[std::min(position_ + offset, tokens_.size() - 1)];
  }

  const Token& Advance() {
    const Token& token = tokens_[position_];
    if (token.kind != TokenKind::kEnd) ++position_;
    return token;
  }

  bool At(TokenKind kind) const { return Current().kind == kind; }

  /// The token at `offset` as a keyword, or `""` if it is not a word.
  ///
  /// A word written in one case throughout reads as its lower-case self, so `FOR`
  /// and `for` are the same keyword and `For` is a name.
  std::string Keyword(size_t offset = 0) const {
    const Token& token = offset == 0 ? Current() : Peek(offset);
    if (!token.IsWord()) return "";
    return vocabulary::Canonical(token.text);
  }

  bool AtWord(std::string_view word) const {
    return Current().IsWord() && Keyword() == word;
  }

  bool AtWord(std::string_view first, std::string_view second) const {
    if (!Current().IsWord()) return false;
    const std::string word = Keyword();
    return word == first || word == second;
  }

  void SkipNewlines() {
    while (At(TokenKind::kNewline)) ++position_;
  }

  /// Whether the next real token is one of `kinds`, past line breaks.
  ///
  /// This is what lets a long pipeline wrap: a line ending just before `|` or
  /// `->` is a continuation, not the end of the statement.
  bool ContinuesWith(TokenKind kind) const {
    size_t offset = 0;
    while (Peek(offset).kind == TokenKind::kNewline) ++offset;
    return Peek(offset).kind == kind;
  }

  bool AcceptToken(TokenKind kind) {
    if (Current().kind != kind) return false;
    Advance();
    return true;
  }

  bool AcceptWord(std::string_view word) {
    if (!AtWord(word)) return false;
    Advance();
    return true;
  }

  /// What is actually here, for a message.
  std::string Found() const {
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
    ReportHere("flow.syntax.unexpected",
               absl::StrCat("Expected ", Quoted(word), ", found ", Found(),
                            "."));
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
  /// The recovery point of the whole grammar, and it is the right one because the
  /// language is one statement per line: whatever went wrong, the next line is a
  /// statement again, so a mistake costs its own line and nothing more.
  void Recover() {
    while (!Current().EndsStatement()) Advance();
    if (At(TokenKind::kNewline)) SkipNewlines();
  }

  /// Require the end of a statement: a line break, a `}`, or the file.
  void EndStatement() {
    if (At(TokenKind::kNewline)) {
      SkipNewlines();
      return;
    }
    if (At(TokenKind::kRightBrace) || At(TokenKind::kEnd)) return;
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
      if (!ExpectWord("flow")) {
        // Not a declaration at all. The line is skipped rather than read as one,
        // so a stray statement outside a flow costs one diagnostic.
        Recover();
        if (position_ == before) Advance();
        continue;
      }
      const Token& keyword = tokens_[position_ - 1];
      result_.flows.push_back(ParseFlow(keyword));
      SkipNewlines();
      if (position_ == before) Advance();
    }
    if (result_.flows.empty()) {
      ReportHere("flow.syntax.unexpected",
                 "A flow file must declare at least one flow.");
    }
  }

  syntax::FlowDeclarationPtr ParseFlow(const Token& keyword) {
    auto declaration = Make<syntax::FlowDeclaration>(keyword);
    declaration->name = ParseDottedName("a flow name");
    // The flow a diagnostic is in, for everything reported until this one ends.
    const std::string outer_flow = flow_name_;
    flow_name_ = declaration->name.text;
    if (!Expect(TokenKind::kLeftBrace)) {
      // Nothing to read the body out of. Give up on this declaration rather than
      // read the rest of the file as its statements.
      Recover();
      flow_name_ = outer_flow;
      return declaration;
    }
    SkipNewlines();
    while (!At(TokenKind::kRightBrace)) {
      if (At(TokenKind::kEnd)) {
        ReportHere("flow.syntax.unclosed",
                   absl::StrCat("Flow ", Quoted(declaration->name.text),
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
      if (position_ == before) Advance();
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
    // so they follow the type and may be written in either order. A port carries
    // one value unless it says otherwise, because most do.
    const std::string written = vocabulary::Canonical(port->type.name);
    if (!port->type.quoted && port->type.parameters.empty() &&
        vocabulary::PortModifierWords().contains(written)) {
      // These used to come first. Say so, rather than report the type after them
      // as a statement that has no business being here.
      Report("flow.form.port-modifier-order",
             absl::StrCat(Quoted(port->type.name),
                          " follows the type: write '", port->name.text,
                          ": TYPE ", port->type.name, "'."),
             TokenAt(port->type.location), Severity::kError, Family::kForm);
      if (written == "stream") {
        port->unary = false;
      } else {
        port->required = true;
      }
      if (Current().IsWord() || At(TokenKind::kString)) port->type = ParseType();
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
  bool DescribeFollows() const {
    if (Peek().kind == TokenKind::kString) return true;
    size_t offset = 1;
    while (Peek(offset).kind == TokenKind::kNewline) ++offset;
    return offset > 1 && Peek(offset).kind == TokenKind::kString &&
           Peek(offset + 1).EndsStatement();
  }

  /// A declaration's description: a string after it, or a string on a line of
  /// its own under it.
  ///
  /// The second spelling is there because a description is prose, and prose that
  /// says anything runs past the width of the declaration it belongs to -- with
  /// `"""` all the more so. It is unambiguous because the string has to be *alone*
  /// on its line: `"hello" -> out` is a statement, since something follows the
  /// string, and a line holding nothing but a string is not a statement in this
  /// language at all.
  std::string ParseDescription() {
    if (At(TokenKind::kString)) return std::string(Advance().string_value);
    size_t offset = 0;
    while (Peek(offset).kind == TokenKind::kNewline) ++offset;
    if (offset == 0 || Peek(offset).kind != TokenKind::kString) return "";
    if (!Peek(offset + 1).EndsStatement()) return "";
    SkipNewlines();
    return std::string(Advance().string_value);
  }

  /// A port's type: a name, a name with type parameters, or a string.
  ///
  /// The name may be dotted, which is how a type registered in a serialisation
  /// registry is written -- `a11.sdk.AudioBuffer` -- and the brackets are how a
  /// generic one says what it holds: `list[a11.NodeFragment]`. A quoted name is a
  /// mimetype.
  syntax::TypeExpression ParseType() {
    syntax::TypeExpression type;
    type.location = syntax::LocationOf(Current());
    if (At(TokenKind::kString)) {
      type.name = std::string(Advance().string_value);
      type.quoted = true;
      return type;
    }
    type.name = ParseDottedName("a port type").text;
    if (AcceptToken(TokenKind::kLeftBracket)) {
      while (true) {
        type.parameters.push_back(ParseType());
        if (!AcceptToken(TokenKind::kComma)) break;
      }
      Expect(TokenKind::kRightBracket, "']' after the type parameters");
    }
    return type;
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
      if (letter == '-' || letter == '.') letter = '_';
    }
    header->alias = Word{alias, syntax::LocationOf(keyword)};
    if (AcceptWord("as")) header->alias = ExpectName("a header alias");
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
    if (!Expect(TokenKind::kLeftBrace)) return body;
    SkipNewlines();
    while (!At(TokenKind::kRightBrace)) {
      if (At(TokenKind::kEnd)) {
        ReportHere("flow.syntax.unclosed", "Missing '}'.");
        return body;
      }
      const size_t before = position_;
      body.push_back(ParseStatement());
      EndStatement();
      if (position_ == before) Advance();
    }
    Advance();
    return body;
  }

  /// Whether a statement-opening word is used as a keyword here.
  bool OpensStatement(std::string_view word) const {
    if (!OpensStatementWord(word)) return false;
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
    const Token& keyword = Current();
    if (keyword.IsWord()) {
      const std::string word = Keyword();
      if (OpensStatement(word)) {
        if (word == "run" || word == "call" || word == "try") {
          auto statement = Make<syntax::CallStatement>(keyword);
          statement->call = ParseCall();
          return statement;
        }
        if (word == "skip") {
          Advance();
          return ParseSkip(keyword);
        }
        if (word == "wait") {
          Advance();
          auto wait = Make<syntax::Wait>(keyword);
          wait->subject = ParseReference();
          if (AcceptWord("timeout")) wait->timeout = ExpectDuration();
          wait->after = ParseAfter();
          return wait;
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
        if (word == "for") return ParseForEach();
        if (word == "repeat") return ParseRepeat();
        if (word == "until" || word == "while") {
          Advance();
          auto until = Make<syntax::Until>(keyword);
          until->condition = ParseExpression();
          until->stop_when = word == "until";
          return until;
        }
        if (word == "if") return ParseIf();
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
        bind->name = Word{std::string(Advance().text),
                          syntax::LocationOf(keyword)};
        Advance();
        if (AtWord("node")) {
          bind->value = ParseNode();
        } else if (AtWord("wait", "drain")) {
          bind->value = ParseStatement();
        } else {
          bind->value = ParseCall();
        }
        return bind;
      }

      if (Peek().kind == TokenKind::kCarry) {
        auto carry = Make<syntax::Carry>(keyword);
        carry->name = Word{std::string(Advance().text),
                           syntax::LocationOf(keyword)};
        Advance();
        carry->pipeline = ParsePipeline();
        return carry;
      }
    }

    auto pipe = Make<syntax::Pipe>(keyword);
    pipe->pipeline = ParsePipeline();
    if (ContinuesWith(TokenKind::kArrow)) SkipNewlines();
    if (!Expect(TokenKind::kArrow, "'->' and a destination port")) return pipe;
    pipe->targets.push_back(ParseReference());
    while (AcceptToken(TokenKind::kComma)) {
      SkipNewlines();
      pipe->targets.push_back(ParseReference());
    }
    pipe->after = ParseAfter();
    return pipe;
  }

  bool NextWordIs(std::string_view word) const {
    size_t offset = 0;
    while (Peek(offset).kind == TokenKind::kNewline) ++offset;
    const Token& token = Peek(offset);
    return token.IsWord() && token.text == word;
  }

  /// `skip pipeline`, or `skip n reference` for the first `n`.
  ///
  /// The counted form takes a reference rather than a pipeline because the count
  /// belongs to the node: it is the node's first `n` values that go unread, for
  /// every reader of it, which is not something a pipeline of one reader's own
  /// could say.
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
      skip->pipeline = std::move(pipeline);
    } else {
      skip->pipeline = ParsePipeline();
    }
    skip->after = ParseAfter();
    return skip;
  }

  /// `fail`, `fail thing`, or `fail code thing`.
  NodePtr ParseFail(const Token& keyword) {
    auto fail = Make<syntax::Fail>(keyword);
    if (!AtStatementEnd()) fail->code = ParseExpression();
    if (!AtStatementEnd()) fail->message = ParseExpression();
    fail->after = ParseAfter();
    return fail;
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
    if (!At(TokenKind::kRightParen)) node->id = ParseExpression();
    Expect(TokenKind::kRightParen);
    if (AcceptWord("in")) node->node_map = ExpectName("a node map name");
    return node;
  }

  bool AtStatementEnd() const {
    return Current().EndsStatement() || AtWord("after");
  }

  /// A trailing `after a, b` on a statement that is not a call.
  std::vector<Word> ParseAfter() {
    std::vector<Word> names;
    if (!AcceptWord("after")) return names;
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
    loop->variable = ExpectName("a loop variable");
    ExpectWord("in");
    {
      const BlockHeader header(this);
      loop->pipeline = ParsePipeline();
    }
    if (AcceptWord("parallel")) loop->parallel = ExpectCount();
    loop->body = ParseBlock();
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
    if (AcceptWord("max")) repeat->max_iterations = ExpectCount();
    repeat->body = ParseBlock();
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
    if (ContinuesWith(TokenKind::kWord) && NextWordIs("else")) SkipNewlines();
    if (AcceptWord("else")) {
      if (AtWord("if")) {
        branch->else_body.push_back(ParseIf());
      } else {
        branch->else_body = ParseBlock();
      }
    }
    return branch;
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
    // puts the action on the stream this flow is attached to. A11 itself draws
    // the line in the same place, between `Action::Run` and `Action::Call`, so a
    // flow says it the way everything else does.
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
        if (position_ == before) Advance();
        break;
      }
      SkipNewlines();
      if (position_ == before) Advance();
    }
    Expect(TokenKind::kRightParen);
    call->modifiers = ParseModifiers();
    return call;
  }

  /// Whether a line break is followed by a modifier for this call.
  ///
  /// Modifiers read well on a line of their own, so a break before one continues
  /// the call -- unless what follows looks like a statement in its own right,
  /// which is what a port called `timeout` left of a `->` is.
  bool ContinuesWithModifier() const {
    size_t offset = 0;
    while (Peek(offset).kind == TokenKind::kNewline) ++offset;
    if (offset == 0) return false;
    if (!vocabulary::ModifierWords().contains(Keyword(offset))) return false;
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
      if (ContinuesWithModifier()) SkipNewlines();
      if (!vocabulary::ModifierWords().contains(Keyword())) break;
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
        if (!ExpectWord("headers")) continue;
        while (true) {
          if (!At(TokenKind::kString)) {
            ReportHere("flow.syntax.unexpected",
                       absl::StrCat("Expected a header name, found ", Found(),
                                    "."));
            break;
          }
          modifiers->forward.push_back(std::string(Advance().string_value));
          if (At(TokenKind::kComma) && Peek().kind == TokenKind::kString) {
            Advance();
            continue;
          }
          break;
        }
      } else if (modifier == "with") {
        while (true) {
          if (!At(TokenKind::kString)) {
            ReportHere("flow.syntax.unexpected",
                       absl::StrCat("Expected a header name, found ", Found(),
                                    "."));
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
      // front of those two. Everything else is a transformation applied to a
      // stream, which is what `|` says, and keeping the bar there is what stops
      // a stage name from swallowing the port that happens to share its name.
      if (ContinuesWith(TokenKind::kPipe) || ContinuesWithBareStage()) {
        SkipNewlines();
      }
      if (AcceptToken(TokenKind::kPipe)) {
        SkipNewlines();
        pipeline->stages.push_back(ParseStage());
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
  bool AtBareStage() const {
    if (!vocabulary::BareStages().contains(Keyword())) return false;
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
  bool ContinuesWithBareStage() const {
    size_t offset = 0;
    while (Peek(offset).kind == TokenKind::kNewline) ++offset;
    if (offset == 0) return false;
    if (!vocabulary::BareStages().contains(Keyword(offset))) return false;
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

  syntax::StagePtr ParseStage() {
    const Token& keyword = Current();
    auto stage = Make<syntax::Stage>(keyword);
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
        // A stream rather than a value: `then` reads this one and then that one,
        // so its argument is whatever a pipeline may start with.
        stage->argument = ParsePostfix();
        break;
    }
    return stage;
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
    if (syntax::IsAnyOf(node.get(),
                        {syntax::NodeKind::kName, syntax::NodeKind::kAttr,
                         syntax::NodeKind::kOutcome,
                         syntax::NodeKind::kError})) {
      return node;
    }
    Report("flow.syntax.unexpected",
           "Expected a port or a node here, like 'out-port' or 'call.port'.",
           at);
    return node;
  }

  NodePtr ParseExpression() { return ParseOr(); }

  NodePtr ParseOr() {
    NodePtr left = ParseAnd();
    while (AtWord("or")) {
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
    while (AtWord("and")) {
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
    if (IsComparison(Current().kind)) {
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
  /// composition cannot express any other way. `-` needs its spaces: `a-b` is one
  /// name, because an action is called `text-upper`.
  NodePtr ParseAdditive() {
    NodePtr left = ParseCast();
    while (At(TokenKind::kPlus) || At(TokenKind::kMinus)) {
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
  /// A `{` opens a block, not a value, in an `if` condition and a `for`'s source,
  /// for the reason Go forbids the same thing in the same places: `if x.y {` has
  /// to keep meaning what it looks like. Brackets of any kind turn it back on, so
  /// `if (x as T{a: 1}).ok { }` is still available.
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

  /// Turns `Tag{...}` back on for as long as it is in scope: inside brackets a
  /// `{` cannot be opening a block.
  class Bracketed {
   public:
    explicit Bracketed(ParserImpl* parser)
        : parser_(parser), outer_(parser->brace_literals_) {
      parser_->brace_literals_ = true;
    }
    ~Bracketed() { parser_->brace_literals_ = outer_; }
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
      // `a11.sdk.Interaction{...}`: a value of a named type, written the way the
      // type's own fields read. A generic one is spelled with `as`, where the
      // brackets cannot be mistaken for an index.
      if (At(TokenKind::kLeftBrace) && brace_literals_) {
        std::optional<std::string> name = syntax::DottedName(node.get());
        if (!name.has_value()) return node;
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
    if (At(TokenKind::kString)) {
      Advance();
      auto literal = Make<syntax::Literal>(token);
      literal->value = syntax::Constant::String(token.string_value);
      return literal;
    }
    if (At(TokenKind::kNumber)) {
      Advance();
      auto literal = Make<syntax::Literal>(token);
      literal->value = token.is_integer
                           ? syntax::Constant::Integer(
                                 static_cast<long long>(token.number))
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
        list->items.push_back(ParseExpression());
        SkipNewlines();
        if (!AcceptToken(TokenKind::kComma)) {
          if (position_ == before) Advance();
          break;
        }
        SkipNewlines();
        if (position_ == before) Advance();
      }
      Expect(TokenKind::kRightBracket);
      return list;
    }
    if (At(TokenKind::kLeftBrace)) return ParseObjectLiteral();
    if (Current().IsWord()) {
      const std::string spelled(Advance().text);
      const std::string word = vocabulary::Canonical(spelled);
      if (word == "true" || word == "false") {
        auto literal = Make<syntax::Literal>(token);
        literal->value = syntax::Constant::Bool(word == "true");
        return literal;
      }
      if (word == "null") return Make<syntax::Literal>(token);
      if (word == "it") return Make<syntax::It>(token);
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
            if (position_ == before) Advance();
            break;
          }
          SkipNewlines();
          if (position_ == before) Advance();
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

  /// `{ key: expr, ... }`, wherever one is allowed.
  ///
  /// Inside the braces a line break is never the end of anything, so an object
  /// with a field per line reads the way it is written.
  NodePtr ParseObjectLiteral() {
    const Token& brace = Current();
    auto object = Make<syntax::ObjectLiteral>(brace);
    if (!Expect(TokenKind::kLeftBrace)) return object;
    result_.value_braces.push_back(brace.start);
    const Bracketed brackets(this);
    SkipNewlines();
    while (!At(TokenKind::kRightBrace)) {
      if (At(TokenKind::kEnd)) {
        ReportHere("flow.syntax.unclosed", "Missing '}'.");
        return object;
      }
      const size_t before = position_;
      std::string key;
      if (At(TokenKind::kString)) {
        key = std::string(Advance().string_value);
      } else {
        key = ExpectName("an object key").text;
      }
      Expect(TokenKind::kColon);
      object->pairs.emplace_back(std::move(key), ParseExpression());
      SkipNewlines();
      if (!AcceptToken(TokenKind::kComma)) {
        if (position_ == before) Advance();
        break;
      }
      SkipNewlines();
      if (position_ == before) Advance();
    }
    Expect(TokenKind::kRightBrace);
    return object;
  }

  Word ParseDottedName(std::string_view what) {
    if (At(TokenKind::kString)) {
      const Token& token = Advance();
      return Word{std::string(token.string_value), syntax::LocationOf(token)};
    }
    const Word first = ExpectName(what);
    if (first.Empty()) return first;
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
  Token TokenAt(const Location& location) const {
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
  /// The flow being read, so every diagnostic says which one it is in.
  std::string flow_name_;
  ParseResult result_;
};

}  // namespace

bool ParseResult::HasErrors() const { return FirstError() != nullptr; }

const Diagnostic* absl_nullable ParseResult::FirstError() const {
  for (const Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == Severity::kError) return &diagnostic;
  }
  return nullptr;
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
