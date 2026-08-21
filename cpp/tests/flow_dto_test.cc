// Copyright 2026 The A11 Authors.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <absl/strings/str_join.h>
#include <gtest/gtest.h>

#include "a11/flow/complete.h"
#include "a11/flow/format.h"
#include "a11/flow/inspect.h"
#include "a11/flow/parser.h"
#include "a11/flow/resolve.h"

namespace a11::flow {
namespace {

ResolveResult Check(std::string_view source) {
  return Resolve(source, Parse(source));
}

std::vector<std::string> Codes(const std::vector<Diagnostic>& diagnostics) {
  std::vector<std::string> codes;
  codes.reserve(diagnostics.size());
  for (const Diagnostic& diagnostic : diagnostics) {
    codes.push_back(diagnostic.code);
  }
  return codes;
}

/// Whether a code is among them. A plain predicate rather than a matcher: these
/// tests link gtest and not gmock, as every other flow test does.
bool Has(const std::vector<std::string>& codes, std::string_view code) {
  return std::find(codes.begin(), codes.end(), code) != codes.end();
}

std::string Joined(const std::vector<Diagnostic>& diagnostics) {
  std::vector<std::string> messages;
  messages.reserve(diagnostics.size());
  for (const Diagnostic& diagnostic : diagnostics) {
    messages.push_back(diagnostic.message);
  }
  return absl::StrJoin(messages, "; ");
}

/// Everything wrong with a document, both passes, as a frontend sees it.
std::vector<Diagnostic> Problems(std::string_view source) {
  const ParseResult parsed = Parse(source);
  ResolveResult resolved = Resolve(source, parsed);
  for (Diagnostic& found : Inspect(source, parsed, resolved)) {
    resolved.diagnostics.push_back(std::move(found));
  }
  return std::move(resolved.diagnostics);
}

constexpr std::string_view kSource = R"(struct Source {
  describe "Where an answer came from."

  url:    string required matching "^https?://" "The page."
  title:  string 1..200
  tags:   string[] unique
  rank:   number 0..1 default 0.5
  kind:   string one of ["page", "paper"] default "page"
  seen:   time
  ttl:    duration default 30s
  parent: Source
}

flow f {
  in  hits:    json stream required
  out sources: Source stream

  hits | map Source{"url": "https://x"} -> sources
}
)";

TEST(FlowDto, ResolvesIntoAShapeAPortMayCarry) {
  const ResolveResult result = Check(kSource);
  EXPECT_TRUE(Joined(result.diagnostics).empty()) << Joined(result.diagnostics);

  ASSERT_EQ(result.program.dtos.size(), 1u);
  const DtoPlan& shape = result.program.dtos.front();
  EXPECT_EQ(shape.name, "Source");
  EXPECT_EQ(shape.description, "Where an answer came from.");
  EXPECT_EQ(shape.fields.size(), 8u);
  EXPECT_FALSE(shape.binary);

  // The order fields were written in is the order they are kept in: a shape's
  // fields have one, and a reader of the plan relies on it.
  EXPECT_EQ(absl::StrJoin(shape.FieldNames(), ","),
            "url,title,tags,rank,kind,seen,ttl,parent");

  const FieldPlan* url = shape.Field("url");
  ASSERT_NE(url, nullptr);
  EXPECT_TRUE(url->required);
  EXPECT_EQ(url->type, "string");
  EXPECT_EQ(url->pattern, "^https?://");
  EXPECT_EQ(url->description, "The page.");

  // `T[]` is `list[T]`: one type, two spellings, and only the formatter cares
  // which was written.
  const FieldPlan* tags = shape.Field("tags");
  ASSERT_NE(tags, nullptr);
  EXPECT_EQ(tags->type, "list");
  EXPECT_EQ(tags->element, "string");
  EXPECT_EQ(tags->declared, "string[]");
  EXPECT_TRUE(tags->unique);

  // A length range and a value range are written the same way and mean what
  // the field's type says they mean.
  const FieldPlan* title = shape.Field("title");
  ASSERT_NE(title, nullptr);
  EXPECT_TRUE(title->range.has_minimum);
  EXPECT_EQ(title->range.minimum.integer, 1);
  EXPECT_EQ(title->range.maximum.integer, 200);

  const FieldPlan* rank = shape.Field("rank");
  ASSERT_NE(rank, nullptr);
  EXPECT_TRUE(rank->has_default);
  EXPECT_DOUBLE_EQ(rank->default_value.AsDouble(), 0.5);

  const FieldPlan* kind = shape.Field("kind");
  ASSERT_NE(kind, nullptr);
  ASSERT_EQ(kind->enumeration.size(), 2u);
  EXPECT_EQ(kind->enumeration[0].text, "page");

  // A shape may name itself, and the port carries the shape by name.
  EXPECT_EQ(shape.Field("parent")->dto_name, "Source");
  const PortPlan* port =
      result.program.Flow("f")->Port("sources", syntax::PortDirection::kOutput);
  ASSERT_NE(port, nullptr);
  EXPECT_EQ(port->type, "Source");
}

TEST(FlowDto, ABareRangeMayBeOpenAtEitherEnd) {
  const ResolveResult result = Check(
      "struct D {\n  a: string 1..\n  b: string ..9\n}\nflow f { in x: D }\n");
  EXPECT_TRUE(Joined(result.diagnostics).empty()) << Joined(result.diagnostics);
  const DtoPlan& shape = result.program.dtos.front();
  EXPECT_TRUE(shape.Field("a")->range.has_minimum);
  EXPECT_FALSE(shape.Field("a")->range.has_maximum);
  EXPECT_FALSE(shape.Field("b")->range.has_minimum);
  EXPECT_TRUE(shape.Field("b")->range.has_maximum);
}

TEST(FlowDto, AShapeOutranksARegistryTagOfTheSameName) {
  // A dotted name is a registry tag and nothing here can check it; an undotted
  // one the file declares is *this* shape, whatever a registry may also know.
  // That precedence is the point of being able to declare one.
  const ResolveResult result = Check(
      "struct AudioBuffer {\n  rate: integer required\n}\n"
      "flow f {\n  in a: AudioBuffer required\n  out b: a11.sdk.AudioBuffer\n"
      "  a -> b\n}\n");
  EXPECT_TRUE(Joined(result.diagnostics).empty()) << Joined(result.diagnostics);
  const FlowPlan* flow = result.program.Flow("f");
  EXPECT_EQ(flow->Port("a", syntax::PortDirection::kInput)->type,
            "AudioBuffer");
  EXPECT_EQ(flow->Port("b", syntax::PortDirection::kOutput)->type,
            "a11.sdk.AudioBuffer");
}

TEST(FlowDto, AShapeMayNotBeNamedAfterABuiltInType) {
  const ResolveResult result =
      Check("struct string {\n  a: string\n}\nflow f { in x: string }\n");
  EXPECT_TRUE(
      Has(Codes(result.diagnostics), "flow.form.struct-shadows-builtin"));
  // And the built-in still means what it always did.
  EXPECT_EQ(
      result.program.Flow("f")->Port("x", syntax::PortDirection::kInput)->type,
      "string");
}

TEST(FlowDto, SaysWhichConstraintsCannotApply) {
  struct Case {
    std::string_view field;
    std::string_view code;
  };

  const Case cases[] = {
      {"a: string unique", "flow.form.field-constraint"},
      {"a: number matching \"x\"", "flow.form.field-constraint"},
      {"a: bool 1..2", "flow.form.field-constraint"},
      {"a: number 9..1", "flow.form.empty-range"},
      {"a: string -3..", "flow.form.field-constraint"},
      {"a: bool default \"yes\"", "flow.form.field-type-mismatch"},
      {"a: number one of [\"x\"]", "flow.form.field-type-mismatch"},
      {"a: string required default \"x\"", "flow.form.default-on-required"},
      {"a: string one of []", "flow.form.one-of-empty"},
      {"a: string one of \"x\"", "flow.form.one-of-not-a-list"},
  };
  for (const Case& one : cases) {
    const std::string source =
        absl::StrCat("struct D {\n  ", one.field, "\n}\nflow f { in x: D }\n");
    EXPECT_TRUE(Has(Codes(Check(source).diagnostics), one.code))
        << one.field << " gave " << Joined(Check(source).diagnostics);
  }
}

TEST(FlowDto, AShapeThatHoldsBytesCannotGoThroughJson) {
  // JSON has nothing to carry a byte string in, so this is not a value that
  // renders oddly -- it is one that cannot be rendered. The rule follows the
  // shapes a shape names, and it survives the stages that only *choose* values.
  const std::string source =
      "struct Blob {\n  body: bytes required\n}\n"
      "struct Wrapper {\n  blob: Blob required\n}\n"
      "struct Plain {\n  name: string required\n}\n"
      "flow f {\n"
      "  in  blobs:  Blob stream required\n"
      "  in  wraps:  Wrapper stream required\n"
      "  in  plains: Plain stream required\n"
      "  out a: string stream\n  out b: string stream\n  out c: string stream\n"
      "  blobs  | json -> a\n"
      "  wraps  | first 2 | json -> b\n"
      "  plains | json -> c\n"
      "}\n";
  const ResolveResult result = Check(source);
  const std::vector<std::string> codes = Codes(result.diagnostics);
  // Twice: the shape that holds them, and the shape that holds that one.
  EXPECT_EQ(std::count(codes.begin(), codes.end(),
                       "flow.form.not-json-representable"),
            2);
  // `packb` is what can, and the message says so.
  EXPECT_NE(Joined(result.diagnostics).find("packb"), std::string::npos);
}

TEST(FlowDto, ChecksAShapeLiteralAgainstTheShape) {
  const std::string source =
      "struct S {\n  id: string required\n  rank: number\n}\n"
      "flow f {\n  in x: string stream required\n  out y: S stream\n"
      "  x | map S{\"id\": it, \"nope\": 1} -> y\n"
      "  x | map S{\"rank\": 1} -> y\n"
      "  x | map S{\"id\": it, \"rank\": \"high\"} -> y\n"
      "}\n";
  const std::vector<std::string> codes = Codes(Check(source).diagnostics);
  EXPECT_TRUE(Has(codes, "flow.form.unknown-field"));
  EXPECT_TRUE(Has(codes, "flow.form.missing-field"));
  EXPECT_TRUE(Has(codes, "flow.form.field-type-mismatch"));
}

TEST(FlowDto, ASpreadStandsDownTheMissingFieldCheck) {
  // With a spread the set of keys is a run-time fact. Guessing at it here would
  // report a flow that is perfectly correct; the coercion catches it later, in
  // the same words, if it turns out to be wrong.
  const ResolveResult result = Check(
      "struct S {\n  id: string required\n  tags: string[]\n}\n"
      "flow f {\n  in x: json stream required\n  out y: S stream\n"
      "  x | map S{...it, \"tags\": [\"seen\"]} -> y\n}\n");
  EXPECT_TRUE(Joined(result.diagnostics).empty()) << Joined(result.diagnostics);
}

TEST(FlowDto, SaysWhenNothingNamesAShape) {
  const std::vector<std::string> codes = Codes(Problems(
      "struct Used {\n  a: string required\n}\n"
      "struct Spare {\n  b: string required\n}\n"
      "flow f {\n  in x: Used required\n  out y: string\n  x.a -> y\n}\n"));
  EXPECT_TRUE(Has(codes, "flow.unused.struct"));
  EXPECT_EQ(std::count(codes.begin(), codes.end(), "flow.unused.struct"), 1);
}

TEST(FlowDto, AFileOfShapesAloneIsAFileOfTypes) {
  // No flow at all is not an error: a file of shapes is what a JSONSchema turns
  // into, and there is nothing in it for anything to be unused *by*.
  EXPECT_TRUE(Problems("struct D {\n  a: string required\n}\n").empty());
}

TEST(FlowDto, FieldsLineUpTheWayPortsDo) {
  const std::string formatted = Format(
                                    "struct D {\n  describe \"d\"\n"
                                    "  id: string required \"the id\"\n"
                                    "  longer: number 0..1 default 0.5\n"
                                    "  s: string one of [\"a\",\"b\"]\n}\n")
                                    .formatted;
  // `id` is also a call modifier; inside a shape it is a field name, and a
  // formatter that read it as a modifier would indent it as a continuation.
  EXPECT_NE(formatted.find("  id:     string required"), std::string::npos)
      << formatted;
  EXPECT_NE(formatted.find("  longer: number 0..1 default 0.5"),
            std::string::npos)
      << formatted;
  // A list after `one of` holds values, not a type's parameters, so it takes
  // its space.
  EXPECT_NE(formatted.find("one of [\"a\", \"b\"]"), std::string::npos)
      << formatted;
  // Formatting twice changes nothing.
  EXPECT_EQ(Format(formatted).formatted, formatted);
}

TEST(FlowDto, OffersTheShapesItsFileDeclaresWhereATypeGoes) {
  const std::string source =
      "struct Source {\n  id: string required\n  url: string\n}\n"
      "flow f {\n  in x: \n}\n";
  const CompleteResult result = CompleteAt(source, source.find("in x: ") + 6);
  std::vector<std::string> names;
  names.reserve(result.proposals.size());
  for (const Proposal& proposal : result.proposals) {
    names.push_back(proposal.name);
  }
  ASSERT_FALSE(names.empty());
  // The file's own shapes first: they are what somebody writing here most
  // likely means, and a built-in is one word away anyway.
  EXPECT_EQ(names.front(), "Source");
  EXPECT_TRUE(Has(names, "string"));
}

TEST(FlowDto, OffersAShapesFieldsInsideItsValue) {
  const std::string source =
      "struct Source {\n  id: string required\n  url: string\n}\n"
      "flow f {\n  in x: string stream required\n  out y: Source stream\n"
      "  x | map Source{} -> y\n}\n";
  const CompleteResult result = CompleteAt(source, source.find("Source{}") + 7);
  std::vector<std::string> names;
  for (const Proposal& proposal : result.proposals) {
    EXPECT_EQ(proposal.kind, ProposalKind::kField);
    names.push_back(proposal.name);
    // A key is quoted and takes the colon that has to follow it.
    if (proposal.name == "id") {
      EXPECT_EQ(proposal.insert, "\"id\": ");
    }
  }
  EXPECT_EQ(absl::StrJoin(names, ","), "id,url");
}

TEST(FlowDto, OffersWhatAFieldMaySayAboutItself) {
  const std::string source = "struct D {\n  a: string \n}\n";
  const CompleteResult result =
      CompleteAt(source, source.find("a: string ") + 10);
  std::vector<std::string> names;
  names.reserve(result.proposals.size());
  for (const Proposal& proposal : result.proposals) {
    names.push_back(proposal.name);
  }
  EXPECT_EQ(absl::StrJoin(names, ","),
            "required,unique,matching,one of,default");
}

// --- one value ---------------------------------------------------------------

TEST(FlowLet, BindsAValueThatStandsWhereAnExpressionDoes) {
  const ResolveResult result = Check(
      "flow f {\n  in codes: number stream required\n  out ok: bool\n"
      "  let code = codes\n  code >= 200 and code < 300 -> ok\n}\n");
  EXPECT_TRUE(Joined(result.diagnostics).empty()) << Joined(result.diagnostics);
  ASSERT_EQ(result.flows.size(), 1u);
  const Symbol* value = nullptr;
  for (const Symbol& symbol : result.flows[0].symbols) {
    if (symbol.name == "code") {
      value = &symbol;
    }
  }
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(value->kind, SymbolKind::kValue);
  EXPECT_TRUE(value->readable);
  // A value is what a stream *was*, not somewhere to put one.
  EXPECT_FALSE(value->writable);
  EXPECT_EQ(value->reads, 2);
}

TEST(FlowLet, AValueIsReadNeverWritten) {
  EXPECT_TRUE(Has(Codes(Check("flow f {\n  in a: string required\n"
                              "  let x = a\n  \"y\" -> x\n}\n")
                            .diagnostics),
                  "flow.name.not-writable"));
}

TEST(FlowLet, TheNameIsTakenLikeAnyOther) {
  EXPECT_TRUE(Has(Codes(Check("flow f {\n  in a: string required\n"
                              "  let a = a\n  out b: string\n}\n")
                            .diagnostics),
                  "flow.name.taken"));
}

TEST(FlowLet, SaysSoWhenNothingReadsOne) {
  // A `let` is lazy, so one nobody reads leaves the stream behind it unread --
  // which is a stalled producer, not merely a dead name.
  EXPECT_TRUE(Has(Codes(Problems("flow f {\n  in a: string stream required\n"
                                 "  out b: string\n  let unread = a\n"
                                 "  \"x\" -> b\n}\n")),
                  "flow.unused.value"));
}

TEST(FlowLet, IsAWordOnlyWhereItOpensAStatement) {
  // No reserved words: a port called `let` is a name like any other, and the
  // rule is the one every other statement word follows.
  const ResolveResult result = Check(
      "flow f {\n  in let: string required\n  out b: string\n  let -> b\n}\n");
  EXPECT_TRUE(Joined(result.diagnostics).empty()) << Joined(result.diagnostics);
}

}  // namespace
}  // namespace a11::flow
