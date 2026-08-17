// Copyright 2026 The A11 Authors.

#include "a11/net/websocket_wire_stream.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/base/thread_annotations.h>
#include <absl/random/random.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>

#include "a11/concurrency/future.h"
#include "a11/net/channel_wire_stream.h"
#include "a11/net/http/url.h"
#include "a11/net/http2.h"
#include "a11/net/internal/binary_channel.h"
#include "a11/net/internal/exception_guarded_callbacks.h"
#include "a11/net/internal/http2_websocket_channel.h"
#include "a11/net/wire_stream.h"
#include "thread/boost_primitives.h"

namespace a11::net {
namespace {


std::string NewWebSocketId() {
  absl::BitGen generator;
  return absl::StrFormat("ws-%016x%016x",
                         absl::Uniform<std::uint64_t>(generator),
                         absl::Uniform<std::uint64_t>(generator));
}

/** Parses a `ws`/`wss` URL, rejecting the schemes this transport cannot dial. */
absl::StatusOr<ParsedUrl> ParseWebSocketUrl(std::string_view url) {
  ABSL_ASSIGN_OR_RETURN(ParsedUrl parsed, ParseUrl(url));
  if (parsed.scheme != "ws" && parsed.scheme != "wss") {
    return absl::InvalidArgumentError(
        "WebSocket URL must start with ws:// or wss://");
  }
  return parsed;
}

}  // namespace

absl::Status WebSocketClientOptions::Validate() const {
  ABSL_RETURN_IF_ERROR(http2_options.Validate());
  ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(headers));
  return framing.Validate();
}

absl::StatusOr<std::shared_ptr<WebSocketWireStream>>
WebSocketWireStream::CreateClient(std::string url, WireStreamOptions options,
                                  WebSocketClientOptions websocket_options) {
  ABSL_RETURN_IF_ERROR(options.Validate());
  ABSL_ASSIGN_OR_RETURN(ParsedUrl parsed, ParseWebSocketUrl(url));
  websocket_options.http2_options.tls.enabled = parsed.secure();
  websocket_options.http2_options.deadline =
      std::min(websocket_options.http2_options.deadline, options.deadline);
  ABSL_RETURN_IF_ERROR(websocket_options.Validate());
  internal::Http2WebSocketClientConfig config{
      // target(), not path: a pathless URL must still request "/", and any
      // query belongs in the request line.
      .host = std::move(parsed.host),
      .port = parsed.port,
      .path = parsed.target(),
      .headers = std::move(websocket_options.headers),
      .http2_options = websocket_options.http2_options,
      .max_message_size = options.max_single_message_size,
  };
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<internal::BinaryChannel> channel,
      internal::MakeHttp2WebSocketClientChannel(std::move(config)));
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<State> state,
                        MakeState(std::move(channel), NewWebSocketId(),
                                  ChannelEndpointRole::kClient, {}, options,
                                  websocket_options.framing));
  return std::make_shared<WebSocketWireStream>(ConstructorToken{},
                                               std::move(state));
}

absl::StatusOr<std::shared_ptr<WebSocketWireStream>>
WebSocketWireStream::CreateAccepted(
    HttpRequest request, std::shared_ptr<Http2ResponseWriter> response,
    WireStreamOptions options, ChannelFramingOptions framing) {
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<internal::BinaryChannel> channel,
                        internal::MakeHttp2WebSocketServerChannel(
                            std::move(request), std::move(response),
                            options.max_single_message_size));
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<State> state,
      MakeState(std::move(channel), NewWebSocketId(),
                ChannelEndpointRole::kServer, {}, options, framing));
  return std::make_shared<WebSocketWireStream>(ConstructorToken{},
                                               std::move(state));
}

absl::Status WebSocketServerOptions::Validate() const {
  if (path.empty() || path.front() != '/') {
    return absl::InvalidArgumentError(
        "WebSocket server path must start with '/'");
  }
  if (bind_address.empty()) {
    return absl::InvalidArgumentError(
        "WebSocket bind_address must not be empty");
  }
  ABSL_RETURN_IF_ERROR(stream_options.Validate());
  ABSL_RETURN_IF_ERROR(framing.Validate());
  return http2_options.Validate();
}

struct WebSocketWireServer::State {
  State(OnWebSocketStream stream_callback, WebSocketServerOptions value_options)
      : on_stream(std::move(stream_callback)),
        options(std::move(value_options)) {}

  mutable thread::Mutex mu;
  const OnWebSocketStream on_stream;
  const WebSocketServerOptions options;
  std::shared_ptr<Http2Server> server ABSL_GUARDED_BY(mu);
};

absl::StatusOr<std::shared_ptr<WebSocketWireServer>>
WebSocketWireServer::Create(OnWebSocketStream on_stream,
                            WebSocketServerOptions options) {
  if (!on_stream) {
    return absl::InvalidArgumentError("on_stream must be callable");
  }
  ABSL_RETURN_IF_ERROR(options.Validate());
  // on_stream is the caller's, and every invocation happens on a connection
  // fibre of A11's, so it is guarded once here. See
  // net/internal/exception_guarded_callbacks.h.
  auto state = std::make_shared<State>(
      internal::GuardOnWebSocketStream(std::move(on_stream)),
      std::move(options));
  std::weak_ptr<State> weak = state;
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<Http2Server> server,
      Http2Server::Create(
          state->options.bind_address, state->options.port,
          [weak](HttpRequest request,
                 std::shared_ptr<Http2ResponseWriter> response) -> a11::Task {
            std::shared_ptr<State> active = weak.lock();
            if (active == nullptr)
              return a11::FailedTask(absl::CancelledError(
                  "WebSocket server is no longer available"));
            if (request.method != "CONNECT" ||
                request.protocol != "websocket" ||
                request.path != active->options.path) {
              absl::Status status = response->SendResponse(
                  404, {{"content-type", "text/plain; charset=utf-8"}},
                  "WebSocket endpoint not found");
              return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
            }
            absl::StatusOr<std::shared_ptr<WebSocketWireStream>> stream =
                WebSocketWireStream::CreateAccepted(
                    std::move(request), std::move(response),
                    active->options.stream_options, active->options.framing);
            if (!stream.ok())
              return a11::FailedTask(stream.status());
            // Guarded where the server adopted it; see
            // WebSocketWireServer::Create.
            return active->on_stream(std::move(*stream));
          },
          state->options.http2_options));
  {
    thread::MutexLock lock(&state->mu);
    state->server = std::move(server);
  }

  struct MakeSharedEnabler final : WebSocketWireServer {
    explicit MakeSharedEnabler(std::shared_ptr<State> state)
        : WebSocketWireServer(std::move(state)) {}
  };

  return std::make_shared<MakeSharedEnabler>(std::move(state));
}

WebSocketWireServer::~WebSocketWireServer() {
  (void)Stop();
}

absl::Status WebSocketWireServer::Stop() {
  std::shared_ptr<Http2Server> server;
  {
    thread::MutexLock lock(&state_->mu);
    server = std::move(state_->server);
  }
  return server == nullptr ? absl::OkStatus() : server->Stop();
}

absl::StatusOr<std::uint16_t> WebSocketWireServer::port() const {
  thread::MutexLock lock(&state_->mu);
  if (state_->server == nullptr) {
    return absl::FailedPreconditionError("WebSocket server is stopped");
  }
  return state_->server->port();
}

bool WebSocketWireServer::running() const {
  thread::MutexLock lock(&state_->mu);
  return state_->server != nullptr && state_->server->running();
}

void* absl_nullable WebSocketWireServer::GetImpl() const {
  thread::MutexLock lock(&state_->mu);
  return state_->server == nullptr ? nullptr : state_->server->GetImpl();
}

}  // namespace a11::net
