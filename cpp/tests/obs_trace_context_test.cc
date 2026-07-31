// Copyright 2026 The A11 Authors.

#include "a11/obs/trace_context.h"

#include <optional>
#include <string>

#include <absl/status/status.h>
#include <gtest/gtest.h>

#include "a11/data/types.h"

namespace a11::obs {
namespace {

data::ByteMap Headers(
    std::initializer_list<std::pair<std::string, std::string>> entries) {
  data::ByteMap headers;
  for (const auto& [name, value] : entries) {
    headers[name] = value;
  }
  return headers;
}

TEST(TraceContextTest, AbsentHeadersYieldNoContext) {
  const auto result = ExtractTraceContext(data::ByteMap{});
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(result->has_value());
}

TEST(TraceContextTest, ParsesValidTraceparent) {
  const auto result = ExtractTraceContext(Headers(
      {{std::string(kTraceparentHeader),
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01"}}));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(result->has_value());
  EXPECT_EQ((*result)->trace_id, "0af7651916cd43dd8448eb211c80319c");
  EXPECT_EQ((*result)->span_id, "b7ad6b7169203331");
  EXPECT_EQ((*result)->trace_flags, 0x01);
  EXPECT_TRUE((*result)->sampled());
}

TEST(TraceContextTest, RejectsMalformedTraceparent) {
  for (const std::string bad : {
           "not-a-traceparent",
           "00-short-b7ad6b7169203331-01",
           "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331",
           "ff-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01",
           "00-00000000000000000000000000000000-b7ad6b7169203331-01",
           "00-0af7651916cd43dd8448eb211c80319c-0000000000000000-01",
           "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-0z",
       }) {
    const auto result =
        ExtractTraceContext(Headers({{std::string(kTraceparentHeader), bad}}));
    EXPECT_FALSE(result.ok()) << "expected rejection for: " << bad;
  }
}

TEST(TraceContextTest, RejectsTracestateWithoutTraceparent) {
  const auto result = ExtractTraceContext(
      Headers({{std::string(kTracestateHeader), "vendor=value"}}));
  EXPECT_FALSE(result.ok());
}

TEST(TraceContextTest, RejectsBaggageWithoutTraceparent) {
  const auto result = ExtractTraceContext(
      Headers({{std::string(kBaggageHeader), "user=alice"}}));
  EXPECT_FALSE(result.ok());
}

TEST(TraceContextTest, ParsesTracestateAndBaggage) {
  const auto result = ExtractTraceContext(Headers({
      {std::string(kTraceparentHeader),
       "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01"},
      {std::string(kTracestateHeader), "vendor=value"},
      {std::string(kBaggageHeader), "user=alice,region=eu%20west"},
  }));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(result->has_value());
  EXPECT_EQ((*result)->tracestate, "vendor=value");
  ASSERT_EQ((*result)->baggage.size(), 2u);
  EXPECT_EQ((*result)->baggage[0].key, "user");
  EXPECT_EQ((*result)->baggage[0].value, "alice");
  EXPECT_EQ((*result)->baggage[1].key, "region");
  EXPECT_EQ((*result)->baggage[1].value, "eu west");
}

TEST(TraceContextTest, InjectRoundTrips) {
  TraceContext context;
  context.trace_id = "0af7651916cd43dd8448eb211c80319c";
  context.span_id = "b7ad6b7169203331";
  context.trace_flags = 0x01;
  context.tracestate = "vendor=value";
  context.baggage.push_back({"user", "alice bob", ""});

  data::ByteMap headers;
  ASSERT_TRUE(InjectTraceContext(context, headers).ok());

  const auto parsed = ExtractTraceContext(headers);
  ASSERT_TRUE(parsed.ok());
  ASSERT_TRUE(parsed->has_value());
  EXPECT_EQ((*parsed)->trace_id, context.trace_id);
  EXPECT_EQ((*parsed)->span_id, context.span_id);
  EXPECT_EQ((*parsed)->trace_flags, context.trace_flags);
  EXPECT_EQ((*parsed)->tracestate, context.tracestate);
  ASSERT_EQ((*parsed)->baggage.size(), 1u);
  EXPECT_EQ((*parsed)->baggage[0].key, "user");
  EXPECT_EQ((*parsed)->baggage[0].value, "alice bob");
}

}  // namespace
}  // namespace a11::obs
