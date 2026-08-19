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

#include <absl/strings/str_cat.h>
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

TEST(FlowComplete, OutsideAFlowThereAreTwoThingsToWrite) {
  const std::vector<std::string> declarations = {"flow", "struct"};
  EXPECT_EQ(Names("|CARET|"), declarations);
  EXPECT_EQ(Names("flow a { }\n|CARET|"), declarations);
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

TEST(FlowComplete, WhatAProposalReplacesIsThePartialWordAndNothingElse) {
  // `[prefix_start, caret)` is what a frontend replaces with what it inserts, so
  // with nothing typed yet it has to be *empty*. It defaulted to zero, which made
  // that range the whole document up to the caret: taking any proposal at a
  // position where no word had been started -- after a `(`, after a `|`, after a
  // space, which is most of them -- deleted everything in front of it.
  const auto replaced = [](std::string_view marked) {
    const size_t caret = marked.find(kCaret);
    const CompleteResult result = At(marked);
    EXPECT_LE(result.prefix_start, caret)
        << "a proposal would replace text after the caret";
    return caret - result.prefix_start;
  };

  // Nothing typed: an empty range, wherever the caret is.
  EXPECT_EQ(replaced(InOuter("  x = run inner(|CARET|)")), 0u);
  EXPECT_EQ(replaced(InOuter("  q |CARET|")), 0u);
  EXPECT_EQ(replaced(InOuter("  |CARET|")), 0u);
  EXPECT_EQ(replaced("|CARET|"), 0u);

  // A partial word: exactly that word, and its text says which.
  const CompleteResult typing = At(InOuter("  x = run inner(que|CARET|)"));
  EXPECT_EQ(typing.prefix, "que");
  EXPECT_EQ(replaced(InOuter("  x = run inner(que|CARET|)")), 3u);
  const CompleteResult stage = At(InOuter("  q | fir|CARET|"));
  EXPECT_EQ(stage.prefix, "fir");
  EXPECT_EQ(replaced(InOuter("  q | fir|CARET|")), 3u);
}

TEST(FlowComplete, AnArgumentIsOfferedWithWhatThePortIsFor) {
  // The question somebody has inside a call's parentheses is "what goes here",
  // and the answer is the port's description. It used to be dropped for exactly
  // the ports that have to be written: `(required)` *replaced* the description
  // rather than joining it, so the list said least about the arguments it was
  // most important about.
  constexpr std::string_view kDescribed =
      "flow inner {\n"
      "  in  topic:    string required \"What to research.\"\n"
      "  in  depth:    integer \"How many passes; three when nothing says.\"\n"
      "  in  findings: string stream required \"One report per investigation.\"\n"
      "  out report:   string\n"
      "  topic -> report\n"
      "}\n"
      "\n"
      "flow outer {\n"
      "  in  q: string\n"
      "  out r: string\n"
      "  x = run inner(|CARET|)\n";

  // Required first, since those are the ones that have to be written.
  EXPECT_EQ(Names(kDescribed),
            (std::vector<std::string>{"topic", "depth", "findings"}));

  const Proposal* topic = Find(kDescribed, "topic");
  ASSERT_NE(topic, nullptr);
  EXPECT_EQ(topic->insert, "topic: ");
  // Both, and the description last: what it is for is the question, and whether
  // it is required is the aside.
  EXPECT_EQ(topic->tail, " (required) — What to research.");
  EXPECT_EQ(topic->type, "string");
  // The whole of it in the popup, since a list line holds one sentence.
  EXPECT_NE(topic->documentation.find("What to research."), std::string::npos);
  EXPECT_NE(topic->documentation.find("required"), std::string::npos);

  // An optional port has no `(required)` to join, and still says what it is for.
  const Proposal* depth = Find(kDescribed, "depth");
  ASSERT_NE(depth, nullptr);
  EXPECT_EQ(depth->tail, " How many passes; three when nothing says.");
  EXPECT_NE(depth->documentation.find("optional"), std::string::npos);

  // A stream is written differently from a single value, so the grey type says
  // which -- and the popup spells it out.
  const Proposal* findings = Find(kDescribed, "findings");
  ASSERT_NE(findings, nullptr);
  EXPECT_EQ(findings->type, "string stream");
  EXPECT_NE(findings->documentation.find("a stream"), std::string::npos);

  // A port already written on the line is not offered again.
  constexpr std::string_view kPartly =
      "flow inner {\n"
      "  in  topic: string required \"What to research.\"\n"
      "  in  depth: integer \"How deep.\"\n"
      "  out report: string\n"
      "  topic -> report\n"
      "}\n"
      "\n"
      "flow outer {\n"
      "  in  q: string\n"
      "  out r: string\n"
      "  x = run inner(topic: q, |CARET|)\n";
  EXPECT_EQ(Names(kPartly), std::vector<std::string>{"depth"});
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
  // So is `one of`, among a field's.
  EXPECT_EQ(vocabulary::OrderedFieldModifiers().size() + 1,
            vocabulary::FieldModifierWords().size());
}

TEST(FlowComplete, AnActionIsOfferedUnderItsOwnNameWhereACallMayBegin) {
  // Somebody who knows the action types its name, not the verb in front of it.
  // Offering action names only *after* `call` meant the list was empty for the
  // whole of `interact_with_llm` until `call ` had been typed, which reads as
  // the editor not knowing the action at all.
  const Proposal* head = Find("flow f {\n  in q: string\n  |CARET|\n}\n",
                              "make_http_request");
  ASSERT_NE(head, nullptr);
  // Taking it writes the statement, not a bare name that could not compile.
  EXPECT_EQ(head->insert, "call make_http_request()");
  EXPECT_EQ(head->caret, static_cast<int>(head->insert.size()) - 1);

  const Proposal* bound =
      Find("flow f {\n  in q: string\n  page = |CARET|\n}\n",
           "make_http_request");
  ASSERT_NE(bound, nullptr);
  EXPECT_EQ(bound->insert, "call make_http_request()");

  // After the verb it is the name alone, since the verb is already there.
  const Proposal* after =
      Find("flow f {\n  in q: string\n  page = call |CARET|\n}\n",
           "make_http_request");
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(after->insert, "make_http_request()");
}

TEST(FlowComplete, AProposalCarriesWhatAPopupWouldShow) {
  // The description beside the list is one line; deciding between two actions
  // needs their ports, and that is the same text a hover gives.
  const Proposal* action =
      Find("flow f {\n  in q: string\n  page = call |CARET|\n}\n",
           "make_http_request");
  ASSERT_NE(action, nullptr);
  EXPECT_NE(action->documentation.find("**Inputs**"), std::string::npos)
      << action->documentation;

  // A stage carries the language's own reference for it, which is the same text
  // hovering the finished word gives: choosing between `collect` and `join` is
  // exactly the moment somebody needs to know what each one does to the stream.
  const Proposal* stage = Find("flow f {\n  in q: string\n  q | |CARET|\n}\n",
                               "collect");
  ASSERT_NE(stage, nullptr);
  EXPECT_NE(stage->documentation.find("a pipeline stage"), std::string::npos)
      << stage->documentation;
  EXPECT_NE(stage->documentation.find("exactly one value"), std::string::npos)
      << stage->documentation;

  // And so does a function, told apart from the stage of the same name.
  const Proposal* function =
      Find("flow f {\n  in q: string\n  n = |CARET|\n}\n", "join");
  ASSERT_NE(function, nullptr);
  EXPECT_NE(function->documentation.find("a built-in function"),
            std::string::npos)
      << function->documentation;
}

TEST(FlowComplete, OffersTheFieldsTheFileSaidAValueHas) {
  // The `default:` case of the member rules used to offer nothing, and said why:
  // nothing knew a value's fields. Two things do -- a pattern names them, and a
  // port declared with a `struct` has them -- and this is where that shows.
  constexpr std::string_view kHead =
      "struct Source {\n  url:  string required\n  rank: number\n}\n\n"
      "flow f {\n  in  lines: string stream required\n"
      "  in  src:   Source required\n  out o:     string stream\n";

  // A value a literal pattern made: the fields are in the text that made it.
  EXPECT_EQ(Names(absl::StrCat(
                kHead,
                "  let who = match(\"name={name} age={age:int}\", lines)\n"
                "  strformat(\"%s\", who.|CARET|\n}\n")),
            (std::vector<std::string>{"name", "age"}));

  // `it` inside a stage that follows one: read off the tokens, because the line
  // being typed is exactly when this is wanted.
  EXPECT_EQ(Names(absl::StrCat(
                kHead,
                "  lines | match \"{level:word}: {rest:rest}\" | map it.|CARET|\n}\n")),
            (std::vector<std::string>{"level", "rest"}));

  // A port declared with a struct, which was missing too.
  EXPECT_EQ(Names(absl::StrCat(kHead, "  strformat(\"%s\", src.|CARET|\n}\n")),
            (std::vector<std::string>{"url", "rank"}));

  // And nothing where nothing is known: no pattern, and a positional one names
  // no fields at all.
  EXPECT_TRUE(Names(absl::StrCat(kHead, "  lines | map it.|CARET|\n}\n")).empty());
  EXPECT_TRUE(
      Names(absl::StrCat(kHead, "  lines | match \"{}:{}\" | map it.|CARET|\n}\n"))
          .empty());
}

}  // namespace
}  // namespace a11::flow