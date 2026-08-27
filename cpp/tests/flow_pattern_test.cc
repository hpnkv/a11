// Copyright 2026 The A11 Authors.

// The pattern language `match` reads.

#include <string>
#include <string_view>
#include <vector>

#include <absl/strings/str_join.h>
#include <gtest/gtest.h>

#include "a11/flow/pattern.h"

namespace a11::flow::pattern {
namespace {

/// What the pattern pulls out of the subject, as `name=text` in hole order, or
/// `<no match>`. Names make the expectations read like the pattern.
std::string Taken(std::string_view pattern, std::string_view subject) {
  const Compiled compiled = Compile(pattern);
  if (!compiled.ok()) {
    return absl::StrCat("<error: ", compiled.error, ">");
  }
  const auto captures = Match(compiled.pattern, subject);
  if (!captures.has_value()) {
    return "<no match>";
  }
  std::vector<std::string> parts;
  for (size_t index = 0; index < captures->size(); ++index) {
    const Hole& hole = compiled.pattern.holes[index];
    parts.push_back(
        absl::StrCat(hole.name.empty() ? std::to_string(index) : hole.name, "=",
                     (*captures)[index].text));
  }
  return absl::StrJoin(parts, " ");
}

std::string Broken(std::string_view pattern) {
  const Compiled compiled = Compile(pattern);
  return compiled.ok() ? "<compiles>" : compiled.error;
}

TEST(FlowPattern, PullsNamedFieldsOutOfALine) {
  // The case the syntax exists for, and the whole of it.
  EXPECT_EQ(Taken("name={name} age={age:int}", "name=Alice   age=27"),
            "name=Alice age=27");
  // Whitespace in the pattern matches whatever run the writer used, which is
  // what stops a pattern breaking on a second space.
  EXPECT_EQ(Taken("name={name} age={age:int}", "name=Bo age=3"),
            "name=Bo age=3");
  EXPECT_EQ(Taken("name={name}\tage={age:int}", "name=Bo    age=3"),
            "name=Bo age=3");
}

TEST(FlowPattern, SearchesRatherThanAnchoring) {
  // No leading or trailing wildcards to write: the pattern is what is being
  // looked for, not a description of the whole line.
  EXPECT_EQ(Taken("age={age:int}", "user bo, age=27, active"), "age=27");
  EXPECT_EQ(Taken("{level:word}: {message:line}",
                  "2026-08-13T10:00:00Z WARN: disk is filling up"),
            "level=WARN message=disk is filling up");
}

TEST(FlowPattern, AHoleTakesAsLittleAsItCanUnlessItIsLast) {
  // The shortest capture that lets the rest fit, so `{a}` stops at the first
  // `,` rather than the last.
  EXPECT_EQ(Taken("{a},{b}", "one,two,three"), "a=one b=two,three");
  EXPECT_EQ(Taken("{a},{b},end", "one,two,end"), "a=one b=two");
}

TEST(FlowPattern, AHoleStaysOnItsLine) {
  // A capture that swallowed a line break would match half of the next record.
  EXPECT_EQ(Taken("a={a} b={b}", "a=1\nb=2"), "<no match>");
  // Unless it says otherwise.
  EXPECT_EQ(Taken("a={a}\nb={rest:rest}", "a=1\nb=2\nc=3"), "a=1 rest=2\nc=3");
  EXPECT_EQ(Taken("first={first:line}", "first=one\nsecond=two"), "first=one");
}

TEST(FlowPattern, TypedHolesTakeOnlyWhatTheirTypeAllows) {
  EXPECT_EQ(Taken("n={n:int}", "n=-42x"), "n=-42");
  EXPECT_EQ(Taken("n={n:number}", "n=3.5kg"), "n=3.5");
  // A trailing point is punctuation, not part of the number.
  EXPECT_EQ(Taken("n={n:number}", "n=1. two"), "n=1");
  EXPECT_EQ(Taken("ok={ok:bool}", "ok=TRUE, next"), "ok=TRUE");
  EXPECT_EQ(Taken("{w:word} rest", "  hello   rest"), "w=hello");
  // A typed hole that cannot fit does not match at all rather than taking
  // something that is not one.
  EXPECT_EQ(Taken("n={n:int}", "n=none"), "<no match>");
}

TEST(FlowPattern, PositionalHolesAreReadByIndex) {
  EXPECT_EQ(Taken("{}:{}", "left:right"), "0=left 1=right");
  EXPECT_FALSE(Compile("{}:{}").pattern.AllNamed());
  EXPECT_TRUE(Compile("{a}:{b}").pattern.AllNamed());
}

TEST(FlowPattern, BracesAreWrittenTwiceToMeanThemselves) {
  EXPECT_EQ(Taken("{{{a}}}", "{x}"), "a=x");
  EXPECT_EQ(Taken("json {{\"k\": {v:int}}}", "json {\"k\": 7}"), "v=7");
}

TEST(FlowPattern, AHoleTakesAtLeastOneCharacter) {
  // A hole that matched nothing, silently, is the shape of mistake this whole
  // exercise is about.
  EXPECT_EQ(Taken("a={a} b", "a= b"), "<no match>");
}

TEST(FlowPattern, SaysWhatIsWrongWithAPattern) {
  EXPECT_EQ(Broken("name={name"),
            "A '{' with no '}' after it. Write '{{' for a literal one.");
  EXPECT_EQ(Broken("name=}"),
            "A '}' with no '{' before it. Write '}}' for a literal one.");
  EXPECT_NE(Broken("{a:wat}").find("not one of the kinds"), std::string::npos);
  EXPECT_NE(Broken("{a} {a}").find("Two holes are both called 'a'"),
            std::string::npos);
  EXPECT_NE(Broken("{a:rest} {b}").find("nothing can follow it"),
            std::string::npos);
  EXPECT_NE(Broken("plain text").find("captures nothing"), std::string::npos);
  EXPECT_NE(Broken("{a b}").find("is not a field name"), std::string::npos);
  // And where, so an editor can point at it.
  EXPECT_EQ(Compile("name={name").column, 5u);
}

TEST(FlowPattern, EveryTypeHasAName) {
  // The names travel in the diagnostics and in the synthesised shape, so a type
  // without one would be a hole in both.
  for (const std::string_view spelled :
       {"string", "int", "number", "bool", "word", "line", "rest", "duration",
        "time", "json"}) {
    const auto type = HoleTypeFromName(spelled);
    ASSERT_TRUE(type.has_value()) << spelled;
    EXPECT_EQ(HoleTypeName(*type), spelled);
  }
  EXPECT_FALSE(HoleTypeFromName("integer").has_value());
}

}  // namespace
}  // namespace a11::flow::pattern
