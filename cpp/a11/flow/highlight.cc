// Copyright 2026 The A11 Authors.

#include "a11/flow/highlight.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <absl/types/span.h>

#include "a11/flow/parser.h"
#include "a11/flow/resolve.h"
#include "a11/flow/token.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow {
namespace {

/// Where in a declaration the classifier is, which is all the state it needs.
///
/// Every one of these is a position the grammar gives a word a meaning in that
/// it has nowhere else. They are the states the plugin's hand-written lexer
/// had, and they are here now instead.
enum class State {
  /// Nowhere in particular: the word before says nothing about this one.
  kDefault,
  /// Straight after `flow`: the flow's own name.
  kAfterFlow,
  /// Straight after `struct`: the shape's own name, which is a type everywhere
  /// else in the file and so is coloured as one here.
  kAfterDto,
  /// Straight after `run`/`call`: the action being dispatched.
  kAfterCall,
  /// Straight after `nodes` or `via`: a node map.
  kAfterNodeMap,
  /// Between a port's `in`/`out` and its `:`.
  kInPortName,
  /// Past a port's `:`: its type, then what the port is like.
  kInPortType,
  /// Straight after `as`: the type a value is being made into.
  kAfterCast,
  /// From a `header` to the end of its line.
  ///
  /// Its own state because a header's `as` is not the cast the same word is
  /// everywhere else: `header "x" as budget` *renames*, so `budget` is a name
  /// and not a type. Without this the alias was coloured as a type and, worse,
  /// the cast state outlived the line and swallowed the next one.
  kInHeader,
  /// Inside the dotted tag of a `Tag{...}` value, up to the `{`.
  ///
  /// Its own state because the decision is made at the front of the chain -- a
  /// `{` after a *dotted* name is a type, a `{` after a bare one opens a block
  /// --
  /// and the parts after the first dot have no way to see that from where they
  /// stand.
  kInTypeTag,
};

class Classifier {
 public:
  explicit Classifier(absl::Span<const Token> tokens) : tokens_(tokens) {}

  std::vector<SemanticToken> Run() {
    std::vector<SemanticToken> out;
    out.reserve(tokens_.size());
    for (size_t index = 0; index < tokens_.size(); ++index) {
      const Token& token = tokens_[index];
      if (token.kind == TokenKind::kEnd) {
        break;
      }
      index_ = index;
      const SemanticKind kind = Classify(token);
      out.push_back(SemanticToken{kind, token.start, token.end, token.line,
                                  token.column});
      Advance(token, kind);
    }
    return out;
  }

 private:
  [[nodiscard]] const Token& At(size_t index) const {
    static const Token kEnd;
    return index < tokens_.size() ? tokens_[index] : kEnd;
  }

  /// The next token that is not a comment or a line break.
  ///
  /// Comments and breaks say nothing, so the context carries across them:
  /// `call`
  /// on one line and its action on the next is still an action name.
  [[nodiscard]] const Token& NextReal(size_t from) const {
    for (size_t index = from; index < tokens_.size(); ++index) {
      const TokenKind kind = tokens_[index].kind;
      if (kind == TokenKind::kComment || kind == TokenKind::kNewline) {
        continue;
      }
      return tokens_[index];
    }
    return At(tokens_.size());
  }

  [[nodiscard]] const Token& Next() const { return NextReal(index_ + 1); }

  /// Whether the word being read is followed by an argument list.
  [[nodiscard]] bool CallFollows() const {
    return At(index_ + 1).kind == TokenKind::kLeftParen;
  }

  /// Whether a single `=` follows, making the word before it a binding name.
  [[nodiscard]] bool AssignmentFollows() const {
    return At(index_ + 1).kind == TokenKind::kEqual;
  }

  /// Whether `in`/`out` here opens a port declaration.
  ///
  /// `in` is four other things as well -- a `for`'s, a node's, the comparison
  /// --
  /// so the word alone will not do. A port is the one shape where a name and a
  /// `:` follow, which is the lookahead the parser makes too.
  [[nodiscard]] bool PortDeclarationFollows() const {
    return At(index_ + 1).IsWord() && At(index_ + 2).kind == TokenKind::kColon;
  }

  // Whether the word here is the start of a longer expression rather than a
  // word standing alone: `error.message`, `levels[0]`, `info(x)`.
  /// Whether the word here is the start of a longer expression rather than a
  /// word standing alone: `error.message`, `levels[0]`, `info(x)`.
  ///
  /// The same test the parser makes before reading a bare word after a `log` as
  /// the level, so the colour and the meaning agree.
  [[nodiscard]] bool ExpressionContinues() const {
    switch (At(index_ + 1).kind) {
      case TokenKind::kDot:
      case TokenKind::kLeftBracket:
      case TokenKind::kLeftParen:
        return true;
      default:
        return false;
    }
  }

  /// Whether something a stage could apply to follows a bare `then`/`where`.
  [[nodiscard]] bool OperandFollows() const {
    switch (At(index_ + 1).kind) {
      case TokenKind::kWord:
      case TokenKind::kString:
      case TokenKind::kNumber:
      case TokenKind::kDuration:
      case TokenKind::kLeftParen:
      case TokenKind::kLeftBracket:
      case TokenKind::kLeftBrace:
        return true;
      default:
        return false;
    }
  }

  /// Whether this word starts the tag of a `Tag{...}` value.
  ///
  /// A `.` somewhere in the name is required, because a bare `name {` is a name
  /// and a block -- which is what keeps `if outcome {` reading as it always
  /// has.
  [[nodiscard]] bool TypeLiteralFollows() const {
    size_t index = index_;
    bool dotted = false;
    while (At(index).IsWord()) {
      if (At(index + 1).kind != TokenKind::kDot) {
        break;
      }
      if (!At(index + 2).IsWord()) {
        break;
      }
      dotted = true;
      index += 2;
    }
    if (At(index + 1).kind != TokenKind::kLeftBrace) {
      return false;
    }
    // A dotted name before a `{` is always a tag: nothing else is written that
    // way.
    return dotted || !LineOpensBlock();
  }

  /// Whether the line being read is a block header: `flow x {`, `if c {`.
  ///
  /// The same judgement `ParserImpl::brace_literals_` makes, arrived at from
  /// the token stream alone: only these words open a body, and each of them
  /// opens it at the head of its line. Everywhere else a `{` holds a value.
  [[nodiscard]] bool LineOpensBlock() const {
    size_t at = index_;
    while (at > 0 && At(at - 1).kind != TokenKind::kNewline) {
      --at;
    }
    while (at < index_ && !At(at).IsWord()) {
      ++at;
    }
    if (!At(at).IsWord()) {
      return false;
    }
    static const auto* const kOpeners = new absl::flat_hash_set<std::string>{
        "flow", "struct", "nodes", "if", "else", "for", "repeat", "try"};
    return kOpeners->contains(vocabulary::Canonical(At(at).text));
  }

  SemanticKind Classify(const Token& token) {
    switch (token.kind) {
      case TokenKind::kComment:
        return SemanticKind::kComment;
      case TokenKind::kString:
        return SemanticKind::kString;
      case TokenKind::kNumber:
        return SemanticKind::kNumber;
      case TokenKind::kDuration:
        return SemanticKind::kDuration;
      case TokenKind::kBad:
        return SemanticKind::kBad;
      case TokenKind::kArrow:
      case TokenKind::kCarry:
      case TokenKind::kPipe:
        return SemanticKind::kFlowOperator;
      case TokenKind::kLeftBrace:
      case TokenKind::kRightBrace:
        return SemanticKind::kBrace;
      case TokenKind::kLeftParen:
      case TokenKind::kRightParen:
        return SemanticKind::kParenthesis;
      case TokenKind::kLeftBracket:
      case TokenKind::kRightBracket:
        return SemanticKind::kBracket;
      case TokenKind::kDot:
      case TokenKind::kColon:
      case TokenKind::kComma:
        return SemanticKind::kPunctuation;
      case TokenKind::kNewline:
        return SemanticKind::kPunctuation;
      case TokenKind::kWord:
        return ClassifyWord(token);
      default:
        return SemanticKind::kOperator;
    }
  }

  SemanticKind ClassifyWord(const Token& token) {
    const std::string word = vocabulary::Canonical(token.text);

    // Whatever follows a `.` is a member, however it is spelled -- unless the
    // dots are part of a type, which the states below own.
    if (after_dot_ && state_ != State::kInPortType &&
        state_ != State::kAfterCast && state_ != State::kInTypeTag) {
      return SemanticKind::kMember;
    }
    // Inside a shape, the word before a `:` is the field's own name -- whatever
    // else that word means elsewhere. Without this a field called `id` or
    // `with` would be coloured as the call modifier it is not.
    if (InFieldPosition() && At(index_ + 1).kind == TokenKind::kColon) {
      return SemanticKind::kIdentifier;
    }
    switch (state_) {
      case State::kInTypeTag:
        return SemanticKind::kType;
      case State::kAfterFlow:
        return SemanticKind::kFlowName;
      case State::kAfterDto:
        return SemanticKind::kType;
      case State::kAfterCall:
        return SemanticKind::kActionName;
      case State::kAfterNodeMap:
        return SemanticKind::kNodeMapName;
      case State::kInPortName:
        // The word between `in`/`out` and the `:` is the port's own name,
        // whatever else it might mean elsewhere.
        return SemanticKind::kIdentifier;
      case State::kAfterCast:
        return SemanticKind::kType;
      case State::kInHeader:
        // `as` and `default` are the declaration's own words; the alias and a
        // written-out constant are what they take.
        if (word == "as" || word == "default") {
          return SemanticKind::kDeclarationKeyword;
        }
        if (vocabulary::ConstantWords().contains(word)) {
          return SemanticKind::kConstant;
        }
        return SemanticKind::kIdentifier;
      case State::kInPortType:
        // Past a port's or a field's `:` a word is its type, built-in, a shape
        // or a registry tag. `stream`, `required`, `matching` and the rest say
        // what the port or field is like instead.
        if (vocabulary::PortModifierWords().contains(word) ||
            vocabulary::FieldModifierWords().contains(word)) {
          return SemanticKind::kDeclarationKeyword;
        }
        return SemanticKind::kType;
      case State::kDefault:
        break;
    }

    if (after_pipe_ && vocabulary::StageTakes(word).has_value()) {
      return SemanticKind::kStage;
    }
    // `a11.sdk.Interaction{...}`: a tag naming a type where no list could know
    // it, so position decides.
    if (TypeLiteralFollows()) {
      return SemanticKind::kType;
    }
    // `then` and `where` are stages without their `|` too -- but only with an
    // operand, since a bare one is a port that happens to share the name.
    if (vocabulary::BareStages().contains(word) && OperandFollows()) {
      return SemanticKind::kStage;
    }
    // Making a node takes parentheses, so `node` is the keyword only where one
    // opens: a port called `node` is a name like any other.
    if (word == "node" && !CallFollows()) {
      return SemanticKind::kIdentifier;
    }
    // A function is only a function where it is called, which is what tells
    // `text(it)` from the port type and the stage of the same name.
    if (vocabulary::Builtins().contains(word) && CallFollows()) {
      return SemanticKind::kBuiltin;
    }
    if (vocabulary::ConstantWords().contains(word)) {
      return SemanticKind::kConstant;
    }
    if (vocabulary::OperatorWords().contains(word)) {
      return SemanticKind::kWordOperator;
    }
    if (vocabulary::DeclarationWords().contains(word)) {
      return SemanticKind::kDeclarationKeyword;
    }
    if (vocabulary::StatementWords().contains(word) ||
        vocabulary::SourceWords().contains(word) ||
        vocabulary::ClauseWords().contains(word)) {
      // A statement word before a single `=` is a binding name: `run = run
      // x()`.
      if (AssignmentFollows()) {
        return SemanticKind::kIdentifier;
      }
      return SemanticKind::kStatementKeyword;
    }
    if (vocabulary::ModifierWords().contains(word)) {
      return SemanticKind::kModifierKeyword;
    }
    if (vocabulary::TypeNames().contains(word)) {
      return SemanticKind::kType;
    }
    if (vocabulary::IsStatusCode(word)) {
      return SemanticKind::kStatusCode;
    }
    // A level is a level only directly after the word that takes one. Elsewhere
    // `error` and `info` are names as ordinary as any other, and a port called
    // one of them should not be coloured as a keyword.
    if (after_log_ && vocabulary::IsLogLevel(word) && !ExpressionContinues()) {
      return SemanticKind::kLogLevel;
    }
    return SemanticKind::kIdentifier;
  }

  void Advance(const Token& token, SemanticKind produced) {
    const bool was_after_dot = after_dot_;
    after_dot_ = token.kind == TokenKind::kDot;
    if (token.kind == TokenKind::kComment) {
      // A comment interrupts nothing.
      after_dot_ = was_after_dot;
      return;
    }
    if (token.kind == TokenKind::kPipe) {
      after_pipe_ = true;
      after_log_ = false;
      return;
    }
    if (token.kind != TokenKind::kNewline) {
      after_pipe_ = false;
    }
    if (token.kind != TokenKind::kNewline) {
      const std::string word = vocabulary::Canonical(token.text);
      after_log_ = token.IsWord() && (word == "log" || word == "logf") &&
                   (produced == SemanticKind::kStatementKeyword ||
                    produced == SemanticKind::kStage);
    }

    // A tag runs from its first word to the `{` that makes it one.
    if (state_ == State::kInTypeTag) {
      if (token.kind == TokenKind::kLeftBrace) {
        state_ = State::kDefault;
      }
      return;
    }
    // A header's declaration runs to the end of its line, and *everything* on
    // it belongs to the declaration -- including the strings, which is what a
    // header's name and a text default are.
    if (state_ == State::kInHeader) {
      if (token.kind == TokenKind::kNewline) {
        state_ = State::kDefault;
      }
      return;
    }

    switch (token.kind) {
      case TokenKind::kNewline:
        // A **declaration** is one line: what follows the next one is not its
        // port's type, and not the type a cast on the line above asked for.
        if (state_ == State::kInPortName || state_ == State::kInPortType ||
            state_ == State::kAfterCast) {
          state_ = State::kDefault;
        }
        return;
      case TokenKind::kColon:
        // A port says which `:` is its own with `in`/`out`; a field has only
        // its
        // position to say so, which is why the depth is counted.
        if (state_ == State::kInPortName || InFieldPosition()) {
          state_ = State::kInPortType;
        }
        return;
      case TokenKind::kLeftBrace:
        if (expect_dto_body_) {
          dto_body_ = braces_;
          expect_dto_body_ = false;
        }
        ++braces_;
        state_ = State::kDefault;
        return;
      case TokenKind::kRightBrace:
        braces_ = std::max(0, braces_ - 1);
        if (dto_body_ >= 0 && braces_ <= dto_body_) {
          dto_body_ = -1;
        }
        state_ = State::kDefault;
        return;
      case TokenKind::kDot:
      case TokenKind::kComma:
      case TokenKind::kLeftBracket:
      case TokenKind::kRightBracket:
        // A type is written with dots and brackets -- `list[a11.Frag]` -- and
        // none of them ends it.
        return;
      case TokenKind::kWord:
        break;
      default:
        state_ = State::kDefault;
        return;
    }

    const std::string word = vocabulary::Canonical(token.text);
    if (state_ == State::kInPortName || state_ == State::kInPortType) {
      return;
    }
    if (state_ == State::kAfterCast) {
      return;
    }
    if (word == "header" && produced == SemanticKind::kDeclarationKeyword) {
      state_ = State::kInHeader;
      return;
    }
    if (produced == SemanticKind::kType && TypeLiteralFollows()) {
      state_ = State::kInTypeTag;
      return;
    }
    if (produced == SemanticKind::kMember || produced == SemanticKind::kStage) {
      state_ = State::kDefault;
      return;
    }
    if (word == "as" && produced == SemanticKind::kDeclarationKeyword) {
      state_ = State::kAfterCast;
      return;
    }
    if ((word == "in" || word == "out") &&
        produced == SemanticKind::kDeclarationKeyword &&
        PortDeclarationFollows()) {
      state_ = State::kInPortName;
      return;
    }
    if (word == "flow" && produced == SemanticKind::kDeclarationKeyword) {
      state_ = State::kAfterFlow;
      return;
    }
    if (word == "struct" && produced == SemanticKind::kDeclarationKeyword &&
        At(index_ + 1).IsWord()) {
      state_ = State::kAfterDto;
      expect_dto_body_ = true;
      return;
    }
    if ((word == "run" || word == "call") &&
        produced == SemanticKind::kStatementKeyword) {
      state_ = State::kAfterCall;
      return;
    }
    if (word == "nodes" && produced == SemanticKind::kDeclarationKeyword) {
      state_ = State::kAfterNodeMap;
      return;
    }
    if (word == "via" && produced == SemanticKind::kModifierKeyword) {
      state_ = State::kAfterNodeMap;
      return;
    }
    state_ = State::kDefault;
  }

  /// Whether a field of a shape may be written here: directly inside a `struct`
  /// body, and not inside a value written in one.
  [[nodiscard]] bool InFieldPosition() const {
    return dto_body_ >= 0 && braces_ == dto_body_ + 1;
  }

  absl::Span<const Token> tokens_;
  size_t index_ = 0;
  State state_ = State::kDefault;
  bool after_dot_ = false;
  bool after_pipe_ = false;
  /// Whether the word just seen was a `log`/`logf` keyword, which is the one
  /// position a level word means a level. See [WordKind].
  bool after_log_ = false;
  /// How many braces are open. Counted only so a field can be told from a value
  /// written inside one; nothing else here needs nesting.
  int braces_ = 0;
  /// The depth the innermost `struct` body opened at, or -1 outside one.
  int dto_body_ = -1;
  /// Whether the next `{` opens a shape's body.
  bool expect_dto_body_ = false;
};

}  // namespace

std::string_view SemanticKindName(SemanticKind kind) {
  switch (kind) {
    case SemanticKind::kComment:
      return "comment";
    case SemanticKind::kString:
      return "string";
    case SemanticKind::kNumber:
      return "number";
    case SemanticKind::kDuration:
      return "duration";
    case SemanticKind::kDeclarationKeyword:
      return "declaration-keyword";
    case SemanticKind::kStatementKeyword:
      return "statement-keyword";
    case SemanticKind::kModifierKeyword:
      return "modifier-keyword";
    case SemanticKind::kStage:
      return "stage";
    case SemanticKind::kBuiltin:
      return "builtin";
    case SemanticKind::kType:
      return "type";
    case SemanticKind::kStatusCode:
      return "status-code";
    case SemanticKind::kLogLevel:
      return "log-level";
    case SemanticKind::kConstant:
      return "constant";
    case SemanticKind::kWordOperator:
      return "word-operator";
    case SemanticKind::kFlowName:
      return "flow-name";
    case SemanticKind::kActionName:
      return "action-name";
    case SemanticKind::kNodeMapName:
      return "node-map-name";
    case SemanticKind::kMember:
      return "member";
    case SemanticKind::kPortName:
      return "port-name";
    case SemanticKind::kIdentifier:
      return "identifier";
    case SemanticKind::kFlowOperator:
      return "flow-operator";
    case SemanticKind::kOperator:
      return "operator";
    case SemanticKind::kBrace:
      return "brace";
    case SemanticKind::kParenthesis:
      return "parenthesis";
    case SemanticKind::kBracket:
      return "bracket";
    case SemanticKind::kPunctuation:
      return "punctuation";
    case SemanticKind::kBad:
      return "bad";
  }
  return "identifier";
}

SemanticKind SemanticKindFromName(std::string_view name) {
  static constexpr SemanticKind kAll[] = {
      SemanticKind::kComment,
      SemanticKind::kString,
      SemanticKind::kNumber,
      SemanticKind::kDuration,
      SemanticKind::kDeclarationKeyword,
      SemanticKind::kStatementKeyword,
      SemanticKind::kModifierKeyword,
      SemanticKind::kStage,
      SemanticKind::kBuiltin,
      SemanticKind::kType,
      SemanticKind::kStatusCode,
      SemanticKind::kLogLevel,
      SemanticKind::kConstant,
      SemanticKind::kWordOperator,
      SemanticKind::kFlowName,
      SemanticKind::kActionName,
      SemanticKind::kNodeMapName,
      SemanticKind::kMember,
      SemanticKind::kPortName,
      SemanticKind::kIdentifier,
      SemanticKind::kFlowOperator,
      SemanticKind::kOperator,
      SemanticKind::kBrace,
      SemanticKind::kParenthesis,
      SemanticKind::kBracket,
      SemanticKind::kPunctuation,
      SemanticKind::kBad,
  };
  for (const SemanticKind kind : kAll) {
    if (SemanticKindName(kind) == name) {
      return kind;
    }
  }
  return SemanticKind::kIdentifier;
}

std::vector<SemanticToken> Highlight(absl::Span<const Token> tokens) {
  return Classifier(tokens).Run();
}

void RefinePorts(std::string_view source,
                 std::vector<SemanticToken>& semantic) {
  const ParseResult parsed = Parse(source);
  const ResolveResult resolved = Resolve(source, parsed);
  if (resolved.flows.empty()) {
    return;
  }

  // Where each top-level declaration begins.
  std::vector<size_t> boundaries;
  boundaries.reserve(parsed.flows.size());
  for (const syntax::FlowDeclarationPtr& flow : parsed.flows) {
    boundaries.push_back(flow->location.start);
  }
  for (const syntax::DtoDeclarationPtr& dto : parsed.dtos) {
    boundaries.push_back(dto->location.start);
  }
  std::sort(boundaries.begin(), boundaries.end());

  /// One flow's span, and the names its ports have.
  struct Span {
    size_t start = 0;
    size_t end = 0;
    absl::flat_hash_set<std::string> ports;
  };

  std::vector<Span> spans;
  for (const ResolvedFlow& flow : resolved.flows) {
    if (flow.declaration == nullptr) {
      continue;
    }
    Span span;
    span.start = flow.declaration->location.start;
    span.end = source.size();
    for (const size_t boundary : boundaries) {
      if (boundary > span.start) {
        span.end = boundary;
        break;
      }
    }
    for (const PortPlan& port : flow.plan.ports) {
      span.ports.insert(port.name);
    }
    if (!span.ports.empty()) {
      spans.push_back(std::move(span));
    }
  }
  if (spans.empty()) {
    return;
  }

  for (SemanticToken& token : semantic) {
    // Only a plain identifier: a member after a `.`, a keyword and a string
    // that happens to spell a port's name are all already what they are.
    if (token.kind != SemanticKind::kIdentifier) {
      continue;
    }
    const std::string_view text =
        source.substr(token.start, token.end - token.start);
    for (const Span& span : spans) {
      if (token.start < span.start || token.start >= span.end) {
        continue;
      }
      if (span.ports.contains(text)) {
        token.kind = SemanticKind::kPortName;
      }
      break;
    }
  }
}

}  // namespace a11::flow
