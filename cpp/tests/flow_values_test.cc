// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/flow/parser.h"
#include "a11/flow/syntax.h"
#include "a11/flow/values.h"

namespace a11::flow {
namespace {

/// The value the expression in a one-statement flow evaluates to.
///
/// Written as a flow rather than built by hand because the shape of an
/// expression is the parser's business, and a test that assembled syntax nodes
/// itself would pass while the grammar it claims to cover had moved on.
absl::StatusOr<Value> EvaluatedIn(std::string_view expression,
                                  const Value& it = Value::Null()) {
  static std::unique_ptr<HostBridge> bridge = NativeHostBridge();
  const std::string source = absl::StrCat("flow f {\n  out o: std::json\n  ",
                                          expression, " -> o\n}\n");
  auto parsed = std::make_shared<ParseResult>(Parse(source));
  if (!parsed->diagnostics.empty()) {
    return absl::InvalidArgumentError(parsed->diagnostics.front().message);
  }
  const syntax::FlowDeclaration& flow = *parsed->flows.front();
  const auto* pipe = syntax::As<syntax::Pipe>(flow.body.front().get());
  if (pipe == nullptr) {
    return absl::InvalidArgumentError("not a pipe");
  }
  EvalContext context;
  context.bridge = bridge.get();
  context.it = it;
  context.has_it = true;
  return Evaluate(*pipe->pipeline->source, context);
}

/// The text of the value that expression gives, for the many cases where the
/// rendering *is* the thing under test.
std::string TextOf(std::string_view expression,
                   const Value& it = Value::Null()) {
  const absl::StatusOr<Value> value = EvaluatedIn(expression, it);
  if (!value.ok()) {
    return absl::StrCat("<error: ", value.status().message(), ">");
  }
  return AsText(*value);
}

TEST(FlowValues, RendersScalarsAsTheReferenceDid) {
  EXPECT_EQ(AsText(Value::Null()), "");
  EXPECT_EQ(AsText(Value::Bool(true)), "true");
  EXPECT_EQ(AsText(Value::Bool(false)), "false");
  EXPECT_EQ(AsText(Value::Integer(3)), "3");
  // The `.0` matters: a number port that carried 3.0 rendered as "3.0", and a
  // flow's own output should not change spelling under a port.
  EXPECT_EQ(AsText(Value::Double(3.0)), "3.0");
  EXPECT_EQ(AsText(Value::Double(3.5)), "3.5");
  EXPECT_EQ(AsText(Value::Double(0.1)), "0.1");
  EXPECT_EQ(AsText(Value::String("hi")), "hi");
  // The window an exponent is used outside of is `repr`'s, not `%g`'s: the
  // digit count has nothing to do with it, so 90.0 is not `9e+01`.
  EXPECT_EQ(AsText(Value::Double(90.0)), "90.0");
  EXPECT_EQ(AsText(Value::Double(1234567890123456.0)), "1234567890123456.0");
  EXPECT_EQ(AsText(Value::Double(1e16)), "1e+16");
  EXPECT_EQ(AsText(Value::Double(1e-5)), "1e-05");
  EXPECT_EQ(AsText(Value::Double(0.0)), "0.0");
  EXPECT_EQ(AsText(Value::Double(-0.0)), "-0.0");
}

TEST(FlowValues, RendersContainersAsSortedJson) {
  const Value object =
      Value::Object({{"b", Value::Integer(2)}, {"a", Value::String("x")}});
  EXPECT_EQ(AsText(object), R"({"a": "x", "b": 2})");
  EXPECT_EQ(AsText(Value::List({Value::Integer(1), Value::Null()})),
            "[1, null]");
  // ensure_ascii, as `json.dumps` defaults to: the same text whichever engine
  // rendered it, and no encoding question for whatever reads it next.
  EXPECT_EQ(AsText(Value::List({Value::String("é")})), R"(["\u00e9"])");
  EXPECT_EQ(AsText(Value::List({Value::String("🙂")})), R"(["\ud83d\ude42"])");
}

TEST(FlowValues, ReadsNumbersOutOfWhateverArrived) {
  EXPECT_EQ(AsNumber(Value::String(" 42 ")).integer(), 42);
  EXPECT_EQ(AsNumber(Value::String("-7")).integer(), -7);
  EXPECT_EQ(AsNumber(Value::String("1.5")).number(), 1.5);
  EXPECT_EQ(AsNumber(Value::String("nonsense")).integer(), 0);
  EXPECT_EQ(AsNumber(Value::Null()).integer(), 0);
  EXPECT_EQ(AsNumber(Value::Bool(true)).integer(), 1);
  EXPECT_EQ(
      AsNumber(Value::List({Value::Integer(1), Value::Integer(2)})).integer(),
      2);
}

TEST(FlowValues, LookupAnswersNothingRatherThanFailing) {
  const Value object = Value::Object({{"a", Value::Integer(1)}});
  EXPECT_EQ(Lookup(object, Value::String("a")).integer(), 1);
  EXPECT_TRUE(Lookup(object, Value::String("missing")).IsNull());
  EXPECT_TRUE(Lookup(Value::Null(), Value::String("a")).IsNull());
  const Value list = Value::List({Value::Integer(1), Value::Integer(2)});
  EXPECT_EQ(Lookup(list, Value::Integer(-1)).integer(), 2);
  EXPECT_TRUE(Lookup(list, Value::Integer(9)).IsNull());
  // A string is not a container of values here, so asking is answered rather
  // than refused.
  EXPECT_TRUE(Lookup(Value::String("abc"), Value::String("length")).IsNull());
}

TEST(FlowValues, TruncateKeepsTheFrontOfWhateverItIs) {
  EXPECT_EQ(AsText(Truncate(Value::String("abcdef"), 3)), "abc");
  // Code points, not bytes: a length is the length a person sees.
  EXPECT_EQ(AsText(Truncate(Value::String("éé"), 1)), "é");
  EXPECT_EQ(Truncate(Value::List({Value::Integer(1), Value::Integer(2)}), 1)
                .items()
                .size(),
            1u);
}

TEST(FlowValues, WritesDurationsTheWayTheLanguageWritesThem) {
  EXPECT_EQ(DurationText(absl::Seconds(0)), "0s");
  EXPECT_EQ(DurationText(absl::Seconds(30)), "30s");
  EXPECT_EQ(DurationText(absl::Seconds(90)), "1m30s");
  EXPECT_EQ(DurationText(absl::Milliseconds(250)), "250ms");
  EXPECT_EQ(DurationText(absl::Minutes(1) + absl::Milliseconds(500)),
            "1m500ms");
  EXPECT_EQ(DurationText(-absl::Seconds(90)), "-1m30s");
  EXPECT_EQ(DurationText(absl::InfiniteDuration()), "forever");
  // A unit spec gives a bare count, ready to go into a metric.
  EXPECT_EQ(DurationText(absl::Seconds(90), "s"), "90");
  EXPECT_EQ(DurationText(absl::Milliseconds(1500), "s"), "1.5");
  // Microseconds of a millisecond and a half are the value, so they are kept.
  EXPECT_EQ(DurationText(absl::Microseconds(1500) + absl::Nanoseconds(500)),
            "1ms500us500ns");
  EXPECT_EQ(DurationText(absl::Seconds(1) + absl::Microseconds(5)), "1s5us");
  // Microseconds of a minute and a half are noise, and go once a coarser unit
  // than the second has already been written.
  EXPECT_EQ(DurationText(absl::Minutes(1) + absl::Microseconds(5)), "1m");
}

TEST(FlowValues, ReadsDurationsBackFromTextAndFromSeconds) {
  EXPECT_EQ(ParseDuration("30s"), absl::Seconds(30));
  EXPECT_EQ(ParseDuration("1m30s"), absl::Seconds(90));
  EXPECT_EQ(ParseDuration("1m30s500ms"),
            absl::Seconds(90) + absl::Milliseconds(500));
  // The sign belongs to the whole, not to the first piece.
  EXPECT_EQ(ParseDuration("-1m30s"), -absl::Seconds(90));
  EXPECT_EQ(ParseDuration("forever"), absl::InfiniteDuration());
  EXPECT_EQ(ParseDuration("12"), absl::Seconds(12));
  EXPECT_FALSE(ParseDuration("30 fortnights").has_value());
  EXPECT_FALSE(ParseDuration("").has_value());
  EXPECT_EQ(AsDuration(Value::Integer(5)), absl::Seconds(5));
  EXPECT_EQ(AsDuration(Value::String("250ms")), absl::Milliseconds(250));
}

TEST(FlowValues, WritesAndReadsInstants) {
  const absl::Time when = absl::FromUnixSeconds(1786000462);
  EXPECT_EQ(TimeText(when), "2026-08-06T07:14:22Z");
  EXPECT_EQ(TimeText(when, "%H:%M:%S"), "07:14:22");
  EXPECT_EQ(TimeText(when, "epoch"), "1786000462");
  // Both ways round, because a timestamp reaches a flow as text far more often
  // than as an instant, and until it is one it cannot be compared with `now()`.
  EXPECT_EQ(AsTime(Value::String("2026-08-06T07:14:22Z")), when);
  // A timestamp with no zone is UTC here, which is the zone every instant this
  // language writes is in.
  EXPECT_EQ(AsTime(Value::String("2026-08-06 07:14:22")), when);
  EXPECT_EQ(AsTime(Value::Integer(1786000462)), when);
  EXPECT_EQ(TimeText(when + absl::Milliseconds(250)),
            "2026-08-06T07:14:22.250000Z");
}

TEST(FlowValues, StrformatIsPrintfAndOnlyPrintf) {
  EXPECT_EQ(Strformat(Value::String("%s of %s"),
                      {Value::String("a"), Value::String("b")}),
            "a of b");
  EXPECT_EQ(Strformat(Value::String("%2$s then %1$s"),
                      {Value::String("a"), Value::String("b")}),
            "b then a");
  EXPECT_EQ(Strformat(Value::String("%-4s|"), {Value::String("x")}), "x   |");
  EXPECT_EQ(Strformat(Value::String("%06.2f"), {Value::Double(3.5)}), "003.50");
  EXPECT_EQ(Strformat(Value::String("%d%%"), {Value::Integer(50)}), "50%");
  // A `%` that starts nothing recognisable says what it looks like.
  EXPECT_EQ(Strformat(Value::String("100% done"), {}), "100% done");
  // A conversion with no value behind it stays visible, which is easier to
  // diagnose than a flow that died formatting a log line.
  EXPECT_EQ(Strformat(Value::String("%3$s"), {Value::String("a")}), "%3$s");
  // A duration unit or a time pattern applied before the conversion.
  EXPECT_EQ(Strformat(Value::String("took %(ms)dms"),
                      {Value::Duration(absl::Seconds(2))}),
            "took 2000ms");
  EXPECT_EQ(
      Strformat(Value::String("%s"), {Value::Duration(absl::Seconds(90))}),
      "1m30s");
}

TEST(FlowValues, StrformatPreservesNulInStringsAndLiteralText) {
  const std::string text("a\0b", 3);
  EXPECT_EQ(Strformat(Value::String("%s"), {Value::String(text)}), text);
  EXPECT_EQ(Strformat(Value::String("%5s"), {Value::String(text)}),
            "  " + text);
  EXPECT_EQ(Strformat(Value::String("%-5s"), {Value::String(text)}),
            text + "  ");
  EXPECT_EQ(Strformat(Value::String("%.2s"), {Value::String(text)}),
            std::string("a\0", 2));
  EXPECT_EQ(Strformat(Value::String("%+05s"), {Value::String(text)}),
            "  " + text);
  EXPECT_EQ(Strformat(Value::String(std::string("%s\0%s", 5)),
                      {Value::String("a"), Value::String("b")}),
            text);
  EXPECT_EQ(StrformatCompiled(CompileStrformat("%s"), {Value::String(text)}),
            text);
}

TEST(FlowValues, EvaluatesLiteralsAndPaths) {
  EXPECT_EQ(TextOf(R"("hello")"), "hello");
  EXPECT_EQ(TextOf("42"), "42");
  EXPECT_EQ(TextOf("30s"), "30s");
  EXPECT_EQ(TextOf(R"({"a": 1, "b": [2, 3]})"), R"({"a": 1, "b": [2, 3]})");
  EXPECT_EQ(TextOf(R"({"a": {"b": 7}}.a.b)"), "7");
  EXPECT_EQ(TextOf("[10, 20][1]"), "20");
}

TEST(FlowValues, EvaluatesComparisonsWithoutDyingOnMixedTypes) {
  EXPECT_EQ(TextOf(R"("3" < 5)"), "true");
  EXPECT_EQ(TextOf("2 == 2"), "true");
  EXPECT_EQ(TextOf(R"("a" < "b")"), "true");
  EXPECT_EQ(TextOf("10s > 9s"), "true");
  EXPECT_EQ(TextOf("not 0"), "true");
  EXPECT_EQ(TextOf(R"(1 in [1, 2])"), "true");
  EXPECT_EQ(TextOf(R"("b" in {"b": 1})"), "true");
  // `or` gives the value, not a boolean, which is what makes it read as a
  // default.
  EXPECT_EQ(TextOf(R"("" or "unknown")"), "unknown");
  EXPECT_EQ(TextOf(R"("here" and "there")"), "there");
}

TEST(FlowValues, OptionDefaultsTestPresenceAndEvaluateTheFallbackLazily) {
  for (const Value& payload :
       {Value::Null(), Value::Bool(false), Value::Integer(0), Value::String(""),
        Value::List({}), Value::Object({})}) {
    const auto result =
        EvaluatedIn("it or b64decode(\"!\")", Value::Option(payload));
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(*result, payload);
    EXPECT_FALSE(result->is_option());
  }
  EXPECT_EQ(TextOf("it or 42", Value::Option(std::nullopt)), "42");
  EXPECT_FALSE(
      EvaluatedIn("it or b64decode(\"!\")", Value::Option(std::nullopt)).ok());
  const auto record = EvaluatedIn("{has_value: false} or 42");
  ASSERT_TRUE(record.ok()) << record.status();
  EXPECT_EQ(record->kind(), Value::Kind::kObject);
  const Value nested = Value::Option(Value::Integer(7));
  const auto result = EvaluatedIn("it or 42", Value::Option(nested));
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_TRUE(result->is_option());
  EXPECT_EQ(*result, nested);
}

TEST(FlowValues, StringEscapesPreserveNulThroughEvaluation) {
  const auto value = EvaluatedIn(R"("before\0after")");
  ASSERT_TRUE(value.ok()) << value.status();
  EXPECT_EQ(*value, Value::String(std::string("before\0after", 12)));
  EXPECT_EQ(TextOf(R"(len("\0\a\b\f\n\r\t\v"))"), "8");
}

TEST(FlowValues, ArithmeticIsThereForTimes) {
  EXPECT_EQ(TextOf("1 + 2"), "3");
  EXPECT_EQ(TextOf("1.5 + 1"), "2.5");
  EXPECT_EQ(TextOf("30s + 30s"), "1m");
  EXPECT_EQ(TextOf("1m - 30s"), "30s");
  EXPECT_EQ(TextOf("-30s + 1m"), "30s");
  EXPECT_FALSE(EvaluatedIn("30s + 30").ok());
  EXPECT_EQ(TextOf(R"(parse_time("2026-08-05T21:54:22Z") + 60s)"),
            "2026-08-05T21:55:22Z");
  EXPECT_EQ(TextOf(R"(60s + parse_time("2026-08-05T21:54:22Z"))"),
            "2026-08-05T21:55:22Z");
  EXPECT_EQ(TextOf(R"(parse_time("2026-08-05T21:55:22Z") - 60s)"),
            "2026-08-05T21:54:22Z");
  EXPECT_EQ(TextOf(R"(parse_time("2026-08-05T21:55:22Z") - )"
                   R"(parse_time("2026-08-05T21:54:22Z"))"),
            "1m");
  // The one combination that means nothing says so rather than inventing an
  // answer.
  EXPECT_FALSE(EvaluatedIn(R"(parse_time("2026-08-05T21:54:22Z") + )"
                           R"(parse_time("2026-08-05T21:54:22Z"))")
                   .ok());
}

TEST(FlowValues, CallsTheFixedFunctionSet) {
  EXPECT_EQ(TextOf(R"(len("abc"))"), "3");
  EXPECT_EQ(TextOf(R"(upper("ab"))"), "AB");
  EXPECT_EQ(TextOf(R"(trim("  x  "))"), "x");
  EXPECT_EQ(TextOf(R"(join(["a", "b"], "-"))"), "a-b");
  EXPECT_EQ(TextOf(R"(join(split("a b c"), ","))"), "a,b,c");
  EXPECT_EQ(TextOf(R"(join(split("a,b", ","), "|"))"), "a|b");
  EXPECT_EQ(TextOf(R"(keys({"b": 1, "a": 2}))"), R"(["a", "b"])");
  EXPECT_EQ(TextOf(R"(values({"b": 1, "a": 2}))"), "[2, 1]");
  EXPECT_EQ(TextOf(R"(merge({"a": 1}, {"b": 2}))"), R"({"a": 1, "b": 2})");
  EXPECT_EQ(TextOf(R"(contains("hello", "ell"))"), "true");
  EXPECT_EQ(TextOf(R"(startswith("hello", ["x", "he"]))"), "true");
  EXPECT_EQ(TextOf(R"(endswith("done.", [".", "?"]))"), "true");
  EXPECT_EQ(TextOf(R"(replace("a-b-c", "-", "+"))"), "a+b+c");
  EXPECT_EQ(TextOf(R"(slice("abcdef", 1, 3))"), "bc");
  EXPECT_EQ(TextOf(R"(slice("abcdef", -2))"), "ef");
  EXPECT_EQ(TextOf(R"(seconds(parse_duration("1m30s")))"), "90");
  EXPECT_EQ(TextOf(R"(strformat("%s!", "hi"))"), "hi!");
  EXPECT_FALSE(EvaluatedIn(R"(nonesuch("x"))").ok());
  const std::vector<Value> removed = {Value::Null(), Value::Integer(1)};
  EXPECT_FALSE(CallBuiltin("default", removed, nullptr).ok());
}

TEST(FlowValues, OptionalProjectionKeepsNullDistinctFromMissing) {
  EXPECT_EQ(TextOf(R"({title: null}?.title.has_value)"), "true");
  EXPECT_EQ(TextOf(R"({title: null}?.other.has_value)"), "false");
  EXPECT_EQ(TextOf(R"({title: 0}?.title or 9)"), "0");
  EXPECT_EQ(TextOf(R"({title: null}?.title or 9)"), "");
  EXPECT_EQ(TextOf(R"({title: 1}?.other or 9)"), "9");
  EXPECT_EQ(TextOf(R"({a: {b: 3}}?.a?.b or 9)"), "3");
  EXPECT_EQ(TextOf(R"({a: null}?.a?.b.has_value)"), "false");
  EXPECT_EQ(TextOf(R"(([null, 5])?[0].has_value)"), "true");
  EXPECT_EQ(TextOf(R"(([null, 5])?[-3].has_value)"), "false");
  EXPECT_EQ(TextOf(R"({a: 7}?["a"] or 9)"), "7");
  EXPECT_EQ(TextOf(R"({a: 7}?["b"] or 9)"), "9");
  EXPECT_FALSE(EvaluatedIn(R"({a: 7}?.b.value)").ok());
  EXPECT_FALSE(EvaluatedIn(R"(get({a: 7}, "a"))").ok());
  EXPECT_FALSE(EvaluatedIn(R"(std::get({a: 7}, "a"))").ok());
}

TEST(FlowValues, ObjectFieldsOmitAbsentOptionsAndUnwrapPresentOnes) {
  const auto value = EvaluatedIn(R"({
    missing: {}?.field,
    empty: {field: null}?.field,
    zero: {field: 0}?.field,
    flag: {field: false}?.field,
    ordinary: {has_value: false}
  })");
  ASSERT_TRUE(value.ok()) << value.status();
  EXPECT_EQ(value->Get("missing"), nullptr);
  ASSERT_NE(value->Get("empty"), nullptr);
  EXPECT_TRUE(value->Get("empty")->IsNull());
  ASSERT_NE(value->Get("zero"), nullptr);
  EXPECT_EQ(value->Get("zero")->integer(), 0);
  ASSERT_NE(value->Get("flag"), nullptr);
  EXPECT_FALSE(value->Get("flag")->boolean());
  ASSERT_NE(value->Get("ordinary"), nullptr);
  EXPECT_FALSE(value->Get("ordinary")->is_option());
  const auto list = EvaluatedIn(R"([{}?.field])");
  ASSERT_TRUE(list.ok()) << list.status();
  ASSERT_EQ(list->items().size(), 1u);
  EXPECT_TRUE(list->items().front().is_option());
}

TEST(FlowValues, Base64GoesBothWaysInBothAlphabets) {
  // Encoding produces text for JSON fields and headers; decoding recovers the
  // original bytes.
  EXPECT_EQ(TextOf(R"(b64encode("hi there"))"), "aGkgdGhlcmU=");
  EXPECT_EQ(EvaluatedIn(R"(b64encode("hi"))")->kind(), Value::Kind::kString);
  EXPECT_EQ(TextOf(R"(text(b64decode("aGkgdGhlcmU=")))"), "hi there");
  EXPECT_EQ(EvaluatedIn(R"(b64decode("aGk="))")->kind(), Value::Kind::kBytes);

  // The two alphabets differ exactly where the bytes need them to: `+` and `/`
  // against `-` and `_`. Written as base64 rather than as escapes, because the
  // language has no `\x` and the bytes that show the difference are not text.
  EXPECT_EQ(TextOf(R"(b64encode(b64decode("++//")))"), "++//");
  EXPECT_EQ(TextOf(R"(b64urlencode(b64decode("++//")))"), "--__");
  EXPECT_EQ(TextOf(R"(b64encode(b64urldecode("--__")))"), "++//");

  // Padding is written, and not insisted on coming back -- the web-safe form is
  // routinely sent without it.
  EXPECT_EQ(TextOf(R"(b64encode("hi"))"), "aGk=");
  EXPECT_EQ(TextOf(R"(text(b64urldecode("aGk")))"), "hi");

  // Round trips, and refuses text that is not base64 at all rather than
  // answering something plausible.
  EXPECT_EQ(TextOf(R"(text(b64decode(b64encode("round trip"))))"),
            "round trip");
  EXPECT_FALSE(EvaluatedIn(R"(b64decode("not base64 at all!"))").ok());
}

TEST(FlowValues, Utf8GoesBetweenTextAndBytesStrictly) {
  const absl::StatusOr<Value> encoded = EvaluatedIn(R"(utf8encode("café"))");
  ASSERT_TRUE(encoded.ok()) << encoded.status();
  EXPECT_EQ(encoded->kind(), Value::Kind::kBytes);
  EXPECT_EQ(TextOf(R"(b64encode(utf8encode("✓")))"), "4pyT");
  EXPECT_EQ(TextOf(R"(utf8decode(utf8encode("café")))"), "café");
  EXPECT_EQ(EvaluatedIn(R"(utf8decode(utf8encode("text")))")->kind(),
            Value::Kind::kString);
  EXPECT_FALSE(EvaluatedIn(R"(utf8decode(b64decode("//8=")))").ok());
}

TEST(FlowValues, ALiteralMaySpreadAnotherIntoItself) {
  const Value record =
      Value::Object({{"a", Value::Integer(1)}, {"b", Value::Integer(2)}});
  // A later key wins over one the spread brought in, and an earlier one does
  // not -- which is what makes `{...it, "tags": [..]}` an override.
  EXPECT_EQ(TextOf(R"({...it, "b": 9})", record), R"({"a": 1, "b": 9})");
  EXPECT_EQ(TextOf(R"({"b": 9, ...it})", record), R"({"a": 1, "b": 2})");
  // `...` is the same thing as `...`.
  EXPECT_EQ(TextOf(R"({...it, "c": 3})", record),
            R"({"a": 1, "b": 2, "c": 3})");

  const Value items = Value::List({Value::Integer(1), Value::Integer(2)});
  EXPECT_EQ(TextOf(R"([...it, 3])", items), "[1, 2, 3]");
  EXPECT_EQ(TextOf(R"([0, ...it])", items), "[0, 1, 2]");

  // Spreading nothing contributes nothing rather than a hole, so a field a
  // producer did not send does not turn into a null in the middle of a list.
  EXPECT_EQ(TextOf(R"([1, ...it.missing, 2])",
                   Value::Object({{"x", Value::Integer(1)}})),
            "[1, 2]");
  // And a value with no keys to take contributes none, rather than an invented
  // one.
  EXPECT_EQ(TextOf(R"({...it, "a": 1})", Value::String("not a record")),
            R"({"a": 1})");
}

TEST(FlowValues, ItIsTheValueAStageIsLookingAt) {
  EXPECT_EQ(TextOf("it", Value::Integer(7)), "7");
  EXPECT_EQ(TextOf("it.name", Value::Object({{"name", Value::String("Ada")}})),
            "Ada");
}

TEST(FlowValues, CoercesToTheBuiltInTypes) {
  EXPECT_EQ(TextOf(R"(42 as string)"), "42");
  EXPECT_EQ(TextOf(R"("42" as integer)"), "42");
  EXPECT_EQ(TextOf(R"("" as bool)"), "false");
  EXPECT_EQ(TextOf(R"(1 as list)"), "[1]");
  EXPECT_EQ(TextOf(R"([1, 2] as list[string])"), R"(["1", "2"])");
  // A tag is the host's, and a host that does not know it says which type and
  // why rather than failing anonymously.
  const absl::StatusOr<Value> unknown =
      EvaluatedIn(R"({"a": 1} as some::unknown::Type)");
  ASSERT_FALSE(unknown.ok());
  EXPECT_NE(unknown.status().message().find("some::unknown::Type"),
            std::string::npos);
}

TEST(FlowValues, CheckedNumericConversions) {
  EXPECT_EQ(TextOf("i8(127)"), "127");
  EXPECT_EQ(TextOf("u8(255)"), "255");
  EXPECT_EQ(TextOf("i32(3.0)"), "3");
  EXPECT_EQ(TextOf(R"(i32("42"))"), "42");
  EXPECT_FALSE(EvaluatedIn("i8(128)").ok());
  EXPECT_FALSE(EvaluatedIn("u8(-1)").ok());
  EXPECT_FALSE(EvaluatedIn("i32(3.5)").ok());
  EXPECT_FALSE(EvaluatedIn("i32(true)").ok());
  EXPECT_FALSE(EvaluatedIn(R"(i32("bad"))").ok());
  EXPECT_FALSE(EvaluatedIn(R"(f64("1e-9999"))").ok());
  EXPECT_EQ(TextOf(R"(u128("340282366920938463463374607431768211455"))"),
            "340282366920938463463374607431768211455");
  EXPECT_FALSE(
      EvaluatedIn(R"(u128("340282366920938463463374607431768211456"))").ok());
}

TEST(FlowValues, CheckedArithmetic) {
  EXPECT_EQ(TextOf("1.5 + 2.5"), "4.0");
  EXPECT_EQ(TextOf("i8(6) * i8(7)"), "42");
  EXPECT_EQ(TextOf("i8(12) / i8(3)"), "4");
  EXPECT_EQ(TextOf("i8(13) % i8(5)"), "3");
  EXPECT_EQ(TextOf("i8(2) + 3"), "5");
  EXPECT_EQ(TextOf("i8(2) + i16(3)"), "5");
  EXPECT_FALSE(EvaluatedIn("i8(127) + i8(1)").ok());
  EXPECT_FALSE(EvaluatedIn("i8(5) / i8(2)").ok());
  EXPECT_FALSE(EvaluatedIn("i8(5) / i8(0)").ok());
  EXPECT_FALSE(EvaluatedIn("f64(1e-300) * f64(1e-300)").ok());
  EXPECT_FALSE(EvaluatedIn("u8(0) - u8(1)").ok());
  EXPECT_FALSE(EvaluatedIn("i8(1) + u8(1)").ok());
  EXPECT_FALSE(EvaluatedIn("1 + true").ok());
  EXPECT_EQ(
      TextOf(R"(u128("18446744073709551616") > u128("18446744073709551615"))"),
      "true");
  EXPECT_EQ(TextOf(R"(u128("1267650600228229401496703205377") > )"
                   R"(f64("1267650600228229401496703205376"))"),
            "true");
}

TEST(FlowValues, InfiniteTimeAndDurationConstants) {
  EXPECT_EQ(TextOf("std::infinite_future()"), "infinite-future");
  EXPECT_EQ(TextOf("std::infinite_past()"), "infinite-past");
  EXPECT_EQ(TextOf("std::infinite_duration()"), "forever");
  EXPECT_EQ(TextOf("-std::infinite_duration()"), "-forever");
  EXPECT_EQ(TextOf("std::zero_duration()"), "0s");
  EXPECT_EQ(TextOf("std::infinite_future() + 1s"), "infinite-future");
  EXPECT_FALSE(
      EvaluatedIn("std::infinite_future() - std::infinite_future()").ok());
  EXPECT_FALSE(EvaluatedIn("std::seconds(9223372036854775806) + 2s").ok());
}

TEST(FlowValues, DurationUnitFunctions) {
  EXPECT_EQ(TextOf("std::nanoseconds(500)"), "500ns");
  EXPECT_EQ(TextOf("std::microseconds(2)"), "2us");
  EXPECT_EQ(TextOf("std::milliseconds(3)"), "3ms");
  EXPECT_EQ(TextOf("std::seconds(4)"), "4s");
  EXPECT_EQ(TextOf("std::minutes(5)"), "5m");
  EXPECT_EQ(TextOf("std::hours(6)"), "6h");
  EXPECT_EQ(TextOf("std::seconds(90s)"), "90");
  EXPECT_EQ(TextOf("std::seconds(250ms)"), "0");
  EXPECT_EQ(TextOf("std::seconds(-250ms)"), "0");
  EXPECT_EQ(TextOf("std::milliseconds(1s)"), "1000");
  EXPECT_EQ(
      TextOf(R"(std::nanoseconds(u128("18446744073709551616")) == )"
             R"(std::seconds(18446744073) + std::nanoseconds(709551616))"),
      "true");
  EXPECT_FALSE(EvaluatedIn("std::seconds(true)").ok());
  EXPECT_FALSE(EvaluatedIn(R"(std::seconds("250ms"))").ok());
  EXPECT_EQ(TextOf(R"(std::seconds(std::parse_duration("250ms")))"), "0");
}

TEST(FlowValues, RoundingFunctionsPreserveNumericTypes) {
  EXPECT_EQ(TextOf("1.5 + 2.5"), "4.0");
  EXPECT_EQ(TextOf("std::floor(f64(2.75))"), "2.0");
  EXPECT_EQ(TextOf("std::ceil(f32(-2.75))"), "-2.0");
  EXPECT_EQ(TextOf("std::round(f64(-2.5))"), "-3.0");
  EXPECT_EQ(TextOf("std::round(i128(7))"), "7");
  EXPECT_EQ(TextOf("std::round(number(2.5)) + number(1)"), "4.0");
  EXPECT_EQ(TextOf("std::floor(2.5) + 1.5"), "3.5");
  EXPECT_FALSE(EvaluatedIn("std::floor(true)").ok());
  EXPECT_FALSE(EvaluatedIn("std::ceil(1s)").ok());
}

TEST(FlowValues, DurationsScaleByNumericValues) {
  EXPECT_EQ(TextOf("250ms * 4"), "1s");
  EXPECT_EQ(TextOf("4 * 250ms"), "1s");
  EXPECT_EQ(TextOf("2s * f64(0.25)"), "500ms");
  EXPECT_EQ(TextOf("f32(-2) * 250ms"), "-500ms");
  EXPECT_EQ(TextOf("std::parse_duration(text(1s * 9223372036854775807)) == "
                   "1s * 9223372036854775807"),
            "true");
  EXPECT_EQ(TextOf(R"(1ns * u128("18446744073709551616") == )"
                   R"(std::seconds(18446744073) + 709551616ns)"),
            "true");
  EXPECT_FALSE(EvaluatedIn("std::infinite_duration() * 0").ok());
  EXPECT_FALSE(EvaluatedIn("std::seconds(9223372036854775806) * 2").ok());
  EXPECT_FALSE(EvaluatedIn("1ns * f64(0.01)").ok());
  EXPECT_FALSE(EvaluatedIn("1s * true").ok());
}

TEST(FlowValues, ScientificLiteralsEvaluateAsFloatingNumbers) {
  EXPECT_EQ(TextOf("1e3 + 2.5E-2"), "1000.025");
  EXPECT_EQ(TextOf("1e3ms * 2"), "2s");
}

TEST(FlowValues, StatusesAreData) {
  const Value record = StatusRecord(absl::NotFoundError("gone"));
  EXPECT_EQ(
      AsText(record),
      R"({"code": "NOT_FOUND", "details": [], "message": "gone", "number": 5,)"
      R"( "ok": false})");
  EXPECT_EQ(StatusCodeOf(Value::String("not_found")),
            absl::StatusCode::kNotFound);
  EXPECT_EQ(StatusCodeOf(Value::String("NOT_FOUND")),
            absl::StatusCode::kNotFound);
  EXPECT_EQ(StatusCodeOf(Value::Integer(5)), absl::StatusCode::kNotFound);
  EXPECT_FALSE(StatusCodeOf(Value::String("nonesuch")).has_value());
  // A record read back is the status it described, which is what lets a flow
  // re-raise one it recovered from.
  const absl::Status again = StatusOfRecord(record);
  EXPECT_EQ(again.code(), absl::StatusCode::kNotFound);
  EXPECT_EQ(again.message(), "gone");
}

TEST(FlowValues, ChunksTravelWholeUntilSomethingLooksInside) {
  static std::unique_ptr<HostBridge> bridge = NativeHostBridge();
  const absl::StatusOr<data::Chunk> chunk =
      bridge->ToChunk(Value::Object({{"a", Value::Integer(1)}}), "");
  ASSERT_TRUE(chunk.ok());
  EXPECT_EQ(chunk->GetMimetype(), "application/json");
  const absl::StatusOr<Value> read = bridge->FromChunk(*chunk);
  ASSERT_TRUE(read.ok());
  EXPECT_EQ(Lookup(*read, Value::String("a")).integer(), 1);
}

TEST(FlowValues, JsonChunkPreservesNestedWideIntegers) {
  auto bridge = NativeHostBridge();
  data::Chunk chunk;
  chunk.metadata = data::ChunkMetadata{.mimetype = "application/json"};
  chunk.data = R"({"n":340282366920938463463374607431768211455})";
  const auto parsed = bridge->FromChunk(chunk);
  ASSERT_TRUE(parsed.ok()) << parsed.status();
  const Value* number = parsed->Get("n");
  ASSERT_NE(number, nullptr);
  EXPECT_EQ(AsText(*number), "340282366920938463463374607431768211455");

  chunk.data = R"({"n":340282366920938463463374607431768211456})";
  EXPECT_TRUE(absl::IsOutOfRange(bridge->FromChunk(chunk).status()));

  const Value nested = AsJson(Value::String(
      R"({"first":{"has_value":true,"value":null},"second":{"has_value":false}})"));
  EXPECT_EQ(
      JsonText(nested),
      R"({"first": {"has_value": true, "value": null}, "second": {"has_value": false}})");
}

}  // namespace

TEST(FlowValues, CheckedTimeParsersAndIntegerAssignmentsRejectInvalidValues) {
  EXPECT_FALSE(EvaluatedIn(R"(duration("1s"))").ok());
  EXPECT_FALSE(EvaluatedIn(R"(time("2026-10-30"))").ok());
  EXPECT_FALSE(EvaluatedIn(R"("1s" as std::duration)").ok());
  EXPECT_FALSE(EvaluatedIn(R"("2026-10-30" as std::time)").ok());
  EXPECT_FALSE(EvaluatedIn(R"(parse_duration("not a duration"))").ok());
  EXPECT_FALSE(EvaluatedIn(R"(parse_time("not a time"))").ok());
  EXPECT_EQ(TextOf(R"(parse_duration("1m30s"))"), "1m30s");
  EXPECT_EQ(TextOf(R"(parse_time("2026-10-30"))"), "2026-10-30T00:00:00Z");
  EXPECT_FALSE(EvaluatedIn(R"flow(int(number("1e100")))flow").ok());
  EXPECT_EQ(TextOf("int(9223372036854775807)"), "9223372036854775807");
}

}  // namespace a11::flow
