// Copyright 2026 The A11 Authors.

#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <gtest/gtest.h>

#include "a11/flow/lexer.h"
#include "a11/flow/token.h"
#include "a11/flow/vocabulary.h"

namespace a11::flow {
namespace {

/// Every token as `kind:text`, which is how the Python lexer's output reads
/// too.
std::vector<std::string> Dump(std::string_view source,
                              LexOptions options = {}) {
  std::vector<std::string> out;
  for (const Token& token : Lex(source, options).tokens) {
    if (token.kind == TokenKind::kEnd) {
      break;
    }
    out.push_back(absl::StrCat(KindName(token.kind), ":", token.text));
  }
  return out;
}

std::vector<std::string> Codes(std::string_view source) {
  std::vector<std::string> out;
  for (const Diagnostic& diagnostic : Lex(source).diagnostics) {
    out.push_back(diagnostic.code);
  }
  return out;
}

TEST(FlowLexer, EveryTokenPointsAtWhatItRead) {
  const std::string source =
      "# a flow\n"
      "flow research {\n"
      "  in  question: string required\n"
      "  x = try call web-fetch(url: question) timeout 30s via scratch\n"
      "  x.text | truncate 200 | where it != \"\" -> answer\n"
      "}\n";
  const LexResult result = Lex(source);
  EXPECT_TRUE(result.diagnostics.empty());
  for (const Token& token : result.tokens) {
    if (token.kind == TokenKind::kEnd) {
      continue;
    }
    // A token's text is the source it covers, and its offsets say where.
    EXPECT_EQ(token.text, std::string_view(source).substr(
                              token.start, token.end - token.start));
    EXPECT_LT(token.start, token.end);
  }
}

TEST(FlowLexer, ANewlineEndsAStatementButABlankLineEndsNothing) {
  EXPECT_EQ(Dump("a\nb"),
            (std::vector<std::string>{"word:a", "newline:\n", "word:b"}));
  // A run of breaks is one, and a leading one is none: the parser would
  // otherwise see statements that are not there.
  EXPECT_EQ(Dump("\n\n\na\n\n\nb\n\n\n"),
            (std::vector<std::string>{"word:a", "newline:\n", "word:b"}));
  EXPECT_EQ(Dump(""), std::vector<std::string>{});
}

TEST(FlowLexer, TheEndTokenIsAlwaysThere) {
  const LexResult result = Lex("flow t {}");
  ASSERT_FALSE(result.tokens.empty());
  EXPECT_EQ(result.tokens.back().kind, TokenKind::kEnd);
  EXPECT_EQ(result.tokens.back().start, 9u);
}

TEST(FlowLexer, ADashJoinsANameButAnArrowDoesNot) {
  EXPECT_EQ(Dump("for-each"), std::vector<std::string>{"word:for-each"});
  EXPECT_EQ(Dump("a -> b"),
            (std::vector<std::string>{"word:a", "->:->", "word:b"}));
  EXPECT_EQ(Dump("state <- step"),
            (std::vector<std::string>{"word:state", "<-:<-", "word:step"}));
  // A `-` in front of a digit is the number's.
  EXPECT_EQ(Dump("-3"), std::vector<std::string>{"number:-3"});
  // And one that is not is arithmetic.
  EXPECT_EQ(Dump("a - b"),
            (std::vector<std::string>{"word:a", "-:-", "word:b"}));
}

TEST(FlowLexer, ReadsNumbersAndDurations) {
  const LexResult result = Lex("42 1.5 250ms 1.5h");
  ASSERT_EQ(result.tokens.size(), 5u);
  EXPECT_EQ(result.tokens[0].kind, TokenKind::kNumber);
  EXPECT_EQ(result.tokens[0].number, 42.0);
  EXPECT_TRUE(result.tokens[0].is_integer);
  EXPECT_EQ(result.tokens[1].kind, TokenKind::kNumber);
  EXPECT_EQ(result.tokens[1].number, 1.5);
  EXPECT_FALSE(result.tokens[1].is_integer);
  EXPECT_EQ(result.tokens[2].kind, TokenKind::kDuration);
  EXPECT_EQ(result.tokens[2].duration, absl::Milliseconds(250));
  EXPECT_EQ(result.tokens[3].kind, TokenKind::kDuration);
  EXPECT_EQ(result.tokens[3].duration, absl::Minutes(90));
}

TEST(FlowLexer, EveryDurationUnitIsRead) {
  for (const std::string_view unit : vocabulary::DurationUnits()) {
    const LexResult result = Lex(absl::StrCat("1", unit));
    ASSERT_EQ(result.tokens.size(), 2u) << unit;
    EXPECT_EQ(result.tokens[0].kind, TokenKind::kDuration) << unit;
    EXPECT_TRUE(result.diagnostics.empty()) << unit;
  }
}

TEST(FlowLexer, AnUnknownUnitIsOneBadTokenAndOneDiagnostic) {
  const LexResult result = Lex("5x");
  ASSERT_EQ(result.tokens.size(), 2u);
  EXPECT_EQ(result.tokens[0].kind, TokenKind::kBad);
  EXPECT_EQ(result.tokens[0].text, "5x");
  ASSERT_EQ(result.diagnostics.size(), 1u);
  EXPECT_EQ(result.diagnostics[0].code, "flow.form.duration-unit");
  // It says what the units are, because the answer is a short list.
  EXPECT_NE(result.diagnostics[0].message.find("ms"), std::string::npos);
}

TEST(FlowLexer, ReadsStringsWithTheirEscapes) {
  const LexResult result = Lex(R"("plain" "with \" inside" "a\nb")");
  ASSERT_EQ(result.tokens.size(), 4u);
  EXPECT_EQ(result.tokens[0].string_value, "plain");
  EXPECT_EQ(result.tokens[1].string_value, "with \" inside");
  EXPECT_EQ(result.tokens[2].string_value, "a\nb");
  // The text is what was written; the value is what it means.
  EXPECT_EQ(result.tokens[2].text, R"("a\nb")");
  EXPECT_TRUE(result.diagnostics.empty());
}

TEST(FlowLexer, AnUnterminatedStringEndsAtItsLine) {
  // Recovery that matters: reading on to the next quote would swallow whole
  // statements, and an editor would light up half the file.
  const LexResult result = Lex("\"unterminated\nnext");
  EXPECT_EQ(Codes("\"unterminated\nnext"),
            std::vector<std::string>{"flow.syntax.unterminated-string"});
  ASSERT_GE(result.tokens.size(), 3u);
  EXPECT_EQ(result.tokens[0].kind, TokenKind::kString);
  EXPECT_EQ(result.tokens[0].text, "\"unterminated");
  EXPECT_EQ(result.tokens[1].kind, TokenKind::kNewline);
  EXPECT_EQ(result.tokens[2].text, "next");
}

TEST(FlowLexer, AnUnknownCharacterIsOneTokenAndTheRestIsStillRead) {
  const LexResult result = Lex("a ; b");
  EXPECT_EQ(Codes("a ; b"),
            std::vector<std::string>{"flow.syntax.unexpected-character"});
  ASSERT_EQ(result.tokens.size(), 4u);
  EXPECT_EQ(result.tokens[1].kind, TokenKind::kBad);
  EXPECT_EQ(result.tokens[2].text, "b");
}

TEST(FlowLexer, CommentsAreTokensOrNotAsAsked) {
  EXPECT_EQ(Dump("# what this is\nlater"),
            (std::vector<std::string>{"comment:# what this is", "newline:\n",
                                      "word:later"}));
  // A parser has no use for them, and dropping them drops the break behind them
  // too: a line with only a comment on it ended nothing, which is exactly what
  // `a11.flow.lexer` does with the same input.
  EXPECT_EQ(Dump("# what this is\nlater", LexOptions{.keep_comments = false}),
            std::vector<std::string>{"word:later"});
}

TEST(FlowLexer, ACommentAtTheEndOfALineDoesNotEatTheBreak) {
  EXPECT_EQ(Dump("a # note\nb"),
            (std::vector<std::string>{"word:a", "comment:# note", "newline:\n",
                                      "word:b"}));
}

TEST(FlowLexer, LinesAndColumnsAreOneBased) {
  const LexResult result = Lex("flow t {\n  in q: string\n}");
  ASSERT_GE(result.tokens.size(), 6u);
  EXPECT_EQ(result.tokens[0].line, 1);
  EXPECT_EQ(result.tokens[0].column, 1);
  EXPECT_EQ(result.tokens[1].column, 6);
  // The `in` on the second line, past two spaces.
  const Token& in_word = result.tokens[4];
  EXPECT_EQ(in_word.text, "in");
  EXPECT_EQ(in_word.line, 2);
  EXPECT_EQ(in_word.column, 3);
}

TEST(FlowLexer, ReadsEveryPunctuationTheGrammarUses) {
  EXPECT_EQ(Dump("{}()[]:,|=.<><=>===!=+-|->"),
            (std::vector<std::string>{
                // A colon's kind is spelled ":" and so is its text, which is
                // why this one reads as three of them.
                "{:{",   "}:}",   "(:(",   "):)", "[:[", "]:]", ":::",
                ",:,",   "|:|",   "=:=",   ".:.", "<:<", ">:>", "<=:<=",
                ">=:>=", "==:==", "!=:!=", "+:+", "-:-", "|:|", "->:->"}));
}

TEST(FlowLexer, ANameMayHoldWhatANameHolds) {
  EXPECT_EQ(Dump("$id _private a1 x-y-z"),
            (std::vector<std::string>{"word:$id", "word:_private", "word:a1",
                                      "word:x-y-z"}));
  // Python's `isalpha` accepts any Unicode letter, so a name in another script
  // lexes as one word here too.
  EXPECT_EQ(Dump("Ünïcødé"), std::vector<std::string>{"word:Ünïcødé"});
}

TEST(FlowLexer, KindNamesRoundTrip) {
  for (const TokenKind kind :
       {TokenKind::kNewline,      TokenKind::kComment,
        TokenKind::kString,       TokenKind::kNumber,
        TokenKind::kDuration,     TokenKind::kWord,
        TokenKind::kDot,          TokenKind::kArrow,
        TokenKind::kCarry,        TokenKind::kEqual,
        TokenKind::kEqualEqual,   TokenKind::kBangEqual,
        TokenKind::kLess,         TokenKind::kLessEqual,
        TokenKind::kGreater,      TokenKind::kGreaterEqual,
        TokenKind::kPlus,         TokenKind::kMinus,
        TokenKind::kPipe,         TokenKind::kColon,
        TokenKind::kComma,        TokenKind::kLeftBrace,
        TokenKind::kRightBrace,   TokenKind::kLeftParen,
        TokenKind::kRightParen,   TokenKind::kLeftBracket,
        TokenKind::kRightBracket, TokenKind::kEnd}) {
    EXPECT_EQ(KindFromName(KindName(kind)), kind) << KindName(kind);
  }
}

// --- the vocabulary
// -----------------------------------------------------------

TEST(FlowVocabulary, CanonicalFollowsTheCompilersCasingRule) {
  EXPECT_EQ(vocabulary::Canonical("for"), "for");
  EXPECT_EQ(vocabulary::Canonical("FOR"), "for");
  // Mixed case is a name, which keeps the rule easy to state and to see.
  EXPECT_EQ(vocabulary::Canonical("For"), "For");
  EXPECT_EQ(vocabulary::Canonical("STARTS-WITH"), "starts-with");
  EXPECT_EQ(vocabulary::Canonical("_"), "_");
  EXPECT_EQ(vocabulary::Canonical(""), "");
}

TEST(FlowVocabulary, EveryStageAndFunctionIsDocumented) {
  // The reference an editor shows is part of the language, so a stage or a
  // function added to the tables without one is a hole here rather than a hover
  // that says nothing. What the words *mean* is checked by reading them; what
  // this checks is that each has been written at all, and written to the shape
  // every consumer renders.
  const auto documented = [](const vocabulary::WordDoc* doc,
                             std::string_view name, bool takes_argument) {
    ASSERT_NE(doc, nullptr) << name << " has no reference text";
    EXPECT_FALSE(doc->summary.empty()) << name;
    EXPECT_FALSE(doc->detail.empty()) << name;
    EXPECT_FALSE(doc->example.empty()) << name;
    // A summary is one sentence, and it is shown as one.
    EXPECT_TRUE(absl::EndsWith(doc->summary, "."))
        << name << ": " << doc->summary;
    // The word itself appears in its own example, or the example is about
    // something else.
    EXPECT_NE(doc->example.find(name), std::string_view::npos)
        << name << ": " << doc->example;
    // `--` is never written in text a reader sees: a colon or an em dash.
    for (const std::string_view text :
         {doc->summary, doc->takes, doc->detail, doc->example}) {
      EXPECT_EQ(text.find("--"), std::string_view::npos)
          << name << ": " << text;
    }
    // Something that takes nothing says nothing about what it takes, so the
    // hover does not print an empty "Takes:" line.
    EXPECT_EQ(doc->takes.empty(), !takes_argument) << name;
  };

  for (const std::string_view stage : vocabulary::Stages()) {
    documented(
        vocabulary::StageDocumentation(stage), stage,
        *vocabulary::StageTakes(stage) != vocabulary::StageArgument::kNone);
  }
  for (const std::string_view name : vocabulary::OrderedBuiltins()) {
    // Every function but `now()` is given something; `now()` is the clock.
    documented(vocabulary::BuiltinDocumentation(name), name, name != "now");
  }

  // A word that is both is documented as both, and they do not say the same
  // thing: `| text` re-writes a stream and `text(x)` re-writes one value.
  const vocabulary::WordDoc* staged = vocabulary::StageDocumentation("text");
  const vocabulary::WordDoc* called = vocabulary::BuiltinDocumentation("text");
  ASSERT_NE(staged, nullptr);
  ASSERT_NE(called, nullptr);
  EXPECT_NE(staged->summary, called->summary);

  // And a word that is neither has nothing to say here.
  EXPECT_EQ(vocabulary::StageDocumentation("flow"), nullptr);
  EXPECT_EQ(vocabulary::BuiltinDocumentation("truncate"), nullptr);
}

TEST(FlowVocabulary, EveryWordOfTheLanguageIsDocumented) {
  // The same contract as the stages and the functions, over every other word
  // set: a statement, a declaration, a modifier, a type, a status code, a
  // constant, a duration unit and a mark of punctuation each have reference
  // text. This is what stops a form reaching a reader as its token's kind --
  // "`|` — flow operator" was the whole of what a hover said about a pipe --
  // and it is why adding a word to the grammar without writing what it does
  // fails here rather than in an editor.
  for (const vocabulary::WordRole role : vocabulary::WordRoles()) {
    const std::string_view role_name = vocabulary::WordRoleName(role);
    for (const std::string_view word : vocabulary::WordsOf(role)) {
      // The role's own table, or whichever documents the word: a set may list a
      // word a neighbouring set documents, which is deliberate. `stream` is in
      // the declarations because a port declaration is where it is offered, and
      // is documented as the port modifier it is.
      const vocabulary::WordDoc* doc = vocabulary::Documentation(role, word);
      if (doc == nullptr) {
        doc = vocabulary::AnyDocumentation(word);
      }
      const std::string where = absl::StrCat(role_name, " '", word, "'");
      ASSERT_NE(doc, nullptr) << where << " has no reference text";
      EXPECT_FALSE(doc->summary.empty()) << where;
      EXPECT_FALSE(doc->detail.empty()) << where;
      EXPECT_FALSE(doc->example.empty()) << where;
      // A summary is one sentence, and it is shown as one.
      EXPECT_TRUE(absl::EndsWith(doc->summary, "."))
          << where << ": " << doc->summary;
      // `--` is never written in text a reader sees: a colon or an em dash.
      for (const std::string_view text :
           {doc->summary, doc->takes, doc->detail, doc->example}) {
        EXPECT_EQ(text.find("--"), std::string_view::npos)
            << where << ": " << text;
      }
      // The word appears in its own example, so the example is about the word
      // rather than about something near it. Case-insensitively, because a
      // status code is written `not_found` and shown `"NOT_FOUND"`.
      EXPECT_NE(
          absl::AsciiStrToLower(doc->example).find(absl::AsciiStrToLower(word)),
          std::string::npos)
          << where << ": " << doc->example;
    }
  }

  // The form the request for all this named. Not "flow operator", which is the
  // token's kind and was the whole answer before.
  const vocabulary::WordDoc* pipe =
      vocabulary::Documentation(vocabulary::WordRole::kSymbol, "|");
  ASSERT_NE(pipe, nullptr);
  EXPECT_EQ(pipe->summary, "Puts a stream through a stage.");
  EXPECT_FALSE(pipe->takes.empty());

  // A role answers for its own words and not for another's: `|` is not a
  // statement, and `run` is not a mark.
  EXPECT_EQ(vocabulary::Documentation(vocabulary::WordRole::kStatement, "|"),
            nullptr);
  EXPECT_EQ(vocabulary::Documentation(vocabulary::WordRole::kSymbol, "run"),
            nullptr);
  // And a word the language does not have is documented nowhere.
  EXPECT_EQ(vocabulary::AnyDocumentation("shuffle"), nullptr);
}

TEST(FlowVocabulary, EveryRoleHasItsOwnNameAndItsOwnWords) {
  // The role names travel in `flow.vocabulary/v1`, so two roles sharing one
  // would be two word sets arriving under one key.
  absl::flat_hash_set<std::string_view> names;
  for (const vocabulary::WordRole role : vocabulary::WordRoles()) {
    EXPECT_TRUE(names.insert(vocabulary::WordRoleName(role)).second)
        << vocabulary::WordRoleName(role) << " is used twice";
    EXPECT_FALSE(vocabulary::WordsOf(role).empty())
        << vocabulary::WordRoleName(role) << " lists no words";
  }
  EXPECT_EQ(names.size(), vocabulary::WordRoles().size());
}

TEST(FlowVocabulary, EveryStageSaysWhatItTakes) {
  for (const std::string_view stage : vocabulary::Stages()) {
    EXPECT_TRUE(vocabulary::StageTakes(stage).has_value()) << stage;
  }
  EXPECT_EQ(vocabulary::StageTakes("first"),
            vocabulary::StageArgument::kNumber);
  EXPECT_EQ(vocabulary::StageTakes("where"),
            vocabulary::StageArgument::kExpression);
  EXPECT_EQ(vocabulary::StageTakes("join"),
            vocabulary::StageArgument::kOptionalString);
  EXPECT_EQ(vocabulary::StageTakes("then"), vocabulary::StageArgument::kStream);
  EXPECT_EQ(vocabulary::StageTakes("collect"),
            vocabulary::StageArgument::kNone);
  // A word that reads like a stage and is not one: the language says so rather
  // than guessing. (`flatten` used to stand here, and is now a stage.)
  EXPECT_FALSE(vocabulary::StageTakes("shuffle").has_value());
}

TEST(FlowVocabulary, TheStagesTheSequenceChecksReasonAboutAreStages) {
  for (const std::string_view stage : vocabulary::ReducingStages()) {
    EXPECT_TRUE(vocabulary::StageTakes(stage).has_value()) << stage;
  }
  for (const std::string_view stage : vocabulary::PositionalStages()) {
    EXPECT_TRUE(vocabulary::StageTakes(stage).has_value()) << stage;
  }
  for (const std::string_view stage : vocabulary::BareStages()) {
    EXPECT_TRUE(vocabulary::StageTakes(stage).has_value()) << stage;
  }
}

TEST(FlowVocabulary, AStatusCodeIsAcceptedInEitherCase) {
  EXPECT_TRUE(vocabulary::IsStatusCode("not_found"));
  EXPECT_TRUE(vocabulary::IsStatusCode("NOT_FOUND"));
  // Dashes where the canonical name has underscores, as `plan.status_code`
  // accepts.
  EXPECT_TRUE(vocabulary::IsStatusCode("not-found"));
  EXPECT_FALSE(vocabulary::IsStatusCode("Not_Found"));
  EXPECT_FALSE(vocabulary::IsStatusCode("nope"));
}

TEST(FlowLexer, ReadsRangesAndSpreadsWithoutBreakingNumbers) {
  // `1..200` is a bound and a bound, not a number with two decimal points in
  // it -- so the number scanner has to stop at the first of a `..`.
  EXPECT_EQ(absl::StrJoin(Dump("1..200"), " "), "number:1 ..:.. number:200");
  EXPECT_EQ(absl::StrJoin(Dump("1.5..2"), " "), "number:1.5 ..:.. number:2");
  EXPECT_EQ(absl::StrJoin(Dump("..9"), " "), "..:.. number:9");
  EXPECT_EQ(absl::StrJoin(Dump("1.."), " "), "number:1 ..:..");
  // A `.` that is not the first of a `..` still belongs to the number.
  EXPECT_EQ(absl::StrJoin(Dump("1.5"), " "), "number:1.5");
  EXPECT_TRUE(Codes("1..200").empty());

  // Longest wins: `...` is a spread, `..` a range, `.` a dot.
  EXPECT_EQ(absl::StrJoin(Dump("...x"), " "), "...:... word:x");
  EXPECT_EQ(absl::StrJoin(Dump("a.b"), " "), "word:a .:. word:b");
  // `...` is *not* a second spelling. Two ways of writing one operator is two
  // ways for a file to differ from one that means the same thing, and the one
  // that survives a chat window and a keyboard without the key is three dots.
  // It is still read as the spread it plainly meant, with the repair attached,
  // so the rest of the statement is still checked.
  EXPECT_EQ(absl::StrJoin(Dump("…it"), " "), "...:… word:it");
  EXPECT_EQ(absl::StrJoin(Codes("…it"), " "),
            "flow.syntax.unexpected-character");
  const std::vector<Diagnostic> said = Lex("…it").diagnostics;
  ASSERT_EQ(said.size(), 1u);
  ASSERT_EQ(said[0].fixes.size(), 1u);
  EXPECT_EQ(said[0].fixes[0].edits[0].text, "...");
}

TEST(FlowLexer, AQuoteInsideAStringIsWrittenWithABackslash) {
  const std::vector<Token> tokens = Lex(R"("say \"hi\" now")").tokens;
  ASSERT_FALSE(tokens.empty());
  EXPECT_EQ(tokens[0].kind, TokenKind::kString);
  EXPECT_EQ(tokens[0].string_value, R"(say "hi" now)");
  EXPECT_TRUE(Codes(R"("say \"hi\"")").empty());
  // The escaped quote does not end the string, so what follows it is still
  // inside: a lexer that stopped there would report the rest of the line as
  // statements.
  EXPECT_EQ(absl::StrJoin(Dump(R"("a\"b" -> c)"), " "),
            R"(string:"a\"b" ->:-> word:c)");
}

TEST(FlowVocabulary, OnlyGenericTypesTakeParameters) {
  EXPECT_EQ(vocabulary::TypeParameters("list").size(), 2u);
  EXPECT_EQ(vocabulary::TypeParameters("object").size(), 3u);
  EXPECT_EQ(vocabulary::TypeParameters("string").size(), 1u);
  EXPECT_EQ(vocabulary::TypeParameters("string")[0], 0);
}

}  // namespace
}  // namespace a11::flow
