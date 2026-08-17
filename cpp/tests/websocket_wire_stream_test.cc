// Copyright 2026 The A11 Authors.

#include "a11/net/websocket_wire_stream.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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
          ->Start(
              [](std::optional<data::WireMessage>) { return a11::ReadyTask(); },
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

// Each sender's messages arrive in the order that sender wrote them, whichever
// path carried them.
//
// A message may go out on the endpoint's own Sender fibre, or on the thread
// that called Send when nothing was queued ahead of it, or from the queue once
// the fibre catches up. Several threads sending at once puts all three in play.
//
// One small packet per message, and not many of them, deliberately. Chunked
// ordering has its own test below; and `Send` has no way to say "not yet", so a
// flood aborts the stream instead of pushing back -- interleaving multi-packet
// messages from four threads exceeds `max_pending_messages` on the receiving
// side, and even single packets abort it if there are enough of them. What is
// under test here is the order of the paths a message can take, not capacity.
// `seq` carries the sender's index in its high bits.
TEST(WebSocketWireStreamTest, PreservesEachSendersOrderUnderConcurrency) {
  constexpr int kSenders = 4;
  constexpr int kPerSender = 12;
  constexpr int kTotal = kSenders * kPerSender;

  thread::Mutex mu;
  thread::CondVar cv;
  std::vector<std::vector<int>> seen(kSenders);
  int total = 0;
  // Somebody has to own the accepted stream: the transport does not hold it for
  // the handler's sake, and letting it go closes the connection.
  std::shared_ptr<WebSocketWireStream> accepted;

  WebSocketServerOptions server_options;
  server_options.port = 0;
  server_options.bind_address = "127.0.0.1";
  auto server = WebSocketWireServer::Create(
      [&](std::shared_ptr<WebSocketWireStream> stream) {
        accepted = stream;
        return stream->Accept(
            [&](std::optional<data::WireMessage> message) {
              if (message.has_value()) {
                thread::MutexLock lock(&mu);
                for (const data::NodeFragment& fragment :
                     message->node_fragments) {
                  const std::uint32_t tag = fragment.seq.value_or(0);
                  seen[tag >> 24U].push_back(
                      static_cast<int>(tag & 0xFFFFFFU));
                  ++total;
                }
                cv.SignalAll();
              }
              return a11::ReadyTask();
            },
            []() { return a11::ReadyTask(); });
      },
      server_options);
  ASSERT_TRUE(server.ok()) << server.status();
  auto port = (*server)->port();
  ASSERT_TRUE(port.ok()) << port.status();

  auto client = WebSocketWireStream::CreateClient(
      "ws://127.0.0.1:" + std::to_string(*port) + "/a11");
  ASSERT_TRUE(client.ok()) << client.status();
  ASSERT_TRUE(
      (*client)
          ->Start(
              [](std::optional<data::WireMessage>) { return a11::ReadyTask(); },
              []() { return a11::ReadyTask(); })
          .Await(absl::Now() + absl::Seconds(10))
          .ok());

  const std::string payload(256, 'x');
  std::vector<std::thread> senders;
  senders.reserve(kSenders);
  for (int index = 0; index < kSenders; ++index) {
    senders.emplace_back([&, index] {
      for (int count = 0; count < kPerSender; ++count) {
        data::WireMessage message{
            .node_fragments = {{
                .id = "node",
                .data = data::Chunk{.data = payload},
                .seq = static_cast<std::uint32_t>((index << 24U) | count),
                .continued = true,
            }}};
        const absl::Status sent = (*client)->Send(std::move(message));
        ASSERT_TRUE(sent.ok()) << sent;
      }
    });
  }
  for (std::thread& sender : senders) {
    sender.join();
  }

  {
    thread::MutexLock lock(&mu);
    const absl::Time limit = absl::Now() + absl::Seconds(30);
    while (total < kTotal && absl::Now() < limit) {
      cv.WaitWithDeadline(&mu, limit);
    }
  }
  thread::MutexLock lock(&mu);
  ASSERT_EQ(total, kTotal);
  for (int index = 0; index < kSenders; ++index) {
    ASSERT_EQ(seen[index].size(), kPerSender) << index;
    for (int count = 0; count < kPerSender; ++count) {
      ASSERT_EQ(seen[index][count], count) << index << " at " << count;
    }
  }
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
