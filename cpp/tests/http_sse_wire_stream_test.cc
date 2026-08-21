// Copyright 2026 The A11 Authors.

#include "a11/net/http_sse_wire_stream.h"
#include "a11/net/server_headers.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/wire_stream_with_recv.h"
#include "thread/boost_primitives.h"

namespace a11::net {
namespace {

std::string TestDataPath(std::string_view name) {
  return ((std::filesystem::path(A11_CPP_SOURCE_ROOT).parent_path() /
           std::filesystem::path(__FILE__))
              .parent_path() /
          "testdata" / name)
      .string();
}

struct SseRecorder {
  thread::Mutex mu;
  std::vector<data::WireMessage> messages;
  bool half_closed = false;
  a11::Promise<a11::Unit> done_promise;
};

OnMessage RecordMessages(const std::shared_ptr<SseRecorder>& recorder) {
  return [recorder](std::optional<data::WireMessage> message) {
    thread::MutexLock lock(&recorder->mu);
    if (message.has_value()) {
      recorder->messages.push_back(std::move(*message));
    } else {
      recorder->half_closed = true;
    }
    return a11::ReadyTask();
  };
}

OnDone RecordDone(const std::shared_ptr<SseRecorder>& recorder) {
  return [recorder]() {
    (void)recorder->done_promise.SetValue(a11::Unit{});
    return a11::ReadyTask();
  };
}

TEST(HttpSseWireStreamTest, AnswersCorsPreflight) {
  HttpSseOptions options;
  options.headers.cors.allow_methods = "*";
  options.headers.cors.allow_headers = "*";
  auto server = HttpSseServer::Create("127.0.0.1", 0, {}, options);
  ASSERT_TRUE(server.ok()) << server.status();

  auto client = Http2Client::Connect("127.0.0.1", (*server)->port())
                    .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status();
  auto response = (*client)
                      ->Request("OPTIONS", "/connect")
                      .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(response.ok()) << response.status();
  EXPECT_EQ(response->head.status, 204);
  EXPECT_EQ(GetHttpHeader(response->head.headers,
                          "access-control-allow-origin"),
            "*");
  EXPECT_EQ(GetHttpHeader(response->head.headers,
                          "access-control-allow-methods"),
            "*");
  EXPECT_EQ(GetHttpHeader(response->head.headers,
                          "access-control-allow-headers"),
            "*");
  // Exposed by default, and it has to be: a browser reads its stream id off the
  // connect response, and a header it may not read is one it did not receive.
  EXPECT_EQ(GetHttpHeader(response->head.headers,
                          "access-control-expose-headers"),
            "x-a11-stream-id, x-a11-outbound");
  // What an A11 server says about itself on every reply.
  EXPECT_EQ(GetHttpHeader(response->head.headers, "server"), "a11");
  EXPECT_EQ(GetHttpHeader(response->head.headers, "x-content-type-options"),
            "nosniff");
  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(HttpSseWireStreamTest, AdmitsBrowsersWithoutBeingConfigured) {
  // The default is permissive on purpose: the whole reason this transport
  // exists is that a page cannot open a socket, and a default that refused
  // pages would put a configuration step in front of every one of them.
  const HttpSseOptions options;
  EXPECT_TRUE(options.headers.cors.enabled);
  EXPECT_EQ(options.headers.cors.allow_origin, "*");
  EXPECT_EQ(options.headers.server, "a11");
  EXPECT_TRUE(options.Validate().ok());
}

TEST(HttpSseWireStreamTest, ACorsPolicyCanBeNarrowedOrTurnedOff) {
  HttpSseOptions narrowed;
  narrowed.headers.cors.allow_origin = "https://example.test";
  ASSERT_TRUE(narrowed.Validate().ok());
  const HttpHeaders headers = CorsHeaders(narrowed.headers.cors);
  EXPECT_EQ(GetHttpHeader(headers, "access-control-allow-origin"),
            "https://example.test");
  // One origin named means the answer differs per origin, and a cache that did
  // not know that would hand the wrong one out.
  EXPECT_EQ(GetHttpHeader(headers, "vary"), "Origin");

  HttpSseOptions off;
  off.headers.cors.enabled = false;
  ASSERT_TRUE(off.Validate().ok());
  EXPECT_TRUE(CorsHeaders(off.headers.cors).empty());

  // Enabled with nothing to allow is the one incoherent combination.
  HttpSseOptions contradictory;
  contradictory.headers.cors.allow_origin = "";
  EXPECT_EQ(contradictory.Validate().code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(ServerHeadersTest, StatesHowEachKindOfReplyMayBeCached) {
  const ServerHeaderOptions options;

  HttpHeaders stream;
  ApplyServerHeaders(options, CachePolicy::kStream, &stream);
  EXPECT_EQ(GetHttpHeader(stream, "cache-control"), "no-store");
  // Not standard, and worth sending: nginx buffers a proxied response by
  // default, which for an event stream means the client sees nothing until the
  // buffer fills.
  EXPECT_EQ(GetHttpHeader(stream, "x-accel-buffering"), "no");

  HttpHeaders document;
  ApplyServerHeaders(options, CachePolicy::kVolatile, &document);
  EXPECT_EQ(GetHttpHeader(document, "cache-control"), "no-cache");

  HttpHeaders unset;
  ApplyServerHeaders(options, CachePolicy::kUnset, &unset);
  EXPECT_FALSE(GetHttpHeader(unset, "cache-control").has_value());
}

TEST(ServerHeadersTest, NeverOverridesWhatARouteAlreadySaid) {
  const ServerHeaderOptions options;
  HttpHeaders headers{{"content-type", "text/event-stream"},
                      {"cache-control", "max-age=1"},
                      {"Server", "something-else"}};
  ApplyServerHeaders(options, CachePolicy::kStream, &headers);
  EXPECT_EQ(GetHttpHeader(headers, "cache-control"), "max-age=1");
  EXPECT_EQ(GetHttpHeader(headers, "content-type"), "text/event-stream");

  // `Server` was already there in another case. Header names are
  // case-insensitive, so adding a second would be sending the header twice with
  // two different values -- counted rather than looked up, because
  // GetHttpHeader matches the stored name exactly and would not find this one.
  int servers = 0;
  for (const auto& [name, value] : headers) {
    if (absl::EqualsIgnoreCase(name, "server")) {
      ++servers;
      EXPECT_EQ(value, "something-else");
    }
  }
  EXPECT_EQ(servers, 1);
}

TEST(ServerHeadersTest, RefusesAValueThatWouldSplitTheHeaderBlock) {
  ServerHeaderOptions options;
  options.cors.allow_origin = "*\r\nx-injected: yes";
  EXPECT_EQ(options.Validate().code(), absl::StatusCode::kInvalidArgument);

  ServerHeaderOptions named;
  named.server = "a11\r\n";
  EXPECT_EQ(named.Validate().code(), absl::StatusCode::kInvalidArgument);
}

TEST(HttpSseWireStreamTest, ExchangesWireMessagesOverHttp2Sse) {
  auto server = HttpSseServer::Create("127.0.0.1", 0);
  ASSERT_TRUE(server.ok()) << server.status();
  auto client = HttpSseClientWireStream::Create(
      "http://127.0.0.1:" + std::to_string((*server)->port()), {}, nullptr,
      {{"X-Client", "native"}, {"X-A11-HTTP-Trace", "once"}});
  ASSERT_TRUE(client.ok()) << client.status();

  auto client_recorder = std::make_shared<SseRecorder>();
  a11::Task client_started = (*client)->Start(RecordMessages(client_recorder),
                                              RecordDone(client_recorder));
  auto accepted =
      (*server)->WaitForStream().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(accepted.ok()) << accepted.status();
  EXPECT_EQ(GetHttpHeader((*accepted)->GetHttpRequestHeaders(), "x-client"),
            "native");
  for (const auto& [name, value] : (*accepted)->GetHttpRequestHeaders()) {
    (void)value;
    EXPECT_EQ(name, absl::AsciiStrToLower(name));
  }
  ASSERT_TRUE(
      (*accepted)->SetHttpResponseHeaders({{"X-Server", "native"}}).ok());
  auto server_recorder = std::make_shared<SseRecorder>();
  a11::Task server_started = (*accepted)->Accept(
      RecordMessages(server_recorder), RecordDone(server_recorder));
  ASSERT_TRUE(server_started.Await(absl::Now() + absl::Seconds(5)).ok());
  ASSERT_TRUE(client_started.Await(absl::Now() + absl::Seconds(5)).ok());
  auto response_headers = (*client)->GetHttpResponseHeaders();
  ASSERT_TRUE(response_headers.has_value());
  EXPECT_EQ(GetHttpHeader(*response_headers, "x-server"), "native");
  EXPECT_EQ(GetHttpHeader(*response_headers, "x-a11-stream-id"),
            (*accepted)->GetId());
  for (const auto& [name, value] : *response_headers) {
    (void)value;
    EXPECT_EQ(name, absl::AsciiStrToLower(name));
  }
  EXPECT_EQ((*client)->GetId(), (*accepted)->GetId());

  data::WireMessage to_server;
  to_server.headers.emplace("application", "client");
  to_server.node_fragments.push_back(data::NodeFragment{
      .id = "client-node", .data = data::Chunk{.data = "payload"}});
  ASSERT_TRUE((*client)->Send(to_server).ok());
  data::WireMessage to_client;
  to_client.headers.emplace("application", "server");
  to_client.node_fragments.push_back(data::NodeFragment{
      .id = "server-node", .data = data::Chunk{.data = "payload"}});
  ASSERT_TRUE((*accepted)->Send(to_client).ok());

  ASSERT_TRUE((*client)->HalfClose({{"client-trailer", "done"}}).ok());
  ASSERT_TRUE((*accepted)->HalfClose({{"server-trailer", "done"}}).ok());
  ASSERT_TRUE((*client)
                  ->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(5))
                  .ok());
  ASSERT_TRUE((*accepted)
                  ->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(5))
                  .ok());
  ASSERT_TRUE(client_recorder->done_promise.future()
                  .Await(absl::Now() + absl::Seconds(5))
                  .ok());
  ASSERT_TRUE(server_recorder->done_promise.future()
                  .Await(absl::Now() + absl::Seconds(5))
                  .ok());

  {
    thread::MutexLock lock(&server_recorder->mu);
    ASSERT_EQ(server_recorder->messages.size(), 1);
    EXPECT_EQ(server_recorder->messages[0].headers.at("application"), "client");
    EXPECT_EQ(server_recorder->messages[0].headers.at("x-a11-http-x-client"),
              "native");
    EXPECT_EQ(server_recorder->messages[0].headers.at("x-a11-http-trace"),
              "once");
    EXPECT_EQ(server_recorder->messages[0].headers.count(
                  "x-a11-http-x-a11-http-trace"),
              0);
    EXPECT_TRUE(server_recorder->half_closed);
  }
  {
    thread::MutexLock lock(&client_recorder->mu);
    ASSERT_EQ(client_recorder->messages.size(), 1);
    EXPECT_EQ(client_recorder->messages[0].headers.at("application"), "server");
    EXPECT_TRUE(client_recorder->half_closed);
  }
  ASSERT_TRUE((*client)->GetTrailers().has_value());
  EXPECT_EQ((*client)->GetTrailers()->at("server-trailer"), "done");
  ASSERT_TRUE((*accepted)->GetTrailers().has_value());
  EXPECT_EQ((*accepted)->GetTrailers()->at("client-trailer"), "done");
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(HttpSseWireStreamTest, PullAdaptersFinishCleanlyAfterConcurrentHalfClose) {
  auto server = HttpSseServer::Create("127.0.0.1", 0);
  ASSERT_TRUE(server.ok()) << server.status();
  auto raw_client = HttpSseClientWireStream::Create(
      "http://127.0.0.1:" + std::to_string((*server)->port()));
  ASSERT_TRUE(raw_client.ok()) << raw_client.status();
  auto client = WireStreamWithRecv::Create(*raw_client);
  ASSERT_TRUE(client.ok()) << client.status();

  a11::Task client_started = (*client)->Start();
  auto raw_accepted =
      (*server)->WaitForStream().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(raw_accepted.ok()) << raw_accepted.status();
  auto accepted = WireStreamWithRecv::Create(*raw_accepted);
  ASSERT_TRUE(accepted.ok()) << accepted.status();
  ASSERT_TRUE((*accepted)->Accept().Await(absl::Now() + absl::Seconds(5)).ok());
  ASSERT_TRUE(client_started.Await(absl::Now() + absl::Seconds(5)).ok());

  data::WireMessage to_server;
  to_server.headers.emplace("from", "client");
  to_server.node_fragments.push_back(data::NodeFragment{
      .id = "client", .data = data::Chunk{.data = "payload"}});
  data::WireMessage to_client;
  to_client.headers.emplace("from", "server");
  to_client.node_fragments.push_back(data::NodeFragment{
      .id = "server", .data = data::Chunk{.data = "payload"}});
  ASSERT_TRUE((*client)->Send(to_server).ok());
  ASSERT_TRUE((*accepted)->Send(to_client).ok());
  auto server_message =
      (*accepted)->Receive().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(server_message.ok()) << server_message.status();
  ASSERT_TRUE(server_message->has_value());
  EXPECT_EQ((**server_message).headers.at("from"), "client");
  auto client_message =
      (*client)->Receive().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client_message.ok()) << client_message.status();
  ASSERT_TRUE(client_message->has_value());
  EXPECT_EQ((**client_message).headers.at("from"), "server");

  ASSERT_TRUE((*client)->HalfClose({{"client-trailer", "done"}}).ok());
  ASSERT_TRUE((*accepted)->HalfClose({{"server-trailer", "done"}}).ok());
  a11::Task client_drained = (*client)->DrainOutgoingMessages();
  a11::Task server_drained = (*accepted)->DrainOutgoingMessages();
  ASSERT_TRUE(client_drained.Await(absl::Now() + absl::Seconds(5)).ok());
  ASSERT_TRUE(server_drained.Await(absl::Now() + absl::Seconds(5)).ok());

  auto server_eof =
      (*accepted)->Receive().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(server_eof.ok()) << server_eof.status();
  EXPECT_FALSE(server_eof->has_value());
  auto client_eof = (*client)->Receive().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client_eof.ok()) << client_eof.status();
  EXPECT_FALSE(client_eof->has_value());
  EXPECT_TRUE((*client)->GetStatus().ok()) << (*client)->GetStatus();
  EXPECT_TRUE((*accepted)->GetStatus().ok()) << (*accepted)->GetStatus();
  EXPECT_EQ((*client)->GetTrailers()->at("server-trailer"), "done");
  EXPECT_EQ((*accepted)->GetTrailers()->at("client-trailer"), "done");
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(HttpSseWireStreamTest, ExchangesTerminalMessagesOverTls) {
  HttpSseOptions server_options;
  server_options.http2_options.tls.enabled = true;
  server_options.http2_options.tls.certificate_pem_file =
      TestDataPath("localhost-cert.pem");
  server_options.http2_options.tls.key_pem_file =
      TestDataPath("localhost-key.pem");
  auto server = HttpSseServer::Create("127.0.0.1", 0, {}, server_options);
  ASSERT_TRUE(server.ok()) << server.status();

  HttpSseOptions client_options;
  client_options.http2_options.tls.enabled = true;
  client_options.http2_options.tls.ca_certificate_pem_file =
      TestDataPath("localhost-cert.pem");
  auto raw_client = HttpSseClientWireStream::Create(
      "https://127.0.0.1:" + std::to_string((*server)->port()), client_options);
  ASSERT_TRUE(raw_client.ok()) << raw_client.status();
  auto client = WireStreamWithRecv::Create(*raw_client);
  ASSERT_TRUE(client.ok()) << client.status();

  a11::Task client_started = (*client)->Start();
  auto raw_accepted =
      (*server)->WaitForStream().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(raw_accepted.ok()) << raw_accepted.status();
  auto accepted = WireStreamWithRecv::Create(*raw_accepted);
  ASSERT_TRUE(accepted.ok()) << accepted.status();
  ASSERT_TRUE((*accepted)->Accept().Await(absl::Now() + absl::Seconds(5)).ok());
  ASSERT_TRUE(client_started.Await(absl::Now() + absl::Seconds(5)).ok());

  ASSERT_TRUE((*client)->HalfClose().ok());
  ASSERT_TRUE((*accepted)->HalfClose().ok());
  ASSERT_TRUE((*client)
                  ->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(5))
                  .ok());
  ASSERT_TRUE((*accepted)
                  ->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(5))
                  .ok());
  auto server_eof =
      (*accepted)->Receive().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(server_eof.ok()) << server_eof.status();
  EXPECT_FALSE(server_eof->has_value());
  auto client_eof = (*client)->Receive().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client_eof.ok()) << client_eof.status();
  EXPECT_FALSE(client_eof->has_value());
  EXPECT_TRUE((*client)->GetStatus().ok()) << (*client)->GetStatus();
  EXPECT_TRUE((*accepted)->GetStatus().ok()) << (*accepted)->GetStatus();
  EXPECT_TRUE((*server)->Stop().ok());
}

// Exchanging many messages each way, parameterised over the outbound delivery
// method, and asserting the two things a caller is promised regardless of it:
// every message arrives, and the half-close arrives after all of them.
class HttpSseOutboundDeliveryTest
    : public testing::TestWithParam<SseOutboundDelivery> {};

TEST_P(HttpSseOutboundDeliveryTest, DeliversEveryMessageBeforeTheHalfClose) {
  constexpr int kMessages = 64;
  HttpSseOptions client_options;
  client_options.outbound = GetParam();
  client_options.max_concurrent_posts = 8;

  auto server = HttpSseServer::Create("127.0.0.1", 0);
  ASSERT_TRUE(server.ok()) << server.status();
  auto client = HttpSseClientWireStream::Create(
      "http://127.0.0.1:" + std::to_string((*server)->port()), client_options,
      nullptr, {{"X-Client", "native"}});
  ASSERT_TRUE(client.ok()) << client.status();

  auto client_recorder = std::make_shared<SseRecorder>();
  a11::Task client_started = (*client)->Start(RecordMessages(client_recorder),
                                              RecordDone(client_recorder));
  auto accepted =
      (*server)->WaitForStream().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(accepted.ok()) << accepted.status();
  auto server_recorder = std::make_shared<SseRecorder>();
  a11::Task server_started = (*accepted)->Accept(
      RecordMessages(server_recorder), RecordDone(server_recorder));
  ASSERT_TRUE(server_started.Await(absl::Now() + absl::Seconds(10)).ok());
  ASSERT_TRUE(client_started.Await(absl::Now() + absl::Seconds(10)).ok());
  EXPECT_EQ((*client)->outbound_delivery(), GetParam());

  for (int index = 0; index < kMessages; ++index) {
    data::WireMessage message;
    message.node_fragments.push_back(data::NodeFragment{
        .id = std::to_string(index), .data = data::Chunk{.data = "payload"}});
    ASSERT_TRUE((*client)->Send(message).ok()) << index;
  }
  ASSERT_TRUE((*client)->HalfClose({{"client-trailer", "done"}}).ok());
  ASSERT_TRUE((*accepted)->HalfClose({{"server-trailer", "done"}}).ok());
  ASSERT_TRUE((*client)
                  ->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(20))
                  .ok());
  ASSERT_TRUE((*accepted)
                  ->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(20))
                  .ok());
  ASSERT_TRUE(server_recorder->done_promise.future()
                  .Await(absl::Now() + absl::Seconds(20))
                  .ok());

  thread::MutexLock lock(&server_recorder->mu);
  // Not "in order": WireMessages carry none, and concurrent POSTs are free to
  // land in any order. What is promised is that all of them land, and that the
  // half-close is not observed until they have.
  //
  // Counted in fragments rather than messages, because the in-process bridge on
  // each endpoint folds queued messages into one frame -- so a burst arrives as
  // fewer, larger messages with every fragment intact. Which index each fragment
  // came from survives in its id for the same reason: a fold keeps one headers
  // map, so the per-message `index` header does not survive it.
  size_t fragments = 0;
  std::vector<bool> seen(kMessages, false);
  for (const data::WireMessage& message : server_recorder->messages) {
    fragments += message.node_fragments.size();
    for (const data::NodeFragment& fragment : message.node_fragments) {
      int index = -1;
      ASSERT_TRUE(absl::SimpleAtoi(fragment.id, &index)) << fragment.id;
      ASSERT_GE(index, 0);
      ASSERT_LT(index, kMessages);
      EXPECT_FALSE(seen[index]) << "message " << index << " arrived twice";
      seen[index] = true;
    }
    // The request headers reach every message, whether they were sent once per
    // POST or once for the whole stream.
    EXPECT_EQ(message.headers.at("x-a11-http-x-client"), "native");
  }
  EXPECT_EQ(fragments, kMessages);
  EXPECT_TRUE(server_recorder->half_closed);
  EXPECT_EQ((*accepted)->GetTrailers()->at("client-trailer"), "done");
  EXPECT_TRUE((*server)->Stop().ok());
}

INSTANTIATE_TEST_SUITE_P(Delivery, HttpSseOutboundDeliveryTest,
                         testing::Values(SseOutboundDelivery::kPost,
                                         SseOutboundDelivery::kStream),
                         [](const testing::TestParamInfo<SseOutboundDelivery>&
                                info) {
                           return info.param == SseOutboundDelivery::kPost
                                      ? "ConcurrentPosts"
                                      : "StreamedBody";
                         });

// Streamed delivery over HTTP/1.1, where the body is a chunked one and the
// connect request already owns its connection -- so the upload has to get one of
// its own. It is also the one configuration in which POST delivery cannot work
// at all, since an HTTP/1.1 connection carries a single request.
TEST(HttpSseWireStreamTest, StreamsOutboundOverHttp11) {
  auto server = HttpSseServer::Create("127.0.0.1", 0);
  ASSERT_TRUE(server.ok()) << server.status();

  HttpSseOptions client_options;
  client_options.outbound = SseOutboundDelivery::kStream;
  client_options.http2_options.client_preference =
      Http2Options::ProtocolPreference::kHttp11;
  auto client = HttpSseClientWireStream::Create(
      "http://127.0.0.1:" + std::to_string((*server)->port()), client_options);
  ASSERT_TRUE(client.ok()) << client.status();

  auto client_recorder = std::make_shared<SseRecorder>();
  a11::Task client_started = (*client)->Start(RecordMessages(client_recorder),
                                              RecordDone(client_recorder));
  auto accepted =
      (*server)->WaitForStream().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(accepted.ok()) << accepted.status();
  auto server_recorder = std::make_shared<SseRecorder>();
  a11::Task server_started = (*accepted)->Accept(
      RecordMessages(server_recorder), RecordDone(server_recorder));
  ASSERT_TRUE(server_started.Await(absl::Now() + absl::Seconds(10)).ok());
  ASSERT_TRUE(client_started.Await(absl::Now() + absl::Seconds(10)).ok());
  EXPECT_EQ((*client)->outbound_delivery(), SseOutboundDelivery::kStream);

  for (int index = 0; index < 8; ++index) {
    data::WireMessage message;
    message.headers.emplace("index", std::to_string(index));
    message.node_fragments.push_back(data::NodeFragment{
        .id = "client-node", .data = data::Chunk{.data = "payload"}});
    ASSERT_TRUE((*client)->Send(message).ok()) << index;
  }
  ASSERT_TRUE((*client)->HalfClose().ok());
  ASSERT_TRUE((*accepted)->HalfClose().ok());
  ASSERT_TRUE((*client)
                  ->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(20))
                  .ok());
  ASSERT_TRUE(server_recorder->done_promise.future()
                  .Await(absl::Now() + absl::Seconds(20))
                  .ok());
  {
    thread::MutexLock lock(&server_recorder->mu);
    EXPECT_EQ(server_recorder->messages.size(), 8);
    EXPECT_TRUE(server_recorder->half_closed);
  }
  EXPECT_TRUE((*server)->Stop().ok());
}

// A pipelined echo, which is the shape the wire benchmark measures and the one
// concurrent delivery is for: messages go out while earlier ones are still
// unanswered, and each echo releases credit for one more.
TEST(HttpSseWireStreamTest, SustainsAPipelinedEchoWithConcurrentPosts) {
  constexpr int kMessages = 200;
  constexpr int kWindow = 32;
  auto echoed = std::make_shared<thread::Mutex>();
  auto count = std::make_shared<int>(0);
  auto changed = std::make_shared<thread::CondVar>();

  auto server = HttpSseServer::Create("127.0.0.1", 0);
  ASSERT_TRUE(server.ok()) << server.status();
  HttpSseOptions client_options;
  client_options.max_concurrent_posts = 8;
  auto client = HttpSseClientWireStream::Create(
      "http://127.0.0.1:" + std::to_string((*server)->port()), client_options);
  ASSERT_TRUE(client.ok()) << client.status();

  // Fragments, not messages: the in-process bridge each SSE endpoint carries
  // folds queued messages into one frame, so a burst of sends can arrive as one
  // larger message. Fragments survive the fold and are what the credit is in.
  OnMessage on_client = [echoed, count,
                         changed](std::optional<data::WireMessage> message) {
    if (message.has_value()) {
      thread::MutexLock lock(echoed.get());
      *count += static_cast<int>(message->node_fragments.size());
      changed->SignalAll();
    }
    return a11::ReadyTask();
  };
  a11::Task client_started =
      (*client)->Start(std::move(on_client), []() { return a11::ReadyTask(); });
  auto accepted =
      (*server)->WaitForStream().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(accepted.ok()) << accepted.status();

  auto received = std::make_shared<std::atomic<int>>(0);
  auto replied = std::make_shared<std::atomic<int>>(0);
  std::weak_ptr<HttpSseServerWireStream> weak_peer = *accepted;
  OnMessage echo = [weak_peer, received,
                    replied](std::optional<data::WireMessage> message) {
    if (!message.has_value()) {
      return a11::ReadyTask();
    }
    const int fragments = static_cast<int>(message->node_fragments.size());
    received->fetch_add(fragments);
    if (std::shared_ptr<HttpSseServerWireStream> peer = weak_peer.lock();
        peer != nullptr) {
      data::WireMessage reply;
      reply.node_fragments.reserve(fragments);
      for (int index = 0; index < fragments; ++index) {
        reply.node_fragments.push_back(
            data::NodeFragment{.id = absl::StrCat("echo-", index),
                               .data = data::Chunk{.data = "1"}});
      }
      const absl::Status sent = peer->Send(std::move(reply));
      if (!sent.ok()) {
        return a11::FailedTask(sent);
      }
      replied->fetch_add(fragments);
    }
    return a11::ReadyTask();
  };
  a11::Task server_started =
      (*accepted)->Accept(std::move(echo), []() { return a11::ReadyTask(); });
  ASSERT_TRUE(server_started.Await(absl::Now() + absl::Seconds(10)).ok());
  ASSERT_TRUE(client_started.Await(absl::Now() + absl::Seconds(10)).ok());

  data::WireMessage payload;
  payload.node_fragments.push_back(data::NodeFragment{
      .id = "bench", .data = data::Chunk{.data = std::string(64, 'x')}});
  // Waits for at least `target` echoes; returns the count it saw, so a stall
  // reports how far it got rather than just timing out.
  const auto await_echoes = [&](int target) {
    thread::MutexLock lock(echoed.get());
    while (*count < target) {
      if (changed->WaitWithDeadline(echoed.get(),
                                    absl::Now() + absl::Seconds(10))) {
        break;
      }
    }
    return *count;
  };
  for (int sent = 0; sent < kMessages; ++sent) {
    if (sent - await_echoes(0) >= kWindow) {
      const int seen = await_echoes(sent - kWindow + 1);
      ASSERT_GE(seen, sent - kWindow + 1)
          << "stalled with " << seen << " echoes after sending " << sent;
    }
    ASSERT_TRUE((*client)->Send(payload).ok()) << sent;
  }
  ASSERT_EQ(await_echoes(kMessages), kMessages)
      << "server received " << received->load() << ", replied "
      << replied->load() << ", client status " << (*client)->GetStatus();
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(HttpSseWireStreamTest, AdvertisesBothOutboundModesOnConnect) {
  auto server = HttpSseServer::Create("127.0.0.1", 0);
  ASSERT_TRUE(server.ok()) << server.status();
  auto client = HttpSseClientWireStream::Create(
      "http://127.0.0.1:" + std::to_string((*server)->port()));
  ASSERT_TRUE(client.ok()) << client.status();
  auto recorder = std::make_shared<SseRecorder>();
  a11::Task started =
      (*client)->Start(RecordMessages(recorder), RecordDone(recorder));
  auto accepted =
      (*server)->WaitForStream().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(accepted.ok()) << accepted.status();
  a11::Task server_started =
      (*accepted)->Accept(RecordMessages(recorder), RecordDone(recorder));
  ASSERT_TRUE(server_started.Await(absl::Now() + absl::Seconds(5)).ok());
  ASSERT_TRUE(started.Await(absl::Now() + absl::Seconds(5)).ok());

  auto headers = (*client)->GetHttpResponseHeaders();
  ASSERT_TRUE(headers.has_value());
  const std::optional<std::string> modes =
      GetHttpHeader(*headers, kSseOutboundModesHeader);
  ASSERT_TRUE(modes.has_value());
  EXPECT_TRUE(absl::StrContains(*modes, kSseOutboundPostToken)) << *modes;
  EXPECT_TRUE(absl::StrContains(*modes, kSseOutboundStreamToken)) << *modes;
  EXPECT_TRUE((*server)->Stop().ok());
}

// A server that does not advertise streamed delivery must not be spoken to that
// way: the client silently keeps posting, which is what makes the option safe to
// set against an unknown peer.
TEST(HttpSseWireStreamTest, FallsBackToPostsWhenTheServerIsSilentOnModes) {
  auto sse_server = HttpSseServer::Create("127.0.0.1", 0);
  ASSERT_TRUE(sse_server.ok()) << sse_server.status();
  const std::uint16_t upstream_port = (*sse_server)->port();

  // A proxy that forwards the connect response with the advertisement stripped,
  // standing in for an SSE server built before streamed delivery existed.
  auto proxy = Http2Server::Create(
      "127.0.0.1", 0,
      [upstream_port](HttpRequest request,
                      std::shared_ptr<Http2ResponseWriter> response)
          -> a11::Task {
        return a11::SubmitTask(
            [upstream_port, request = std::move(request),
             response = std::move(response)]() mutable -> absl::Status {
              ABSL_ASSIGN_OR_RETURN(
                  std::shared_ptr<Http2Client> upstream,
                  Http2Client::Connect("127.0.0.1", upstream_port)
                      .Await(absl::Now() + absl::Seconds(5)));
              ABSL_ASSIGN_OR_RETURN(
                  std::shared_ptr<Http2ResponseStream> upstream_stream,
                  upstream->RequestStream(request.method, request.path,
                                          request.headers,
                                          std::move(request.body)));
              ABSL_ASSIGN_OR_RETURN(
                  HttpResponseHead head,
                  upstream_stream->Headers().Await(absl::Now() +
                                                   absl::Seconds(5)));
              EraseHttpHeader(&head.headers, kSseOutboundModesHeader);
              ABSL_RETURN_IF_ERROR(
                  response->SendHeaders(head.status, head.headers));
              while (true) {
                ABSL_ASSIGN_OR_RETURN(
                    std::optional<std::string> chunk,
                    upstream_stream->Read().Await(absl::InfiniteFuture()));
                if (!chunk.has_value()) {
                  break;
                }
                ABSL_RETURN_IF_ERROR(response->Write(std::move(*chunk)));
              }
              return response->Finish();
            });
      });
  ASSERT_TRUE(proxy.ok()) << proxy.status();

  HttpSseOptions options;
  options.outbound = SseOutboundDelivery::kStream;
  auto client = HttpSseClientWireStream::Create(
      "http://127.0.0.1:" + std::to_string((*proxy)->port()), options);
  ASSERT_TRUE(client.ok()) << client.status();
  auto recorder = std::make_shared<SseRecorder>();
  a11::Task started =
      (*client)->Start(RecordMessages(recorder), RecordDone(recorder));
  auto accepted =
      (*sse_server)->WaitForStream().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(accepted.ok()) << accepted.status();
  auto server_recorder = std::make_shared<SseRecorder>();
  a11::Task server_started = (*accepted)->Accept(
      RecordMessages(server_recorder), RecordDone(server_recorder));
  ASSERT_TRUE(server_started.Await(absl::Now() + absl::Seconds(10)).ok());
  ASSERT_TRUE(started.Await(absl::Now() + absl::Seconds(10)).ok());
  EXPECT_EQ((*client)->outbound_delivery(), SseOutboundDelivery::kPost);
  EXPECT_TRUE((*proxy)->Stop().ok());
  EXPECT_TRUE((*sse_server)->Stop().ok());
}

}  // namespace
}  // namespace a11::net
