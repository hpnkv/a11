// Copyright 2026 The A11 Authors.

#include "a11/net/websocket_signalling.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/base/thread_annotations.h>
#include <absl/container/flat_hash_map.h>
#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/http/url.h"
#include "a11/net/http2.h"
#include "a11/net/internal/binary_channel.h"
#include "a11/net/internal/http2_websocket_channel.h"
#include "a11/net/signalling.h"
#include "thread/boost_primitives.h"

namespace a11::net {
namespace {

absl::StatusOr<std::string> IdentityUrl(std::string url,
                                        std::string_view identity) {
  if (!absl::StartsWith(url, "ws://") && !absl::StartsWith(url, "wss://")) {
    return absl::InvalidArgumentError(
        "WebSocket signalling URL must start with ws:// or wss://");
  }
  if (const size_t marker = url.find("{id}"); marker != std::string::npos) {
    url.replace(marker, 4, identity);
    return url;
  }
  if (url.empty() || url.back() != '/') {
    url.push_back('/');
  }
  url.append(identity);
  return url;
}

/** Parses a `ws`/`wss` signalling URL, rejecting other schemes. */
absl::StatusOr<ParsedUrl> ParseWebSocketUrl(std::string_view url) {
  ABSL_ASSIGN_OR_RETURN(ParsedUrl parsed, ParseUrl(url));
  if (parsed.scheme != "ws" && parsed.scheme != "wss") {
    return absl::InvalidArgumentError(
        "WebSocket signalling URL must start with ws:// or wss://");
  }
  return parsed;
}

absl::Status CallbackFailure(const std::exception& error) {
  return absl::UnknownError(
      absl::StrCat("WebSocket signalling callback raised: ", error.what()));
}

}  // namespace

absl::Status WebSocketSignallingClientOptions::Validate() const {
  if (deadline <= absl::InfinitePast()) {
    return absl::InvalidArgumentError("signalling deadline is invalid");
  }
  if (max_message_size == 0) {
    return absl::InvalidArgumentError(
        "signalling max_message_size must be positive");
  }
  return http2_options.Validate();
}

struct WebSocketSignallingClient::State {
  explicit State(std::string value_identity)
      : identity(std::move(value_identity)),
        startup_promise(
            std::make_shared<
                a11::Promise<std::shared_ptr<WebSocketSignallingClient>>>()),
        startup(startup_promise->future()) {}

  mutable thread::Mutex mu;
  const std::string identity;
  std::shared_ptr<internal::BinaryChannel> channel ABSL_GUARDED_BY(mu);
  std::shared_ptr<WebSocketSignallingClient> pending_client ABSL_GUARDED_BY(mu);
  OnSignallingMessage on_message ABSL_GUARDED_BY(mu);
  std::deque<SignallingMessage> incoming ABSL_GUARDED_BY(mu);
  bool pumping ABSL_GUARDED_BY(mu) = false;
  bool connected ABSL_GUARDED_BY(mu) = false;
  bool closed ABSL_GUARDED_BY(mu) = false;
  absl::Status status ABSL_GUARDED_BY(mu);
  const std::shared_ptr<
      a11::Promise<std::shared_ptr<WebSocketSignallingClient>>>
      startup_promise;
  const a11::Future<std::shared_ptr<WebSocketSignallingClient>> startup;
};

a11::Future<std::shared_ptr<WebSocketSignallingClient>>
WebSocketSignallingClient::Connect(std::string url, std::string identity,
                                   OnSignallingMessage on_message,
                                   WebSocketSignallingClientOptions options) {
  absl::Status validation = data::ValidateName(identity);
  if (!validation.ok()) {
    return a11::FailedFuture<std::shared_ptr<WebSocketSignallingClient>>(
        validation);
  }
  validation = options.Validate();
  if (!validation.ok()) {
    return a11::FailedFuture<std::shared_ptr<WebSocketSignallingClient>>(
        validation);
  }
  absl::StatusOr<std::string> identity_url =
      IdentityUrl(std::move(url), identity);
  if (!identity_url.ok()) {
    return a11::FailedFuture<std::shared_ptr<WebSocketSignallingClient>>(
        identity_url.status());
  }
  absl::StatusOr<ParsedUrl> parsed = ParseWebSocketUrl(*identity_url);
  if (!parsed.ok()) {
    return a11::FailedFuture<std::shared_ptr<WebSocketSignallingClient>>(
        parsed.status());
  }
  if (options.deadline <= absl::Now()) {
    return a11::FailedFuture<std::shared_ptr<WebSocketSignallingClient>>(
        absl::DeadlineExceededError(
            "WebSocket signalling connect deadline exceeded"));
  }
  if (!on_message) {
    on_message = [](SignallingMessage) {
      return a11::ReadyTask();
    };
  }
  options.http2_options.tls.enabled = parsed->secure();
  options.http2_options.deadline =
      std::min(options.http2_options.deadline, options.deadline);
  internal::Http2WebSocketClientConfig config{
      .host = std::move(parsed->host),
      .port = parsed->port,
      .path = parsed->target(),
      .headers = {},
      .http2_options = options.http2_options,
      .max_message_size = options.max_message_size,
  };
  absl::StatusOr<std::shared_ptr<internal::BinaryChannel>> channel =
      internal::MakeHttp2WebSocketClientChannel(std::move(config));
  if (!channel.ok()) {
    return a11::FailedFuture<std::shared_ptr<WebSocketSignallingClient>>(
        channel.status());
  }

  auto state = std::make_shared<State>(std::move(identity));
  {
    thread::MutexLock lock(&state->mu);
    state->channel = std::move(*channel);
    state->on_message = std::move(on_message);
  }
  auto client =
      std::make_shared<WebSocketSignallingClient>(ConstructorToken{}, state);
  std::shared_ptr<internal::BinaryChannel> active_channel;
  {
    thread::MutexLock lock(&state->mu);
    state->pending_client = client;
    active_channel = state->channel;
  }
  std::weak_ptr<State> weak = state;
  internal::BinaryChannelCallbacks callbacks{
      .on_open =
          [weak]() {
            std::shared_ptr<State> state = weak.lock();
            if (state == nullptr) {
              return;
            }
            std::shared_ptr<WebSocketSignallingClient> client;
            {
              thread::MutexLock lock(&state->mu);
              if (state->closed) {
                return;
              }
              state->connected = true;
              client = std::move(state->pending_client);
            }
            if (client != nullptr) {
              (void)state->startup_promise->SetValue(std::move(client));
            }
          },
      .on_message =
          [weak](std::string raw) {
            std::shared_ptr<State> state = weak.lock();
            if (state == nullptr) {
              return;
            }
            absl::StatusOr<SignallingMessage> message =
                SignallingMessage::FromJson(raw);
            if (!message.ok()) {
              Fail(state, message.status());
              return;
            }
            bool start = false;
            bool wrong_recipient = false;
            {
              thread::MutexLock lock(&state->mu);
              if (state->closed) {
                return;
              }
              if (!message->recipient.empty() &&
                  message->recipient != state->identity) {
                wrong_recipient = true;
              } else {
                state->incoming.push_back(std::move(*message));
                if (!state->pumping) {
                  state->pumping = true;
                  start = true;
                }
              }
            }
            if (wrong_recipient) {
              Fail(state, absl::PermissionDeniedError(
                              "Signalling message has the wrong recipient"));
              return;
            }
            if (start) {
              a11::Schedule([state]() { Pump(state); });
            }
          },
      .on_error =
          [weak](absl::Status status) {
            if (std::shared_ptr<State> state = weak.lock(); state != nullptr) {
              Fail(state, std::move(status));
            }
          },
      .on_closed =
          [weak]() {
            if (std::shared_ptr<State> state = weak.lock(); state != nullptr) {
              Fail(state,
                   absl::UnavailableError("Signalling WebSocket was closed"));
            }
          },
      .on_buffered_amount_low = []() {},
  };
  validation = active_channel->SetCallbacks(std::move(callbacks));
  if (!validation.ok()) {
    Fail(state, validation);
    return state->startup;
  }
  a11::Schedule([state, active_channel = std::move(active_channel)]() {
    absl::Status opened = active_channel->Open();
    if (!opened.ok()) {
      Fail(state, std::move(opened));
    }
  });
  return state->startup;
}

void WebSocketSignallingClient::Pump(const std::shared_ptr<State>& state) {
  while (true) {
    SignallingMessage message;
    OnSignallingMessage callback;
    {
      thread::MutexLock lock(&state->mu);
      if (state->closed || state->incoming.empty()) {
        state->pumping = false;
        return;
      }
      message = std::move(state->incoming.front());
      state->incoming.pop_front();
      callback = state->on_message;
    }
    absl::Status status;
    try {
      status = callback(std::move(message)).Await().status();
    } catch (const std::exception& error) {
      status = CallbackFailure(error);
    } catch (...) {
      status = absl::UnknownError(
          "WebSocket signalling callback raised an exception");
    }
    if (!status.ok()) {
      Fail(state, std::move(status));
      return;
    }
  }
}

void WebSocketSignallingClient::Fail(const std::shared_ptr<State>& state,
                                     absl::Status status) {
  if (status.ok()) {
    status = absl::UnknownError("Signalling WebSocket failed");
  }
  std::shared_ptr<internal::BinaryChannel> channel;
  {
    thread::MutexLock lock(&state->mu);
    if (state->closed) {
      return;
    }
    state->closed = true;
    state->connected = false;
    state->status = status;
    state->incoming.clear();
    state->pending_client.reset();
    channel = state->channel;
  }
  (void)state->startup_promise->SetStatus(status);
  if (channel != nullptr) {
    (void)channel->ResetCallbacks();
    (void)channel->Close();
  }
}

WebSocketSignallingClient::~WebSocketSignallingClient() {
  (void)Close();
}

absl::Status WebSocketSignallingClient::Send(SignallingMessage message) {
  std::shared_ptr<internal::BinaryChannel> channel;
  {
    thread::MutexLock lock(&state_->mu);
    if (!state_->connected || state_->closed) {
      return state_->status.ok() ? absl::FailedPreconditionError(
                                       "Signalling WebSocket is not connected")
                                 : state_->status;
    }
    if (!message.sender.empty() && message.sender != state_->identity) {
      return absl::PermissionDeniedError(
          "A signalling client cannot impersonate another identity");
    }
    message.sender = state_->identity;
    channel = state_->channel;
  }
  ABSL_ASSIGN_OR_RETURN(std::string encoded, message.ToJson());
  return channel->Send(std::move(encoded));
}

absl::Status WebSocketSignallingClient::SetOnMessage(
    OnSignallingMessage on_message) {
  if (!on_message) {
    return absl::InvalidArgumentError(
        "Signalling on_message callback must be callable");
  }
  thread::MutexLock lock(&state_->mu);
  if (state_->closed) {
    return state_->status;
  }
  state_->on_message = std::move(on_message);
  return absl::OkStatus();
}

absl::Status WebSocketSignallingClient::Close() {
  std::shared_ptr<internal::BinaryChannel> channel;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->closed) {
      return absl::OkStatus();
    }
    state_->closed = true;
    state_->connected = false;
    state_->status = absl::CancelledError("Signalling WebSocket closed");
    channel = state_->channel;
  }
  (void)state_->startup_promise->SetStatus(
      absl::CancelledError("Signalling WebSocket closed"));
  if (channel == nullptr) {
    return absl::OkStatus();
  }
  (void)channel->ResetCallbacks();
  return channel->Close();
}

std::string WebSocketSignallingClient::identity() const {
  return state_->identity;
}

bool WebSocketSignallingClient::connected() const {
  thread::MutexLock lock(&state_->mu);
  return state_->connected && !state_->closed;
}

absl::Status WebSocketSignallingClient::GetStatus() const {
  thread::MutexLock lock(&state_->mu);
  return state_->status;
}

void* absl_nullable WebSocketSignallingClient::GetImpl() const {
  thread::MutexLock lock(&state_->mu);
  return state_->channel == nullptr ? nullptr : state_->channel->GetImpl();
}

absl::Status WebSocketSignallingServerOptions::Validate() const {
  if (path_prefix.empty() || path_prefix.front() != '/' ||
      path_prefix.back() != '/') {
    return absl::InvalidArgumentError(
        "WebSocket signalling path_prefix must start and end with '/'");
  }
  if (bind_address.empty()) {
    return absl::InvalidArgumentError("signalling bind_address is empty");
  }
  if (max_message_size == 0) {
    return absl::InvalidArgumentError(
        "signalling max_message_size must be positive");
  }
  return http2_options.Validate();
}

struct WebSocketSignallingServer::State {
  struct Connection {
    std::shared_ptr<internal::BinaryChannel> channel;
    std::shared_ptr<SignallingEndpoint> endpoint;
  };

  State(std::shared_ptr<SignallingService> value_service,
        WebSocketSignallingServerOptions value_options)
      : service(std::move(value_service)), options(std::move(value_options)) {}

  mutable thread::Mutex mu;
  bool running ABSL_GUARDED_BY(mu) = true;
  const std::shared_ptr<SignallingService> service;
  const WebSocketSignallingServerOptions options;
  std::shared_ptr<Http2Server> server ABSL_GUARDED_BY(mu);
  absl::flat_hash_map<std::string, Connection> connections ABSL_GUARDED_BY(mu);
};

absl::StatusOr<std::shared_ptr<WebSocketSignallingServer>>
WebSocketSignallingServer::Create(std::shared_ptr<SignallingService> service,
                                  WebSocketSignallingServerOptions options) {
  if (service == nullptr) {
    return absl::InvalidArgumentError(
        "WebSocket signalling service must not be null");
  }
  ABSL_RETURN_IF_ERROR(options.Validate());
  auto state = std::make_shared<State>(std::move(service), std::move(options));
  std::weak_ptr<State> weak = state;
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<Http2Server> server,
      Http2Server::Create(
          state->options.bind_address, state->options.port,
          [weak](HttpRequest request,
                 std::shared_ptr<Http2ResponseWriter> response) -> a11::Task {
            std::shared_ptr<State> state = weak.lock();
            if (state == nullptr)
              return a11::FailedTask(
                  absl::CancelledError("Signalling server stopped"));
            if (request.method != "CONNECT" ||
                request.protocol != "websocket" ||
                !absl::StartsWith(request.path, state->options.path_prefix) ||
                request.path.size() <= state->options.path_prefix.size()) {
              absl::Status status = response->SendResponse(
                  404, {{"content-type", "text/plain; charset=utf-8"}},
                  "Signalling endpoint not found");
              return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
            }
            std::string identity =
                request.path.substr(state->options.path_prefix.size());
            const size_t suffix = identity.find_first_of("?#");
            if (suffix != std::string::npos)
              identity.erase(suffix);
            absl::Status validation = data::ValidateName(identity);
            if (!validation.ok())
              return a11::FailedTask(validation);

            absl::StatusOr<std::shared_ptr<internal::BinaryChannel>> channel =
                internal::MakeHttp2WebSocketServerChannel(
                    std::move(request), std::move(response),
                    state->options.max_message_size);
            if (!channel.ok())
              return a11::FailedTask(channel.status());
            std::weak_ptr<internal::BinaryChannel> weak_channel = *channel;
            absl::StatusOr<std::shared_ptr<SignallingEndpoint>> endpoint =
                state->service->Connect(
                    identity, [weak_channel](SignallingMessage message) {
                      std::shared_ptr<internal::BinaryChannel> channel =
                          weak_channel.lock();
                      if (channel == nullptr) {
                        return a11::FailedTask(absl::UnavailableError(
                            "Signalling WebSocket disconnected"));
                      }
                      absl::StatusOr<std::string> encoded = message.ToJson();
                      if (!encoded.ok())
                        return a11::FailedTask(encoded.status());
                      absl::Status sent = channel->Send(std::move(*encoded));
                      return sent.ok() ? a11::ReadyTask()
                                       : a11::FailedTask(sent);
                    });
            if (!endpoint.ok())
              return a11::FailedTask(endpoint.status());

            std::weak_ptr<State> state_weak = state;
            const std::string pinned_identity = identity;
            std::weak_ptr<SignallingEndpoint> endpoint_weak = *endpoint;
            internal::BinaryChannelCallbacks callbacks{
                .on_open = []() {},
                .on_message =
                    [state_weak, endpoint_weak,
                     pinned_identity](std::string raw) {
                      absl::StatusOr<SignallingMessage> message =
                          SignallingMessage::FromJson(raw);
                      std::shared_ptr<SignallingEndpoint> endpoint =
                          endpoint_weak.lock();
                      if (!message.ok() || endpoint == nullptr ||
                          (!message->sender.empty() &&
                           message->sender != pinned_identity)) {
                        if (std::shared_ptr<State> state = state_weak.lock();
                            state != nullptr)
                          Remove(state, pinned_identity);
                        return;
                      }
                      message->sender = pinned_identity;
                      if (!endpoint->Send(std::move(*message)).ok()) {
                        if (std::shared_ptr<State> state = state_weak.lock();
                            state != nullptr)
                          Remove(state, pinned_identity);
                      }
                    },
                .on_error =
                    [state_weak, pinned_identity](absl::Status status) {
                      LOG(ERROR) << "Signalling WebSocket for "
                                 << pinned_identity << " failed: " << status;
                      if (std::shared_ptr<State> state = state_weak.lock();
                          state != nullptr)
                        Remove(state, pinned_identity);
                    },
                .on_closed =
                    [state_weak, pinned_identity]() {
                      if (std::shared_ptr<State> state = state_weak.lock();
                          state != nullptr)
                        Remove(state, pinned_identity);
                    },
                .on_buffered_amount_low = []() {},
            };
            validation = (*channel)->SetCallbacks(std::move(callbacks));
            if (!validation.ok()) {
              (void)(*endpoint)->Close();
              return a11::FailedTask(validation);
            }
            {
              thread::MutexLock lock(&state->mu);
              if (!state->running) {
                (void)(*endpoint)->Close();
                return a11::FailedTask(
                    absl::CancelledError("Signalling server stopped"));
              }
              state->connections.insert_or_assign(
                  identity, State::Connection{.channel = *channel,
                                              .endpoint = *endpoint});
            }
            validation = (*channel)->Open();
            if (!validation.ok()) {
              Remove(state, identity);
              return a11::FailedTask(validation);
            }
            return a11::ReadyTask();
          },
          state->options.http2_options));
  {
    thread::MutexLock lock(&state->mu);
    state->server = std::move(server);
  }

  struct MakeSharedEnabler final : WebSocketSignallingServer {
    explicit MakeSharedEnabler(std::shared_ptr<State> state)
        : WebSocketSignallingServer(std::move(state)) {}
  };

  return std::make_shared<MakeSharedEnabler>(std::move(state));
}

void WebSocketSignallingServer::Remove(const std::shared_ptr<State>& state,
                                       std::string identity) {
  State::Connection connection;
  {
    thread::MutexLock lock(&state->mu);
    const auto found = state->connections.find(identity);
    if (found == state->connections.end()) {
      return;
    }
    connection = std::move(found->second);
    state->connections.erase(found);
  }
  if (connection.channel != nullptr) {
    (void)connection.channel->ResetCallbacks();
    (void)connection.channel->Close();
  }
  if (connection.endpoint != nullptr) {
    (void)connection.endpoint->Close();
  }
}

WebSocketSignallingServer::~WebSocketSignallingServer() {
  (void)Stop();
}

absl::Status WebSocketSignallingServer::Stop() {
  absl::flat_hash_map<std::string, State::Connection> connections;
  std::shared_ptr<Http2Server> server;
  {
    thread::MutexLock lock(&state_->mu);
    if (!state_->running) {
      return absl::OkStatus();
    }
    state_->running = false;
    connections.swap(state_->connections);
    server = std::move(state_->server);
  }
  for (auto& [identity, connection] : connections) {
    (void)identity;
    if (connection.channel != nullptr) {
      (void)connection.channel->ResetCallbacks();
      (void)connection.channel->Close();
    }
    if (connection.endpoint != nullptr) {
      (void)connection.endpoint->Close();
    }
  }
  return server == nullptr ? absl::OkStatus() : server->Stop();
}

std::uint16_t WebSocketSignallingServer::port() const {
  thread::MutexLock lock(&state_->mu);
  return state_->server == nullptr ? 0 : state_->server->port();
}

bool WebSocketSignallingServer::running() const {
  thread::MutexLock lock(&state_->mu);
  return state_->running && state_->server != nullptr &&
         state_->server->running();
}

std::shared_ptr<SignallingService> WebSocketSignallingServer::service() const {
  return state_->service;
}

void* absl_nullable WebSocketSignallingServer::GetImpl() const {
  thread::MutexLock lock(&state_->mu);
  return state_->server == nullptr ? nullptr : state_->server->GetImpl();
}

}  // namespace a11::net
