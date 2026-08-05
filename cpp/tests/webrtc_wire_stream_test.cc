// Copyright 2026 The A11 Authors.

#include "a11/net/webrtc_wire_stream.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/websocket_signalling.h"
#include "a11/net/wire_stream.h"
#include "thread/boost_primitives.h"

namespace a11::net {
namespace {

struct WebRtcRecorder {
  thread::Mutex mu;
  std::vector<data::WireMessage> messages;
  bool half_closed = false;
  a11::Promise<a11::Unit> done;
};

OnMessage WebRtcOnMessage(const std::shared_ptr<WebRtcRecorder>& recorder) {
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

OnDone WebRtcOnDone(const std::shared_ptr<WebRtcRecorder>& recorder) {
  return [recorder]() {
    (void)recorder->done.SetValue(a11::Unit{});
    return a11::ReadyTask();
  };
}

TEST(WebRtcWireStreamTest, EstablishesAndFragmentsLargeWireMessages) {
  auto signalling = SignallingService::Create();
  auto server_recorder = std::make_shared<WebRtcRecorder>();
  auto accepted =
      std::make_shared<a11::Promise<std::shared_ptr<WebRtcWireStream>>>();
  auto server = WebRtcWireServer::Create(
      "server", signalling,
      [server_recorder, accepted](std::shared_ptr<WebRtcWireStream> stream) {
        return a11::SubmitTask(
            [server_recorder, accepted,
             stream = std::move(stream)]() mutable -> absl::Status {
              absl::Status status =
                  stream
                      ->Accept(WebRtcOnMessage(server_recorder),
                               WebRtcOnDone(server_recorder))
                      .Await(absl::Now() + absl::Seconds(10))
                      .status();
              if (!status.ok()) {
                (void)accepted->SetStatus(status);
                return status;
              }
              (void)accepted->SetValue(stream);
              return absl::OkStatus();
            });
      });
  ASSERT_TRUE(server.ok()) << server.status();

  auto client = WebRtcWireStream::CreateClient("client", "server", signalling);
  ASSERT_TRUE(client.ok()) << client.status();
  auto client_recorder = std::make_shared<WebRtcRecorder>();
  a11::Task client_started = (*client)->Start(WebRtcOnMessage(client_recorder),
                                              WebRtcOnDone(client_recorder));
  ASSERT_TRUE(client_started.Await(absl::Now() + absl::Seconds(10)).ok())
      << client_started.Await().status();
  auto server_stream =
      accepted->future().Await(absl::Now() + absl::Seconds(10));
  ASSERT_TRUE(server_stream.ok()) << server_stream.status();

  const std::string large_payload(220 * 1024, 'x');
  data::WireMessage large;
  large.node_fragments.push_back(data::NodeFragment{
      .id = "large", .data = data::Chunk{.data = large_payload}});
  ASSERT_TRUE((*client)->Send(std::move(large)).ok());
  data::WireMessage reply;
  reply.node_fragments.push_back(
      data::NodeFragment{.id = "reply", .data = data::Chunk{.data = "hello"}});
  ASSERT_TRUE((*server_stream)->Send(std::move(reply)).ok());

  ASSERT_TRUE((*client)->HalfClose({{"client", "done"}}).ok());
  ASSERT_TRUE((*server_stream)->HalfClose({{"server", "done"}}).ok());
  ASSERT_TRUE((*client)
                  ->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(10))
                  .ok());
  ASSERT_TRUE((*server_stream)
                  ->DrainOutgoingMessages()
                  .Await(absl::Now() + absl::Seconds(10))
                  .ok());
  ASSERT_TRUE(client_recorder->done.future()
                  .Await(absl::Now() + absl::Seconds(10))
                  .ok());
  ASSERT_TRUE(server_recorder->done.future()
                  .Await(absl::Now() + absl::Seconds(10))
                  .ok());
  EXPECT_TRUE((*client)->GetStatus().ok()) << (*client)->GetStatus();
  EXPECT_TRUE((*server_stream)->GetStatus().ok())
      << (*server_stream)->GetStatus();
  {
    thread::MutexLock lock(&server_recorder->mu);
    ASSERT_EQ(server_recorder->messages.size(), 1);
    auto chunk = server_recorder->messages[0].node_fragments[0].GetChunk();
    ASSERT_TRUE(chunk.ok());
    EXPECT_EQ((*chunk)->data, large_payload);
    EXPECT_TRUE(server_recorder->half_closed);
  }
  {
    thread::MutexLock lock(&client_recorder->mu);
    ASSERT_EQ(client_recorder->messages.size(), 1);
    EXPECT_TRUE(client_recorder->half_closed);
  }
  EXPECT_TRUE((*server)->Stop().ok());
}

TEST(WebRtcWireStreamTest, EstablishesThroughWebSocketSignallingBridge) {
  auto signalling = SignallingService::Create();
  WebSocketSignallingServerOptions signalling_options;
  signalling_options.bind_address = "127.0.0.1";
  signalling_options.port = 0;
  auto signalling_server =
      WebSocketSignallingServer::Create(signalling, signalling_options);
  ASSERT_TRUE(signalling_server.ok()) << signalling_server.status();

  auto server_recorder = std::make_shared<WebRtcRecorder>();
  auto accepted =
      std::make_shared<a11::Promise<std::shared_ptr<WebRtcWireStream>>>();
  auto server = WebRtcWireServer::Create(
      "server", signalling,
      [server_recorder, accepted](std::shared_ptr<WebRtcWireStream> stream) {
        return a11::SubmitTask(
            [server_recorder, accepted,
             stream = std::move(stream)]() mutable -> absl::Status {
              absl::Status status =
                  stream
                      ->Accept(WebRtcOnMessage(server_recorder),
                               WebRtcOnDone(server_recorder))
                      .Await(absl::Now() + absl::Seconds(10))
                      .status();
              if (!status.ok()) {
                return status;
              }
              (void)accepted->SetValue(stream);
              return absl::OkStatus();
            });
      });
  ASSERT_TRUE(server.ok()) << server.status();
  auto network_signalling = WebSocketSignallingClient::Connect(
      "ws://127.0.0.1:" + std::to_string((*signalling_server)->port()),
      "client");
  auto transport = network_signalling.Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(transport.ok()) << transport.status();
  auto client = WebRtcWireStream::CreateClient("server", *transport);
  ASSERT_TRUE(client.ok()) << client.status();
  auto client_recorder = std::make_shared<WebRtcRecorder>();
  ASSERT_TRUE((*client)
                  ->Start(WebRtcOnMessage(client_recorder),
                          WebRtcOnDone(client_recorder))
                  .Await(absl::Now() + absl::Seconds(10))
                  .ok());
  auto server_stream =
      accepted->future().Await(absl::Now() + absl::Seconds(10));
  ASSERT_TRUE(server_stream.ok()) << server_stream.status();

  data::WireMessage message;
  message.node_fragments.push_back(data::NodeFragment{
      .id = "network", .data = data::Chunk{.data = "signalled"}});
  ASSERT_TRUE((*client)->Send(std::move(message)).ok());
  ASSERT_TRUE((*client)->HalfClose().ok());
  ASSERT_TRUE((*server_stream)->HalfClose().ok());
  ASSERT_TRUE(server_recorder->done.future()
                  .Await(absl::Now() + absl::Seconds(10))
                  .ok());
  {
    thread::MutexLock lock(&server_recorder->mu);
    ASSERT_EQ(server_recorder->messages.size(), 1);
  }
  EXPECT_TRUE((*server)->Stop().ok());
  EXPECT_TRUE((*signalling_server)->Stop().ok());
}

}  // namespace
}  // namespace a11::net
