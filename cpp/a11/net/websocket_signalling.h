// Copyright 2026 The A11 Authors.

#ifndef A11_NET_WEBSOCKET_SIGNALLING_H_
#define A11_NET_WEBSOCKET_SIGNALLING_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/net/http2.h"
#include "a11/net/signalling.h"

namespace a11::net {

struct WebSocketSignallingClientOptions {
  Http2Options http2_options;
  absl::Time deadline = absl::InfiniteFuture();
  size_t max_message_size = 1024 * 1024;

  absl::Status Validate() const;
};

class WebSocketSignallingClient final
    : public SignallingTransport,
      public std::enable_shared_from_this<WebSocketSignallingClient> {
 private:
  struct State;

  struct ConstructorToken {};

 public:
  static a11::Future<std::shared_ptr<WebSocketSignallingClient>> Connect(
      std::string url, std::string identity,
      OnSignallingMessage on_message = {},
      WebSocketSignallingClientOptions options = {});

  ~WebSocketSignallingClient() override;

  absl::Status Send(SignallingMessage message) override;
  absl::Status SetOnMessage(OnSignallingMessage on_message) override;
  absl::Status Close() override;
  [[nodiscard]] std::string identity() const override;
  [[nodiscard]] bool connected() const override;
  [[nodiscard]] absl::Status GetStatus() const override;
  [[nodiscard]] void* absl_nullable GetImpl() const;

  explicit WebSocketSignallingClient(ConstructorToken,
                                     std::shared_ptr<State> state)
      : state_(std::move(state)) {}

 private:
  static void Pump(const std::shared_ptr<State>& state);
  static void Fail(const std::shared_ptr<State>& state, absl::Status status);

  std::shared_ptr<State> state_;
};

struct WebSocketSignallingServerOptions {
  std::string path_prefix = "/";
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 0;
  Http2Options http2_options;
  size_t max_message_size = 1024 * 1024;

  absl::Status Validate() const;
};

class WebSocketSignallingServer
    : public std::enable_shared_from_this<WebSocketSignallingServer> {
 public:
  static absl::StatusOr<std::shared_ptr<WebSocketSignallingServer>> Create(
      std::shared_ptr<SignallingService> service,
      WebSocketSignallingServerOptions options = {});

  ~WebSocketSignallingServer();

  absl::Status Stop();
  [[nodiscard]] std::uint16_t port() const;
  [[nodiscard]] bool running() const;
  [[nodiscard]] std::shared_ptr<SignallingService> service() const;
  [[nodiscard]] void* absl_nullable GetImpl() const;

 private:
  struct State;

  explicit WebSocketSignallingServer(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  static void Remove(const std::shared_ptr<State>& state, std::string identity);

  std::shared_ptr<State> state_;
};

}  // namespace a11::net

#endif  // A11_NET_WEBSOCKET_SIGNALLING_H_
