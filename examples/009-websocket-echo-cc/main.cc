// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>
#include <memory>
#include <string>

#include <absl/log/log.h>
#include <absl/status/status_macros.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/websocket_wire_stream.h"
#include "a11/service/session.h"

namespace {
absl::Status Run() {
  a11::net::WebSocketServerOptions options;
  options.path = "/ws";
  ABSL_ASSIGN_OR_RETURN(
      auto server,
      a11::net::WebSocketWireServer::Create(
          [](std::shared_ptr<a11::net::WebSocketWireStream> stream) {
            return a11::Submit<a11::Unit>(
                [stream = std::move(stream)]() -> absl::StatusOr<a11::Unit> {
                  ABSL_ASSIGN_OR_RETURN(
                      auto session, a11::service::SessionWithRecv::Create());
                  ABSL_ASSIGN_OR_RETURN(
                      auto started,
                      session->AddStream(stream,
                                         a11::service::StreamMode::kAccept));
                  ABSL_RETURN_IF_ERROR(started.Await().status());
                  while (true) {
                    auto received = session->Receive().Await();
                    if (!received.ok()) {
                      session->Abort(received.status()).IgnoreError();
                      return received.status();
                    }
                    if (!received->has_value()) {
                      break;
                    }
                    ABSL_RETURN_IF_ERROR(session->Send(std::move(**received)));
                  }
                  ABSL_RETURN_IF_ERROR(session->HalfClose());
                  ABSL_RETURN_IF_ERROR(session->Done().Await().status());
                  ABSL_RETURN_IF_ERROR(session->GetStatus());
                  return a11::Unit{};
                });
          },
          options));
  ABSL_ASSIGN_OR_RETURN(auto port, server->port());
  ABSL_ASSIGN_OR_RETURN(auto session, a11::service::SessionWithRecv::Create());
  a11::net::WebSocketClientOptions client_options;
  client_options.headers.emplace_back("hello", "world");
  ABSL_ASSIGN_OR_RETURN(
      auto stream, a11::net::WebSocketWireStream::CreateClient(
                       "ws://127.0.0.1:" + std::to_string(port) + "/ws", {},
                       std::move(client_options)));
  ABSL_ASSIGN_OR_RETURN(auto started, session->AddStream(stream));
  ABSL_RETURN_IF_ERROR(started.Await().status());
  absl::Status result = absl::OkStatus();
  std::string text;
  while (std::cout << "> " << std::flush, std::getline(std::cin, text)) {
    if (text == "exit") {
      break;
    }
    a11::data::WireMessage message;
    message.node_fragments.push_back(a11::data::NodeFragment{
        .id = "test",
        .data =
            a11::data::Chunk{
                .metadata = a11::data::ChunkMetadata{.mimetype = "text/plain"},
                .ref = {},
                .data = std::move(text)},
        .seq = std::nullopt,
        .continued = false});
    result = session->Send(std::move(message));
    if (!result.ok()) {
      break;
    }
    auto received = session->Receive().Await();
    if (!received.ok()) {
      result = received.status();
      break;
    }
    if (!received->has_value()) {
      break;
    }
    for (const auto& fragment : (**received).node_fragments) {
      if (const auto* chunk = std::get_if<a11::data::Chunk>(&fragment.data)) {
        std::cout << "Echo: " << chunk->data << '\n';
      }
    }
  }
  if (result.ok()) {
    result = session->HalfClose();
  }
  if (!result.ok()) {
    session->Abort(result).IgnoreError();
  }
  const auto completed = session->Done().Await();
  if (result.ok()) {
    result = completed.status();
  }
  if (result.ok()) {
    result = session->GetStatus();
  }
  const auto stopped = server->Stop();
  if (result.ok()) {
    result = stopped;
  }
  return result;
}
}  // namespace

int main() {
  const absl::Status status = Run();
  if (!status.ok()) {
    LOG(ERROR) << status;
    return 1;
  }
  return 0;
}
