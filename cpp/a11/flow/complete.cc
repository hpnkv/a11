// Copyright 2026 The A11 Authors.

#include "a11/flow/complete.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/strings/str_cat.h>

#include "a11/flow/lexer.h"
#include "a11/flow/navigate.h"
#include "a11/flow/parser.h"
#include "a11/flow/pattern.h"
#include "a11/flow/plan.h"
#include "a11/flow/resolve.h"
#include "a11/flow/syntax.h"
#include "a11/flow/token.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow {
namespace {

/// What a fragment is wrapped in to be completed as a flow body.
///
/// A `.flow` file declares flows; a fragment injected into a Python string is
/// the *inside* of one, and an editor completing in one wants the names that
/// fragment binds. Wrapping is how it gets them without a second grammar: the
/// text becomes a flow, the offset moves by the length of this, and everything
/// afterwards is the ordinary path.
constexpr std::string_view kFragmentPrefix = "flow __fragment__ {\n";

constexpr size_t kNoIndex = static_cast<size_t>(-1);

/// Whether a token is one of the three things that end a statement, which is
/// also what tells the completer where the statement under the caret began.
bool BoundaryKind(TokenKind kind) {
  return kind == TokenKind::kNewline || kind == TokenKind::kLeftBrace ||
         kind == TokenKind::kRightBrace;
}

/// A description as the grey text beside a proposal: one sentence, one line.
///
/// An action's description is prose written for a model and runs to a
/// paragraph; a completion list has one line per item, and a paragraph in it
/// pushes every other item off the screen. The first sentence is what a reader
/// scanning the list actually uses, and hover shows the rest.
std::string Summary(std::string_view description) {
  if (description.empty()) return "";
  std::string_view first = description.substr(0, description.find('\n'));
  const size_t stop = first.find(". ");
  if (stop != std::string_view::npos) first = first.substr(0, stop + 1);
  constexpr size_t kWidest = 72;
  if (first.size() > kWidest) {
    return absl::StrCat(" ", first.substr(0, kWidest - 1), "...");
  }
  return absl::StrCat(" ", first);
}

/// The grey text an editor shows after a stage, saying what it takes.
std::string_view StageTail(vocabulary::StageArgument argument) {
  switch (argument) {
    case vocabulary::StageArgument::kNone:
      return "";
    case vocabulary::StageArgument::kNumber:
      return " n";
    case vocabulary::StageArgument::kExpression:
      return " expr";
    case vocabulary::StageArgument::kString:
      return " \"text\"";
    case vocabulary::StageArgument::kOptionalString:
      return " [\"text\"]";
    case vocabulary::StageArgument::kOptionalExpression:
      return " [expr]";
    case vocabulary::StageArgument::kSortKey:
      return " [by expr] [desc]";
    case vocabulary::StageArgument::kFold:
      return " start as name, expr";
    case vocabulary::StageArgument::kDuration:
      // Not a concrete `30s`: the two stages that take one want very different
      // numbers, and a hint that reads as a default is a hint that gets kept.
      return " duration";
    case vocabulary::StageArgument::kStream:
      return " source";
    case vocabulary::StageArgument::kLog:
      return " [level] [expr]";
    case vocabulary::StageArgument::kLogFormat:
      return " [level] \"fmt\" [arg, ...]";
  }
  return "";
}

/// Which of the language's word tables documents a proposal of this kind, or
/// `nullopt` where the proposal names something this document or the host
/// declared rather than a word of the language.
///
/// A name the *document* bound gets its text from the plan it was resolved to,
/// beside where this is called; this is only for the fixed vocabulary. `kField`
/// is deliberately absent: a field proposal may be a status's `code` or a
/// shape's own field of the same name, and answering with the status text for
/// the latter would be confidently wrong.
std::optional<vocabulary::WordRole> RoleOf(ProposalKind kind) {
  switch (kind) {
    case ProposalKind::kStatement:
      return vocabulary::WordRole::kStatement;
    case ProposalKind::kDeclaration:
      return vocabulary::WordRole::kDeclaration;
    case ProposalKind::kModifier:
      return vocabulary::WordRole::kModifier;
    case ProposalKind::kStage:
      return vocabulary::WordRole::kStage;
    case ProposalKind::kFunction:
      return vocabulary::WordRole::kBuiltin;
    case ProposalKind::kType:
      return vocabulary::WordRole::kType;
    case ProposalKind::kStatusCode:
      return vocabulary::WordRole::kStatusCode;
    case ProposalKind::kLogLevel:
      return vocabulary::WordRole::kLogLevel;
    case ProposalKind::kConstant:
      return vocabulary::WordRole::kConstant;
    case ProposalKind::kPortModifier:
      return vocabulary::WordRole::kPortModifier;
    case ProposalKind::kFlow:
    case ProposalKind::kPort:
    case ProposalKind::kNode:
    case ProposalKind::kNodeMap:
    case ProposalKind::kCall:
    case ProposalKind::kBarrier:
    case ProposalKind::kVariable:
    case ProposalKind::kHeader:
    case ProposalKind::kField:
      return std::nullopt;
  }
  return std::nullopt;
}

/// Everything that may be written at one offset in one document.
///
/// Written as a class because every decision needs the same four things -- the
/// tokens, the statement the caret is in, the tree and the resolved names --
/// and threading those through twenty free functions would say less about the
/// rules than it cost.
class Completer {
 public:
  Completer(std::string_view source, size_t offset,
            const catalogue::Catalogue& known)
      : source_(source), offset_(offset < source.size() ? offset : source.size()),
        known_(known) {
    LexResult lexed = Lex(source_, LexOptions{.keep_comments = true});
    tokens_ = std::move(lexed.tokens);
    parsed_ = ParseTokens(source_, tokens_, std::move(lexed.diagnostics));
    resolved_ = Resolve(source_, parsed_);
    Locate();
  }

  /// Whether this text is a flow body rather than a file of flows.
  bool IsFragment() const {
    if (!parsed_.flows.empty() || !parsed_.dtos.empty()) return false;
    for (const Token& token : tokens_) {
      if (token.kind == TokenKind::kEnd) break;
      if (token.kind == TokenKind::kNewline ||
          token.kind == TokenKind::kComment) {
        continue;
      }
      // The first thing that is not a blank line: a fragment is anything that
      // is not somebody part-way through typing a declaration.
      if (!token.IsWord()) return true;
      const std::string word = vocabulary::Canonical(token.text);
      return word != "flow" && word != "struct";
    }
    return false;
  }

  CompleteResult Run() {
    CompleteResult result;
    result.prefix = prefix_;
    result.prefix_start = prefix_start_;
    if (blocked_) return result;
    Propose();
    result.proposals = std::move(proposals_);
    return result;
  }

 private:
  // -- position ---------------------------------------------------------------

  /// Find the caret: which token it is in, what word it is part-way through,
  /// and which tokens count as being *before* it.
  void Locate() {
    cut_ = 0;
    // No partial word means one that starts *at the caret* and is empty, which
    // is what `prefix_start` has to say: a frontend replaces `[prefix_start,
    // caret)` with what it inserts. Left at zero, that range was the whole
    // document up to the caret -- so taking any proposal at a position where
    // nothing had been typed yet, which is most of them, deleted the file in
    // front of it.
    prefix_start_ = offset_;
    for (size_t index = 0; index < tokens_.size(); ++index) {
      const Token& token = tokens_[index];
      cut_ = index;
      if (token.kind == TokenKind::kEnd) break;
      if (token.end <= offset_) {
        cut_ = index + 1;
        continue;
      }
      if (token.start >= offset_) break;
      // The caret is inside this token. A word or a number is one being typed,
      // and the completion is about what may stand where it stands; anything
      // else -- inside a string, a comment, or half of a `->` -- is a place
      // where offering the language's words would be noise.
      if (token.IsWord() || token.kind == TokenKind::kNumber) {
        prefix_ = std::string(source_.substr(token.start, offset_ - token.start));
        prefix_start_ = token.start;
      } else {
        blocked_ = true;
      }
      break;
    }
    // A word the caret sits at the end of is still the word being typed.
    if (prefix_.empty() && !blocked_ && cut_ > 0) {
      const Token& last = tokens_[cut_ - 1];
      if ((last.IsWord() || last.kind == TokenKind::kNumber) &&
          last.end == offset_) {
        prefix_ = std::string(last.text);
        prefix_start_ = last.start;
        --cut_;
      }
      // A comment and a string both run to the end of their line, so the caret
      // sitting at the end of one is the caret being *in* one -- somebody
      // typing prose, where the language's words are noise.
      if ((last.kind == TokenKind::kComment ||
           last.kind == TokenKind::kString) &&
          last.end == offset_) {
        blocked_ = true;
      }
    }
    Survey();
  }

  /// Walk the tokens before the caret once, noting the things every rule asks
  /// about: how deep the braces are, what opened each of them, and where the
  /// statement under the caret began.
  void Survey() {
    for (size_t index = 0; index < cut_; ++index) {
      const TokenKind kind = tokens_[index].kind;
      if (kind == TokenKind::kLeftBrace) {
        open_braces_.push_back(index);
      } else if (kind == TokenKind::kRightBrace) {
        if (!open_braces_.empty()) {
          closed_open_ = open_braces_.back();
          open_braces_.pop_back();
        }
      }
      if (BoundaryKind(kind)) statement_start_ = index + 1;
    }
    // The lexer drops a trailing run of line breaks, because a break with
    // nothing after it ends nothing -- which is right for a parser and wrong
    // here: the caret on a fresh line is at the head of a statement, and the
    // only record of that break is the text itself.
    const size_t tail = cut_ == 0 ? 0 : tokens_[cut_ - 1].end;
    if (tail <= offset_ &&
        source_.substr(tail, offset_ - tail).find('\n') != std::string_view::npos) {
      statement_start_ = cut_;
    }
    for (size_t index = statement_start_; index < cut_; ++index) {
      if (tokens_[index].kind != TokenKind::kComment) line_.push_back(index);
    }
    // The flow the caret is in: the last one declared above it. A file being
    // typed usually has its problem in the flow at the bottom, and a
    // declaration that has not been closed yet still owns everything after it.
    for (const ResolvedFlow& flow : resolved_.flows) {
      if (flow.declaration == nullptr) continue;
      if (flow.declaration->location.start <= offset_) flow_ = &flow;
    }
  }

  // -- the token before the caret ---------------------------------------------

  const Token* Previous(size_t back = 0) const {
    if (line_.size() <= back) return nullptr;
    return &tokens_[line_[line_.size() - 1 - back]];
  }

  /// The canonical form of the word before the caret, or empty if it is not
  /// one.
  std::string PreviousWord(size_t back = 0) const {
    const Token* token = Previous(back);
    if (token == nullptr || !token->IsWord()) return "";
    return vocabulary::Canonical(token->text);
  }

  bool PreviousIs(TokenKind kind) const {
    const Token* token = Previous();
    return token != nullptr && token->kind == kind;
  }

  /// The first word of the statement holding `index`, canonicalised.
  std::string StatementWordAt(size_t index) const {
    size_t start = index;
    while (start > 0 && !BoundaryKind(tokens_[start - 1].kind)) --start;
    for (size_t at = start; at <= index; ++at) {
      const Token& token = tokens_[at];
      if (token.kind == TokenKind::kComment) continue;
      return token.IsWord() ? vocabulary::Canonical(token.text) : "";
    }
    return "";
  }

  /// Whether the caret is inside a block one of these words opened.
  bool InsideBlockOf(std::string_view word) const {
    for (const size_t brace : open_braces_) {
      if (StatementWordAt(brace) == word) return true;
    }
    return false;
  }

  /// Whether anything on this line is one of these kinds.
  bool LineHas(TokenKind kind) const {
    for (const size_t index : line_) {
      if (tokens_[index].kind == kind) return true;
    }
    return false;
  }

  /// How many parentheses on this line are still open.
  int OpenParens() const {
    int depth = 0;
    for (const size_t index : line_) {
      if (tokens_[index].kind == TokenKind::kLeftParen) ++depth;
      if (tokens_[index].kind == TokenKind::kRightParen) --depth;
    }
    return depth;
  }

  /// The action a `run`/`call` on this line names, or empty.
  ///
  /// Read off the tokens rather than the resolved plan on purpose: the
  /// statement the caret is in is usually half-written, and the word after
  /// `run` is there long before the step it belongs to resolves to anything.
  std::string CalledAction() const {
    for (size_t at = 0; at + 1 < line_.size(); ++at) {
      const Token& word = tokens_[line_[at]];
      if (!word.IsWord()) continue;
      const std::string canonical = vocabulary::Canonical(word.text);
      if (canonical != "run" && canonical != "call") continue;
      const Token& next = tokens_[line_[at + 1]];
      if (next.IsWord()) return std::string(next.text);
    }
    return "";
  }

  // -- proposals --------------------------------------------------------------

  void Add(std::string name, ProposalKind kind, std::string_view tail = "",
           std::string_view type = "") {
    Proposal proposal;
    proposal.name = std::move(name);
    proposal.kind = kind;
    proposal.insert = proposal.name;
    proposal.tail = std::string(tail);
    proposal.type = std::string(type);
    // A word of the language has the language's own reference text, and it is
    // the text a hover over the finished word gives: the popup beside the list
    // is the same question, asked a moment earlier.
    if (const std::optional<vocabulary::WordRole> role = RoleOf(kind);
        role.has_value()) {
      proposal.documentation = WordMarkdown(proposal.name, *role);
    }
    proposals_.push_back(std::move(proposal));
  }

  void AddStages() {
    for (const std::string_view stage : vocabulary::Stages()) {
      Add(std::string(stage), ProposalKind::kStage,
          StageTail(*vocabulary::StageTakes(stage)));
    }
  }

  void AddFunctions() {
    for (const std::string_view name : vocabulary::OrderedBuiltins()) {
      Proposal proposal;
      proposal.name = std::string(name);
      proposal.kind = ProposalKind::kFunction;
      // A function is never written without its parentheses, so taking one
      // writes them and leaves the caret between them.
      proposal.insert = absl::StrCat(name, "()");
      proposal.caret = static_cast<int>(name.size()) + 1;
      proposal.documentation = BuiltinMarkdown(name);
      proposals_.push_back(std::move(proposal));
    }
  }

  void AddTypes() {
    // The shapes this file declares come first: they are what somebody writing
    // in this file most likely means, and a built-in is one word away anyway.
    for (const DtoPlan& dto : resolved_.program.dtos) {
      Proposal proposal;
      proposal.name = dto.name;
      proposal.kind = ProposalKind::kType;
      proposal.insert = dto.name;
      proposal.tail = dto.description.empty()
                          ? absl::StrCat(" ", dto.fields.size(), " fields")
                          : Summary(dto.description);
      proposal.documentation = ShapeMarkdown(dto);
      proposals_.push_back(std::move(proposal));
    }
    for (const std::string_view name : vocabulary::OrderedTypeNames()) {
      Add(std::string(name), ProposalKind::kType);
    }
    // Then the tags the host knows. Last because a built-in is what most ports
    // carry, and a list that opened with twenty registry tags would bury them.
    for (const catalogue::TypeInfo& type : known_.types()) {
      Proposal proposal;
      proposal.name = type.tag;
      proposal.kind = ProposalKind::kType;
      proposal.insert = type.tag;
      proposal.tail = Summary(type.shape.description);
      proposal.documentation = ShapeMarkdown(type.shape);
      proposals_.push_back(std::move(proposal));
    }
  }

  void AddStatusCodes() {
    for (const std::string_view code : vocabulary::StatusCodes()) {
      Add(std::string(code), ProposalKind::kStatusCode);
    }
  }

  void AddLogLevels() {
    for (const std::string_view level : vocabulary::LogLevels()) {
      Add(std::string(level), ProposalKind::kLogLevel);
    }
  }

  void AddConstants(bool with_it) {
    Add("true", ProposalKind::kConstant);
    Add("false", ProposalKind::kConstant);
    Add("null", ProposalKind::kConstant);
    // `it` is the value a stage is looking at, so it means nothing where there
    // is no value in hand -- a condition, a header, an argument.
    if (with_it) Add("it", ProposalKind::kConstant);
  }

  ProposalKind KindOf(const Symbol& symbol) const {
    switch (symbol.kind) {
      case SymbolKind::kInputPort:
      case SymbolKind::kOutputPort:
        return ProposalKind::kPort;
      case SymbolKind::kHeader:
        return ProposalKind::kHeader;
      case SymbolKind::kCall:
        return ProposalKind::kCall;
      case SymbolKind::kNode:
        return ProposalKind::kNode;
      case SymbolKind::kNodeMap:
        return ProposalKind::kNodeMap;
      case SymbolKind::kBarrier:
        return ProposalKind::kBarrier;
      case SymbolKind::kValue:
        return ProposalKind::kVariable;
      case SymbolKind::kLoopVariable:
      case SymbolKind::kCarry:
        return ProposalKind::kVariable;
    }
    return ProposalKind::kPort;
  }

  /// The type a port symbol was declared with, for the grey text beside it.
  std::string TypeOf(const Symbol& symbol) const {
    if (flow_ == nullptr) return "";
    if (symbol.kind != SymbolKind::kInputPort &&
        symbol.kind != SymbolKind::kOutputPort) {
      return "";
    }
    const syntax::PortDirection direction =
        symbol.kind == SymbolKind::kInputPort ? syntax::PortDirection::kInput
                                             : syntax::PortDirection::kOutput;
    const PortPlan* port = flow_->plan.Port(symbol.name, direction);
    return port == nullptr ? "" : port->declared;
  }

  /// Every name bound above the caret that a predicate accepts.
  ///
  /// Above the caret, because a flow reads top to bottom even though it does
  /// not *run* that way: offering a step's name on the line before the step
  /// exists would be offering to write something that cannot resolve.
  template <typename Predicate>
  void AddNames(Predicate accept) {
    if (flow_ == nullptr) return;
    const size_t statement =
        line_.empty() ? offset_ : tokens_[line_.front()].start;
    for (const Symbol& symbol : flow_->symbols) {
      if (symbol.location.start > offset_) continue;
      // Nor a name this very statement is binding: `let x = ` should not offer
      // `x`, and a flow reads in order, so nothing bound here is in scope yet.
      if (symbol.location.start >= statement) continue;
      if (!accept(symbol)) continue;
      Add(symbol.name, KindOf(symbol), "", TypeOf(symbol));
    }
  }

  /// The flow a call names, if it is one of this file's.
  const FlowPlan* FlowNamed(std::string_view name) const {
    return resolved_.program.Flow(name);
  }

  /// The ports of a called flow, offered as `step.port`.
  void AddCallPorts(syntax::PortDirection direction) {
    if (flow_ == nullptr) return;
    for (const Symbol& symbol : flow_->symbols) {
      if (symbol.kind != SymbolKind::kCall) continue;
      if (symbol.location.start > offset_) continue;
      const FlowPlan* target = FlowNamed(symbol.action);
      if (target == nullptr) continue;
      for (const PortPlan& port : target->ports) {
        if (port.direction != direction) continue;
        Add(absl::StrCat(symbol.name, ".", port.name), ProposalKind::kPort,
            port.required ? " (required)" : "", port.declared);
      }
    }
  }

  // -- the rules --------------------------------------------------------------

  void Propose() {
    // Outside a flow or a shape there are exactly two things a file may say
    // next.
    if (open_braces_.empty()) {
      Add("flow", ProposalKind::kDeclaration, " name { }");
      Add("struct", ProposalKind::kDeclaration, " Name { }");
      return;
    }

    // Inside a shape's body nothing else applies: a shape holds fields, and
    // offering statements there would be offering what cannot be written.
    if (InDtoBody()) {
      ProposeField();
      return;
    }
    // Inside a value of a declared shape, the fields of that shape are what the
    // keys may be -- which is the one place the language knows the keys of an
    // object it is looking at.
    if (ProposeShapeField()) return;

    if (PreviousIs(TokenKind::kPipe)) {
      AddStages();
      return;
    }
    if (PreviousIs(TokenKind::kDot)) {
      ProposeMembers();
      return;
    }
    if (PreviousIs(TokenKind::kArrow) ||
        (PreviousIs(TokenKind::kComma) && LineHas(TokenKind::kArrow))) {
      ProposeDestinations();
      return;
    }
    if (PreviousIs(TokenKind::kCarry)) {
      ProposeSources();
      return;
    }
    // `let name = stream`: naming the value first, then reading one. Neither
    // half is a statement position, so the ordinary list would be noise.
    if (!line_.empty() && tokens_[line_.front()].IsWord() &&
        vocabulary::Canonical(tokens_[line_.front()].text) == "let") {
      if (LineHas(TokenKind::kEqual)) ProposeSources();
      return;
    }
    if (ProposeDeclaration()) return;
    if (ProposeCall()) return;
    if (ProposeAfterWord()) return;
    if (ProposeExpression()) return;
    if (line_.empty()) {
      ProposeStatement();
      return;
    }
    // A word standing alone at the head of a statement is a pipeline source,
    // and the two stages that may be written without a `|` are what may join it
    // to something else.
    if (Previous() != nullptr && Previous()->IsWord() && line_.size() == 1) {
      for (const std::string_view stage : vocabulary::Stages()) {
        if (!vocabulary::BareStages().contains(stage)) continue;
        Add(std::string(stage), ProposalKind::kStage,
            StageTail(*vocabulary::StageTakes(stage)));
      }
    }
  }

  /// Whether the caret is directly inside a `struct` body.
  ///
  /// The outermost open brace is the body when the word before it is a shape's
  /// name and the word before *that* is `struct`; "directly" is the depth
  /// being one, since a value written in a field is not a place a field may be
  /// declared.
  bool InDtoBody() const {
    if (open_braces_.size() != 1) return false;
    const size_t brace = open_braces_.front();
    return brace >= 2 && tokens_[brace - 1].IsWord() &&
           tokens_[brace - 2].IsWord() &&
           vocabulary::Canonical(tokens_[brace - 2].text) == "struct";
  }

  /// A field being written: its type, then what may bound it.
  void ProposeField() {
    if (line_.empty()) {
      Add("describe", ProposalKind::kDeclaration, " \"...\"");
      return;
    }
    if (!LineHas(TokenKind::kColon)) return;  // naming the field
    if (PreviousIs(TokenKind::kColon)) {
      AddTypes();
      return;
    }
    const std::string previous = PreviousWord();
    // These take an argument, and until it is written nothing else may be.
    if (previous == "matching" || previous == "default" || previous == "of") {
      return;
    }
    for (const std::string_view modifier : vocabulary::OrderedFieldModifiers()) {
      bool present = false;
      for (const size_t index : line_) {
        const Token& token = tokens_[index];
        if (token.IsWord() &&
            vocabulary::Canonical(token.text) ==
                modifier.substr(0, modifier.find(' '))) {
          present = true;
        }
      }
      if (present) continue;
      Add(std::string(modifier), ProposalKind::kPortModifier,
          FieldModifierTail(modifier));
    }
  }

  /// What a field modifier takes, as grey text after it.
  static std::string FieldModifierTail(std::string_view modifier) {
    if (modifier == "matching") return " \"pattern\"";
    if (modifier == "one of") return " [values]";
    if (modifier == "default") return " value";
    return "";
  }

  /// The keys of a `Shape{...}` being written, when the shape is one this file
  /// declares.
  ///
  /// True when it answered, so the ordinary expression rules do not also run:
  /// inside those braces a bare word is a key and nothing else.
  bool ProposeShapeField() {
    if (open_braces_.empty()) return false;
    const size_t brace = open_braces_.back();
    if (brace == 0 || !tokens_[brace - 1].IsWord()) return false;
    // The dotted tag of `a11.sdk.AudioBuffer{` is several tokens; walk back
    // over them so a registered type's fields are offered the way a shape's
    // are.
    size_t start = brace - 1;
    while (start >= 2 && tokens_[start - 1].kind == TokenKind::kDot &&
           tokens_[start - 2].IsWord()) {
      start -= 2;
    }
    std::string named;
    for (size_t at = start; at < brace; ++at) {
      absl::StrAppend(&named, tokens_[at].text);
    }
    const DtoPlan* shape = resolved_.program.Dto(named);
    if (shape == nullptr) {
      // A type the host knows is the same idea as a shape this file declares,
      // and the catalogue records it in the same form -- so this is one code
      // path rather than two.
      const catalogue::TypeInfo* known = known_.Type(named);
      if (known != nullptr) shape = &known->shape;
    }
    if (shape == nullptr) return false;
    // Past a key's `:` the value is an ordinary expression again.
    for (const size_t index : line_) {
      if (tokens_[index].kind == TokenKind::kColon) return false;
    }
    if (PreviousIs(TokenKind::kColon)) return false;
    for (const FieldPlan& field : shape->fields) {
      Proposal proposal;
      proposal.name = field.name;
      proposal.kind = ProposalKind::kField;
      // A key is quoted and takes its colon, which is the whole of what has to
      // follow it.
      proposal.insert = absl::StrCat("\"", field.name, "\": ");
      proposal.tail = field.required ? " (required)" : "";
      proposal.type = field.declared;
      proposals_.push_back(std::move(proposal));
    }
    return true;
  }

  /// What follows a `.`: only what the thing before it actually has.
  void ProposeMembers() {
    const Token* base = Previous(1);
    if (base == nullptr || !base->IsWord() || flow_ == nullptr) return;
    // `it` is the value a stage is looking at, so what it has is whatever the
    // stage before said it would be.
    if (vocabulary::Canonical(base->text) == "it") {
      AddPatternFields(PatternBeforeCaret());
      return;
    }
    const Symbol* symbol = nullptr;
    for (const Symbol& candidate : flow_->symbols) {
      if (candidate.name == base->text) symbol = &candidate;
    }
    if (symbol == nullptr) return;
    switch (symbol->kind) {
      case SymbolKind::kCall: {
        // Offer outputs before inputs, followed by the call status.
        const FlowPlan* target = FlowNamed(symbol->action);
        if (target != nullptr) {
          for (const PortPlan& port : target->ports) {
            if (port.direction != syntax::PortDirection::kOutput) continue;
            Add(port.name, ProposalKind::kPort, "", port.declared);
          }
          for (const PortPlan& port : target->ports) {
            if (port.direction != syntax::PortDirection::kInput) continue;
            Add(port.name, ProposalKind::kPort,
                port.required ? " (required)" : "", port.declared);
          }
        }
        Add("status", ProposalKind::kField);
        return;
      }
      case SymbolKind::kBarrier:
        for (const std::string_view field : vocabulary::OrderedStatusFields()) {
          Add(std::string(field), ProposalKind::kField);
        }
        return;
      case SymbolKind::kNode:
        // Nodes expose their destination id as a value.
        Add("id", ProposalKind::kField);
        return;
      default:
        // Infer fields only from a match pattern or declared struct.
        if (AddPatternFields(symbol->pattern)) return;
        AddShapeFields(ShapeOfSymbol(*symbol));
        return;
    }
  }

  /// The struct a name carries, where the file said which: a port declared with
  /// one, or a value read from such a port.
  std::string ShapeOfSymbol(const Symbol& symbol) const {
    if (flow_ == nullptr) return "";
    for (const syntax::PortDirection direction :
         {syntax::PortDirection::kOutput, syntax::PortDirection::kInput}) {
      if (const PortPlan* port = flow_->plan.Port(symbol.name, direction);
          port != nullptr) {
        return port->type;
      }
    }
    return "";
  }

  /// The fields of a declared struct, where `named` is one.
  bool AddShapeFields(const std::string& named) {
    if (named.empty()) return false;
    const DtoPlan* shape = resolved_.program.Dto(named);
    if (shape == nullptr) {
      // A registry tag is a shape too, and the host knows its fields.
      const catalogue::TypeInfo* type = known_.Type(named);
      if (type == nullptr) return false;
      shape = &type->shape;
    }
    for (const FieldPlan& field : shape->fields) {
      Add(field.name, ProposalKind::kField,
          field.required ? " (required)" : "",
          field.declared.empty() ? field.type : field.declared);
    }
    return !shape->fields.empty();
  }

  /// The fields a `match` pattern names.
  ///
  /// The one place the language knows the fields of a value nobody declared:
  /// they are written in the pattern that made it. A positional pattern names
  /// nothing, so there is nothing to offer for one.
  bool AddPatternFields(const std::string& text) {
    if (text.empty()) return false;
    const pattern::Compiled compiled = pattern::Compile(text);
    if (!compiled.ok()) return false;
    bool any = false;
    for (const pattern::Hole& hole : compiled.pattern.holes) {
      if (hole.name.empty()) continue;
      Add(hole.name, ProposalKind::kField, "",
          std::string(pattern::HoleTypeName(hole.type)));
      any = true;
    }
    return any;
  }

  /// Return the nearest preceding `match` pattern on the caret's line.
  ///
  /// Token scanning supports incomplete `map it.` and `where it.` expressions.
  std::string PatternBeforeCaret() const {
    const size_t first = line_.empty() ? 0 : line_.front();
    for (size_t at = cut_; at > first; --at) {
      const size_t index = at - 1;
      if (index + 1 >= tokens_.size()) continue;
      if (!tokens_[index].IsWord()) continue;
      if (vocabulary::Canonical(tokens_[index].text) != "match") continue;
      if (tokens_[index + 1].kind != TokenKind::kString) return "";
      // The *value*, not the slice: `text` still has its quotes round it, and a
      // stray quote after a `rest` hole makes the pattern one nothing can
      // follow.
      return tokens_[index + 1].string_value;
    }
    return "";
  }

  void ProposeDestinations() {
    AddNames([](const Symbol& symbol) { return symbol.writable; });
    AddCallPorts(syntax::PortDirection::kInput);
  }

  void ProposeSources() {
    AddNames([](const Symbol& symbol) {
      return symbol.readable && symbol.kind != SymbolKind::kNodeMap;
    });
    AddCallPorts(syntax::PortDirection::kOutput);
  }

  /// What has a status: what `wait`, `status`, `cancel`, `drain` and `after`
  /// name.
  void ProposeSubjects() {
    AddNames([](const Symbol& symbol) {
      switch (symbol.kind) {
        case SymbolKind::kCall:
        case SymbolKind::kBarrier:
        case SymbolKind::kNode:
        case SymbolKind::kInputPort:
        case SymbolKind::kOutputPort:
          return true;
        default:
          return false;
      }
    });
  }

  /// A port or header declaration: the type, and then what it is like.
  bool ProposeDeclaration() {
    if (line_.empty()) return false;
    const Token& head = tokens_[line_.front()];
    if (!head.IsWord()) return false;
    const std::string word = vocabulary::Canonical(head.text);
    if (word == "in" || word == "out") {
      if (!LineHas(TokenKind::kColon)) return true;  // naming the port
      if (PreviousIs(TokenKind::kColon)) {
        AddTypes();
        return true;
      }
      // Past the type: what the port says about itself, minus what it has said.
      for (const std::string_view modifier : vocabulary::OrderedPortModifiers()) {
        bool present = false;
        for (const size_t index : line_) {
          const Token& token = tokens_[index];
          if (token.IsWord() && vocabulary::Canonical(token.text) == modifier) {
            present = true;
          }
        }
        if (!present) Add(std::string(modifier), ProposalKind::kPortModifier);
      }
      return true;
    }
    if (word == "header") {
      const std::string previous = PreviousWord();
      if (previous == "as" || previous == "default") return true;
      if (!LineHas(TokenKind::kString)) return true;
      for (const std::string_view part : {"as", "default"}) {
        bool present = false;
        for (const size_t index : line_) {
          const Token& token = tokens_[index];
          if (token.IsWord() && vocabulary::Canonical(token.text) == part) {
            present = true;
          }
        }
        if (!present) Add(std::string(part), ProposalKind::kDeclaration);
      }
      return true;
    }
    if (word == "nodes" && line_.size() == 1) return true;  // naming the map
    if (word == "describe") return true;
    return false;
  }

  /// Everything about a `run`/`call`: its target, its arguments, its modifiers.
  bool ProposeCall() {
    const std::string previous = PreviousWord();
    if (previous == "run" || previous == "call") {
      // A call names another flow of this file, or an action the world has.
      // The flows first: a file that factored a step into a flow of its own
      // means that one, and an action of the same name would be a surprise.
      const auto offer = [&](std::string_view name,
                             std::string_view description, ProposalKind kind,
                             std::string documentation) {
        Proposal proposal;
        proposal.name = std::string(name);
        proposal.kind = kind;
        // A call is a name and its arguments, so taking the name writes the
        // parentheses the arguments go in and leaves the caret in them.
        proposal.insert = absl::StrCat(name, "()");
        proposal.caret = static_cast<int>(name.size()) + 1;
        proposal.tail = Summary(description);
        // What the call target *is*, in full, for the popup beside the list.
        // A one-line tail is what fits; deciding between two actions needs
        // their ports, and leaving that out is what sent a reader to the docs.
        proposal.documentation = std::move(documentation);
        proposals_.push_back(std::move(proposal));
      };
      absl::flat_hash_set<std::string> offered;
      for (const FlowPlan& candidate : resolved_.program.flows) {
        if (flow_ != nullptr && candidate.name == flow_->plan.name) continue;
        offered.insert(candidate.name);
        offer(candidate.name, candidate.description, ProposalKind::kFlow,
              FlowMarkdown(candidate));
      }
      for (const catalogue::ActionInfo& action : known_.actions()) {
        if (offered.contains(action.name)) continue;
        offer(action.name, action.description, ProposalKind::kCall,
              ActionMarkdown(action));
      }
      return true;
    }
    const std::string action = CalledAction();
    if (action.empty()) return false;
    if (OpenParens() > 0) {
      if (PreviousIs(TokenKind::kLeftParen) || PreviousIs(TokenKind::kComma)) {
        ProposeArguments(action);
        return true;
      }
      return false;  // a value: the expression rules have it
    }
    if (!LineHas(TokenKind::kRightParen)) return false;
    // After the arguments: the modifiers, and the operand of the one just
    // given.
    if (previous == "via") {
      AddNames([](const Symbol& symbol) {
        return symbol.kind == SymbolKind::kNodeMap;
      });
      return true;
    }
    if (previous == "after") {
      ProposeSubjects();
      return true;
    }
    if (previous == "forward") {
      Add("headers", ProposalKind::kModifier, " \"x-name\"");
      return true;
    }
    if (previous == "timeout" || previous == "with" || previous == "id" ||
        previous == "headers") {
      return true;  // a duration, a header name, an expression
    }
    for (const std::string_view modifier : vocabulary::OrderedModifiers()) {
      Add(std::string(modifier), ProposalKind::kModifier);
    }
    return true;
  }

  /// The input ports of the flow being called, each taking its colon with it.
  void ProposeArguments(std::string_view action) {
    /// Whether this line has already given the argument.
    const auto given = [&](std::string_view port) {
      for (const size_t index : line_) {
        const Token& token = tokens_[index];
        if (token.IsWord() && token.text == port) return true;
      }
      return false;
    };
    const auto offer = [&](std::string_view name, std::string_view type,
                           bool required, bool unary,
                           std::string_view description) {
      if (given(name)) return;
      Proposal proposal;
      proposal.name = std::string(name);
      proposal.kind = ProposalKind::kPort;
      // An argument is a name and a colon; writing the colon is writing what
      // the grammar requires, not guessing what the author meant.
      proposal.insert = absl::StrCat(name, ": ");
      // Both, and in that order. `(required)` used to *replace* the
      // description, so the ports that have to be written were the ones the
      // list said least about -- which is backwards: what a port is for is the
      // question, and whether it is required is the aside.
      if (required) absl::StrAppend(&proposal.tail, " (required)");
      if (!description.empty()) {
        if (required) absl::StrAppend(&proposal.tail, " —");
        absl::StrAppend(&proposal.tail, Summary(description));
      }
      // A stream port is written differently from a single value, so the type
      // says which. This is the grey text beside the name in every frontend.
      proposal.type = absl::StrCat(type, unary ? "" : " stream");
      // The whole description, for the popup beside the list: a completion line
      // holds one sentence and a port's description is prose.
      proposal.documentation =
          PortMarkdown(name, type, required, unary, description);
      proposals_.push_back(std::move(proposal));
    };

    if (const FlowPlan* target = FlowNamed(action); target != nullptr) {
      for (const PortPlan& port : target->ports) {
        if (port.direction != syntax::PortDirection::kInput) continue;
        offer(port.name, port.declared.empty() ? port.type : port.declared,
              port.required, port.unary, port.description);
      }
      return;
    }
    // Not a flow of this file: an action the world has, which is exactly what
    // the catalogue is for. Required first, since those are the ones that have
    // to be written.
    const catalogue::ActionInfo* known = known_.Action(action);
    if (known == nullptr) return;
    for (const bool required : {true, false}) {
      for (const catalogue::PortInfo& port : known->inputs) {
        if (port.required != required) continue;
        offer(port.name, port.type, port.required, port.unary,
              port.description);
      }
    }
  }

  /// The words that take a named thing after them, wherever they stand.
  bool ProposeAfterWord() {
    const std::string previous = PreviousWord();
    if (previous.empty()) return false;
    if (previous == "fail") {
      AddStatusCodes();
      return true;
    }
    if (previous == "log" || previous == "logf") {
      // A level, and then whatever a value may be: the level is optional, so
      // both are offered rather than the level alone.
      AddLogLevels();
      return false;
    }
    if (previous == "wait") {
      // `wait first of a, b` and `wait all of a, b` share the position with the
      // single subject, so both the words and the subjects are offered.
      Add("first", ProposalKind::kModifier, " of ");
      Add("all", ProposalKind::kModifier, " of ");
      ProposeSubjects();
      return true;
    }
    if (previous == "drain" || previous == "cancel" || previous == "status") {
      ProposeSubjects();
      return true;
    }
    if (previous == "skip") {
      ProposeSources();
      return true;
    }
    if (previous == "after") {
      ProposeSubjects();
      return true;
    }
    if (previous == "in" && !line_.empty() &&
        vocabulary::Canonical(tokens_[line_.front()].text) == "for") {
      ProposeSources();
      return true;
    }
    if (previous == "for" || previous == "repeat" || previous == "nodes" ||
        previous == "max" || previous == "parallel") {
      // A name being bound, a count, or a width: nothing to offer, and offering
      // the names in scope here would suggest rebinding one.
      return true;
    }
    if (previous == "try") {
      Add("run", ProposalKind::kStatement);
      Add("call", ProposalKind::kStatement);
      return true;
    }
    return false;
  }

  /// Where a value is wanted: the functions, the constants, and what is
  /// readable.
  bool ProposeExpression() {
    const Token* previous = Previous();
    if (previous == nullptr) return false;
    bool value_wanted = false;
    switch (previous->kind) {
      case TokenKind::kEqualEqual:
      case TokenKind::kBangEqual:
      case TokenKind::kLess:
      case TokenKind::kLessEqual:
      case TokenKind::kGreater:
      case TokenKind::kGreaterEqual:
      case TokenKind::kPlus:
      case TokenKind::kMinus:
      case TokenKind::kLeftParen:
      case TokenKind::kLeftBracket:
      case TokenKind::kColon:
      case TokenKind::kComma:
        value_wanted = true;
        break;
      default:
        break;
    }
    // A stage that takes an expression is looking at a value, and so is the
    // only place `it` means anything.
    bool in_stage = false;
    const std::string word = PreviousWord();
    if (!word.empty()) {
      const auto stage = vocabulary::StageTakes(word);
      if (stage.has_value() &&
          (*stage == vocabulary::StageArgument::kExpression ||
           *stage == vocabulary::StageArgument::kOptionalExpression ||
           *stage == vocabulary::StageArgument::kFold)) {
        // Only where the word is being used as a stage: `where` after a `|`, or
        // bare with an operand to come.
        in_stage = true;
        value_wanted = true;
      }
      // `sort` is followed by its own two words before any expression, and
      // `by` is what puts a value after it.
      if (stage.has_value() &&
          *stage == vocabulary::StageArgument::kSortKey) {
        Add("by", ProposalKind::kModifier);
        Add("desc", ProposalKind::kModifier);
      }
      if (word == "by") {
        in_stage = true;
        value_wanted = true;
      }
      if (vocabulary::OperatorWords().contains(word) || word == "if" ||
          word == "until" || word == "while") {
        value_wanted = true;
      }
    }
    if (PreviousIs(TokenKind::kEqual)) {
      // A bind: `x = run ...`, `n = node()`, `s = wait x`, or a value.
      Add("run", ProposalKind::kStatement);
      Add("call", ProposalKind::kStatement);
      Add("try", ProposalKind::kStatement);
      Add("node", ProposalKind::kDeclaration, "()");
      Add("wait", ProposalKind::kStatement);
      Add("drain", ProposalKind::kStatement);
      ProposeSources();
      AddFunctions();
      AddConstants(false);
      AddCallTargets();
      return true;
    }
    if (!value_wanted) return false;
    AddFunctions();
    AddConstants(in_stage);
    ProposeSources();
    return true;
  }

  /// Every action and sibling flow, offered under its own name.
  ///
  /// Where a call may begin -- the head of a statement, the right of a `=` --
  /// but the word `call` has not been written yet. Somebody who knows the
  /// action types its name, not the verb, and a list that only knew action
  /// names *after* `call` answered that with nothing at all. Taking one writes
  /// the verb too, so what lands in the file is the statement, not a bare
  /// name.
  void AddCallTargets() {
    const auto offer = [&](std::string_view name, std::string_view description,
                           ProposalKind kind, std::string documentation) {
      Proposal proposal;
      proposal.name = std::string(name);
      proposal.kind = kind;
      proposal.insert = absl::StrCat("call ", name, "()");
      proposal.caret = static_cast<int>(name.size()) + 6;
      proposal.tail = Summary(description);
      proposal.documentation = std::move(documentation);
      proposals_.push_back(std::move(proposal));
    };
    absl::flat_hash_set<std::string> offered;
    for (const FlowPlan& candidate : resolved_.program.flows) {
      if (flow_ != nullptr && candidate.name == flow_->plan.name) continue;
      offered.insert(candidate.name);
      offer(candidate.name, candidate.description, ProposalKind::kFlow,
            FlowMarkdown(candidate));
    }
    for (const catalogue::ActionInfo& action : known_.actions()) {
      if (offered.contains(action.name)) continue;
      offer(action.name, action.description, ProposalKind::kCall,
            ActionMarkdown(action));
    }
  }

  /// The head of a statement: everything a flow may say, and every name it has.
  void ProposeStatement() {
    const bool in_repeat = InsideBlockOf("repeat");
    for (const std::string_view statement : vocabulary::OrderedStatements()) {
      // A loop tail belongs to a `repeat`, and saying so anywhere else would be
      // offering to write something that cannot compile.
      if ((statement == "until" || statement == "while") && !in_repeat) continue;
      Add(std::string(statement), ProposalKind::kStatement);
    }
    for (const std::string_view declaration : vocabulary::OrderedDeclarations()) {
      // The declarations that open a line. `flow` opens a file rather than a
      // statement in one, and the rest of them stand in the middle of a
      // declaration they cannot begin: `as` and `default` belong to a header,
      // `stream` and `required` to a port, and `node` to the name being bound
      // to it.
      if (declaration == "flow" || declaration == "as" ||
          declaration == "default" || declaration == "stream" ||
          declaration == "required" || declaration == "node") {
        continue;
      }
      Add(std::string(declaration), ProposalKind::kDeclaration);
    }
    // `else` only where an `if` has just closed, which is the one place it may
    // stand.
    if (closed_open_ != kNoIndex && Previous() == nullptr &&
        StatementWordAt(closed_open_) == "if") {
      Add("else", ProposalKind::kStatement, " { }");
    }
    AddNames([](const Symbol& symbol) {
      return symbol.kind != SymbolKind::kNodeMap;
    });
    AddCallTargets();
  }

  std::string_view source_;
  size_t offset_ = 0;
  std::vector<Token> tokens_;
  ParseResult parsed_;
  ResolveResult resolved_;
  /// What the world outside this document contains: see [CompleteAt].
  const catalogue::Catalogue& known_;
  const ResolvedFlow* flow_ = nullptr;

  /// Tokens `[0, cut_)` are before the caret.
  size_t cut_ = 0;
  std::string prefix_;
  size_t prefix_start_ = 0;
  bool blocked_ = false;

  /// Indices of the `{` still open at the caret, outermost first.
  std::vector<size_t> open_braces_;
  /// The `{` the most recent `}` closed, for `else`.
  size_t closed_open_ = kNoIndex;
  size_t statement_start_ = 0;
  /// The statement under the caret, comments dropped.
  std::vector<size_t> line_;
  std::vector<Proposal> proposals_;
};

}  // namespace

CompleteResult CompleteAt(std::string_view source, size_t offset,
                          const catalogue::Catalogue& known) {
  Completer completer(source, offset, known);
  if (!completer.IsFragment()) return completer.Run();
  const std::string wrapped =
      absl::StrCat(kFragmentPrefix, source, "\n}");
  CompleteResult result =
      Completer(wrapped, offset + kFragmentPrefix.size(), known).Run();
  result.prefix_start = result.prefix_start >= kFragmentPrefix.size()
                            ? result.prefix_start - kFragmentPrefix.size()
                            : 0;
  return result;
}

std::string_view ProposalKindName(ProposalKind kind) {
  switch (kind) {
    case ProposalKind::kStatement:
      return "statement";
    case ProposalKind::kDeclaration:
      return "declaration";
    case ProposalKind::kModifier:
      return "modifier";
    case ProposalKind::kStage:
      return "stage";
    case ProposalKind::kFunction:
      return "function";
    case ProposalKind::kType:
      return "type";
    case ProposalKind::kStatusCode:
      return "status-code";
    case ProposalKind::kLogLevel:
      return "log-level";
    case ProposalKind::kConstant:
      return "constant";
    case ProposalKind::kPortModifier:
      return "port-modifier";
    case ProposalKind::kFlow:
      return "flow";
    case ProposalKind::kPort:
      return "port";
    case ProposalKind::kNode:
      return "node";
    case ProposalKind::kNodeMap:
      return "node-map";
    case ProposalKind::kCall:
      return "call";
    case ProposalKind::kBarrier:
      return "barrier";
    case ProposalKind::kVariable:
      return "variable";
    case ProposalKind::kHeader:
      return "header";
    case ProposalKind::kField:
      return "field";
  }
  return "statement";
}

}  // namespace a11::flow
