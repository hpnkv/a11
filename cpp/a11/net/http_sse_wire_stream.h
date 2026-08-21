// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief WireStream transport over HTTP Server-Sent Events.
 *
 * An HttpSseWireStream carries A11 WireMessage traffic over an HTTP channel:
 * the server streams messages back as Server-Sent Events while the client
 * posts outgoing messages to a companion endpoint. Pick this transport when
 * only ordinary HTTP is available -- it is a firewall- and proxy-friendly
 * alternative to WebSockets. HttpSseClientWireStream dials out,
 * HttpSseServerWireStream is the accepted counterpart, and HttpSseServer hosts
 * them. Delivery carries no global ordering guarantee but is synchronised on
 * closure -- a reader observes every delivered message before the stream
 * completes.
 *
 * ## HTTP/2 and HTTP/1.1
 *
 * Over HTTP/2 the event stream and the outbound direction are streams on one
 * connection. HTTP/1.1 has no multiplexing and an A11 client connection carries
 * a single request, so the outbound direction needs a connection of its own:
 * either one for all of it (a streamed request body, which a non-multiplexed
 * connection prefers whenever the server advertises it) or one per message
 * (POST delivery, the interop fallback). Nothing about the protocol changes --
 * only how many sockets it takes.
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
inline constexpr std::string_view kSseStreamIdHeader = "x-a11-stream-id";
/** Prefix under which application HTTP headers are tunneled over SSE. */
inline constexpr std::string_view kSseHttpHeaderPrefix = "x-a11-http-";
/**
 * Response header on the connect response listing the outbound delivery modes
 * the server accepts, comma-separated -- `post`, `stream`. A server that omits
 * it is understood to accept `post` only, which is what every deployed SSE
 * server did before streamed delivery existed.
 */
inline constexpr std::string_view kSseOutboundModesHeader = "x-a11-outbound";
/** Token for one-POST-per-message outbound delivery. */
inline constexpr std::string_view kSseOutboundPostToken = "post";
/** Token for long-lived-request-body outbound delivery. */
inline constexpr std::string_view kSseOutboundStreamToken = "stream";
/**
 * Content type of a streamed outbound request body.
 *
 * The body is a sequence of frames, each a four-byte little-endian payload
 * length followed by that many bytes of the JSON WireMessage encoding the POST
 * route carries one of. HTTP/2 sends it as DATA frames and HTTP/1.1 as a chunked
 * body; neither framing is a message boundary, which is why the length prefix is
 * there.
 */
inline constexpr std::string_view kSseWireStreamContentType =
    "application/vnd.a11.wire-stream+json";
/** Default path on which a client opens the SSE event stream. */
inline constexpr std::string_view kDefaultSseConnectEndpoint = "/connect";
/** Default message-post path template (`{id}` is the stream id). */
inline constexpr std::string_view kDefaultSseMessageEndpoint =
    "/streams/{id}/message";

/**
 * @brief How an SSE client hands its outbound WireMessages to the server.
 *
 * The inbound direction is always the SSE event stream. Only the outbound one
 * has a choice, and it is a reachability/throughput trade rather than a
 * correctness one -- the server accepts both, on the same endpoint, and a stream
 * and a series of POSTs deliver the same messages.
 */
enum class SseOutboundDelivery {
  /**
   * One HTTP POST per message.
   *
   * Universally reachable: a browser needs nothing but `fetch()` per message,
   * which is the whole reason this transport exists. A11 issues these
   * concurrently up to `max_concurrent_posts` -- WireMessages need no global
   * order -- and serialises only where order is load-bearing: everything
   * outstanding is delivered before a half-close, and an abort goes out ahead of
   * whatever is still in flight.
   */
  kPost,
  /**
   * One long-lived request body carrying every outbound message.
   *
   * HTTP/2 DATA frames or an HTTP/1.1 chunked body, framed as
   * kSseWireStreamContentType describes. Removes the one-request-per-message
   * ceiling, and ordering comes from the stream itself. Needs a client that can
   * write a request body incrementally, which `fetch()` cannot do portably --
   * so this is for C++, Python and other capable backends. Falls back to kPost
   * against a server that does not advertise `stream` in
   * kSseOutboundModesHeader.
   */
  kStream,
};

/**
 * @brief Endpoint paths and transport tuning for an HTTP SSE wire stream.
 *
 * A POST to `connect_endpoint` opens the SSE event stream;
 * `message_endpoint` (a path template containing the stream id) receives
 * outbound messages, either one per POST or as one streamed request body.
 * `http2_options` and `stream_options` tune the underlying transport.
 */
struct HttpSseOptions {
  WireStreamOptions stream_options;  ///< Per-stream buffering and deadline.
  Http2Options http2_options;  ///< Shared HTTP/2 transport and TLS policy.
  std::string connect_endpoint =
      std::string(kDefaultSseConnectEndpoint);  ///< SSE connect POST path.
  std::string message_endpoint =
      std::string(kDefaultSseMessageEndpoint);  ///< Outbound endpoint template.
  /// Client-side outbound delivery method; servers accept either.
  SseOutboundDelivery outbound = SseOutboundDelivery::kPost;
  /// Server-side: whether a streamed outbound request body is accepted, and so
  /// advertised in kSseOutboundModesHeader. Clearing it leaves clients with
  /// POST-per-message -- which is what an intermediary that will not carry a
  /// long-lived request body forces, and over HTTP/1.1 costs a connection per
  /// message.
  bool accept_streamed_outbound = true;
  /// Outbound POSTs a client keeps in flight at once (kPost only). One restores
  /// the strictly serialised behaviour; the bound is what stops a fast producer
  /// from spending every stream on the HTTP/2 connection at once.
  size_t max_concurrent_posts = 16;
  std::string cors_allow_origin;    ///< Access-Control-Allow-Origin value.
  std::string cors_allow_methods;   ///< Access-Control-Allow-Methods value.
  std::string cors_allow_headers;   ///< Access-Control-Allow-Headers value.
  std::string cors_expose_headers;  ///< Access-Control-Expose-Headers value.

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

  /**
   * @brief The outbound delivery method actually in use.
   *
   * Equals the requested HttpSseOptions::outbound once connected, except where a
   * kStream request fell back to kPost because the server did not advertise it.
   * Meaningful only after the transport has opened.
   */
  [[nodiscard]] SseOutboundDelivery outbound_delivery() const;

  HttpSseClientWireStream(ConstructorToken, std::string url,
                          HttpSseOptions options,
                          InProcessWireStream::Pair pair,
                          std::shared_ptr<State> state,
                          std::shared_ptr<ClientState> client_state);

 protected:
  a11::Task OpenTransport() override;
  absl::Status Transmit(data::WireMessage message) override;
  void* absl_nullable TransportImpl() const override;
  void TransportDone() override;

 private:
  static void ReceiveSseLoop(
      const std::shared_ptr<HttpSseClientWireStream>& self);

  /// Opens the streamed outbound request body, or leaves delivery on kPost when
  /// @p response_headers do not advertise the streamed mode.
  absl::Status OpenOutboundStream(const HttpHeaders& response_headers);
  /// Sends one already-encoded message on the streamed outbound body.
  absl::Status TransmitOnStream(std::string payload, bool terminal);
  /// Sends one already-encoded message as its own POST, awaiting the response.
  absl::Status TransmitAsPost(std::string payload);
  /// Blocks until an outbound POST slot is free, then claims it.
  absl::Status ClaimPostSlot();
  /// Releases a slot claimed by ClaimPostSlot and wakes anyone waiting.
  void ReleasePostSlot();
  /// Blocks until every outbound POST handed over so far has completed.
  absl::Status AwaitPostsDelivered();

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
  /// Reads a whole outbound direction off one streamed request body.
  static a11::Task HandleMessageStream(
      const std::shared_ptr<State>& state, std::string stream_id,
      HttpRequest request, std::shared_ptr<Http2ResponseWriter> response);

  std::shared_ptr<State> state_;

  friend class HttpSseServerWireStream;
};

/** Alias for HttpSseServer, spelled to match the WireStream naming family. */
using HttpSseWireStreamServer = HttpSseServer;

}  // namespace a11::net

#endif  // A11_NET_HTTP_SSE_WIRE_STREAM_H_
