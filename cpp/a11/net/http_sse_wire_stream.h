// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief WireStream transport over HTTP/2 Server-Sent Events.
 *
 * An HttpSseWireStream carries A11 WireMessage traffic over an HTTP/2 channel:
 * the server streams messages back as Server-Sent Events while the client
 * posts outgoing messages to a companion endpoint. Pick this transport when
 * only ordinary HTTP is available -- it is a firewall- and proxy-friendly
 * alternative to WebSockets. HttpSseClientWireStream dials out,
 * HttpSseServerWireStream is the accepted counterpart, and HttpSseServer hosts
 * them. Delivery carries no global ordering guarantee but is synchronised on
 * closure -- a reader observes every delivered message before the stream
 * completes.
 */

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

/** Response header naming the stream id assigned to an SSE connection. */
inline constexpr std::string_view kSseStreamIdHeader = "X-A11-Stream-Id";
/** Prefix under which application HTTP headers are tunneled over SSE. */
inline constexpr std::string_view kSseHttpHeaderPrefix = "x-a11-http-";
/** Default path on which a client opens the SSE event stream. */
inline constexpr std::string_view kDefaultSseConnectEndpoint = "/connect";
/** Default message-post path template (`{id}` is the stream id). */
inline constexpr std::string_view kDefaultSseMessageEndpoint =
    "/streams/{id}/message";

/**
 * @brief Endpoint paths and transport tuning for an HTTP SSE wire stream.
 *
 * `connect_endpoint` opens the SSE event stream; `message_endpoint` (a path
 * template containing the stream id) receives posted outbound messages.
 * `http2_options` and `stream_options` tune the underlying transport.
 */
struct HttpSseOptions {
  WireStreamOptions stream_options;
  Http2Options http2_options;
  std::string connect_endpoint = std::string(kDefaultSseConnectEndpoint);
  std::string message_endpoint = std::string(kDefaultSseMessageEndpoint);
  std::string cors_allow_origin;
  std::string cors_allow_methods;
  std::string cors_allow_headers;

  /** @return OK if the options are internally consistent. */
  absl::Status Validate() const;
};

/**
 * @brief Common base for the client and server HTTP SSE wire streams.
 *
 * Implements the WireStream interface on top of an internal in-process bridge,
 * delegating the actual HTTP/2 transport to the concrete subclass. It also
 * exposes the transport-level HTTP request/response headers so agents can
 * attach or inspect auth and routing metadata.
 */
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

  /** @return The HTTP headers carried on the underlying SSE request. */
  [[nodiscard]] HttpHeaders GetHttpRequestHeaders() const;
  /** @return The negotiated SSE response headers, or nullopt if not yet
   * arrived. Prefer awaiting WaitForHttpHeaders() first. */
  [[nodiscard]] std::optional<HttpHeaders> GetHttpResponseHeaders() const;
  /** Sets HTTP headers to send on the SSE request; call before connecting. */
  absl::Status SetHttpRequestHeaders(HttpHeaders headers);
  /** Sets HTTP headers to send on the SSE response (server side). */
  absl::Status SetHttpResponseHeaders(HttpHeaders headers);
  /** @return An awaitable that resolves once HTTP headers have been
   * exchanged. */
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

/**
 * @brief The client-side HTTP SSE wire stream, dialing out to a server URL.
 *
 * The transport an agent uses to exchange messages with a remote service over
 * HTTP/2 SSE. The connection opens lazily and runs asynchronously.
 */
class HttpSseClientWireStream final : public HttpSseWireStream {
 private:
  struct ClientState;

  struct ConstructorToken {};

 public:
  /**
   * @brief Creates a client SSE wire stream connecting to @p url.
   *
   * @param url The server URL to connect to.
   * @param options Endpoint paths and transport tuning.
   * @param client Optional existing Http2Client to reuse; a new one is created
   *     when null.
   * @param request_headers Extra HTTP headers to send on the connect request.
   * @return The stream, or an error status.
   */
  static absl::StatusOr<std::shared_ptr<HttpSseClientWireStream>> Create(
      std::string url, HttpSseOptions options = {},
      std::shared_ptr<Http2Client> client = nullptr,
      HttpHeaders request_headers = {});

  /** @return The HTTP/2 client backing this stream (reusable for more
   * streams). */
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

/**
 * @brief The server-side HTTP SSE wire stream, accepted from a client.
 *
 * Delivered to an HttpSseServer's OnHttpSseConnect handler (or via
 * WaitForStream) when a client opens an SSE connection.
 */
class HttpSseServerWireStream final : public HttpSseWireStream {
 private:
  struct ServerStreamState;

  struct ConstructorToken {};

 public:
  /** @return An awaitable that resolves once the stream is fully established
   * and ready for the server to send on. */
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

/** Callback invoked with each accepted server-side SSE wire stream. */
using OnHttpSseConnect =
    std::function<a11::Task(std::shared_ptr<HttpSseServerWireStream>)>;

/**
 * @brief Hosts HTTP SSE wire streams on top of an HTTP/2 server.
 *
 * Accepts A11 clients connecting over HTTP/2 SSE. Either supply an on_connect
 * callback invoked per stream, or pull streams explicitly with WaitForStream().
 */
class HttpSseServer : public std::enable_shared_from_this<HttpSseServer> {
 public:
  /**
   * @brief Creates and starts an SSE server accepting A11 wire streams.
   *
   * @param bind_address Local address to bind to.
   * @param port TCP port to listen on; 0 selects an ephemeral port.
   * @param on_connect Optional callback invoked for each accepted stream.
   * @param options Endpoint paths and transport tuning.
   * @return The running server, or an error status.
   */
  static absl::StatusOr<std::shared_ptr<HttpSseServer>> Create(
      std::string bind_address, std::uint16_t port,
      OnHttpSseConnect on_connect = {}, HttpSseOptions options = {});

  ~HttpSseServer();

  /** @return An awaitable resolving to the next incoming SSE wire stream. */
  a11::Future<std::shared_ptr<HttpSseServerWireStream>> WaitForStream();
  /** Stops the server and releases its resources. */
  absl::Status Stop();
  /** @return The port listened on, resolved even for an ephemeral 0. */
  [[nodiscard]] std::uint16_t port() const;
  /** @return Whether the server is currently running. */
  [[nodiscard]] bool running() const;
  /** @return The underlying HTTP/2 server. */
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

/** Alias for HttpSseServer, spelled to match the WireStream naming family. */
using HttpSseWireStreamServer = HttpSseServer;

}  // namespace a11::net

#endif  // A11_NET_HTTP_SSE_WIRE_STREAM_H_
