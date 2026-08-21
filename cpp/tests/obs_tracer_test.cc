// Copyright 2026 The A11 Authors.

#include <optional>
#include <string>

#include <absl/status/status.h>
#include <gtest/gtest.h>

#include "a11/data/types.h"
#include "a11/obs/provider.h"
#include "a11/obs/span.h"
#include "a11/obs/trace_context.h"
#include "a11/obs/tracer.h"

namespace a11::obs {
namespace {

const RecordedSpan* FindSpan(const std::vector<RecordedSpan>& spans,
                             std::string_view name) {
  for (const RecordedSpan& span : spans) {
    if (span.name == name) {
      return &span;
    }
  }
  return nullptr;
}

TraceContext RemoteParent() {
  TraceContext context;
  context.trace_id = "0af7651916cd43dd8448eb211c80319c";
  context.span_id = "b7ad6b7169203331";
  context.trace_flags = 0x01;
  return context;
}

class TracerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ProviderOptions options;
    options.exporter = ExporterKind::kInMemory;
    options.use_simple_processor = true;
    ASSERT_TRUE(Configure(options).ok());
    ClearRecordedSpans();
  }

  void TearDown() override { Shutdown(); }
};

TEST(TracerUnconfiguredTest, ReturnsInactiveSpans) {
  Shutdown();
  ASSERT_FALSE(IsConfigured());
  Span span = Tracer::StartSpan("noop", SpanKind::kServer, nullptr);
  EXPECT_FALSE(span.IsRecording());
  data::ByteMap headers;
  EXPECT_TRUE(span.InjectContext(headers).ok());
  EXPECT_TRUE(headers.empty());
}

TEST_F(TracerTest, ServerSpanContinuesRemoteTrace) {
  const TraceContext parent = RemoteParent();
  {
    Span span = Tracer::StartSpan("handle", SpanKind::kServer, &parent);
    EXPECT_TRUE(span.IsRecording());
    span.SetAttribute("a11.action.name", "greet");
    span.AddEvent("action.call", {{"a11.action.target", "child"}});
    span.SetStatus(absl::OkStatus());
  }
  const auto spans = GetRecordedSpans();
  const RecordedSpan* handle = FindSpan(spans, "handle");
  ASSERT_NE(handle, nullptr);
  EXPECT_EQ(handle->trace_id, parent.trace_id);
  EXPECT_EQ(handle->parent_span_id, parent.span_id);
  EXPECT_EQ(handle->kind, SpanKind::kServer);
  EXPECT_EQ(handle->status_code, 1);
  ASSERT_EQ(handle->events.size(), 1u);
  EXPECT_EQ(handle->events[0].name, "action.call");
}

TEST_F(TracerTest, ChildSpanSharesTraceAndParents) {
  const TraceContext parent = RemoteParent();
  std::string parent_span_id;
  {
    Span root = Tracer::StartSpan("root", SpanKind::kServer, &parent);
    {
      Span child = Tracer::StartChildSpan("child", SpanKind::kInternal, root);
      EXPECT_TRUE(child.IsRecording());
    }
  }
  const auto spans = GetRecordedSpans();
  const RecordedSpan* root = FindSpan(spans, "root");
  const RecordedSpan* child = FindSpan(spans, "child");
  ASSERT_NE(root, nullptr);
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(child->trace_id, root->trace_id);
  EXPECT_EQ(child->parent_span_id, root->span_id);
}

TEST_F(TracerTest, ErrorStatusIsRecorded) {
  {
    Span span = Tracer::StartSpan("fails", SpanKind::kInternal, nullptr);
    span.SetStatus(absl::InternalError("boom"));
  }
  const auto spans = GetRecordedSpans();
  const RecordedSpan* fails = FindSpan(spans, "fails");
  ASSERT_NE(fails, nullptr);
  EXPECT_EQ(fails->status_code, 2);
  EXPECT_EQ(fails->status_description, "boom");
}

TEST_F(TracerTest, PropagatesBaggageToHeaders) {
  TraceContext parent = RemoteParent();
  parent.baggage.push_back({"user", "alice", ""});
  data::ByteMap headers;
  {
    Span span = Tracer::StartSpan("handle", SpanKind::kServer, &parent);
    ASSERT_TRUE(span.InjectContext(headers).ok());
  }
  ASSERT_TRUE(headers.contains(std::string(kBaggageHeader)));
  EXPECT_EQ(headers[std::string(kBaggageHeader)], "user=alice");
  // The injected traceparent continues the same trace as the parent.
  const auto parsed = ExtractTraceContext(headers);
  ASSERT_TRUE(parsed.ok());
  ASSERT_TRUE(parsed->has_value());
  EXPECT_EQ((*parsed)->trace_id, parent.trace_id);
}

TEST(TracerBaggagePromotionTest, ConfiguredBaggageKeysBecomeAttributes) {
  ProviderOptions options;
  options.exporter = ExporterKind::kInMemory;
  options.use_simple_processor = true;
  options.baggage_span_attributes = {"langfuse.session.id"};
  ASSERT_TRUE(Configure(options).ok());
  ClearRecordedSpans();

  TraceContext parent;
  parent.trace_id = "0af7651916cd43dd8448eb211c80319c";
  parent.span_id = "b7ad6b7169203331";
  parent.trace_flags = 0x01;
  parent.baggage.push_back({"langfuse.session.id", "sess-42", ""});
  parent.baggage.push_back({"unpromoted", "x", ""});
  {
    Span span = Tracer::StartSpan("handle", SpanKind::kServer, &parent);
    // A child inherits the baggage and also gets the promoted attribute.
    Span child = Tracer::StartChildSpan("child", SpanKind::kInternal, span);
  }

  const auto spans = GetRecordedSpans();
  for (std::string_view name : {"handle", "child"}) {
    const RecordedSpan* span = FindSpan(spans, name);
    ASSERT_NE(span, nullptr) << name;
    bool has_session = false;
    bool has_unpromoted = false;
    for (const auto& [key, value] : span->attributes) {
      if (key == "langfuse.session.id") {
        has_session = true;
        EXPECT_EQ(value, "sess-42");
      }
      if (key == "unpromoted") {
        has_unpromoted = true;
      }
    }
    EXPECT_TRUE(has_session) << name;
    EXPECT_FALSE(has_unpromoted) << name;
  }
  Shutdown();
}

TEST_F(TracerTest, RootSpanPinsPreassignedTraceId) {
  const std::string preassigned = "1234567890abcdef1234567890abcdef";
  {
    Span span =
        Tracer::StartRootSpan("session", SpanKind::kServer, preassigned);
    EXPECT_TRUE(span.IsRecording());
  }
  const auto spans = GetRecordedSpans();
  const RecordedSpan* session = FindSpan(spans, "session");
  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->trace_id, preassigned);
}

}  // namespace
}  // namespace a11::obs
