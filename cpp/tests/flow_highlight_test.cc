// Copyright 2026 The A11 Authors.

#include "a11/flow/highlight.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <absl/strings/str_cat.h>
#include <gtest/gtest.h>

#include "a11/flow/lexer.h"

namespace a11::flow {
namespace {

/// Every token as `semantic-kind:text`, which is what a colour scheme sees.
std::vector<std::string> Coloured(std::string_view source,
                                  bool resolve = false) {
  const LexResult lexed = Lex(source);
  std::vector<SemanticToken> semantic = Highlight(lexed.tokens);
  // The second pass, where a test is about something only the resolver knows.
  if (resolve) RefinePorts(source, semantic);
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

TEST(FlowHighlight, ALevelIsALevelOnlyWhereALogTakesOne) {
  EXPECT_EQ(Coloured("log warning \"hm\""),
            (std::vector<std::string>{"statement-keyword:log",
                                      "log-level:warning", "string:\"hm\""}));
  EXPECT_EQ(Coloured("a | logf error \"hm\""),
            (std::vector<std::string>{"identifier:a", "flow-operator:|",
                                      "stage:logf", "log-level:error",
                                      "string:\"hm\""}));
  // Anywhere else `error` is a name as ordinary as any other, so a port called
  // one of the five is not coloured as a keyword.
  EXPECT_EQ(Coloured("error -> out"),
            (std::vector<std::string>{"identifier:error", "flow-operator:->",
                                      "declaration-keyword:out"}));
  EXPECT_EQ(Coloured("log error.message"),
            (std::vector<std::string>{"statement-keyword:log",
                                      "identifier:error", "punctuation:.",
                                      "member:message"}));
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

TEST(FlowHighlight, OfTiesASkipsOutputsToTheirCall) {
  EXPECT_EQ(Coloured("skip o1, o2 of act"),
            (std::vector<std::string>{
                "statement-keyword:skip", "identifier:o1", "punctuation:,",
                "identifier:o2", "statement-keyword:of", "identifier:act"}));
  // The same word before a `=` is a binding name, as every other clause and
  // statement word already is.
  EXPECT_EQ(Coloured("of = run act()"),
            (std::vector<std::string>{"identifier:of", "operator:=",
                                      "statement-keyword:run",
                                      "action-name:act", "parenthesis:(",
                                      "parenthesis:)"}));
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

TEST(FlowHighlight, ADeclarationEndsAtItsLine) {
  // `as` names a type in a cast and *renames* in a header, and the state it put
  // the classifier in used to outlive the line -- so the first word of the next
  // line was coloured as a type. Two header lines in a row is the shape that
  // showed it.
  EXPECT_EQ(Coloured("flow f {\n"
                     "  header \"x-a\" as a\n"
                     "  header \"x-b\" as b default 3\n"
                     "}"),
            (std::vector<std::string>{
                "declaration-keyword:flow", "flow-name:f", "brace:{",
                "declaration-keyword:header", "string:\"x-a\"",
                "declaration-keyword:as", "identifier:a",
                "declaration-keyword:header", "string:\"x-b\"",
                "declaration-keyword:as", "identifier:b",
                "declaration-keyword:default", "number:3", "brace:}"}));

  // And a real cast still names a type, on its own line and no further.
  EXPECT_EQ(Coloured("x as a11.Chunk\nheader \"y\" as y"),
            (std::vector<std::string>{
                "identifier:x", "declaration-keyword:as", "type:a11",
                "punctuation:.", "type:Chunk",
                "declaration-keyword:header", "string:\"y\"",
                "declaration-keyword:as", "identifier:y"}));
}

TEST(FlowHighlight, APortLooksDifferentFromAFlowsOwnNames) {
  // The one distinction that needs name resolution, so the one thing the second
  // pass decides: a port crosses the flow's boundary and a node does not, and a
  // reader following the data wants to see which is which.
  constexpr std::string_view kSource =
      "flow f {\n"
      "  in  question: string required\n"
      "  out answer:   string\n"
      "  pages = node()\n"
      "  question -> pages\n"
      "  pages -> answer\n"
      "}";
  const std::vector<std::string> plain = Coloured(kSource);
  const std::vector<std::string> resolved = Coloured(kSource, true);

  // Lexically they are all identifiers: nothing in the token stream tells them
  // apart, which is exactly why the second pass exists.
  EXPECT_NE(std::find(plain.begin(), plain.end(), "identifier:question"),
            plain.end());
  EXPECT_NE(std::find(plain.begin(), plain.end(), "identifier:pages"),
            plain.end());

  // Resolved, the ports say so -- the declaration and every mention -- and the
  // node stays an ordinary name.
  EXPECT_EQ(std::count(resolved.begin(), resolved.end(),
                       "port-name:question"),
            2);
  EXPECT_EQ(std::count(resolved.begin(), resolved.end(), "port-name:answer"),
            2);
  EXPECT_EQ(std::count(resolved.begin(), resolved.end(), "identifier:pages"),
            3);
  EXPECT_EQ(std::count(resolved.begin(), resolved.end(), "port-name:pages"), 0);
}

TEST(FlowHighlight, OnlyAPlainIdentifierBecomesAPort) {
  // A member after a `.`, and a string that happens to spell a port's name, are
  // already what they are.
  const std::vector<std::string> resolved = Coloured(
      "flow f {\n"
      "  in  url: string required\n"
      "  out b:   string\n"
      "  hit = run x(url: url)\n"
      "  hit.url -> b\n"
      "  \"url\" -> b\n"
      "}",
      true);
  EXPECT_NE(std::find(resolved.begin(), resolved.end(), "member:url"),
            resolved.end());
  EXPECT_NE(std::find(resolved.begin(), resolved.end(), "string:\"url\""),
            resolved.end());
  // The declaration, the argument's value, and nothing else.
  EXPECT_EQ(std::count(resolved.begin(), resolved.end(), "port-name:url"), 3);
}

TEST(FlowHighlight, AShapesFieldsAreNotTheFlowsPorts) {
  // A `struct` written after a flow is not inside it, so a field that happens to
  // share a port's name is a field.
  const std::vector<std::string> resolved = Coloured(
      "flow f {\n  in url: string required\n  out b: string\n"
      "  url -> b\n}\n"
      "struct D {\n  url: string required\n}",
      true);
  EXPECT_EQ(std::count(resolved.begin(), resolved.end(), "port-name:url"), 2);
  EXPECT_EQ(std::count(resolved.begin(), resolved.end(), "identifier:url"), 1);
}

}  // namespace
}  // namespace a11::flow
