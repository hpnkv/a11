// Copyright 2026 The A11 Authors.

#include "a11/flow/complete.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/strings/str_cat.h>

#include "a11/flow/lexer.h"
#include "a11/flow/parser.h"
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
    case vocabulary::StageArgument::kStream:
      return " source";
  }
  return "";
}

/// Everything that may be written at one offset in one document.
///
/// Written as a class because every decision needs the same four things -- the
/// tokens, the statement the caret is in, the tree and the resolved names -- and
/// threading those through twenty free functions would say less about the rules
/// than it cost.
class Completer {
 public:
  Completer(std::string_view source, size_t offset)
      : source_(source), offset_(offset < source.size() ? offset : source.size()) {
    LexResult lexed = Lex(source_, LexOptions{.keep_comments = true});
    tokens_ = std::move(lexed.tokens);
    parsed_ = ParseTokens(source_, tokens_, std::move(lexed.diagnostics));
    resolved_ = Resolve(source_, parsed_);
    Locate();
  }

  /// Whether this text is a flow body rather than a file of flows.
  bool IsFragment() const {
    if (!parsed_.flows.empty()) return false;
    for (const Token& token : tokens_) {
      if (token.kind == TokenKind::kEnd) break;
      if (token.kind == TokenKind::kNewline ||
          token.kind == TokenKind::kComment) {
        continue;
      }
      // The first thing that is not a blank line: a fragment is anything that
      // is not somebody part-way through typing `flow`.
      return !(token.IsWord() && vocabulary::Canonical(token.text) == "flow");
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

  /// Find the caret: which token it is in, what word it is part-way through, and
  /// which tokens count as being *before* it.
  void Locate() {
    cut_ = 0;
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
      // sitting at the end of one is the caret being *in* one -- somebody typing
      // prose, where the language's words are noise.
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

  /// The canonical form of the word before the caret, or empty if it is not one.
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
  /// Read off the tokens rather than the resolved plan on purpose: the statement
  /// the caret is in is usually half-written, and the word after `run` is there
  /// long before the step it belongs to resolves to anything.
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
      proposals_.push_back(std::move(proposal));
    }
  }

  void AddTypes() {
    for (const std::string_view name : vocabulary::OrderedTypeNames()) {
      Add(std::string(name), ProposalKind::kType);
    }
  }

  void AddStatusCodes() {
    for (const std::string_view code : vocabulary::StatusCodes()) {
      Add(std::string(code), ProposalKind::kStatusCode);
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
  /// Above the caret, because a flow reads top to bottom even though it does not
  /// *run* that way: offering a step's name on the line before the step exists
  /// would be offering to write something that cannot resolve.
  template <typename Predicate>
  void AddNames(Predicate accept) {
    if (flow_ == nullptr) return;
    for (const Symbol& symbol : flow_->symbols) {
      if (symbol.location.start > offset_) continue;
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
    // Outside a flow there is exactly one thing a file may say next.
    if (open_braces_.empty()) {
      Add("flow", ProposalKind::kDeclaration, " name { }");
      return;
    }

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
    if (ProposeDeclaration()) return;
    if (ProposeCall()) return;
    if (ProposeAfterWord()) return;
    if (ProposeExpression()) return;
    if (line_.empty()) {
      ProposeStatement();
      return;
    }
    // A word standing alone at the head of a statement is a pipeline source, and
    // the two stages that may be written without a `|` are what may join it to
    // something else.
    if (Previous() != nullptr && Previous()->IsWord() && line_.size() == 1) {
      for (const std::string_view stage : vocabulary::Stages()) {
        if (!vocabulary::BareStages().contains(stage)) continue;
        Add(std::string(stage), ProposalKind::kStage,
            StageTail(*vocabulary::StageTakes(stage)));
      }
    }
  }

  /// What follows a `.`: only what the thing before it actually has.
  void ProposeMembers() {
    const Token* base = Previous(1);
    if (base == nullptr || !base->IsWord() || flow_ == nullptr) return;
    const Symbol* symbol = nullptr;
    for (const Symbol& candidate : flow_->symbols) {
      if (candidate.name == base->text) symbol = &candidate;
    }
    if (symbol == nullptr) return;
    switch (symbol->kind) {
      case SymbolKind::kCall: {
        // What a call has: its ports, and how it went. Outputs first, because
        // reading one is what a step after it is for.
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
        // A node's id is the one thing about it that is a value: what an action
        // told where to write is told.
        Add("id", ProposalKind::kField);
        return;
      default:
        // A port or a variable carries whatever the producer sent. Nothing here
        // knows its fields, and guessing would offer a name that is not there.
        return;
    }
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

  /// What has a status: what `wait`, `status`, `cancel`, `drain` and `after` name.
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
      // A call names another flow of this file, or an action registered where
      // the flow runs -- which is not knowable from the text, so what can be
      // offered is the flows.
      for (const FlowPlan& candidate : resolved_.program.flows) {
        if (flow_ != nullptr && candidate.name == flow_->plan.name) continue;
        Proposal proposal;
        proposal.name = candidate.name;
        proposal.kind = ProposalKind::kFlow;
        // A call is a name and its arguments, so taking the name writes the
        // parentheses the arguments go in and leaves the caret in them.
        proposal.insert = absl::StrCat(candidate.name, "()");
        proposal.caret = static_cast<int>(candidate.name.size()) + 1;
        proposal.tail = candidate.description;
        proposals_.push_back(std::move(proposal));
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
    // After the arguments: the modifiers, and the operand of the one just given.
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
    const FlowPlan* target = FlowNamed(action);
    if (target == nullptr) return;
    for (const PortPlan& port : target->ports) {
      if (port.direction != syntax::PortDirection::kInput) continue;
      bool given = false;
      for (const size_t index : line_) {
        const Token& token = tokens_[index];
        if (token.IsWord() && token.text == port.name) given = true;
      }
      if (given) continue;
      Proposal proposal;
      proposal.name = port.name;
      proposal.kind = ProposalKind::kPort;
      // An argument is a name and a colon; writing the colon is writing what
      // the grammar requires, not guessing what the author meant.
      proposal.insert = absl::StrCat(port.name, ": ");
      proposal.tail = port.required ? " (required)" : "";
      proposal.type = port.declared;
      proposals_.push_back(std::move(proposal));
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
    if (previous == "wait" || previous == "drain" || previous == "cancel" ||
        previous == "status") {
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

  /// Where a value is wanted: the functions, the constants, and what is readable.
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
    // A stage that takes an expression is looking at a value, and so is the only
    // place `it` means anything.
    bool in_stage = false;
    const std::string word = PreviousWord();
    if (!word.empty()) {
      const auto stage = vocabulary::StageTakes(word);
      if (stage.has_value() &&
          *stage == vocabulary::StageArgument::kExpression) {
        // Only where the word is being used as a stage: `where` after a `|`, or
        // bare with an operand to come.
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
      return true;
    }
    if (!value_wanted) return false;
    AddFunctions();
    AddConstants(in_stage);
    ProposeSources();
    return true;
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
      // `stream` and `required` to a port, and `node` to the name being bound to
      // it.
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
  }

  std::string_view source_;
  size_t offset_ = 0;
  std::vector<Token> tokens_;
  ParseResult parsed_;
  ResolveResult resolved_;
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

CompleteResult CompleteAt(std::string_view source, size_t offset) {
  Completer completer(source, offset);
  if (!completer.IsFragment()) return completer.Run();
  const std::string wrapped =
      absl::StrCat(kFragmentPrefix, source, "\n}");
  CompleteResult result =
      Completer(wrapped, offset + kFragmentPrefix.size()).Run();
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
