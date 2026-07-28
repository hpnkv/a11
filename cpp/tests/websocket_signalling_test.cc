// Copyright 2026 The A11 Authors.

#include "a11/net/websocket_signalling.h"

#include <memory>
#include <string>

#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/future.h"
#include "a11/net/signalling.h"

namespace a11::net {
namespace {

TEST(WebSocketSignallingTest, BridgesNetworkClientToProgrammaticService) {
  auto service = SignallingService::Create();
  WebSocketSignallingServerOptions options;
  options.bind_address = "127.0.0.1";
  options.port = 0;
  auto server = WebSocketSignallingServer::Create(service, options);
  ASSERT_TRUE(server.ok()) << server.status();
  ASSERT_NE((*server)->port(), 0);
  auto delivered = std::make_shared<a11::Promise<SignallingMessage>>();
  auto receiver =
      service->Connect("receiver", [delivered](SignallingMessage message) {
        (void)delivered->SetValue(std::move(message));
        return a11::ReadyTask();
      });
  ASSERT_TRUE(receiver.ok()) << receiver.status();

  auto client =
      WebSocketSignallingClient::Connect(
          "ws://127.0.0.1:" + std::to_string((*server)->port()), "client")
          .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(client.ok()) << client.status() << " port=" << (*server)->port();
  ASSERT_TRUE(
      (*client)
          ->Send(SignallingMessage{
              .type = SignallingMessageType::kCandidate,
              .recipient = "receiver",
              .candidate = "candidate:1 1 UDP 1 127.0.0.1 1234 typ host",
              .mid = "0"})
          .ok());
  auto message = delivered->future().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(message.ok()) << message.status();
  EXPECT_EQ(message->sender, "client");
  EXPECT_EQ(message->recipient, "receiver");
  EXPECT_EQ(message->mid, "0");
  EXPECT_TRUE((*client)->Close().ok());
  EXPECT_TRUE((*server)->Stop().ok());
}

}  // namespace
}  // namespace a11::net
