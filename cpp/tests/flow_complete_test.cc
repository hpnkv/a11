// Copyright 2026 The A11 Authors.

// What is offered where.
//
// These are the cases the IntelliJ plugin's Kotlin completion was held to, moved
// here with it: deciding which of a language's words are legal at one offset is a
// question about text, and it has one answer wherever it is asked. The caret is
// written `|CARET|` in the sources, which is only a marker -- what is completed is
// the text without it, at the offset it stood at.

#include "a11/flow/complete.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "a11/flow/vocabulary.h"

namespace a11::flow {
namespace {

constexpr std::string_view kCaret = "|CARET|";

CompleteResult At(std::string_view marked) {
  const size_t caret = marked.find(kCaret);
  EXPECT_NE(caret, std::string_view::npos) << "no |CARET| in the source";
  std::string source(marked.substr(0, caret));
  source.append(marked.substr(caret + kCaret.size()));
  return CompleteAt(source, caret);
}

std::vector<std::string> Names(std::string_view marked) {
  std::vector<std::string> names;
  for (const Proposal& proposal : At(marked).proposals) {
    names.push_back(proposal.name);
  }
  return names;
}

bool Offers(const std::vector<std::string>& names, std::string_view wanted) {
  for (const std::string& name : names) {
    if (name == wanted) return true;
  }
  return false;
}

const Proposal* Find(std::string_view marked, std::string_view name) {
  static CompleteResult held;
  held = At(marked);
  for (const Proposal& proposal : held.proposals) {
    if (proposal.name == name) return &proposal;
  }
  return nullptr;
}

TEST(FlowComplete, OutsideAFlowThereIsOneThingToWrite) {
  EXPECT_EQ(Names("|CARET|"), std::vector<std::string>{"flow"});
  EXPECT_EQ(Names("flow a { }\n|CARET|"), std::vector<std::string>{"flow"});
}

TEST(FlowComplete, AfterAPipeOnlyAStageCanFollow) {
  const std::vector<std::string> offered =
      Names("flow t {\n  in q: string\n  q | |CARET|");
  EXPECT_TRUE(Offers(offered, "truncate"));
  EXPECT_TRUE(Offers(offered, "collect"));
  EXPECT_TRUE(Offers(offered, "where"));
  // Nothing that is not a stage: no names, no functions.
  EXPECT_FALSE(Offers(offered, "q"));
  EXPECT_FALSE(Offers(offered, "len"));
  const Proposal* truncate =
      Find("flow t {\n  in q: string\n  q | |CARET|", "truncate");
  ASSERT_NE(truncate, nullptr);
  EXPECT_EQ(truncate->kind, ProposalKind::kStage);
  EXPECT_EQ(truncate->tail, " n");
}

TEST(FlowComplete, APortsTypeComesAfterItsColon) {
  EXPECT_TRUE(Offers(Names("flow t {\n  in q: |CARET|"), "string"));
  EXPECT_EQ(Names("flow t {\n  in q: string |CARET|"),
            (std::vector<std::string>{"stream", "required"}));
  EXPECT_EQ(Names("flow t {\n  in q: string stream |CARET|"),
            std::vector<std::string>{"required"});
}

// The two flows every call case needs: one to call, and one calling it.
constexpr std::string_view kSiblings =
    "flow inner {\n"
    "  in  question: string required\n"
    "  in  limit:    number\n"
    "  out reply:    string\n"
    "  question -> reply\n"
    "}\n"
    "\n"
    "flow outer {\n"
    "  in  q: string\n"
    "  out a: string\n";

std::string InOuter(std::string_view tail) {
  return std::string(kSiblings).append(tail);
}

TEST(FlowComplete, ACallNamesASiblingFlowThenItsPorts) {
  EXPECT_TRUE(Offers(Names(InOuter("  x = run |CARET|")), "inner"));

  const std::string arguments = InOuter("  x = run inner(|CARET|)");
  EXPECT_EQ(Names(arguments), (std::vector<std::string>{"question", "limit"}));
  const Proposal* question = Find(arguments, "question");
  ASSERT_NE(question, nullptr);
  // Taking it writes the colon that has to follow it.
  EXPECT_EQ(question->insert, "question: ");
  EXPECT_EQ(question->tail, " (required)");
  EXPECT_EQ(question->type, "string");
}

TEST(FlowComplete, ACallsPortsAndItsStatusFollowItsDot) {
  const std::string source =
      InOuter("  x = run inner(question: q)\n  x.|CARET|");
  EXPECT_EQ(Names(source),
            (std::vector<std::string>{"reply", "question", "limit",
                                      "status"}));
}

TEST(FlowComplete, ABarriersDotOffersTheFieldsOfAStatus) {
  EXPECT_EQ(Names("flow t {\n  in q: string\n  x = run a(q: q)\n"
                  "  held = wait x\n  held.|CARET|"),
            (std::vector<std::string>{"ok", "code", "number", "message"}));
}

TEST(FlowComplete, ANodesDotOffersItsIdAndNothingElse) {
  EXPECT_EQ(Names("flow t {\n  in q: string\n  n = node()\n  n.|CARET|"),
            std::vector<std::string>{"id"});
}

TEST(FlowComplete, AnArrowIsFollowedBySomewhereWritable) {
  const std::vector<std::string> offered = Names(InOuter(
      "  n = node()\n  x = run inner(question: q)\n  q -> |CARET|"));
  EXPECT_TRUE(Offers(offered, "a"));
  EXPECT_TRUE(Offers(offered, "n"));
  EXPECT_TRUE(Offers(offered, "x.question"));
  // Not the `in` port, and not the call itself: neither can be written.
  EXPECT_FALSE(Offers(offered, "q"));
  EXPECT_FALSE(Offers(offered, "x"));
}

TEST(FlowComplete, FailIsFollowedByTheStatusCodes) {
  const std::vector<std::string> offered =
      Names("flow t {\n  in q: string\n  fail |CARET|");
  EXPECT_TRUE(Offers(offered, "not_found"));
  EXPECT_TRUE(Offers(offered, "unavailable"));
}

TEST(FlowComplete, AModifierFollowsACallsClosingParenthesis) {
  EXPECT_EQ(Names("flow t {\n  in q: string\n  x = run a(q: q) |CARET|"),
            (std::vector<std::string>{"tee", "via", "timeout", "after", "with",
                                      "id", "forward headers"}));
}

TEST(FlowComplete, ViaIsFollowedByTheNodeMapsDeclared) {
  EXPECT_EQ(Names("flow t {\n  in q: string\n  nodes scratch\n"
                  "  x = run a(q: q) via |CARET|"),
            std::vector<std::string>{"scratch"});
}

TEST(FlowComplete, WaitIsFollowedByWhatHasAStatus) {
  const std::vector<std::string> offered =
      Names("flow t {\n  in q: string\n  out a: string\n"
            "  n = node()\n  x = run act(q: q)\n  wait |CARET|");
  EXPECT_TRUE(Offers(offered, "x"));
  EXPECT_TRUE(Offers(offered, "n"));
  EXPECT_TRUE(Offers(offered, "q"));
  EXPECT_TRUE(Offers(offered, "a"));
}

TEST(FlowComplete, AStatementStartOffersStatementsAndNames) {
  const std::vector<std::string> offered =
      Names("flow t {\n  in q: string\n  out a: string\n"
            "  x = run act(q: q)\n  |CARET|");
  for (const std::string_view statement : {"run", "call", "wait", "if", "for"}) {
    EXPECT_TRUE(Offers(offered, statement)) << statement;
  }
  for (const std::string_view name : {"q", "a", "x"}) {
    EXPECT_TRUE(Offers(offered, name)) << name;
  }
  // `until` belongs to a repeat, and there is none here.
  EXPECT_FALSE(Offers(offered, "until"));
}

TEST(FlowComplete, UntilAndACarryAreOfferedInsideARepeat) {
  const std::vector<std::string> offered =
      Names("flow t {\n  in q: string\n  repeat state = 0 max 3 {\n    |CARET|");
  EXPECT_TRUE(Offers(offered, "until"));
  EXPECT_TRUE(Offers(offered, "while"));
  EXPECT_TRUE(Offers(offered, "state"));
  EXPECT_TRUE(Offers(offered, "index"));
}

TEST(FlowComplete, ElseIsOfferedWhereAnIfHasJustClosed) {
  EXPECT_TRUE(Offers(Names("flow t {\n  in q: string\n  out a: string\n"
                           "  if len(q) > 0 {\n    q -> a\n  }\n  |CARET|"),
                     "else"));
}

TEST(FlowComplete, ItIsOfferedInsideTheStagesThatLookAtAValue) {
  EXPECT_TRUE(Offers(Names("flow t {\n  in q: string\n  q | where |CARET|"),
                     "it"));
  // And nowhere else: there is no value being looked at in a condition.
  EXPECT_FALSE(Offers(Names("flow t {\n  in q: string\n  if |CARET|"), "it"));
}

TEST(FlowComplete, AFunctionTakesItsParenthesesWithIt) {
  const Proposal* len = Find("flow t {\n  in q: string\n  if |CARET|", "len");
  ASSERT_NE(len, nullptr);
  EXPECT_EQ(len->insert, "len()");
  EXPECT_EQ(len->caret, 4);
}

TEST(FlowComplete, ANameIsNotOfferedAboveTheStatementThatBindsIt) {
  EXPECT_FALSE(Offers(Names("flow t {\n  in q: string\n  out a: string\n"
                            "  |CARET|\n  x = run act(q: q)"),
                      "x"));
}

TEST(FlowComplete, AFragmentOffersWhatItBindsItself) {
  // A string marked `language=A11Flow`: no declarations, but the names it does
  // bind are still names.
  const std::vector<std::string> offered = Names("x = run act(q: q)\n  |CARET|");
  EXPECT_TRUE(Offers(offered, "x"));
  EXPECT_TRUE(Offers(offered, "wait"));
}

TEST(FlowComplete, NothingIsOfferedInsideAStringOrAComment) {
  EXPECT_TRUE(Names("flow t {\n  in q: string\n  # a note |CARET|").empty());
  EXPECT_TRUE(Names("flow t {\n  describe \"half a |CARET|").empty());
}

TEST(FlowComplete, ThePartialWordIsReported) {
  const CompleteResult result =
      At("flow t {\n  in q: string\n  q | trun|CARET|");
  EXPECT_EQ(result.prefix, "trun");
  // Unfiltered: every frontend filters by its own rules, and filtering twice
  // would drop what a fuzzy matcher would have kept.
  EXPECT_TRUE(Offers(Names("flow t {\n  in q: string\n  q | trun|CARET|"),
                     "collect"));
  EXPECT_EQ(result.prefix_start, result.prefix_start);
}

TEST(FlowComplete, EveryProposalKindHasAName) {
  // The names travel in `flow.completions/v1`, so a kind without one would be a
  // hole in the format rather than a missing string.
  for (int kind = 0; kind <= static_cast<int>(ProposalKind::kField); ++kind) {
    EXPECT_FALSE(ProposalKindName(static_cast<ProposalKind>(kind)).empty());
  }
}

TEST(FlowComplete, TheOrderedTablesCoverTheirSets) {
  // The ordered lists are what a caret offers and the sets are what the grammar
  // consults; one table behind both is what keeps a word added to the language
  // from being invisible in an editor.
  EXPECT_EQ(vocabulary::OrderedStatements().size(),
            vocabulary::StatementWords().size());
  EXPECT_EQ(vocabulary::OrderedDeclarations().size(),
            vocabulary::DeclarationWords().size());
  EXPECT_EQ(vocabulary::OrderedTypeNames().size(),
            vocabulary::TypeNames().size());
  EXPECT_EQ(vocabulary::OrderedBuiltins().size(),
            vocabulary::Builtins().size());
  EXPECT_EQ(vocabulary::OrderedStatusFields().size(),
            vocabulary::StatusFields().size());
  EXPECT_EQ(vocabulary::OrderedPortModifiers().size(),
            vocabulary::PortModifierWords().size());
  // `forward headers` is one modifier and two words.
  EXPECT_EQ(vocabulary::OrderedModifiers().size() + 1,
            vocabulary::ModifierWords().size());
}

}  // namespace
}  // namespace a11::flow

