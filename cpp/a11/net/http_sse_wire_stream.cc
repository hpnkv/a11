// Copyright 2026 The A11 Authors.

#include "a11/net/http_sse_wire_stream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <absl/strings/str_split.h>
#include <absl/strings/strip.h>
#include <absl/time/clock.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/json.h"
#include "a11/data/msgpack.h"
#include "a11/data/types.h"
#include "a11/net/http/url.h"
#include "a11/net/http2.h"
#include "a11/net/in_process_wire_stream.h"
#include "a11/net/internal/exception_guarded_callbacks.h"
#include "a11/net/wire_stream.h"
#include "a11/status.h"
#include "a11/uuid.h"
#include "absl/strings/match.h"
#include "thread/boost_primitives.h"

namespace a11::net {
namespace {

/**
 * The SSE base path: the URL's path with any trailing slash removed, so that
 * joining an endpoint onto it never produces a double slash. The query is not
 * part of it -- ParseUrl keeps that separate, and SSE builds its own targets.
 */
std::string BasePathOf(const ParsedUrl& url) {
  std::string base_path = url.path;
  while (base_path.size() > 1 && base_path.back() == '/') {
    base_path.pop_back();
  }
  return base_path;
}

absl::StatusOr<std::string> ResolveEndpoint(std::string_view base_path,
                                            std::string_view endpoint) {
  if (endpoint.empty()) {
    return absl::InvalidArgumentError("HTTP SSE endpoint must not be empty");
  }
  if (endpoint.front() == '/') {
    return std::string(endpoint);
  }
  if (base_path.empty() || base_path == "/") {
    return absl::StrCat("/", endpoint);
  }
  return absl::StrCat(base_path, "/", endpoint);
}

absl::StatusOr<std::string> FormatMessageEndpoint(std::string endpoint,
                                                  std::string_view stream_id) {
  const size_t marker = endpoint.find("{id}");
  if (marker == std::string::npos) {
    return absl::InvalidArgumentError(
        "message_endpoint must contain an {id} placeholder");
  }
  endpoint.replace(marker, 4, stream_id);
  if (absl::StrContains(endpoint, "{id}")) {
    return absl::InvalidArgumentError(
        "message_endpoint must contain exactly one {id} placeholder");
  }
  return endpoint;
}

std::string NewSseId() {
  return NewStreamId("sse-");
}

absl::Status HttpStatusError(const HttpResponse& response,
                             std::string_view operation) {
  if (response.head.status >= 200 && response.head.status < 300) {
    return absl::OkStatus();
  }
  return {StatusCodeFromHttp(response.head.status),
          response.body.empty()
              ? absl::StrCat(operation, " returned HTTP ", response.head.status)
              : response.body};
}

a11::Task StatusTask(absl::Status status) {
  return status.ok() ? a11::ReadyTask() : a11::FailedTask(std::move(status));
}

bool IsTerminal(const data::WireMessage& message) {
  return data::IsHalfCloseMessage(message);
}

bool IsAbort(const data::WireMessage& message) {
  if (!IsTerminal(message)) {
    return false;
  }
  return message.headers.find(kAbortStatusHeader) != message.headers.end();
}

/** Fixed little-endian length prefix in front of every streamed message. */
constexpr size_t kStreamFramePrefix = sizeof(std::uint32_t);

std::string FrameStreamedMessage(const std::string& payload) {
  std::string framed;
  framed.reserve(kStreamFramePrefix + payload.size());
  auto length = static_cast<std::uint32_t>(payload.size());
  for (size_t index = 0; index < kStreamFramePrefix; ++index) {
    framed.push_back(static_cast<char>(length & 0xffU));
    length >>= 8U;
  }
  framed.append(payload);
  return framed;
}

std::uint32_t DecodeStreamFrameLength(std::string_view framed) {
  std::uint32_t length = 0;
  for (size_t index = 0; index < kStreamFramePrefix; ++index) {
    length |=
        static_cast<std::uint32_t>(static_cast<unsigned char>(framed[index]))
        << (index * 8U);
  }
  return length;
}

/** Whether a comma-separated mode list contains @p token. */
bool AdvertisesMode(std::string_view list, std::string_view token) {
  for (std::string_view mode : absl::StrSplit(list, ',')) {
    if (absl::EqualsIgnoreCase(absl::StripAsciiWhitespace(mode), token)) {
      return true;
    }
  }
  return false;
}

/**
 * The tunneled form of an outbound request's HTTP headers.
 *
 * Framing headers describe the request itself and are not application metadata;
 * every other field is namespaced under kSseHttpHeaderPrefix exactly once, and
 * repeats of one name are joined the way HTTP defines. Shared by the POST route
 * (once per message) and the streamed route (once for the whole stream, which
 * makes the same headers reach every message on it).
 */
absl::StatusOr<absl::flat_hash_map<std::string, std::string>> TunneledHeaders(
    const HttpHeaders& headers) {
  absl::flat_hash_map<std::string, std::string> combined;
  for (const auto& [name, value] : headers) {
    if (name == "content-type" || name == "content-length" ||
        name == "transfer-encoding" || name == kSseStreamIdHeader) {
      continue;
    }
    auto [iterator, inserted] = combined.emplace(name, value);
    if (!inserted) {
      iterator->second = absl::StrCat(iterator->second, ", ", value);
    }
  }
  absl::flat_hash_map<std::string, std::string> tunneled;
  tunneled.reserve(combined.size());
  for (auto& [name, value] : combined) {
    std::string wire_name = absl::StartsWith(name, kSseHttpHeaderPrefix)
                                ? name
                                : absl::StrCat(kSseHttpHeaderPrefix, name);
    ABSL_RETURN_IF_ERROR(data::ValidateName(wire_name));
    tunneled.insert_or_assign(std::move(wire_name), std::move(value));
  }
  return tunneled;
}

}  // namespace

struct HttpSseWireStream::State {
  State(std::string value_id, HttpSseOptions value_options)
      : id(std::move(value_id)),
        options(std::move(value_options)),
        headers_promise(std::make_shared<a11::Promise<a11::Unit>>()),
        headers_future(headers_promise->future()) {}

  mutable thread::Mutex mu;
  std::string id ABSL_GUARDED_BY(mu);
  HttpSseOptions options ABSL_GUARDED_BY(mu);
  bool started ABSL_GUARDED_BY(mu) = false;
  bool transport_open ABSL_GUARDED_BY(mu) = false;
  bool transport_finished ABSL_GUARDED_BY(mu) = false;
  bool local_terminal_transmitted ABSL_GUARDED_BY(mu) = false;
  bool remote_terminal_received ABSL_GUARDED_BY(mu) = false;
  bool suppress_outbound_terminal ABSL_GUARDED_BY(mu) = false;
  std::optional<absl::Status> transport_status ABSL_GUARDED_BY(mu);
  HttpHeaders request_headers ABSL_GUARDED_BY(mu);
  // The path a server stream was accepted on. Written once, before the stream
  // reaches on_connect; the only thing distinguishing two streams on a
  // prefix-served port.
  std::string request_path ABSL_GUARDED_BY(mu);
  std::optional<HttpHeaders> response_headers ABSL_GUARDED_BY(mu);
  const std::shared_ptr<a11::Promise<a11::Unit>> headers_promise;
  const a11::Task headers_future;
};

namespace {

/**
 * Whether @p path (already stripped of its query) is a connect endpoint.
 *
 * The exact endpoint is matched as it always was; a prefix matches anything
 * strictly beneath it, which is how one port serves many streams.
 */
bool MatchesConnectEndpoint(std::string_view path,
                            const HttpSseOptions& options) {
  if (path == options.connect_endpoint) {
    return true;
  }
  return !options.connect_endpoint_prefix.empty() &&
         absl::StartsWith(path, options.connect_endpoint_prefix) &&
         path.size() > options.connect_endpoint_prefix.size();
}

}  // namespace

absl::Status HttpSseOptions::Validate() const {
  ABSL_RETURN_IF_ERROR(stream_options.Validate());
  ABSL_RETURN_IF_ERROR(http2_options.Validate());
  if (connect_endpoint.empty() || connect_endpoint.front() != '/') {
    return absl::InvalidArgumentError(
        "connect_endpoint must be an absolute path");
  }
  if (!connect_endpoint_prefix.empty() &&
      (connect_endpoint_prefix.front() != '/' ||
       connect_endpoint_prefix.back() != '/')) {
    return absl::InvalidArgumentError(
        "connect_endpoint_prefix must start and end with '/'");
  }
  if (message_endpoint.empty() || message_endpoint.front() != '/' ||
      !absl::StrContains(message_endpoint, "{id}")) {
    return absl::InvalidArgumentError(
        "message_endpoint must be an absolute path containing {id}");
  }
  if (max_concurrent_posts == 0) {
    return absl::InvalidArgumentError(
        "max_concurrent_posts must admit at least one outbound POST");
  }
  ABSL_RETURN_IF_ERROR(headers.Validate());
  return absl::OkStatus();
}

HttpSseWireStream::HttpSseWireStream(Role role, const std::string& id,
                                     const HttpSseOptions& options,
                                     InProcessWireStream::Pair pair,
                                     std::shared_ptr<State> state)
    : role_(role),
      application_(std::move(pair.first)),
      bridge_(std::move(pair.second)),
      state_(std::move(state)) {
  (void)id;
  (void)options;
}

absl::Status HttpSseWireStream::Send(data::WireMessage message) {
  return application_->Send(std::move(message));
}

a11::Task HttpSseWireStream::Start(OnMessage on_message, OnDone on_done) {
  return StartEndpoint(false, std::move(on_message), std::move(on_done));
}

a11::Task HttpSseWireStream::Accept(OnMessage on_message, OnDone on_done) {
  return StartEndpoint(true, std::move(on_message), std::move(on_done));
}

a11::Task HttpSseWireStream::StartEndpoint(bool accept, OnMessage on_message,
                                           OnDone on_done) {
  if (!on_message || !on_done) {
    return a11::FailedTask(
        absl::InvalidArgumentError("on_message and on_done must be callable"));
  }
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->started) {
      return a11::FailedTask(
          absl::FailedPreconditionError("WireStream is already started"));
    }
    if ((accept && role_ == Role::kClient) ||
        (!accept && role_ == Role::kServer)) {
      return a11::FailedTask(absl::UnimplementedError(
          accept ? "An SSE client stream cannot accept"
                 : "An SSE server stream cannot start as a client"));
    }
    state_->started = true;
  }
  std::shared_ptr<HttpSseWireStream> self = shared_from_this();
  return a11::SubmitTask(
      [self = std::move(self), accept, on_message = std::move(on_message),
       on_done = std::move(on_done)]() mutable -> absl::Status {
        absl::Status status = self->OpenTransport().Await().status();
        if (!status.ok()) {
          self->FailTransport(status);
          return status;
        }
        status = self->StartInternalBridge().Await().status();
        if (!status.ok()) {
          self->FailTransport(status);
          return status;
        }
        a11::Task started =
            accept ? self->application_->Accept(std::move(on_message),
                                                std::move(on_done))
                   : self->application_->Start(std::move(on_message),
                                               std::move(on_done));
        status = started.Await().status();
        if (!status.ok()) {
          self->FailTransport(status);
        }
        return status;
      });
}

a11::Task HttpSseWireStream::StartInternalBridge() {
  std::weak_ptr<HttpSseWireStream> weak = shared_from_this();
  OnMessage on_message = [weak](std::optional<data::WireMessage> message) {
    if (std::shared_ptr<HttpSseWireStream> self = weak.lock();
        self != nullptr) {
      return self->HandleBridgeMessage(std::move(message));
    }
    return a11::FailedTask(
        absl::CancelledError("HTTP SSE WireStream was destroyed"));
  };
  OnDone on_done = [weak]() {
    if (std::shared_ptr<HttpSseWireStream> self = weak.lock();
        self != nullptr) {
      return self->HandleBridgeDone();
    }
    return a11::ReadyTask();
  };
  return role_ == Role::kClient
             ? bridge_->Accept(std::move(on_message), std::move(on_done))
             : bridge_->Start(std::move(on_message), std::move(on_done));
}

a11::Task HttpSseWireStream::HandleBridgeMessage(
    std::optional<data::WireMessage> message) {
  data::WireMessage outbound;
  if (message.has_value()) {
    outbound = std::move(*message);
  } else {
    outbound = data::MakeHalfCloseMessage(
        bridge_->GetTrailers().value_or(data::ByteMap{}));
  }
  const bool terminal = IsTerminal(outbound);
  absl::Status status = Transmit(std::move(outbound));
  if (status.ok() && terminal) {
    thread::MutexLock lock(&state_->mu);
    state_->local_terminal_transmitted = true;
  }
  if (!status.ok()) {
    FailTransport(status);
  }
  return StatusTask(std::move(status));
}

a11::Task HttpSseWireStream::HandleBridgeDone() {
  absl::Status terminal_status;
  bool send_abort = false;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->transport_finished) {
      return a11::ReadyTask();
    }
    terminal_status = bridge_->GetStatus();
    send_abort = !state_->suppress_outbound_terminal &&
                 !state_->local_terminal_transmitted &&
                 !state_->remote_terminal_received && !terminal_status.ok();
  }
  if (send_abort) {
    absl::StatusOr<std::string> packed = data::PackStatus(terminal_status);
    if (packed.ok()) {
      data::WireMessage abort;
      abort.headers.emplace(std::string(kAbortStatusHeader),
                            std::move(*packed));
      absl::Status sent = Transmit(std::move(abort));
      if (!sent.ok()) {
        thread::MutexLock lock(&state_->mu);
        if (!state_->transport_status.has_value()) {
          state_->transport_status = sent;
        }
      }
    }
  }
  {
    thread::MutexLock lock(&state_->mu);
    state_->transport_finished = true;
  }
  TransportDone();
  return a11::ReadyTask();
}

a11::Task HttpSseWireStream::ReceiveTransportMessage(
    data::WireMessage message) {
  const bool terminal = IsTerminal(message);
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->remote_terminal_received) {
      return a11::FailedTask(absl::FailedPreconditionError(
          "The SSE peer sent data after a terminal WireMessage"));
    }
    if (terminal) {
      state_->remote_terminal_received = true;
    }
  }
  const bool abort = IsAbort(message);
  absl::Status sent = bridge_->Send(std::move(message));
  if (!sent.ok()) {
    return a11::FailedTask(sent);
  }
  if (terminal && !abort) {
    return bridge_->DrainOutgoingMessages();
  }
  return a11::ReadyTask();
}

void HttpSseWireStream::FailTransport(absl::Status status) {
  if (status.ok()) {
    status = absl::UnknownError("HTTP SSE transport failed");
  }
  bool abort = false;
  {
    thread::MutexLock lock(&state_->mu);
    if (!state_->transport_status.has_value()) {
      state_->transport_status = status;
      state_->suppress_outbound_terminal = true;
      abort = true;
    }
  }
  if (abort) {
    (void)bridge_->Abort(std::move(status));
  }
}

absl::Status HttpSseWireStream::HalfClose(data::ByteMap trailers) {
  return application_->HalfClose(std::move(trailers));
}

a11::Task HttpSseWireStream::DrainOutgoingMessages() {
  return application_->DrainOutgoingMessages();
}

absl::Status HttpSseWireStream::Abort(absl::Status status) {
  return application_->Abort(std::move(status));
}

absl::Status HttpSseWireStream::SetDeadline(absl::Time deadline) {
  {
    thread::MutexLock lock(&state_->mu);
    state_->options.stream_options.deadline = deadline;
    state_->options.http2_options.deadline = deadline;
  }
  ABSL_RETURN_IF_ERROR(application_->SetDeadline(deadline));
  return bridge_->SetDeadline(deadline);
}

absl::Time HttpSseWireStream::deadline() const {
  return application_->deadline();
}

absl::Status HttpSseWireStream::GetStatus() const {
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->transport_status.has_value() &&
        !state_->transport_status->ok()) {
      return *state_->transport_status;
    }
  }
  return application_->GetStatus();
}

std::optional<data::ByteMap> HttpSseWireStream::GetTrailers() const {
  return application_->GetTrailers();
}

std::string HttpSseWireStream::GetId() const {
  thread::MutexLock lock(&state_->mu);
  return state_->id;
}

void* absl_nullable HttpSseWireStream::GetImpl() const {
  return TransportImpl();
}

HttpHeaders HttpSseWireStream::GetHttpRequestHeaders() const {
  thread::MutexLock lock(&state_->mu);
  return state_->request_headers;
}

std::string HttpSseWireStream::GetRequestPath() const {
  thread::MutexLock lock(&state_->mu);
  return state_->request_path;
}

std::optional<HttpHeaders> HttpSseWireStream::GetHttpResponseHeaders() const {
  thread::MutexLock lock(&state_->mu);
  return state_->response_headers;
}

absl::Status HttpSseWireStream::SetHttpRequestHeaders(HttpHeaders headers) {
  NormalizeHttpHeaders(&headers);
  ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(headers));
  thread::MutexLock lock(&state_->mu);
  if (role_ != Role::kClient) {
    return absl::FailedPreconditionError(
        "Only SSE client streams set HTTP request headers");
  }
  if (state_->started) {
    return absl::FailedPreconditionError(
        "HTTP request headers cannot change after start");
  }
  state_->request_headers = std::move(headers);
  return absl::OkStatus();
}

absl::Status HttpSseWireStream::SetHttpResponseHeaders(HttpHeaders headers) {
  NormalizeHttpHeaders(&headers);
  ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(headers));
  thread::MutexLock lock(&state_->mu);
  if (role_ != Role::kServer) {
    return absl::FailedPreconditionError(
        "Only SSE server streams set HTTP response headers");
  }
  if (state_->transport_open) {
    return absl::FailedPreconditionError(
        "HTTP response headers have already been sent");
  }
  state_->response_headers = std::move(headers);
  return absl::OkStatus();
}

a11::Task HttpSseWireStream::WaitForHttpHeaders() const {
  return state_->headers_future;
}

void HttpSseWireStream::MarkHttpHeadersReady(HttpHeaders headers) {
  bool publish = false;
  {
    thread::MutexLock lock(&state_->mu);
    if (!state_->transport_open) {
      state_->transport_open = true;
      state_->response_headers = std::move(headers);
      publish = true;
    }
  }
  if (publish) {
    (void)state_->headers_promise->SetValue(a11::Unit{});
  }
}

void HttpSseWireStream::SetId(std::string id) {
  thread::MutexLock lock(&state_->mu);
  state_->id = std::move(id);
}

HttpSseOptions HttpSseWireStream::options() const {
  thread::MutexLock lock(&state_->mu);
  return state_->options;
}

std::shared_ptr<InProcessWireStream> HttpSseWireStream::bridge() const {
  return bridge_;
}

struct HttpSseClientWireStream::ClientState {
  ClientState(ParsedUrl value_url, std::string value_connect_path,
              std::string value_message_endpoint,
              std::shared_ptr<Http2Client> value_client)
      : url(std::move(value_url)),
        connect_path(std::move(value_connect_path)),
        message_endpoint(std::move(value_message_endpoint)),
        client(std::move(value_client)) {}

  mutable thread::Mutex mu;
  const ParsedUrl url;
  const std::string connect_path;
  const std::string message_endpoint;
  std::shared_ptr<Http2Client> client ABSL_GUARDED_BY(mu);
  std::shared_ptr<Http2ResponseStream> response ABSL_GUARDED_BY(mu);
  /// The delivery method in force, which is the requested one unless a kStream
  /// request met a server that does not advertise it.
  SseOutboundDelivery outbound ABSL_GUARDED_BY(mu) = SseOutboundDelivery::kPost;
  /// The streamed outbound request body, and the connection opened for it when
  /// the connect connection could not carry a second request (HTTP/1.1).
  std::shared_ptr<Http2DuplexStream> upload ABSL_GUARDED_BY(mu);
  std::shared_ptr<Http2Client> upload_client ABSL_GUARDED_BY(mu);
  bool upload_finished ABSL_GUARDED_BY(mu) = false;
  /// Outbound POSTs handed over and not yet answered, and the maximum.
  size_t posts_in_flight ABSL_GUARDED_BY(mu) = 0;
  size_t max_posts_in_flight ABSL_GUARDED_BY(mu) = 1;
  thread::CondVar posts_changed;
};

HttpSseClientWireStream::HttpSseClientWireStream(
    ConstructorToken, const std::string& url, const HttpSseOptions& options,
    InProcessWireStream::Pair pair, std::shared_ptr<State> state,
    std::shared_ptr<ClientState> client_state)
    : HttpSseWireStream(Role::kClient, url, options, std::move(pair),
                        std::move(state)),
      client_state_(std::move(client_state)) {}

absl::StatusOr<std::shared_ptr<HttpSseClientWireStream>>
HttpSseClientWireStream::Create(std::string url, HttpSseOptions options,
                                std::shared_ptr<Http2Client> client,
                                HttpHeaders request_headers) {
  ABSL_ASSIGN_OR_RETURN(ParsedUrl parsed, ParseUrl(url));
  if (parsed.scheme != "http" && parsed.scheme != "https") {
    return absl::InvalidArgumentError(
        "SSE service URL must start with http:// or https://");
  }
  const std::string base_path = BasePathOf(parsed);
  const bool secure = parsed.scheme == "https";
  if (!secure && options.http2_options.tls.enabled) {
    return absl::InvalidArgumentError(
        "A TLS HTTP/2 client requires an https:// SSE URL");
  }
  if (secure) {
    options.http2_options.tls.enabled = true;
  }
  if (client != nullptr && client->secure() != secure) {
    return absl::InvalidArgumentError(
        "The supplied HTTP/2 client security does not match the SSE URL");
  }
  ABSL_RETURN_IF_ERROR(options.Validate());
  NormalizeHttpHeaders(&request_headers);
  ABSL_RETURN_IF_ERROR(ValidateHttpHeaders(request_headers));
  ABSL_ASSIGN_OR_RETURN(std::string connect_path,
                        ResolveEndpoint(base_path, options.connect_endpoint));
  ABSL_ASSIGN_OR_RETURN(std::string message_endpoint,
                        ResolveEndpoint(base_path, options.message_endpoint));
  ABSL_RETURN_IF_ERROR(
      FormatMessageEndpoint(message_endpoint, "validation").status());
  ABSL_ASSIGN_OR_RETURN(
      InProcessWireStream::Pair pair,
      InProcessWireStream::CreatePair(options.stream_options));
  if (options.http2_options.deadline == absl::InfiniteFuture()) {
    options.http2_options.deadline = options.stream_options.deadline;
  }
  auto state = std::make_shared<State>(NewSseId(), options);
  {
    thread::MutexLock lock(&state->mu);
    state->request_headers = std::move(request_headers);
  }
  auto client_state = std::make_shared<ClientState>(
      std::move(parsed), std::move(connect_path), std::move(message_endpoint),
      std::move(client));
  {
    thread::MutexLock lock(&client_state->mu);
    // Delivery starts at POST whatever was asked for: kStream is only in force
    // once the connect response has advertised it. See OpenOutboundStream.
    client_state->outbound = SseOutboundDelivery::kPost;
    client_state->max_posts_in_flight = options.max_concurrent_posts;
  }
  return std::make_shared<HttpSseClientWireStream>(
      ConstructorToken{}, std::move(url), std::move(options), std::move(pair),
      std::move(state), std::move(client_state));
}

std::shared_ptr<Http2Client> HttpSseClientWireStream::client() const {
  thread::MutexLock lock(&client_state_->mu);
  return client_state_->client;
}

a11::Task HttpSseClientWireStream::OpenTransport() {
  std::shared_ptr<HttpSseClientWireStream> self =
      std::static_pointer_cast<HttpSseClientWireStream>(shared_from_this());
  return a11::SubmitTask([self = std::move(self)]() mutable -> absl::Status {
    HttpSseOptions options = self->options();
    std::shared_ptr<Http2Client> client;
    ParsedUrl url;
    std::string connect_path;
    std::string scheme;
    {
      thread::MutexLock lock(&self->client_state_->mu);
      client = self->client_state_->client;
      url = self->client_state_->url;
      connect_path = self->client_state_->connect_path;
      scheme = self->client_state_->url.scheme;
    }
    if (client == nullptr) {
      ABSL_ASSIGN_OR_RETURN(
          client,
          Http2Client::Connect(url.host, url.port, options.http2_options)
              .Await(options.stream_options.deadline));
      thread::MutexLock lock(&self->client_state_->mu);
      self->client_state_->client = client;
    }
    HttpHeaders request_headers = self->GetHttpRequestHeaders();
    SetHttpHeader(&request_headers, "accept", "text/event-stream");
    ABSL_ASSIGN_OR_RETURN(
        std::shared_ptr<Http2ResponseStream> response,
        client->RequestStream("POST", connect_path, std::move(request_headers),
                              {}, scheme));
    ABSL_ASSIGN_OR_RETURN(
        HttpResponseHead head,
        response->Headers().Await(options.stream_options.deadline));
    if (head.status < 200 || head.status >= 300) {
      std::string body;
      while (true) {
        ABSL_ASSIGN_OR_RETURN(
            std::optional<std::string> chunk,
            response->Read().Await(options.stream_options.deadline));
        if (!chunk.has_value()) {
          break;
        }
        body.append(*chunk);
      }
      return {StatusCodeFromHttp(head.status),
              body.empty()
                  ? absl::StrCat("SSE connect returned HTTP ", head.status)
                  : body};
    }
    const std::optional<std::string> stream_id =
        GetHttpHeader(head.headers, kSseStreamIdHeader);
    if (!stream_id.has_value() || stream_id->empty()) {
      return absl::DataLossError(
          "SSE response did not include x-a11-stream-id");
    }
    const std::optional<std::string> content_type =
        GetHttpHeader(head.headers, "content-type");
    if (!content_type.has_value() ||
        !absl::StartsWithIgnoreCase(*content_type, "text/event-stream")) {
      return absl::DataLossError("SSE response did not use text/event-stream");
    }
    {
      thread::MutexLock lock(&self->client_state_->mu);
      self->client_state_->response = response;
    }
    self->SetId(*stream_id);
    // The outbound stream is opened before the inbound loop starts, so a send
    // issued the moment Start() resolves already has somewhere to go.
    const std::shared_ptr<Http2Client> connected = self->client();
    const bool prefer_stream =
        options.outbound == SseOutboundDelivery::kStream ||
        (connected != nullptr && !connected->multiplexed());
    if (prefer_stream) {
      ABSL_RETURN_IF_ERROR(self->OpenOutboundStream(head.headers));
    }
    if (connected != nullptr && !connected->multiplexed()) {
      thread::MutexLock lock(&self->client_state_->mu);
      if (self->client_state_->outbound == SseOutboundDelivery::kPost) {
        // Still POST, so every message will take a connection of its own -- and
        // two fresh connections race their handshakes, which would hand the
        // server an order the sender never chose. One at a time is what restores
        // it, at a round trip per message. The streamed body above is how to
        // have both, and is why this is the fallback rather than the norm.
        self->client_state_->max_posts_in_flight = 1;
      }
    }
    self->MarkHttpHeadersReady(head.headers);
    a11::Schedule([self]() { ReceiveSseLoop(self); });
    return absl::OkStatus();
  });
}

SseOutboundDelivery HttpSseClientWireStream::outbound_delivery() const {
  thread::MutexLock lock(&client_state_->mu);
  return client_state_->outbound;
}

absl::Status HttpSseClientWireStream::OpenOutboundStream(
    const HttpHeaders& response_headers) {
  const std::optional<std::string> modes =
      GetHttpHeader(response_headers, kSseOutboundModesHeader);
  if (!modes.has_value() || !AdvertisesMode(*modes, kSseOutboundStreamToken)) {
    // An older server, or one configured not to accept a streamed body. POST is
    // the mode every SSE server has always accepted, so fall back to it rather
    // than failing a connection that works.
    VLOG(1) << "a11 sse: server does not advertise streamed outbound delivery; "
               "falling back to one POST per message";
    return absl::OkStatus();
  }
  const HttpSseOptions stream_options = options();
  std::shared_ptr<Http2Client> client;
  ParsedUrl url;
  std::string endpoint;
  {
    thread::MutexLock lock(&client_state_->mu);
    client = client_state_->client;
    url = client_state_->url;
    endpoint = client_state_->message_endpoint;
  }
  if (client == nullptr) {
    return absl::UnavailableError("SSE HTTP client is not connected");
  }
  // An HTTP/1.1 connection carries one request, and the connect request is
  // already on it, so the upload body needs its own. Over HTTP/2 both ride the
  // same connection as two streams.
  std::shared_ptr<Http2Client> upload_client;
  if (!client->multiplexed()) {
    ABSL_ASSIGN_OR_RETURN(
        upload_client,
        Http2Client::Connect(url.host, url.port, stream_options.http2_options)
            .Await(stream_options.stream_options.deadline));
    client = upload_client;
  }
  ABSL_ASSIGN_OR_RETURN(std::string path,
                        FormatMessageEndpoint(std::move(endpoint), GetId()));
  HttpHeaders headers = GetHttpRequestHeaders();
  SetHttpHeader(&headers, "content-type",
                std::string(kSseWireStreamContentType));
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<Http2DuplexStream> upload,
      client->RequestStreamingBody("POST", std::move(path), std::move(headers),
                                   url.scheme));
  {
    thread::MutexLock lock(&client_state_->mu);
    client_state_->upload = upload;
    client_state_->upload_client = std::move(upload_client);
    client_state_->outbound = SseOutboundDelivery::kStream;
  }
  // The server answers the upload request's headers as soon as it has adopted
  // the stream, and keeps the response open until the body ends. Nothing sends
  // on the strength of that answer -- writes are already flowing -- so it is
  // watched rather than awaited, and a refusal fails the transport.
  std::weak_ptr<HttpSseClientWireStream> weak =
      std::static_pointer_cast<HttpSseClientWireStream>(shared_from_this());
  const absl::Time deadline = stream_options.stream_options.deadline;
  a11::Schedule([weak, upload = std::move(upload), deadline]() mutable {
    absl::StatusOr<HttpResponseHead> head = upload->Headers().Await(deadline);
    absl::Status status = head.status();
    if (status.ok() && (head->status < 200 || head->status >= 300)) {
      status = absl::Status(
          StatusCodeFromHttp(head->status),
          absl::StrCat("SSE outbound stream returned HTTP ", head->status));
    }
    if (status.ok()) {
      status = upload->Done().Await(deadline).status();
    }
    std::shared_ptr<HttpSseClientWireStream> self = weak.lock();
    if (self == nullptr) {
      return;
    }
    std::shared_ptr<Http2Client> held_upload_client;
    bool finished = false;
    {
      thread::MutexLock lock(&self->client_state_->mu);
      finished = self->client_state_->upload_finished;
      // Owned only when the upload needed a connection of its own; closing it
      // here rather than at half-close is what keeps the terminating chunk and
      // the close in the right order.
      held_upload_client = std::move(self->client_state_->upload_client);
    }
    // A stream this side already ended is expected to come apart; only a failure
    // that arrives while messages could still be going out is news.
    if (!status.ok() && !finished) {
      self->FailTransport(status);
    }
    if (held_upload_client != nullptr) {
      (void)held_upload_client->Close();
    }
  });
  return absl::OkStatus();
}

/**
 * Hands one outbound message to the transport.
 *
 * Runs on the internal bridge's sender fibre, one message at a time, and is the
 * only place the two delivery methods differ. Neither method awaits an ordinary
 * message: A11 WireMessages carry no global order, so a POST is handed to its own
 * request and the next one overlaps it, and a streamed write is posted to the
 * loop like every other socket write. Order is imposed only where it is
 * load-bearing -- see the terminal and abort cases below.
 */
absl::Status HttpSseClientWireStream::Transmit(data::WireMessage message) {
  const bool terminal = IsTerminal(message);
  const bool abort = IsAbort(message);
  ABSL_ASSIGN_OR_RETURN(std::string payload, data::WireMessageToJson(message));
  SseOutboundDelivery delivery;
  {
    thread::MutexLock lock(&client_state_->mu);
    delivery = client_state_->outbound;
  }
  if (delivery == SseOutboundDelivery::kStream) {
    return TransmitOnStream(payload, terminal);
  }
  if (abort) {
    // At the earliest possibility, ahead of whatever is still in flight. The
    // peer treats an abort as final and discards the rest, so overtaking data
    // POSTs is the intent rather than a hazard.
    return TransmitAsPost(std::move(payload));
  }
  if (terminal) {
    // A half-close says "nothing more follows", and the peer enforces it, so
    // everything handed over already has to land first. HTTP/2 gives no ordering
    // across streams; this barrier is where that ordering comes from.
    ABSL_RETURN_IF_ERROR(AwaitPostsDelivered());
    return TransmitAsPost(std::move(payload));
  }
  ABSL_RETURN_IF_ERROR(ClaimPostSlot());
  std::shared_ptr<HttpSseClientWireStream> self =
      std::static_pointer_cast<HttpSseClientWireStream>(shared_from_this());
  a11::Schedule(
      [self = std::move(self), payload = std::move(payload)]() mutable {
        const absl::Status sent = self->TransmitAsPost(std::move(payload));
        self->ReleasePostSlot();
        if (!sent.ok()) {
          // Nothing is waiting on this POST's return, so a failure reaches the
          // application the same way a failed socket write does: through the
          // stream's lifecycle.
          self->FailTransport(sent);
        }
      });
  return absl::OkStatus();
}

absl::Status HttpSseClientWireStream::TransmitAsPost(std::string payload) {
  std::shared_ptr<Http2Client> client;
  std::string endpoint;
  ParsedUrl url;
  {
    thread::MutexLock lock(&client_state_->mu);
    client = client_state_->client;
    endpoint = client_state_->message_endpoint;
    url = client_state_->url;
  }
  if (client == nullptr) {
    return absl::UnavailableError("SSE HTTP client is not connected");
  }
  std::string scheme = url.scheme;
  // One connection, one request: the event stream is already on the connect
  // connection and will outlive every message, so over HTTP/1.1 this POST needs
  // a connection of its own. It is dropped when the request completes, which is
  // the cost of POST-per-message on HTTP/1.1 -- and why a non-multiplexed
  // connection prefers the streamed body, reaching this path only against a
  // server that will not take one, and for the abort that overtakes it.
  std::shared_ptr<Http2Client> post_client;
  if (!client->multiplexed()) {
    const HttpSseOptions post_options = options();
    ABSL_ASSIGN_OR_RETURN(
        post_client,
        Http2Client::Connect(url.host, url.port, post_options.http2_options)
            .Await(post_options.stream_options.deadline));
    client = post_client;
  }
  ABSL_ASSIGN_OR_RETURN(std::string path,
                        FormatMessageEndpoint(std::move(endpoint), GetId()));
  HttpHeaders headers = GetHttpRequestHeaders();
  SetHttpHeader(&headers, "content-type", "application/json");
  ABSL_ASSIGN_OR_RETURN(
      HttpResponse response,
      client
          ->Request("POST", std::move(path), std::move(headers),
                    std::move(payload), std::move(scheme))
          .Await(deadline()));
  return HttpStatusError(response, "SSE message request");
}

absl::Status HttpSseClientWireStream::TransmitOnStream(
    const std::string& payload, bool terminal) {
  std::shared_ptr<Http2DuplexStream> upload;
  {
    thread::MutexLock lock(&client_state_->mu);
    if (client_state_->upload_finished) {
      return absl::FailedPreconditionError(
          "The SSE outbound stream has already ended");
    }
    upload = client_state_->upload;
    if (terminal) {
      client_state_->upload_finished = true;
    }
  }
  if (upload == nullptr) {
    return absl::UnavailableError("The SSE outbound stream is not open");
  }
  ABSL_RETURN_IF_ERROR(upload->Write(FrameStreamedMessage(payload)));
  if (terminal) {
    // Ending the request body is the peer's end-of-stream. It lands behind the
    // write above because both go through the connection's one FIFO queue.
    return upload->Finish();
  }
  return absl::OkStatus();
}

absl::Status HttpSseClientWireStream::ClaimPostSlot() {
  const absl::Time until = deadline();
  thread::MutexLock lock(&client_state_->mu);
  while (client_state_->posts_in_flight >= client_state_->max_posts_in_flight) {
    if (client_state_->posts_changed.WaitWithDeadline(&client_state_->mu,
                                                      until)) {
      return absl::DeadlineExceededError(
          "Outbound SSE POSTs did not drain before the stream deadline");
    }
  }
  ++client_state_->posts_in_flight;
  return absl::OkStatus();
}

void HttpSseClientWireStream::ReleasePostSlot() {
  thread::MutexLock lock(&client_state_->mu);
  if (client_state_->posts_in_flight > 0) {
    --client_state_->posts_in_flight;
  }
  client_state_->posts_changed.SignalAll();
}

absl::Status HttpSseClientWireStream::AwaitPostsDelivered() {
  const absl::Time until = deadline();
  thread::MutexLock lock(&client_state_->mu);
  while (client_state_->posts_in_flight > 0) {
    if (client_state_->posts_changed.WaitWithDeadline(&client_state_->mu,
                                                      until)) {
      return absl::DeadlineExceededError(
          "Outbound SSE POSTs did not drain before the stream deadline");
    }
  }
  return absl::OkStatus();
}

void HttpSseClientWireStream::ReceiveSseLoop(
    const std::shared_ptr<HttpSseClientWireStream>& self) {
  std::shared_ptr<Http2ResponseStream> response;
  {
    thread::MutexLock lock(&self->client_state_->mu);
    response = self->client_state_->response;
  }
  if (response == nullptr) {
    self->FailTransport(
        absl::InternalError("SSE response stream is not initialized"));
    return;
  }
  std::string line_buffer;
  std::vector<std::string> data_lines;
  bool saw_terminal = false;
  auto process_event = [&]() -> absl::Status {
    if (data_lines.empty()) {
      return absl::OkStatus();
    }
    std::string payload;
    for (size_t index = 0; index < data_lines.size(); ++index) {
      if (index != 0) {
        payload.push_back('\n');
      }
      payload.append(data_lines[index]);
    }
    data_lines.clear();
    ABSL_ASSIGN_OR_RETURN(data::WireMessage message,
                          data::WireMessageFromJson(payload));
    if (saw_terminal) {
      return absl::FailedPreconditionError(
          "SSE peer sent an event after a terminal WireMessage");
    }
    saw_terminal = IsTerminal(message);
    return self->ReceiveTransportMessage(std::move(message)).Await().status();
  };

  while (true) {
    absl::StatusOr<std::optional<std::string>> chunk =
        response->Read().Await(self->deadline());
    if (!chunk.ok()) {
      self->FailTransport(chunk.status());
      return;
    }
    if (!chunk->has_value()) {
      break;
    }
    line_buffer.append(**chunk);
    while (true) {
      const size_t newline = line_buffer.find('\n');
      if (newline == std::string::npos) {
        break;
      }
      std::string line = line_buffer.substr(0, newline);
      line_buffer.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty()) {
        absl::Status status = process_event();
        if (!status.ok()) {
          self->FailTransport(status);
          return;
        }
        continue;
      }
      if (line.front() == ':') {
        continue;
      }
      const size_t colon = line.find(':');
      std::string_view field = colon == std::string::npos
                                   ? std::string_view(line)
                                   : std::string_view(line).substr(0, colon);
      std::string_view value = colon == std::string::npos
                                   ? std::string_view()
                                   : std::string_view(line).substr(colon + 1);
      if (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
      }
      if (field == "data") {
        data_lines.emplace_back(value);
      }
    }
  }
  if (!line_buffer.empty()) {
    if (!line_buffer.empty() && line_buffer.back() == '\r') {
      line_buffer.pop_back();
    }
    if (absl::StartsWith(line_buffer, "data:")) {
      std::string_view value(line_buffer);
      value.remove_prefix(5);
      if (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
      }
      data_lines.emplace_back(value);
    }
  }
  if (!data_lines.empty()) {
    absl::Status status = process_event();
    if (!status.ok()) {
      self->FailTransport(status);
      return;
    }
  }
  if (!saw_terminal) {
    self->FailTransport(absl::UnavailableError(
        "SSE response ended before a terminal WireMessage"));
  }
}

void HttpSseClientWireStream::TransportDone() {
  // A transport that failed before its half-close went out leaves the outbound
  // body open; end it here so the server's reader sees the stream close rather
  // than waiting for the connection to.
  std::shared_ptr<Http2DuplexStream> upload;
  {
    thread::MutexLock lock(&client_state_->mu);
    if (!client_state_->upload_finished) {
      client_state_->upload_finished = true;
      upload = client_state_->upload;
    }
  }
  if (upload != nullptr) {
    (void)upload->Finish();
  }
}

void* absl_nullable HttpSseClientWireStream::TransportImpl() const {
  thread::MutexLock lock(&client_state_->mu);
  if (client_state_->response != nullptr) {
    return client_state_->response.get();
  }
  return client_state_->client.get();
}

struct HttpSseServerWireStream::ServerStreamState {
  ServerStreamState(std::shared_ptr<Http2ResponseWriter> value_response,
                    std::function<void(std::string)> remove_callback)
      : response(std::move(value_response)),
        remove(std::move(remove_callback)) {}

  const std::shared_ptr<Http2ResponseWriter> response;
  const std::shared_ptr<a11::Promise<a11::Unit>> accepted_promise =
      std::make_shared<a11::Promise<a11::Unit>>();
  const a11::Task accepted = accepted_promise->future();
  const std::function<void(std::string)> remove;
};

HttpSseServerWireStream::HttpSseServerWireStream(
    ConstructorToken, const std::string& id, const HttpSseOptions& options,
    InProcessWireStream::Pair pair, std::shared_ptr<State> state,
    std::shared_ptr<ServerStreamState> server_state)
    : HttpSseWireStream(Role::kServer, id, options, std::move(pair),
                        std::move(state)),
      server_state_(std::move(server_state)) {}

a11::Task HttpSseServerWireStream::Accepted() const {
  return server_state_->accepted;
}

a11::Task HttpSseServerWireStream::OpenTransport() {
  const std::shared_ptr<Http2ResponseWriter>& response =
      server_state_->response;
  if (response == nullptr) {
    return a11::FailedTask(
        absl::FailedPreconditionError("SSE response writer is missing"));
  }
  HttpHeaders headers = GetHttpResponseHeaders().value_or(HttpHeaders{});
  SetHttpHeader(&headers, std::string(kSseStreamIdHeader), GetId());
  SetHttpHeader(&headers, "content-type", "text/event-stream");
  // `cache-control` is not set here: the response-header policy has already
  // supplied `no-store` for a stream, which is stricter than the `no-cache`
  // this line used to hardcode. A session's messages should not be written to
  // anybody's disk on the way past.
  // Both outbound modes reach the same endpoint, so what a client may do with it
  // has to be said here. A client that only knows POST ignores the field.
  SetHttpHeader(
      &headers, std::string(kSseOutboundModesHeader),
      options().accept_streamed_outbound
          ? absl::StrCat(kSseOutboundPostToken, ", ", kSseOutboundStreamToken)
          : std::string(kSseOutboundPostToken));
  absl::Status status = response->SendHeaders(200, headers);
  if (!status.ok()) {
    return a11::FailedTask(status);
  }
  std::weak_ptr<HttpSseServerWireStream> weak =
      std::static_pointer_cast<HttpSseServerWireStream>(shared_from_this());
  a11::Task response_done = response->Done();
  a11::Schedule([weak, response_done = std::move(response_done)]() mutable {
    absl::Status completion = response_done.Await().status();
    if (completion.ok()) {
      return;
    }
    if (std::shared_ptr<HttpSseServerWireStream> self = weak.lock();
        self != nullptr) {
      self->FailTransport(completion);
      self->TransportDone();
    }
  });
  MarkHttpHeadersReady(std::move(headers));
  (void)server_state_->accepted_promise->SetValue(a11::Unit{});
  return a11::ReadyTask();
}

absl::Status HttpSseServerWireStream::Transmit(data::WireMessage message) {
  ABSL_ASSIGN_OR_RETURN(std::string payload, data::WireMessageToJson(message));
  const bool terminal = IsTerminal(message);
  const std::shared_ptr<Http2ResponseWriter>& response =
      server_state_->response;
  if (response == nullptr) {
    return absl::UnavailableError("SSE response writer is no longer available");
  }
  ABSL_RETURN_IF_ERROR(
      response->Write(absl::StrCat("data: ", payload, "\n\n")));
  if (terminal) {
    return response->Finish();
  }
  return absl::OkStatus();
}

void* absl_nullable HttpSseServerWireStream::TransportImpl() const {
  return server_state_->response.get();
}

void HttpSseServerWireStream::TransportDone() {
  const std::shared_ptr<Http2ResponseWriter>& response =
      server_state_->response;
  const std::function<void(std::string)>& remove = server_state_->remove;
  if (response != nullptr && !response->finished()) {
    (void)response->Finish();
  }
  if (remove) {
    remove(GetId());
  }
}

struct HttpSseServer::State {
  State(HttpSseOptions value_options, OnHttpSseConnect connect_callback)
      : options(std::move(value_options)),
        // Guarded on the way in: it runs on a connection fibre of A11's, so
        // net/internal/exception_guarded_callbacks.h is where a raised
        // exception becomes the failed Task the caller below awaits.
        on_connect(
            internal::GuardOnHttpSseConnect(std::move(connect_callback))) {}

  mutable thread::Mutex mu;
  const HttpSseOptions options;
  const OnHttpSseConnect on_connect;
  bool stopped ABSL_GUARDED_BY(mu) = false;
  std::shared_ptr<Http2Server> http2_server ABSL_GUARDED_BY(mu);
  absl::flat_hash_map<std::string, std::shared_ptr<HttpSseServerWireStream>>
      streams ABSL_GUARDED_BY(mu);
  std::deque<std::shared_ptr<HttpSseServerWireStream>> incoming
      ABSL_GUARDED_BY(mu);
  std::deque<
      std::shared_ptr<a11::Promise<std::shared_ptr<HttpSseServerWireStream>>>>
      waiters ABSL_GUARDED_BY(mu);
};

namespace {

/// The headers every reply from this server carries, for a given kind of reply.
///
/// One place, so a route added later cannot quietly omit them. See
/// a11/net/server_headers.h.
HttpHeaders ServerHeaders(const HttpSseOptions& options,
                          CachePolicy cache = CachePolicy::kUnset) {
  HttpHeaders headers;
  ApplyServerHeaders(options.headers, cache, &headers);
  return headers;
}

a11::Task SendHttpStatus(const std::shared_ptr<Http2ResponseWriter>& response,
                         const absl::Status& status, HttpHeaders headers = {}) {
  SetHttpHeader(&headers, "content-type", "text/plain; charset=utf-8");
  return StatusTask(response->SendResponse(StatusCodeToHttp(status.code()),
                                           std::move(headers),
                                           std::string(status.message())));
}

std::string PathWithoutQuery(std::string path) {
  const size_t query = path.find_first_of("?#");
  if (query != std::string::npos) {
    path.erase(query);
  }
  return path;
}

absl::StatusOr<std::string> MatchMessagePath(std::string_view path,
                                             std::string_view pattern) {
  const size_t marker = pattern.find("{id}");
  if (marker == std::string_view::npos) {
    return absl::InternalError("SSE message route has no {id} marker");
  }
  const std::string_view prefix = pattern.substr(0, marker);
  const std::string_view suffix = pattern.substr(marker + 4);
  if (!absl::StartsWith(path, prefix) || !absl::EndsWith(path, suffix) ||
      path.size() < prefix.size() + suffix.size()) {
    return absl::NotFoundError("Path does not match the SSE message route");
  }
  const size_t length = path.size() - prefix.size() - suffix.size();
  if (length == 0) {
    return absl::InvalidArgumentError(
        "SSE message path has an empty stream ID");
  }
  return std::string(path.substr(prefix.size(), length));
}

}  // namespace

absl::StatusOr<std::shared_ptr<HttpSseServer>> HttpSseServer::Create(
    std::string bind_address, std::uint16_t port, OnHttpSseConnect on_connect,
    HttpSseOptions options) {
  ABSL_RETURN_IF_ERROR(options.Validate());
  // A POST to the message endpoint that declares the streamed wire format is an
  // open-ended sequence of messages rather than a document, so the HTTP server
  // has to hand it over as it arrives instead of buffering it to its end -- which
  // for this body never comes until the stream does. Composed with whatever the
  // owner already asked to stream rather than replacing it.
  auto inherited = std::move(options.http2_options.stream_request_body);
  options.http2_options.stream_request_body = [pattern =
                                                   options.message_endpoint,
                                               inherited =
                                                   std::move(inherited)](
                                                  std::string_view method,
                                                  std::string_view path,
                                                  const HttpHeaders& headers) {
    if (inherited != nullptr && inherited(method, path, headers)) {
      return true;
    }
    if (!absl::EqualsIgnoreCase(method, "POST")) {
      return false;
    }
    const std::optional<std::string> content_type =
        GetHttpHeader(headers, "content-type");
    return content_type.has_value() &&
           absl::StartsWithIgnoreCase(*content_type,
                                      kSseWireStreamContentType) &&
           MatchMessagePath(PathWithoutQuery(std::string(path)), pattern).ok();
  };
  auto state =
      std::make_shared<State>(std::move(options), std::move(on_connect));
  std::weak_ptr<State> weak = state;
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<Http2Server> server,
      Http2Server::Create(
          std::move(bind_address), port,
          [weak](HttpRequest request,
                 std::shared_ptr<Http2ResponseWriter> response) {
            std::shared_ptr<State> held_state = weak.lock();
            if (held_state == nullptr) {
              return SendHttpStatus(
                  response, absl::UnavailableError("SSE server stopped"));
            }
            return HandleRequest(held_state, std::move(request),
                                 std::move(response));
          },
          state->options.http2_options));
  {
    thread::MutexLock lock(&state->mu);
    state->http2_server = std::move(server);
  }

  struct MakeSharedEnabler final : HttpSseServer {
    explicit MakeSharedEnabler(std::shared_ptr<State> state)
        : HttpSseServer(std::move(state)) {}
  };

  return std::make_shared<MakeSharedEnabler>(std::move(state));
}

HttpSseServer::~HttpSseServer() {
  (void)Stop();
}

a11::Task HttpSseServer::HandleRequest(
    const std::shared_ptr<State>& state, HttpRequest request,
    std::shared_ptr<Http2ResponseWriter> response) {
  const std::string path = PathWithoutQuery(request.path);
  std::string described;
  const bool is_describe =
      MatchDescribePath(path, state->options.describe, &described);
  const bool is_connect = MatchesConnectEndpoint(path, state->options);
  if (IsPreflight(state->options.headers.cors, request.method) &&
      (is_connect || is_describe ||
       MatchMessagePath(path, state->options.message_endpoint).ok())) {
    return StatusTask(
        response->SendResponse(204, ServerHeaders(state->options)));
  }
  if (is_describe) {
    if (std::optional<a11::Task> answered = TryDescribeOverHttp(
            state->options.describe, request, response,
            ServerHeaders(state->options, CachePolicy::kVolatile));
        answered.has_value()) {
      return std::move(*answered);
    }
  }
  if (request.method == "POST" && is_connect) {
    return HandleConnect(state, std::move(request), std::move(response));
  }
  if (request.method == "POST") {
    absl::StatusOr<std::string> stream_id =
        MatchMessagePath(path, state->options.message_endpoint);
    if (stream_id.ok()) {
      // A body still open after its headers is the streamed outbound direction;
      // a complete one is a single posted message. Both reach the same endpoint,
      // so which it is comes from the request rather than from the route.
      return request.body_stream != nullptr
                 ? HandleMessageStream(state, std::move(*stream_id),
                                       std::move(request), std::move(response))
                 : HandleMessage(state, std::move(*stream_id),
                                 std::move(request), std::move(response));
    }
  }
  return SendHttpStatus(response,
                        absl::NotFoundError("No matching SSE endpoint"),
                        ServerHeaders(state->options));
}

a11::Task HttpSseServer::HandleConnect(
    const std::shared_ptr<State>& state, HttpRequest request,
    std::shared_ptr<Http2ResponseWriter> response) {
  return a11::SubmitTask([state, request = std::move(request),
                          response =
                              std::move(response)]() mutable -> absl::Status {
    absl::StatusOr<InProcessWireStream::Pair> pair =
        InProcessWireStream::CreatePair(state->options.stream_options);
    if (!pair.ok()) {
      return SendHttpStatus(response, pair.status(),
                            ServerHeaders(state->options))
          .Await()
          .status();
    }
    const std::string id = NewSseId();
    auto base_state =
        std::make_shared<HttpSseWireStream::State>(id, state->options);
    {
      thread::MutexLock lock(&base_state->mu);
      base_state->request_headers = request.headers;
      base_state->request_path = request.path;
      base_state->response_headers =
          ServerHeaders(state->options, CachePolicy::kStream);
    }
    std::weak_ptr<State> weak = state;
    auto remove = [weak](const std::string& stream_id) {
      if (std::shared_ptr<State> server = weak.lock(); server != nullptr) {
        thread::MutexLock lock(&server->mu);
        server->streams.erase(stream_id);
      }
    };
    auto stream_state =
        std::make_shared<HttpSseServerWireStream::ServerStreamState>(
            response, std::move(remove));
    auto stream = std::make_shared<HttpSseServerWireStream>(
        HttpSseServerWireStream::ConstructorToken{}, id, state->options,
        std::move(*pair), std::move(base_state), std::move(stream_state));
    std::shared_ptr<a11::Promise<std::shared_ptr<HttpSseServerWireStream>>>
        waiter;
    OnHttpSseConnect on_connect;
    bool stopped = false;
    {
      thread::MutexLock lock(&state->mu);
      stopped = state->stopped;
      if (!stopped) {
        state->streams.emplace(id, stream);
        on_connect = state->on_connect;
        if (!on_connect) {
          if (!state->waiters.empty()) {
            waiter = std::move(state->waiters.front());
            state->waiters.pop_front();
          } else {
            state->incoming.push_back(stream);
          }
        }
      }
    }
    // Refused outside the lock. Sending a response crosses to the libuv loop and
    // waits there, and what it wakes on the way -- a finished response stream,
    // its writer's done promise -- can reach back into this server. Deciding
    // under the lock and acting outside it is the shape every other close and
    // send in this file follows, for the same reason.
    if (stopped) {
      return SendHttpStatus(response,
                            absl::UnavailableError("SSE server stopped"),
                            ServerHeaders(state->options))
          .Await()
          .status();
    }
    if (on_connect) {
      const absl::Status callback_status = on_connect(stream).Await().status();
      if (!callback_status.ok()) {
        stream->FailTransport(callback_status);
        return response->headers_sent()
                   ? callback_status
                   : SendHttpStatus(response, callback_status,
                                    ServerHeaders(state->options))
                         .Await()
                         .status();
      }
    } else if (waiter != nullptr) {
      (void)waiter->SetValue(stream);
    }
    absl::Status accepted = stream->Accepted()
                                .Await(state->options.stream_options.deadline)
                                .status();
    if (!accepted.ok()) {
      stream->FailTransport(accepted);
      return response->headers_sent()
                 ? accepted
                 : SendHttpStatus(response, accepted,
                                  ServerHeaders(state->options))
                       .Await()
                       .status();
    }
    return absl::OkStatus();
  });
}

a11::Task HttpSseServer::HandleMessage(
    const std::shared_ptr<State>& state, std::string stream_id,
    HttpRequest request, std::shared_ptr<Http2ResponseWriter> response) {
  return a11::SubmitTask([state, stream_id = std::move(stream_id),
                          request = std::move(request),
                          response =
                              std::move(response)]() mutable -> absl::Status {
    std::shared_ptr<HttpSseServerWireStream> stream;
    {
      thread::MutexLock lock(&state->mu);
      const auto iterator = state->streams.find(stream_id);
      if (iterator != state->streams.end()) {
        stream = iterator->second;
      }
    }
    if (stream == nullptr) {
      return SendHttpStatus(response,
                            absl::NotFoundError(absl::StrCat(
                                "No active SSE stream has ID ", stream_id)),
                            ServerHeaders(state->options))
          .Await()
          .status();
    }
    if (request.body.size() >
        state->options.stream_options.max_single_message_size) {
      const absl::Status status =
          absl::OutOfRangeError("Incoming SSE JSON WireMessage is too large");
      stream->FailTransport(status);
      return SendHttpStatus(response, status, ServerHeaders(state->options))
          .Await()
          .status();
    }
    absl::StatusOr<data::WireMessage> message =
        data::WireMessageFromJson(request.body);
    if (!message.ok()) {
      stream->FailTransport(message.status());
      return SendHttpStatus(response, message.status(),
                            ServerHeaders(state->options))
          .Await()
          .status();
    }
    absl::StatusOr<absl::flat_hash_map<std::string, std::string>> tunneled =
        TunneledHeaders(request.headers);
    if (!tunneled.ok()) {
      stream->FailTransport(tunneled.status());
      return SendHttpStatus(response, tunneled.status(),
                            ServerHeaders(state->options))
          .Await()
          .status();
    }
    for (auto& [name, value] : *tunneled) {
      message->headers.insert_or_assign(name, std::move(value));
    }
    absl::Status received =
        stream->ReceiveTransportMessage(std::move(*message)).Await().status();
    if (!received.ok()) {
      stream->FailTransport(received);
      return SendHttpStatus(response, received, ServerHeaders(state->options))
          .Await()
          .status();
    }
    return response->SendResponse(204, ServerHeaders(state->options));
  });
}

a11::Task HttpSseServer::HandleMessageStream(
    const std::shared_ptr<State>& state, std::string stream_id,
    HttpRequest request, std::shared_ptr<Http2ResponseWriter> response) {
  return a11::SubmitTask(
      [state, stream_id = std::move(stream_id), request = std::move(request),
       response = std::move(response)]() mutable -> absl::Status {
        std::shared_ptr<HttpSseServerWireStream> stream;
        {
          thread::MutexLock lock(&state->mu);
          const auto iterator = state->streams.find(stream_id);
          if (iterator != state->streams.end()) {
            stream = iterator->second;
          }
        }
        if (stream == nullptr) {
          return SendHttpStatus(response,
                                absl::NotFoundError(absl::StrCat(
                                    "No active SSE stream has ID ", stream_id)),
                                ServerHeaders(state->options))
              .Await()
              .status();
        }
        // The request's headers describe the whole stream, so they are tunneled once
        // and attached to every message on it -- which is what a client posting the
        // same headers per message would have produced.
        absl::StatusOr<absl::flat_hash_map<std::string, std::string>> tunneled =
            TunneledHeaders(request.headers);
        if (!tunneled.ok()) {
          stream->FailTransport(tunneled.status());
          return SendHttpStatus(response, tunneled.status(),
                                ServerHeaders(state->options))
              .Await()
              .status();
        }
        // Answer the head before reading: the client is already writing, and the
        // response is how it learns the stream id was good. It stays open until the
        // body ends, because a finished response would close the stream the body is
        // still arriving on.
        ABSL_RETURN_IF_ERROR(
            response->SendHeaders(200, ServerHeaders(state->options)));

        const size_t message_limit =
            state->options.stream_options.max_single_message_size;
        const absl::Time deadline = state->options.stream_options.deadline;
        const std::shared_ptr<Http2RequestBodyStream>& body =
            request.body_stream;
        std::string buffer;
        size_t consumed = 0;
        auto fail = [&](const absl::Status& status) {
          stream->FailTransport(status);
          (void)body->Cancel(status);
          (void)response->Abort(status);
          return status;
        };
        while (true) {
          absl::StatusOr<std::optional<std::string>> chunk =
              body->Read().Await(deadline);
          if (!chunk.ok()) {
            return fail(chunk.status());
          }
          if (!chunk->has_value()) {
            break;
          }
          buffer.append(**chunk);
          while (true) {
            if (buffer.size() - consumed < kStreamFramePrefix) {
              break;
            }
            const std::uint32_t length = DecodeStreamFrameLength(
                std::string_view(buffer).substr(consumed, kStreamFramePrefix));
            if (length > message_limit) {
              return fail(absl::OutOfRangeError(
                  "Incoming SSE streamed WireMessage is too large"));
            }
            if (buffer.size() - consumed - kStreamFramePrefix < length) {
              // A body chunk is whatever the transport handed over -- an HTTP/2 DATA
              // frame is typically 16 KiB -- so a large message accumulates over
              // several of them. The length prefix says how much is coming, so the
              // buffer can be grown once instead of on every append.
              buffer.reserve(consumed + kStreamFramePrefix + length);
              break;  // Await the rest of this frame.
            }
            const std::string_view payload = std::string_view(buffer).substr(
                consumed + kStreamFramePrefix, length);
            absl::StatusOr<data::WireMessage> message =
                data::WireMessageFromJson(payload);
            consumed += kStreamFramePrefix + length;
            if (!message.ok()) {
              return fail(message.status());
            }
            for (const auto& [name, value] : *tunneled) {
              message->headers.insert_or_assign(name, value);
            }
            const absl::Status received =
                stream->ReceiveTransportMessage(std::move(*message))
                    .Await()
                    .status();
            if (!received.ok()) {
              return fail(received);
            }
          }
          // Reclaim what has been delivered rather than growing the buffer for the
          // life of the stream; a partial frame is all that is ever carried over.
          if (consumed != 0) {
            buffer.erase(0, consumed);
            consumed = 0;
          }
        }
        if (buffer.size() != consumed) {
          return fail(absl::DataLossError(
              "SSE outbound stream ended inside a WireMessage frame"));
        }
        return response->Finish();
      });
}

a11::Future<std::shared_ptr<HttpSseServerWireStream>>
HttpSseServer::WaitForStream() {
  thread::MutexLock lock(&state_->mu);
  if (state_->stopped) {
    return a11::FailedFuture<std::shared_ptr<HttpSseServerWireStream>>(
        absl::CancelledError("SSE server is stopped"));
  }
  if (!state_->incoming.empty()) {
    std::shared_ptr<HttpSseServerWireStream> stream =
        std::move(state_->incoming.front());
    state_->incoming.pop_front();
    return a11::ReadyFuture(std::move(stream));
  }
  auto promise = std::make_shared<
      a11::Promise<std::shared_ptr<HttpSseServerWireStream>>>();
  a11::Future<std::shared_ptr<HttpSseServerWireStream>> future =
      promise->future();
  state_->waiters.push_back(std::move(promise));
  return future;
}

absl::Status HttpSseServer::Stop() {
  std::vector<std::shared_ptr<HttpSseServerWireStream>> streams;
  std::deque<
      std::shared_ptr<a11::Promise<std::shared_ptr<HttpSseServerWireStream>>>>
      waiters;
  std::shared_ptr<Http2Server> server;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->stopped) {
      return absl::OkStatus();
    }
    state_->stopped = true;
    for (auto& [id, stream] : state_->streams) {
      (void)id;
      streams.push_back(std::move(stream));
    }
    state_->streams.clear();
    state_->incoming.clear();
    waiters.swap(state_->waiters);
    server = state_->http2_server;
  }
  const absl::Status stopped = absl::CancelledError("SSE server stopped");
  for (const auto& stream : streams) {
    stream->FailTransport(stopped);
  }
  for (const auto& waiter : waiters) {
    (void)waiter->SetStatus(stopped);
  }
  return server != nullptr ? server->Stop() : absl::OkStatus();
}

std::uint16_t HttpSseServer::port() const {
  thread::MutexLock lock(&state_->mu);
  return state_->http2_server != nullptr ? state_->http2_server->port() : 0;
}

bool HttpSseServer::running() const {
  thread::MutexLock lock(&state_->mu);
  return !state_->stopped && state_->http2_server != nullptr &&
         state_->http2_server->running();
}

std::shared_ptr<Http2Server> HttpSseServer::http2_server() const {
  thread::MutexLock lock(&state_->mu);
  return state_->http2_server;
}

}  // namespace a11::net
