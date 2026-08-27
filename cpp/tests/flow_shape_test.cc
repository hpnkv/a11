// Copyright 2026 The A11 Authors.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/log/absl_check.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <gtest/gtest.h>

#include "a11/flow/parser.h"
#include "a11/flow/resolve.h"
#include "a11/flow/values.h"

namespace a11::flow {
namespace {

/// A program's shapes, kept alive for as long as a test needs them.
struct Compiled {
  ParseResult parsed;
  ResolveResult resolved;

  [[nodiscard]] const DtoPlan& Shape(std::string_view name) const {
    const DtoPlan* found = resolved.program.Dto(name);
    ABSL_CHECK(found != nullptr) << "no shape " << name;
    return *found;
  }
};

Compiled Compile(std::string_view source) {
  Compiled compiled;
  compiled.parsed = Parse(source);
  compiled.resolved = Resolve(source, compiled.parsed);
  return compiled;
}

Value Object(Value::Pairs pairs) {
  return Value::Object(std::move(pairs));
}

constexpr std::string_view kShapes = R"(struct Source {
  id:    string required
  url:   string required matching "^https?://"
  kind:  string one of ["page", "paper"] default "page"
  tags:  string[] unique
  rank:  number 0..1 default 0.5
  title: string 1..10
  ttl:   duration 1s..1m
}

struct Cited {
  source: Source required
  seen:   Source[]
}
)";

TEST(FlowShape, FillsDefaultsAndKeepsDeclarationOrder) {
  const Compiled compiled = Compile(kShapes);
  const absl::StatusOr<Value> made = CoerceShape(
      compiled.Shape("Source"),
      Object({{"id", Value::String("a")}, {"url", Value::String("https://x")}}),
      {.shapes = &compiled.resolved.program});
  ASSERT_TRUE(made.ok()) << made.status();
  ASSERT_EQ(made->kind(), Value::Kind::kObject);

  // The fields the shape declares, in the order it declared them. A field that
  // is neither required nor defaulted is simply absent, which is what lets a
  // flow ask `if not thing.field`.
  std::vector<std::string> keys;
  for (const auto& [key, unused] : made->pairs()) {
    keys.push_back(key);
  }
  EXPECT_EQ(absl::StrJoin(keys, ","), "id,url,kind,rank");
  EXPECT_EQ(made->Get("kind")->text(), "page");
  EXPECT_DOUBLE_EQ(made->Get("rank")->number(), 0.5);
}

TEST(FlowShape, DropsWhatTheShapeDoesNotDeclare) {
  // Extra data is how `{...it, ..}` is useful, and a producer that sent more
  // than this reader declared has done nothing wrong. Writing such a key out by
  // hand is a different thing, and the resolver says so.
  const Compiled compiled = Compile(kShapes);
  const absl::StatusOr<Value> made =
      CoerceShape(compiled.Shape("Source"),
                  Object({{"id", Value::String("a")},
                          {"url", Value::String("https://x")},
                          {"whatever", Value::Integer(7)}}),
                  {.shapes = &compiled.resolved.program});
  ASSERT_TRUE(made.ok()) << made.status();
  EXPECT_EQ(made->Get("whatever"), nullptr);
}

TEST(FlowShape, SaysWhichFieldWasWrongAndWhy) {
  const Compiled compiled = Compile(kShapes);
  const CoerceContext context{.shapes = &compiled.resolved.program};
  const auto fails = [&](Value::Pairs pairs) {
    const absl::StatusOr<Value> made = CoerceShape(
        compiled.Shape("Source"), Object(std::move(pairs)), context);
    return made.ok() ? std::string() : std::string(made.status().message());
  };
  const Value::Pairs ok = {{"id", Value::String("a")},
                           {"url", Value::String("https://x")}};

  EXPECT_NE(fails({{"url", Value::String("https://x")}}).find("'id'"),
            std::string::npos);
  EXPECT_NE(
      fails({{"id", Value::String("a")}, {"url", Value::String("ftp://x")}})
          .find("url"),
      std::string::npos);
  EXPECT_NE(fails({{"id", Value::String("a")},
                   {"url", Value::String("https://x")},
                   {"kind", Value::String("scroll")}})
                .find("not one of"),
            std::string::npos);
  EXPECT_NE(fails({{"id", Value::String("a")},
                   {"url", Value::String("https://x")},
                   {"rank", Value::Double(2.0)}})
                .find("most allowed"),
            std::string::npos);
  EXPECT_NE(fails({{"id", Value::String("a")},
                   {"url", Value::String("https://x")},
                   {"title", Value::String("far too long to fit")}})
                .find("most allowed"),
            std::string::npos);
  EXPECT_NE(
      fails({{"id", Value::String("a")},
             {"url", Value::String("https://x")},
             {"tags", Value::List({Value::String("a"), Value::String("a")})}})
          .find("twice"),
      std::string::npos);

  // A duration bound compares by its length, however the value arrived.
  EXPECT_TRUE(fails({{"id", Value::String("a")},
                     {"url", Value::String("https://x")},
                     {"ttl", Value::String("30s")}})
                  .empty());
  EXPECT_NE(fails({{"id", Value::String("a")},
                   {"url", Value::String("https://x")},
                   {"ttl", Value::Duration(absl::Hours(1))}})
                .find("most allowed"),
            std::string::npos);
}

TEST(FlowShape, FollowsTheShapesAShapeNames) {
  const Compiled compiled = Compile(kShapes);
  const CoerceContext context{.shapes = &compiled.resolved.program};
  const Value inner =
      Object({{"id", Value::String("a")}, {"url", Value::String("https://x")}});

  const absl::StatusOr<Value> made = CoerceShape(
      compiled.Shape("Cited"),
      Object({{"source", inner}, {"seen", Value::List({inner, inner})}}),
      context);
  ASSERT_TRUE(made.ok()) << made.status();
  // The nested shape was validated too, so its defaults are there.
  EXPECT_EQ(made->Get("source")->Get("kind")->text(), "page");
  EXPECT_EQ(made->Get("seen")->items().size(), 2u);

  // And a failure inside one says where it was, by path.
  const absl::StatusOr<Value> bad = CoerceShape(
      compiled.Shape("Cited"),
      Object(
          {{"source", inner},
           {"seen",
            Value::List({inner, Object({{"id", Value::String("b")},
                                        {"url", Value::String("nope")}})})}}),
      context);
  ASSERT_FALSE(bad.ok());
  EXPECT_NE(std::string(bad.status().message()).find("seen[1]"),
            std::string::npos)
      << bad.status().message();
}

TEST(FlowShape, ARecordIsARecordAndNothingElse) {
  const Compiled compiled = Compile(kShapes);
  const CoerceContext context{.shapes = &compiled.resolved.program};
  const absl::StatusOr<Value> made = CoerceShape(
      compiled.Shape("Source"), Value::String("just a string"), context);
  ASSERT_FALSE(made.ok());
  // The message lists what the shape holds, so the author can see what was
  // wanted rather than only that something was wrong.
  EXPECT_NE(std::string(made.status().message()).find("id, url"),
            std::string::npos)
      << made.status().message();
}

TEST(FlowShape, ACastNamesAShapeBeforeItNamesATag) {
  // `Source` is this file's declared shape, and a
  // registry that also knew the name would not be asked.
  const Compiled compiled = Compile(kShapes);
  syntax::TypeExpression type;
  type.name = "Source";
  const absl::StatusOr<Value> made = Coerce(
      Object({{"id", Value::String("a")}, {"url", Value::String("https://x")}}),
      type, {.shapes = &compiled.resolved.program});
  ASSERT_TRUE(made.ok()) << made.status();
  EXPECT_EQ(made->Get("kind")->text(), "page");

  // With no shapes to consult and no host either, the message says which type
  // nothing knew.
  const absl::StatusOr<Value> nowhere = Coerce(Value::Null(), type, {});
  ASSERT_FALSE(nowhere.ok());
  EXPECT_NE(std::string(nowhere.status().message()).find("Source"),
            std::string::npos);
}

/// A host that swaps a validated record for something only it understands.
class AdoptingBridge : public HostBridge {
 public:
  absl::StatusOr<Value> Coerce(std::string_view tag,
                               const Value& value) override {
    (void)tag;
    return value;
  }

  absl::StatusOr<data::Chunk> ToChunk(const Value& value,
                                      std::string_view mimetype) override {
    (void)value;
    (void)mimetype;
    return absl::UnimplementedError("not for this test");
  }

  absl::StatusOr<Value> FromChunk(const data::Chunk& chunk) override {
    (void)chunk;
    return absl::UnimplementedError("not for this test");
  }

  absl::StatusOr<Value> Adopt(const DtoPlan& shape, const Program& program,
                              const Value& value) override {
    (void)program;
    adopted.push_back(shape.name);
    Value::Pairs pairs = value.pairs();
    pairs.emplace_back("__shape__", Value::String(shape.name));
    return Value::Object(std::move(pairs));
  }

  std::vector<std::string> adopted;
};

TEST(FlowShape, TheHostIsAskedHowItWouldRatherHoldAValidatedRecord) {
  // Validation is on this side of the boundary -- one implementation of what a
  // shape means -- and `Adopt` is only about how the host presents the result.
  // The Python bindings answer it with a pydantic model.
  const Compiled compiled = Compile(kShapes);
  AdoptingBridge bridge;
  const absl::StatusOr<Value> made = CoerceShape(
      compiled.Shape("Cited"),
      Object({{"source", Object({{"id", Value::String("a")},
                                 {"url", Value::String("https://x")}})}}),
      {.bridge = &bridge, .shapes = &compiled.resolved.program});
  ASSERT_TRUE(made.ok()) << made.status();
  // Innermost first, and once per record: the nested shape was adopted too.
  EXPECT_EQ(absl::StrJoin(bridge.adopted, ","), "Source,Cited");
  EXPECT_EQ(made->Get("__shape__")->text(), "Cited");
}

}  // namespace
}  // namespace a11::flow
