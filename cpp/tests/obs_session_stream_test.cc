// Copyright 2026 The A11 Authors.

#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include <absl/status/status.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/in_process_wire_stream.h"
#include "a11/obs/provider.h"
#include "a11/service/session.h"
#include "thread/boost_primitives.h"

namespace a11::obs {
namespace {

constexpr char kSessionTrace[] = "0af7651916cd43dd8448eb211c80319c";
constexpr char kStreamTrace[] = "1234567890abcdef1234567890abcdef";

const RecordedSpan* Find(const std::vector<RecordedSpan>& spans,
                         std::string_view name) {
  for (const RecordedSpan& span : spans) {
    if (span.name == name) {
      return &span;
    }
  }
  return nullptr;
}

const RecordedEvent* FindEvent(const RecordedSpan& span,
                               std::string_view name) {
  for (const RecordedEvent& event : span.events) {
    if (event.name == name) {
      return &event;
    }
  }
  return nullptr;
}

std::string Attr(const RecordedEvent& event, std::string_view key) {
  for (const auto& [k, v] : event.attributes) {
    if (k == key) {
      return v;
    }
  }
  return "";
}

class SessionStreamSpanTest : public ::testing::Test {
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

TEST_F(SessionStreamSpanTest, SessionSpanUsesSessionIdAsTrace) {
  service::SessionOptions options;
  options.no_stream_timeout = absl::InfiniteDuration();
  auto session = *service::Session::Create(kSessionTrace, {}, {}, {}, options);
  ASSERT_TRUE(session->HalfClose().ok());
  ASSERT_TRUE(session->Done().Await(absl::Now() + absl::Seconds(5)).ok());

  const auto spans = GetRecordedSpans();
  const RecordedSpan* span = Find(spans, "a11.session");
  ASSERT_NE(span, nullptr);
  EXPECT_EQ(span->trace_id, kSessionTrace);
  EXPECT_EQ(span->kind, SpanKind::kServer);
}

TEST_F(SessionStreamSpanTest, StreamSpansSharePreassignedTraceAndRecordSends) {
  auto pair = net::InProcessWireStream::CreatePair(std::nullopt, std::nullopt,
                                                   std::nullopt, kStreamTrace);
  ASSERT_TRUE(pair.ok()) << pair.status();
  auto [first, second] = *pair;

  std::atomic<bool> first_done = false;
  std::atomic<bool> second_done = false;
  ASSERT_TRUE(first
                  ->Start(
                      [](const std::optional<data::WireMessage>&) {
                        return a11::ReadyTask();
                      },
                      [&first_done]() {
                        first_done = true;
                        return a11::ReadyTask();
                      })
                  .Await()
                  .ok());
  ASSERT_TRUE(second
                  ->Accept(
                      [](const std::optional<data::WireMessage>&) {
                        return a11::ReadyTask();
                      },
                      [&second_done]() {
                        second_done = true;
                        return a11::ReadyTask();
                      })
                  .Await()
                  .ok());

  data::WireMessage message{.node_fragments = {{
                                .id = "node",
                                .data = data::Chunk{.data = "payload"},
                                .seq = 0,
                                .continued = false,
                            }}};
  ASSERT_TRUE(first->Send(message).ok());
  ASSERT_TRUE(first->HalfClose().ok());
  ASSERT_TRUE(second->HalfClose().ok());

  const absl::Time limit = absl::Now() + absl::Seconds(5);
  while ((!first_done || !second_done) && absl::Now() < limit) {
    thread::SleepFor(absl::Milliseconds(1));
  }
  ASSERT_TRUE(first_done);
  ASSERT_TRUE(second_done);

  const auto spans = GetRecordedSpans();
  int stream_spans = 0;
  const RecordedEvent* send_event = nullptr;
  for (const RecordedSpan& span : spans) {
    if (span.name != "a11.wire_stream") {
      continue;
    }
    ++stream_spans;
    EXPECT_EQ(span.trace_id, kStreamTrace);
    if (const RecordedEvent* event = FindEvent(span, "a11.wire.send")) {
      send_event = event;
    }
  }
  EXPECT_EQ(stream_spans, 2);
  ASSERT_NE(send_event, nullptr);
  EXPECT_EQ(Attr(*send_event, "a11.wire.node_fragments"), "1");
  EXPECT_EQ(Attr(*send_event, "a11.wire.action_messages"), "0");
  EXPECT_FALSE(Attr(*send_event, "a11.wire.bytes").empty());
}

}  // namespace
}  // namespace a11::obs
