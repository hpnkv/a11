// Copyright 2026 The A11 Authors.

#ifndef A11_NET_HTTP_SSE_WIRE_STREAM_H_
#define A11_NET_HTTP_SSE_WIRE_STREAM_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/http2.h"
#include "a11/net/in_process_wire_stream.h"
#include "a11/net/wire_stream.h"

namespace a11::net {

inline constexpr std::string_view kSseStreamIdHeader = "x-a11-stream-id";
inline constexpr std::string_view kSseHttpHeaderPrefix = "x-a11-http-";
inline constexpr std::string_view kDefaultSseConnectEndpoint = "/connect";
inline constexpr std::string_view kDefaultSseMessageEndpoint =
    "/streams/{id}/message";

struct HttpSseOptions {
  WireStreamOptions stream_options;
  Http2Options http2_options;
  std::string connect_endpoint = std::string(kDefaultSseConnectEndpoint);
  std::string message_endpoint = std::string(kDefaultSseMessageEndpoint);

  absl::Status Validate() const;
};

class HttpSseWireStream
    : public WireStream,
      public std::enable_shared_from_this<HttpSseWireStream> {
 public:
  ~HttpSseWireStream() override = default;

  using WireStream::HalfClose;
  using WireStream::SetDeadline;

  absl::Status Send(data::WireMessage message) override;
  a11::Task Start(OnMessage on_message, OnDone on_done) override;
  a11::Task Accept(OnMessage on_message, OnDone on_done) override;
  absl::Status HalfClose(data::ByteMap trailers) override;
  a11::Task DrainOutgoingMessages() override;
  absl::Status Abort(absl::Status status) override;
  absl::Status SetDeadline(absl::Time deadline) override;

  [[nodiscard]] absl::Time deadline() const override;
  [[nodiscard]] absl::Status GetStatus() const override;
  [[nodiscard]] std::optional<data::ByteMap> GetTrailers() const override;
  [[nodiscard]] std::string GetId() const override;
  [[nodiscard]] void* absl_nullable GetImpl() const override;

  [[nodiscard]] HttpHeaders GetHttpRequestHeaders() const;
  [[nodiscard]] std::optional<HttpHeaders> GetHttpResponseHeaders() const;
  absl::Status SetHttpRequestHeaders(HttpHeaders headers);
  absl::Status SetHttpResponseHeaders(HttpHeaders headers);
  a11::Task WaitForHttpHeaders() const;

 protected:
  enum class Role { kClient, kServer };
  struct State;

  HttpSseWireStream(Role role, std::string id, HttpSseOptions options,
                    InProcessWireStream::Pair pair,
                    std::shared_ptr<State> state);

  a11::Task StartEndpoint(bool accept, OnMessage on_message, OnDone on_done);
  a11::Task StartInternalBridge();
  a11::Task HandleBridgeMessage(std::optional<data::WireMessage> message);
  a11::Task HandleBridgeDone();
  a11::Task ReceiveTransportMessage(data::WireMessage message);
  void FailTransport(absl::Status status);
  void MarkHttpHeadersReady(HttpHeaders headers);
  void SetId(std::string id);
  [[nodiscard]] HttpSseOptions options() const;
  [[nodiscard]] std::shared_ptr<InProcessWireStream> bridge() const;

  virtual a11::Task OpenTransport() = 0;
  virtual absl::Status Transmit(data::WireMessage message) = 0;
  virtual void* absl_nullable TransportImpl() const = 0;

  virtual void TransportDone() {}

 private:
  Role role_;
  std::shared_ptr<InProcessWireStream> application_;
  std::shared_ptr<InProcessWireStream> bridge_;
  std::shared_ptr<State> state_;

  friend class HttpSseServer;
};

class HttpSseClientWireStream final : public HttpSseWireStream {
 private:
  struct ClientState;

  struct ConstructorToken {};

 public:
  static absl::StatusOr<std::shared_ptr<HttpSseClientWireStream>> Create(
      std::string url, HttpSseOptions options = {},
      std::shared_ptr<Http2Client> client = nullptr,
      HttpHeaders request_headers = {});

  [[nodiscard]] std::shared_ptr<Http2Client> client() const;

  HttpSseClientWireStream(ConstructorToken, std::string url,
                          HttpSseOptions options,
                          InProcessWireStream::Pair pair,
                          std::shared_ptr<State> state,
                          std::shared_ptr<ClientState> client_state);

 protected:
  a11::Task OpenTransport() override;
  absl::Status Transmit(data::WireMessage message) override;
  void* absl_nullable TransportImpl() const override;

 private:
  static void ReceiveSseLoop(
      const std::shared_ptr<HttpSseClientWireStream>& self);

  std::shared_ptr<ClientState> client_state_;
};

class HttpSseServer;

class HttpSseServerWireStream final : public HttpSseWireStream {
 private:
  struct ServerStreamState;

  struct ConstructorToken {};

 public:
  a11::Task Accepted() const;

  HttpSseServerWireStream(ConstructorToken, std::string id,
                          HttpSseOptions options,
                          InProcessWireStream::Pair pair,
                          std::shared_ptr<State> state,
                          std::shared_ptr<ServerStreamState> server_state);

 protected:
  a11::Task OpenTransport() override;
  absl::Status Transmit(data::WireMessage message) override;
  void* absl_nullable TransportImpl() const override;
  void TransportDone() override;

 private:
  std::shared_ptr<ServerStreamState> server_state_;

  friend class HttpSseServer;
};

using OnHttpSseConnect =
    std::function<a11::Task(std::shared_ptr<HttpSseServerWireStream>)>;

class HttpSseServer : public std::enable_shared_from_this<HttpSseServer> {
 public:
  static absl::StatusOr<std::shared_ptr<HttpSseServer>> Create(
      std::string bind_address, std::uint16_t port,
      OnHttpSseConnect on_connect = {}, HttpSseOptions options = {});

  ~HttpSseServer();

  a11::Future<std::shared_ptr<HttpSseServerWireStream>> WaitForStream();
  absl::Status Stop();
  [[nodiscard]] std::uint16_t port() const;
  [[nodiscard]] bool running() const;
  [[nodiscard]] std::shared_ptr<Http2Server> http2_server() const;

 private:
  struct State;

  explicit HttpSseServer(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  static a11::Task HandleRequest(const std::shared_ptr<State>& state,
                                 HttpRequest request,
                                 std::shared_ptr<Http2ResponseWriter> response);
  static a11::Task HandleConnect(const std::shared_ptr<State>& state,
                                 HttpRequest request,
                                 std::shared_ptr<Http2ResponseWriter> response);
  static a11::Task HandleMessage(const std::shared_ptr<State>& state,
                                 std::string stream_id, HttpRequest request,
                                 std::shared_ptr<Http2ResponseWriter> response);

  std::shared_ptr<State> state_;

  friend class HttpSseServerWireStream;
};

using HttpSseWireStreamServer = HttpSseServer;

}  // namespace a11::net

#endif  // A11_NET_HTTP_SSE_WIRE_STREAM_H_
