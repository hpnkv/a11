// Copyright 2026 The A11 Authors.

#include "a11/net/websocket_wire_stream.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"

namespace a11::net {
namespace {

TEST(WebSocketWireStreamTest, ClientAndServerExchangeBinaryWireProtocol) {
  a11::Promise<std::shared_ptr<WebSocketWireStream>> accepted_promise;
  auto accepted_future = accepted_promise.future();
  a11::Promise<data::WireMessage> server_message_promise;
  auto server_message = server_message_promise.future();
  std::atomic<bool> server_done = false;

  WebSocketServerOptions server_options;
  server_options.port = 0;
  server_options.bind_address = "127.0.0.1";
  auto server = *WebSocketWireServer::Create(
      [&accepted_promise, &server_message_promise,
       &server_done](std::shared_ptr<WebSocketWireStream> stream) {
        const absl::Status published = accepted_promise.SetValue(stream);
        if (!published.ok()) {
          return a11::FailedTask(published);
        }
        return stream->Accept(
            [&server_message_promise](
                std::optional<data::WireMessage> message) {
              if (message.has_value()) {
                (void)server_message_promise.SetValue(std::move(*message));
              }
              return a11::ReadyTask();
            },
            [&server_done]() {
              server_done = true;
              return a11::ReadyTask();
            });
      },
      server_options);
  auto port = server->port();
  ASSERT_TRUE(port.ok()) << port.status();

  auto client = *WebSocketWireStream::CreateClient(
      "ws://127.0.0.1:" + std::to_string(*port) + "/a11");
  std::atomic<bool> client_done = false;
  ASSERT_TRUE(
      client
          ->Start(
              [](std::optional<data::WireMessage>) { return a11::ReadyTask(); },
              [&client_done]() {
                client_done = true;
                return a11::ReadyTask();
              })
          .Await(absl::Now() + absl::Seconds(5))
          .ok());
  auto accepted = accepted_future.Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(accepted.ok()) << accepted.status();

  data::WireMessage message{
      .node_fragments = {{.id = "node",
                          .data = data::Chunk{.data = "websocket"},
                          .seq = 0,
                          .continued = false}}};
  ASSERT_TRUE(client->Send(message).ok());
  auto received = server_message.Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(received.ok()) << received.status();
  EXPECT_EQ(*received, message);

  ASSERT_TRUE(client->HalfClose().ok());
  ASSERT_TRUE((*accepted)->HalfClose().ok());
  ASSERT_TRUE(client->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(5))
                  .ok());
  ASSERT_TRUE((*accepted)
                  ->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(5))
                  .ok());
  const absl::Time done_limit = absl::Now() + absl::Seconds(5);
  while ((!client_done || !server_done) && absl::Now() < done_limit) {
    thread::SleepFor(absl::Milliseconds(1));
  }
  EXPECT_TRUE(client_done);
  EXPECT_TRUE(server_done);
  EXPECT_TRUE(server->Stop().ok());
}

TEST(WebSocketWireStreamTest, ClientAndServerExchangeOverHttp1) {
  a11::Promise<std::shared_ptr<WebSocketWireStream>> accepted_promise;
  auto accepted_future = accepted_promise.future();
  a11::Promise<data::WireMessage> server_message_promise;
  auto server_message = server_message_promise.future();

  WebSocketServerOptions server_options;
  server_options.port = 0;
  server_options.bind_address = "127.0.0.1";
  auto server = *WebSocketWireServer::Create(
      [&accepted_promise,
       &server_message_promise](std::shared_ptr<WebSocketWireStream> stream) {
        const absl::Status published = accepted_promise.SetValue(stream);
        if (!published.ok()) {
          return a11::FailedTask(published);
        }
        return stream->Accept(
            [&server_message_promise](std::optional<data::WireMessage> msg) {
              if (msg.has_value()) {
                (void)server_message_promise.SetValue(std::move(*msg));
              }
              return a11::ReadyTask();
            },
            []() { return a11::ReadyTask(); });
      },
      server_options);
  auto port = server->port();
  ASSERT_TRUE(port.ok()) << port.status();

  // Force the client onto RFC 6455 over HTTP/1.1; the cleartext server sniffs
  // the upgrade request and accepts it over an Http1Connection.
  WebSocketClientOptions client_options;
  client_options.http2_options.client_preference =
      Http2Options::ProtocolPreference::kHttp11;
  auto client = WebSocketWireStream::CreateClient(
      "ws://127.0.0.1:" + std::to_string(*port) + "/a11", {}, client_options);
  ASSERT_TRUE(client.ok()) << client.status();
  ASSERT_TRUE(
      (*client)
          ->Start([](std::optional<data::WireMessage>) { return a11::ReadyTask(); },
                  []() { return a11::ReadyTask(); })
          .Await(absl::Now() + absl::Seconds(5))
          .ok());
  auto accepted = accepted_future.Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(accepted.ok()) << accepted.status();

  data::WireMessage message{
      .node_fragments = {{.id = "node",
                          .data = data::Chunk{.data = "ws-over-http1"},
                          .seq = 0,
                          .continued = false}}};
  ASSERT_TRUE((*client)->Send(message).ok());
  auto received = server_message.Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(received.ok()) << received.status();
  EXPECT_EQ(*received, message);

  ASSERT_TRUE((*client)->HalfClose().ok());
  ASSERT_TRUE((*accepted)->HalfClose().ok());
  ASSERT_TRUE((*client)
                  ->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(5))
                  .ok());
  EXPECT_TRUE(server->Stop().ok());
}

TEST(WebSocketWireStreamTest, ReassemblesMultipleLargeChunkedMessagesInOrder) {
  a11::Promise<std::shared_ptr<WebSocketWireStream>> accepted_promise;
  a11::Future<std::shared_ptr<WebSocketWireStream>> accepted_future =
      accepted_promise.future();
  a11::Promise<std::vector<data::WireMessage>> messages_promise;
  a11::Future<std::vector<data::WireMessage>> messages_future =
      messages_promise.future();
  std::vector<data::WireMessage> messages;

  WebSocketServerOptions server_options;
  server_options.port = 0;
  server_options.bind_address = "127.0.0.1";
  server_options.framing.split_size = 1024;
  auto server = WebSocketWireServer::Create(
      [&accepted_promise, &messages_promise,
       &messages](std::shared_ptr<WebSocketWireStream> stream) {
        const absl::Status published = accepted_promise.SetValue(stream);
        if (!published.ok()) {
          return a11::FailedTask(published);
        }
        return stream->Accept(
            [&messages_promise,
             &messages](std::optional<data::WireMessage> message) {
              if (message.has_value()) {
                messages.push_back(std::move(*message));
                if (messages.size() == 2) {
                  (void)messages_promise.SetValue(std::move(messages));
                }
              }
              return a11::ReadyTask();
            },
            []() { return a11::ReadyTask(); });
      },
      server_options);
  ASSERT_TRUE(server.ok()) << server.status();
  auto port = (*server)->port();
  ASSERT_TRUE(port.ok()) << port.status();

  WebSocketClientOptions client_options;
  client_options.framing.split_size = 1024;
  auto client = WebSocketWireStream::CreateClient(
      "ws://127.0.0.1:" + std::to_string(*port) + "/a11", {}, client_options);
  ASSERT_TRUE(client.ok()) << client.status();
  ASSERT_TRUE(
      (*client)
          ->Start(
              [](std::optional<data::WireMessage>) { return a11::ReadyTask(); },
              []() { return a11::ReadyTask(); })
          .Await(absl::Now() + absl::Seconds(5))
          .ok());
  auto accepted = accepted_future.Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(accepted.ok()) << accepted.status();

  const std::string first_payload(256 * 1024, 'a');
  const std::string second_payload(192 * 1024, 'b');
  data::WireMessage first{
      .node_fragments = {{.id = "first",
                          .data = data::Chunk{.data = first_payload},
                          .seq = 0,
                          .continued = false}}};
  data::WireMessage second{
      .node_fragments = {{.id = "second",
                          .data = data::Chunk{.data = second_payload},
                          .seq = 0,
                          .continued = false}}};
  ASSERT_TRUE((*client)->Send(std::move(first)).ok());
  ASSERT_TRUE((*client)->Send(std::move(second)).ok());

  auto received = messages_future.Await(absl::Now() + absl::Seconds(10));
  ASSERT_TRUE(received.ok()) << received.status();
  ASSERT_EQ(received->size(), 2);
  ASSERT_EQ((*received)[0].node_fragments.size(), 1);
  ASSERT_EQ((*received)[1].node_fragments.size(), 1);
  EXPECT_EQ(std::get<data::Chunk>((*received)[0].node_fragments[0].data).data,
            first_payload);
  EXPECT_EQ(std::get<data::Chunk>((*received)[1].node_fragments[0].data).data,
            second_payload);

  EXPECT_TRUE((*client)->HalfClose().ok());
  EXPECT_TRUE((*accepted)->HalfClose().ok());
  EXPECT_TRUE((*client)
                  ->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(5))
                  .ok());
  EXPECT_TRUE((*accepted)
                  ->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(5))
                  .ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

}  // namespace
}  // namespace a11::net
