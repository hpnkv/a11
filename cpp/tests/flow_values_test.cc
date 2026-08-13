// Copyright 2026 The A11 Authors.

#include "a11/flow/values.h"

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
  const std::string source =
      absl::StrCat("flow f {\n  out o: json\n  ", expression, " -> o\n}\n");
  auto parsed = std::make_shared<ParseResult>(Parse(source));
  if (!parsed->diagnostics.empty()) {
    return absl::InvalidArgumentError(parsed->diagnostics.front().message);
  }
  const syntax::FlowDeclaration& flow = *parsed->flows.front();
  const auto* pipe = syntax::As<syntax::Pipe>(flow.body.front().get());
  if (pipe == nullptr) return absl::InvalidArgumentError("not a pipe");
  EvalContext context;
  context.bridge = bridge.get();
  context.it = it;
  context.has_it = true;
  return Evaluate(*pipe->pipeline->source, context);
}

/// The text of the value that expression gives, for the many cases where the
/// rendering *is* the thing under test.
std::string TextOf(std::string_view expression, const Value& it = Value::Null()) {
  const absl::StatusOr<Value> value = EvaluatedIn(expression, it);
  if (!value.ok()) return absl::StrCat("<error: ", value.status().message(), ">");
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
  const Value object = Value::Object({{"b", Value::Integer(2)},
                                      {"a", Value::String("x")}});
  EXPECT_EQ(AsText(object), R"({"a": "x", "b": 2})");
  EXPECT_EQ(AsText(Value::List({Value::Integer(1), Value::Null()})),
            "[1, null]");
  // ensure_ascii, as `json.dumps` defaults to: the same text whichever engine
  // rendered it, and no encoding question for whatever reads it next.
  EXPECT_EQ(AsText(Value::List({Value::String("é")})),
            R"(["\u00e9"])");
  EXPECT_EQ(AsText(Value::List({Value::String("🙂")})),
            R"(["\ud83d\ude42"])");
}

TEST(FlowValues, ReadsNumbersOutOfWhateverArrived) {
  EXPECT_EQ(AsNumber(Value::String(" 42 ")).integer(), 42);
  EXPECT_EQ(AsNumber(Value::String("-7")).integer(), -7);
  EXPECT_EQ(AsNumber(Value::String("1.5")).number(), 1.5);
  EXPECT_EQ(AsNumber(Value::String("nonsense")).integer(), 0);
  EXPECT_EQ(AsNumber(Value::Null()).integer(), 0);
  EXPECT_EQ(AsNumber(Value::Bool(true)).integer(), 1);
  EXPECT_EQ(AsNumber(Value::List({Value::Integer(1), Value::Integer(2)}))
                .integer(),
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
  EXPECT_EQ(Strformat(Value::String("%s"),
                      {Value::Duration(absl::Seconds(90))}),
            "1m30s");
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

TEST(FlowValues, ArithmeticIsThereForTimes) {
  EXPECT_EQ(TextOf("1 + 2"), "3");
  EXPECT_EQ(TextOf("1.5 + 1"), "2.5");
  EXPECT_EQ(TextOf("30s + 30s"), "1m");
  EXPECT_EQ(TextOf("1m - 30s"), "30s");
  // A bare number beside a duration counts as seconds.
  EXPECT_EQ(TextOf("30s + 30"), "1m");
  EXPECT_EQ(TextOf(R"(time("2026-08-05T21:54:22Z") + 60s)"),
            "2026-08-05T21:55:22Z");
  EXPECT_EQ(TextOf(R"(time("2026-08-05T21:55:22Z") - )"
                   R"(time("2026-08-05T21:54:22Z"))"),
            "1m");
  // The one combination that means nothing says so rather than inventing an
  // answer.
  EXPECT_FALSE(EvaluatedIn(R"(time("2026-08-05T21:54:22Z") + )"
                           R"(time("2026-08-05T21:54:22Z"))")
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
  EXPECT_EQ(TextOf(R"(get({"a": 1}, "b", "fallback"))"), "fallback");
  EXPECT_EQ(TextOf(R"(merge({"a": 1}, {"b": 2}))"), R"({"a": 1, "b": 2})");
  EXPECT_EQ(TextOf(R"(contains("hello", "ell"))"), "true");
  EXPECT_EQ(TextOf(R"(starts-with("hello", ["x", "he"]))"), "true");
  EXPECT_EQ(TextOf(R"(ends-with("done.", [".", "?"]))"), "true");
  EXPECT_EQ(TextOf(R"(replace("a-b-c", "-", "+"))"), "a+b+c");
  EXPECT_EQ(TextOf(R"(slice("abcdef", 1, 3))"), "bc");
  EXPECT_EQ(TextOf(R"(slice("abcdef", -2))"), "ef");
  EXPECT_EQ(TextOf(R"(default("", "fallback"))"), "fallback");
  EXPECT_EQ(TextOf(R"(default(0, "fallback"))"), "0");
  EXPECT_EQ(TextOf(R"(seconds(duration("1m30s")))"), "90.0");
  EXPECT_EQ(TextOf(R"(strformat("%s!", "hi"))"), "hi!");
  EXPECT_FALSE(EvaluatedIn(R"(nonesuch("x"))").ok());
}

TEST(FlowValues, ItIsTheValueAStageIsLookingAt) {
  EXPECT_EQ(TextOf("it", Value::Integer(7)), "7");
  EXPECT_EQ(TextOf("it.name",
                   Value::Object({{"name", Value::String("Ada")}})),
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
      EvaluatedIn(R"({"a": 1} as some.unknown.Type)");
  ASSERT_FALSE(unknown.ok());
  EXPECT_NE(unknown.status().message().find("some.unknown.Type"),
            std::string::npos);
}

TEST(FlowValues, StatusesAreData) {
  const Value record = StatusRecord(absl::NotFoundError("gone"));
  EXPECT_EQ(AsText(record),
            R"({"code": "NOT_FOUND", "message": "gone", "number": 5,)"
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

}  // namespace
}  // namespace a11::flow
