// Copyright 2026 The A11 Authors.

#include "a11/flow/highlight.h"

#include <string>
#include <string_view>
#include <vector>

#include <absl/strings/str_cat.h>
#include <gtest/gtest.h>

#include "a11/flow/lexer.h"

namespace a11::flow {
namespace {

/// Every token as `semantic-kind:text`, which is what a colour scheme sees.
std::vector<std::string> Coloured(std::string_view source) {
  const LexResult lexed = Lex(source);
  const std::vector<SemanticToken> semantic = Highlight(lexed.tokens);
  std::vector<std::string> out;
  for (const SemanticToken& token : semantic) {
    if (token.kind == SemanticKind::kPunctuation &&
        source.substr(token.start, token.end - token.start) == "\n") {
      continue;  // a line break has no colour worth asserting
    }
    out.push_back(absl::StrCat(SemanticKindName(token.kind), ":",
                               source.substr(token.start,
                                             token.end - token.start)));
  }
  return out;
}

TEST(FlowHighlight, ADeclarationNamesItsFlow) {
  EXPECT_EQ(Coloured("flow research {}"),
            (std::vector<std::string>{"declaration-keyword:flow",
                                      "flow-name:research", "brace:{",
                                      "brace:}"}));
}

TEST(FlowHighlight, AKeywordMayBeUpperCaseButNotMixed) {
  EXPECT_EQ(Coloured("FLOW research"),
            (std::vector<std::string>{"declaration-keyword:FLOW",
                                      "flow-name:research"}));
  // `Flow` is a name, which is what the compiler makes of it too.
  EXPECT_EQ(Coloured("Flow research"),
            (std::vector<std::string>{"identifier:Flow", "identifier:research"}));
}

TEST(FlowHighlight, APortDeclarationReadsAsOne) {
  EXPECT_EQ(Coloured("in question: string required \"What to find out.\""),
            (std::vector<std::string>{
                "declaration-keyword:in", "identifier:question",
                "punctuation::", "type:string", "declaration-keyword:required",
                "string:\"What to find out.\""}));
}

TEST(FlowHighlight, APortTypeMayBeATagOrGenericAndStopsAtTheLine) {
  EXPECT_EQ(Coloured("out audio: a11.sdk.AudioBuffer stream"),
            (std::vector<std::string>{
                "declaration-keyword:out", "identifier:audio", "punctuation::",
                "type:a11", "punctuation:.", "type:sdk", "punctuation:.",
                "type:AudioBuffer", "declaration-keyword:stream"}));
  EXPECT_EQ(Coloured("in frames: list[a11.NodeFragment] required"),
            (std::vector<std::string>{
                "declaration-keyword:in", "identifier:frames", "punctuation::",
                "type:list", "bracket:[", "type:a11", "punctuation:.",
                "type:NodeFragment", "bracket:]",
                "declaration-keyword:required"}));
  // A declaration is one line: the statement under it is a statement.
  EXPECT_EQ(Coloured("in q: string\npage.text"),
            (std::vector<std::string>{"declaration-keyword:in", "identifier:q",
                                      "punctuation::", "type:string",
                                      "identifier:page", "punctuation:.",
                                      "member:text"}));
}

TEST(FlowHighlight, InOpensAPortDeclarationOnlyWhereANameAndAColonFollow) {
  EXPECT_EQ(Coloured("for hit in search.hits"),
            (std::vector<std::string>{"statement-keyword:for",
                                      "identifier:hit",
                                      "declaration-keyword:in",
                                      "identifier:search", "punctuation:.",
                                      "member:hits"}));
}

TEST(FlowHighlight, AWordIsAStageOnlyAfterAPipe) {
  EXPECT_EQ(Coloured("page.text | truncate 200 | count"),
            (std::vector<std::string>{"identifier:page", "punctuation:.",
                                      "member:text", "flow-operator:|",
                                      "stage:truncate", "number:200",
                                      "flow-operator:|", "stage:count"}));
  // The same words nowhere near a pipe: `text` is the port type it is here, and
  // `count` is nothing in particular.
  EXPECT_EQ(Coloured("text count"),
            (std::vector<std::string>{"type:text", "identifier:count"}));
}

TEST(FlowHighlight, ThenAndWhereAreStagesWithoutAPipeGivenAnOperand) {
  EXPECT_EQ(Coloured("hits where it.ok"),
            (std::vector<std::string>{"identifier:hits", "stage:where",
                                      "constant:it", "punctuation:.",
                                      "member:ok"}));
  // With nothing to be a stage *of*, both are ordinary names.
  EXPECT_EQ(Coloured("then"), std::vector<std::string>{"identifier:then"});
  EXPECT_EQ(Coloured("where -> answer"),
            (std::vector<std::string>{"identifier:where", "flow-operator:->",
                                      "identifier:answer"}));
}

TEST(FlowHighlight, ACallNamesAnActionAndItsModifiersAreModifiers) {
  EXPECT_EQ(
      Coloured("page = try call web-fetch(url: hit.url) timeout 20s via fetched"),
      (std::vector<std::string>{
          "identifier:page", "operator:=", "statement-keyword:try",
          "statement-keyword:call", "action-name:web-fetch", "parenthesis:(",
          "identifier:url", "punctuation::", "identifier:hit", "punctuation:.",
          "member:url", "parenthesis:)", "modifier-keyword:timeout",
          "duration:20s", "modifier-keyword:via", "node-map-name:fetched"}));
}

TEST(FlowHighlight, ABindingMayStillBeCalledRun) {
  // `run = run x()` is legal -- a statement word before `=` is a name.
  EXPECT_EQ(Coloured("run = run some-action"),
            (std::vector<std::string>{"identifier:run", "operator:=",
                                      "statement-keyword:run",
                                      "action-name:some-action"}));
}

TEST(FlowHighlight, AContextCarriesAcrossALineBreakAndAComment) {
  EXPECT_EQ(Coloured("call\n  some-action"),
            (std::vector<std::string>{"statement-keyword:call",
                                      "action-name:some-action"}));
  EXPECT_EQ(Coloured("call # which one\n  some-action"),
            (std::vector<std::string>{"statement-keyword:call",
                                      "comment:# which one",
                                      "action-name:some-action"}));
}

TEST(FlowHighlight, NodeIsTheKeywordOnlyWhereItMakesANode) {
  EXPECT_EQ(Coloured("said = node() in scratch"),
            (std::vector<std::string>{
                "identifier:said", "operator:=", "declaration-keyword:node",
                "parenthesis:(", "parenthesis:)", "declaration-keyword:in",
                "identifier:scratch"}));
  // Making one takes parentheses, so a bare `node` is a name.
  EXPECT_EQ(Coloured("node -> answer"),
            (std::vector<std::string>{"identifier:node", "flow-operator:->",
                                      "identifier:answer"}));
}

TEST(FlowHighlight, AFunctionIsAFunctionWhereItIsCalled) {
  EXPECT_EQ(Coloured("| map join(it, \", \")"),
            (std::vector<std::string>{"flow-operator:|", "stage:map",
                                      "builtin:join", "parenthesis:(",
                                      "constant:it", "punctuation:,",
                                      "string:\", \"", "parenthesis:)"}));
  // `join` with no argument list after it is the stage, not the function.
  EXPECT_EQ(Coloured("| join"),
            (std::vector<std::string>{"flow-operator:|", "stage:join"}));
}

TEST(FlowHighlight, AStatusCodeIsAConstantAndAFieldIsAField) {
  EXPECT_EQ(Coloured("fail not_found \"gone\""),
            (std::vector<std::string>{"statement-keyword:fail",
                                      "status-code:not_found",
                                      "string:\"gone\""}));
  EXPECT_EQ(Coloured("fail NOT_FOUND"),
            (std::vector<std::string>{"statement-keyword:fail",
                                      "status-code:NOT_FOUND"}));
  // `ok` after a dot is the field of a status record, not the code.
  EXPECT_EQ(Coloured("outcome.ok"),
            (std::vector<std::string>{"identifier:outcome", "punctuation:.",
                                      "member:ok"}));
}

TEST(FlowHighlight, ATagBeforeABraceIsAType) {
  EXPECT_EQ(Coloured("| map a11.sdk.Interaction{role: \"user\"}"),
            (std::vector<std::string>{
                "flow-operator:|", "stage:map", "type:a11", "punctuation:.",
                "type:sdk", "punctuation:.", "type:Interaction", "brace:{",
                "identifier:role", "punctuation::", "string:\"user\"",
                "brace:}"}));
  // A bare name before a brace is a name and a block, which is what keeps
  // `if outcome {` reading the way it always has.
  EXPECT_EQ(Coloured("if outcome {"),
            (std::vector<std::string>{"statement-keyword:if",
                                      "identifier:outcome", "brace:{"}));
}

TEST(FlowHighlight, ACastNamesAType) {
  EXPECT_EQ(Coloured("x as a11.sdk.AudioBuffer"),
            (std::vector<std::string>{"identifier:x",
                                      "declaration-keyword:as", "type:a11",
                                      "punctuation:.", "type:sdk",
                                      "punctuation:.", "type:AudioBuffer"}));
}

TEST(FlowHighlight, EveryTokenIsClassifiedAndNoneIsInvented) {
  const std::string source =
      "flow t {\n  in q: string\n  q | first 1 -> a  # done\n}\n";
  const LexResult lexed = Lex(source);
  const std::vector<SemanticToken> semantic = Highlight(lexed.tokens);
  // One entry per token, the closing `end` aside: a frontend pairs them up by
  // index and by offset.
  EXPECT_EQ(semantic.size(), lexed.tokens.size() - 1);
  for (size_t index = 0; index < semantic.size(); ++index) {
    EXPECT_EQ(semantic[index].start, lexed.tokens[index].start);
    EXPECT_EQ(semantic[index].end, lexed.tokens[index].end);
    EXPECT_EQ(semantic[index].line, lexed.tokens[index].line);
  }
}

TEST(FlowHighlight, SemanticKindNamesRoundTrip) {
  for (const SemanticKind kind :
       {SemanticKind::kComment, SemanticKind::kString, SemanticKind::kNumber,
        SemanticKind::kDuration, SemanticKind::kDeclarationKeyword,
        SemanticKind::kStatementKeyword, SemanticKind::kModifierKeyword,
        SemanticKind::kStage, SemanticKind::kBuiltin, SemanticKind::kType,
        SemanticKind::kStatusCode, SemanticKind::kConstant,
        SemanticKind::kWordOperator, SemanticKind::kFlowName,
        SemanticKind::kActionName, SemanticKind::kNodeMapName,
        SemanticKind::kMember, SemanticKind::kIdentifier,
        SemanticKind::kFlowOperator, SemanticKind::kOperator,
        SemanticKind::kBrace, SemanticKind::kParenthesis,
        SemanticKind::kBracket, SemanticKind::kPunctuation,
        SemanticKind::kBad}) {
    EXPECT_EQ(SemanticKindFromName(SemanticKindName(kind)), kind)
        << SemanticKindName(kind);
  }
}

}  // namespace
}  // namespace a11::flow
