// Copyright 2026 The A11 Authors.

#include <memory>
#include <string>

#include <absl/status/status.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/actions/action.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/obs/provider.h"
#include "a11/obs/trace_context.h"

namespace a11::actions {
namespace {

constexpr char kTraceId[] = "0af7651916cd43dd8448eb211c80319c";
constexpr char kSpanId[] = "b7ad6b7169203331";
constexpr char kTraceparent[] =
    "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";

ActionSchema Schema(std::string name) {
  return ActionSchema{.name = std::move(name)};
}

ActionHandler OkHandler() {
  return [](const std::shared_ptr<Action>&) {
    return a11::SubmitTask([]() -> absl::Status { return absl::OkStatus(); });
  };
}

ActionHandler FailHandler() {
  return [](const std::shared_ptr<Action>&) {
    return a11::SubmitTask(
        []() -> absl::Status { return absl::DataLossError("boom"); });
  };
}

// Parent handler that opens a nested action and runs it to completion.
ActionHandler ParentRunsChildHandler() {
  return [](std::shared_ptr<Action> action) {
    return a11::SubmitTask([action = std::move(action)]() -> absl::Status {
      auto child = action->MakeNested(Schema("child"));
      if (!child.ok()) {
        return child.status();
      }
      if (absl::Status bound = (*child)->BindHandler(OkHandler());
          !bound.ok()) {
        return bound;
      }
      auto ran = (*child)->Run();
      if (!ran.ok()) {
        return ran.status();
      }
      return (*child)->Wait(absl::Seconds(5)).Await().status();
    });
  };
}

const obs::RecordedSpan* Find(const std::vector<obs::RecordedSpan>& spans,
                              std::string_view name) {
  for (const obs::RecordedSpan& span : spans) {
    if (span.name == name) {
      return &span;
    }
  }
  return nullptr;
}

class ActionSpanTest : public ::testing::Test {
 protected:
  void SetUp() override {
    obs::ProviderOptions options;
    options.exporter = obs::ExporterKind::kInMemory;
    options.use_simple_processor = true;
    ASSERT_TRUE(obs::Configure(options).ok());
    obs::ClearRecordedSpans();
  }

  void TearDown() override { obs::Shutdown(); }
};

TEST_F(ActionSpanTest, RunWithTraceparentEmitsServerSpan) {
  auto action = *Action::Create(Schema("greet"), "greet", OkHandler());
  ASSERT_TRUE(
      action->SetHeader(std::string(obs::kTraceparentHeader), kTraceparent)
          .ok());
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(absl::Seconds(5)).Await().ok());

  const auto spans = obs::GetRecordedSpans();
  const obs::RecordedSpan* span = Find(spans, "greet");
  ASSERT_NE(span, nullptr);
  EXPECT_EQ(span->trace_id, kTraceId);
  EXPECT_EQ(span->parent_span_id, kSpanId);
  EXPECT_EQ(span->kind, obs::SpanKind::kServer);
  EXPECT_EQ(span->status_code, 1);
}

TEST_F(ActionSpanTest, RunWithoutTraceparentEmitsNoTelemetry) {
  auto action = *Action::Create(Schema("silent"), "silent", OkHandler());
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(absl::Seconds(5)).Await().ok());

  const auto spans = obs::GetRecordedSpans();
  EXPECT_EQ(Find(spans, "silent"), nullptr);
}

TEST_F(ActionSpanTest, InconsistentTraceContextFailsTheAction) {
  auto action = *Action::Create(Schema("bad"), "bad", OkHandler());
  ASSERT_TRUE(
      action->SetHeader(std::string(obs::kTraceparentHeader), "not-valid")
          .ok());
  EXPECT_FALSE(action->Run().ok());
}

TEST_F(ActionSpanTest, FailingHandlerRecordsErrorStatus) {
  auto action = *Action::Create(Schema("fails"), "fails", FailHandler());
  ASSERT_TRUE(
      action->SetHeader(std::string(obs::kTraceparentHeader), kTraceparent)
          .ok());
  ASSERT_TRUE(action->Run().ok());
  EXPECT_FALSE(action->Wait(absl::Seconds(5)).Await().ok());

  const auto spans = obs::GetRecordedSpans();
  const obs::RecordedSpan* span = Find(spans, "fails");
  ASSERT_NE(span, nullptr);
  EXPECT_EQ(span->status_code, 2);
  EXPECT_EQ(span->status_description, "boom");
  // FailHandler returns absl::DataLossError -> canonical upper-case code.
  bool has_error_type = false;
  for (const auto& [key, value] : span->attributes) {
    if (key == "error.type") {
      has_error_type = true;
      EXPECT_EQ(value, "DATA_LOSS");
    }
  }
  EXPECT_TRUE(has_error_type);
}

TEST_F(ActionSpanTest, ExposesOwnTraceAndSpanIds) {
  std::string handler_trace_id;
  std::string handler_span_id;
  ActionHandler capture = [&](const std::shared_ptr<Action>& action) {
    return a11::SubmitTask([&, action]() -> absl::Status {
      handler_trace_id = action->TraceId();
      handler_span_id = action->SpanId();
      return absl::OkStatus();
    });
  };
  auto action = *Action::Create(Schema("ids"), "ids", capture);
  ASSERT_TRUE(
      action->SetHeader(std::string(obs::kTraceparentHeader), kTraceparent)
          .ok());
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(absl::Seconds(5)).Await().ok());

  EXPECT_EQ(handler_trace_id, kTraceId);
  EXPECT_EQ(handler_span_id.size(), 16u);
  EXPECT_NE(handler_span_id, "0000000000000000");

  // The id observed inside the handler matches the exported span.
  const auto spans = obs::GetRecordedSpans();
  const obs::RecordedSpan* span = Find(spans, "ids");
  ASSERT_NE(span, nullptr);
  EXPECT_EQ(span->span_id, handler_span_id);
  EXPECT_EQ(span->trace_id, handler_trace_id);
}

TEST_F(ActionSpanTest, UserSetSpanStatusIsNotOverridden) {
  ActionHandler mark_error = [](const std::shared_ptr<Action>& action) {
    return a11::SubmitTask([action]() -> absl::Status {
      action->SetSpanStatus(obs::SpanStatus::kError, "boom");
      return absl::OkStatus();  // action itself succeeds
    });
  };
  auto action = *Action::Create(Schema("status"), "status", mark_error);
  ASSERT_TRUE(
      action->SetHeader(std::string(obs::kTraceparentHeader), kTraceparent)
          .ok());
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(absl::Seconds(5)).Await().ok());

  const auto spans = obs::GetRecordedSpans();
  const obs::RecordedSpan* span = Find(spans, "status");
  ASSERT_NE(span, nullptr);
  EXPECT_EQ(span->status_code, 2);  // error, not the auto-ok
  EXPECT_EQ(span->status_description, "boom");
}

TEST_F(ActionSpanTest, UntracedActionHasEmptyIds) {
  std::string trace_id = "unset";
  ActionHandler capture = [&](const std::shared_ptr<Action>& action) {
    return a11::SubmitTask([&, action]() -> absl::Status {
      trace_id = action->TraceId();
      return absl::OkStatus();
    });
  };
  auto action = *Action::Create(Schema("untraced"), "untraced", capture);
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(absl::Seconds(5)).Await().ok());
  EXPECT_TRUE(trace_id.empty());
}

TEST_F(ActionSpanTest, NestedActionGetsChildSpanInSameTrace) {
  auto action =
      *Action::Create(Schema("parent"), "parent", ParentRunsChildHandler());
  ASSERT_TRUE(
      action->SetHeader(std::string(obs::kTraceparentHeader), kTraceparent)
          .ok());
  ASSERT_TRUE(action->Run().ok());
  ASSERT_TRUE(action->Wait(absl::Seconds(5)).Await().ok());

  const auto spans = obs::GetRecordedSpans();
  const obs::RecordedSpan* parent = Find(spans, "parent");
  const obs::RecordedSpan* child = Find(spans, "child");
  ASSERT_NE(parent, nullptr);
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(child->trace_id, parent->trace_id);
  EXPECT_EQ(child->trace_id, kTraceId);
  EXPECT_EQ(child->parent_span_id, parent->span_id);
  EXPECT_EQ(child->kind, obs::SpanKind::kInternal);
}

}  // namespace
}  // namespace a11::actions
