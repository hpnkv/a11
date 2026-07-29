// Copyright 2026 The A11 Authors.

#include "a11/net/http_sse_wire_stream.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

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

TEST(HttpSseWireStreamTest, ExchangesWireMessagesOverHttp2Sse) {
  auto server = HttpSseServer::Create("127.0.0.1", 0);
  ASSERT_TRUE(server.ok()) << server.status();
  auto client = HttpSseClientWireStream::Create(
      "http://127.0.0.1:" + std::to_string((*server)->port()), {}, nullptr,
      {{"x-client", "native"}});
  ASSERT_TRUE(client.ok()) << client.status();

  auto client_recorder = std::make_shared<SseRecorder>();
  a11::Task client_started = (*client)->Start(RecordMessages(client_recorder),
                                              RecordDone(client_recorder));
  auto accepted =
      (*server)->WaitForStream().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(accepted.ok()) << accepted.status();
  EXPECT_EQ(GetHttpHeader((*accepted)->GetHttpRequestHeaders(), "x-client"),
            "native");
  ASSERT_TRUE(
      (*accepted)->SetHttpResponseHeaders({{"x-server", "native"}}).ok());
  auto server_recorder = std::make_shared<SseRecorder>();
  a11::Task server_started = (*accepted)->Accept(
      RecordMessages(server_recorder), RecordDone(server_recorder));
  ASSERT_TRUE(server_started.Await(absl::Now() + absl::Seconds(5)).ok());
  ASSERT_TRUE(client_started.Await(absl::Now() + absl::Seconds(5)).ok());
  auto response_headers = (*client)->GetHttpResponseHeaders();
  ASSERT_TRUE(response_headers.has_value());
  EXPECT_EQ(GetHttpHeader(*response_headers, "x-server"), "native");
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

}  // namespace
}  // namespace a11::net
