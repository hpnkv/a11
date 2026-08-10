// Copyright 2026 The A11 Authors.

#include "a11/net/http_sse_wire_stream.h"

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

#include <absl/container/flat_hash_map.h>
#include <absl/random/random.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
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
#include "a11/net/wire_stream.h"
#include "a11/status.h"
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
  if (endpoint.find("{id}") != std::string::npos) {
    return absl::InvalidArgumentError(
        "message_endpoint must contain exactly one {id} placeholder");
  }
  return endpoint;
}

std::string NewSseId() {
  absl::BitGen generator;
  return absl::StrFormat("sse-%016x%016x",
                         absl::Uniform<std::uint64_t>(generator),
                         absl::Uniform<std::uint64_t>(generator));
}

absl::Status HttpStatusError(const HttpResponse& response,
                             std::string_view operation) {
  if (response.head.status >= 200 && response.head.status < 300) {
    return absl::OkStatus();
  }
  return absl::Status(
      StatusCodeFromHttp(response.head.status),
      response.body.empty()
          ? absl::StrCat(operation, " returned HTTP ", response.head.status)
          : response.body);
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
  std::optional<HttpHeaders> response_headers ABSL_GUARDED_BY(mu);
  const std::shared_ptr<a11::Promise<a11::Unit>> headers_promise;
  const a11::Task headers_future;
};

absl::Status HttpSseOptions::Validate() const {
  ABSL_RETURN_IF_ERROR(stream_options.Validate());
  ABSL_RETURN_IF_ERROR(http2_options.Validate());
  if (connect_endpoint.empty() || connect_endpoint.front() != '/') {
    return absl::InvalidArgumentError(
        "connect_endpoint must be an absolute path");
  }
  if (message_endpoint.empty() || message_endpoint.front() != '/' ||
      message_endpoint.find("{id}") == std::string::npos) {
    return absl::InvalidArgumentError(
        "message_endpoint must be an absolute path containing {id}");
  }
  for (const auto* value : {&cors_allow_origin, &cors_allow_methods,
                            &cors_allow_headers, &cors_expose_headers}) {
    if (value->find_first_of("\r\n") != std::string::npos) {
      return absl::InvalidArgumentError(
          "CORS option values must not contain newlines");
    }
  }
  return absl::OkStatus();
}

HttpSseWireStream::HttpSseWireStream(Role role, std::string id,
                                     HttpSseOptions options,
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
};

HttpSseClientWireStream::HttpSseClientWireStream(
    ConstructorToken, std::string url, HttpSseOptions options,
    InProcessWireStream::Pair pair, std::shared_ptr<State> state,
    std::shared_ptr<ClientState> client_state)
    : HttpSseWireStream(Role::kClient, std::move(url), std::move(options),
                        std::move(pair), std::move(state)),
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
  ABSL_ASSIGN_OR_RETURN(
      std::string connect_path,
      ResolveEndpoint(base_path, options.connect_endpoint));
  ABSL_ASSIGN_OR_RETURN(
      std::string message_endpoint,
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
      return absl::Status(
          StatusCodeFromHttp(head.status),
          body.empty() ? absl::StrCat("SSE connect returned HTTP ", head.status)
                       : body);
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
    self->MarkHttpHeadersReady(head.headers);
    a11::Schedule([self]() { ReceiveSseLoop(std::move(self)); });
    return absl::OkStatus();
  });
}

absl::Status HttpSseClientWireStream::Transmit(data::WireMessage message) {
  ABSL_ASSIGN_OR_RETURN(std::string payload, data::WireMessageToJson(message));
  std::shared_ptr<Http2Client> client;
  std::string endpoint;
  std::string scheme;
  {
    thread::MutexLock lock(&client_state_->mu);
    client = client_state_->client;
    endpoint = client_state_->message_endpoint;
    scheme = client_state_->url.scheme;
  }
  if (client == nullptr) {
    return absl::UnavailableError("SSE HTTP client is not connected");
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
    ConstructorToken, std::string id, HttpSseOptions options,
    InProcessWireStream::Pair pair, std::shared_ptr<State> state,
    std::shared_ptr<ServerStreamState> server_state)
    : HttpSseWireStream(Role::kServer, std::move(id), std::move(options),
                        std::move(pair), std::move(state)),
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
  SetHttpHeader(&headers, "cache-control", "no-cache");
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
        on_connect(std::move(connect_callback)) {}

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

HttpHeaders CorsHeaders(const HttpSseOptions& options) {
  HttpHeaders headers;
  if (!options.cors_allow_origin.empty()) {
    SetHttpHeader(&headers, "access-control-allow-origin",
                  options.cors_allow_origin);
  }
  if (!options.cors_allow_methods.empty()) {
    SetHttpHeader(&headers, "access-control-allow-methods",
                  options.cors_allow_methods);
  }
  if (!options.cors_allow_headers.empty()) {
    SetHttpHeader(&headers, "access-control-allow-headers",
                  options.cors_allow_headers);
  }
  if (!options.cors_expose_headers.empty()) {
    SetHttpHeader(&headers, "access-control-expose-headers",
                  options.cors_expose_headers);
  }
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
  auto state =
      std::make_shared<State>(std::move(options), std::move(on_connect));
  std::weak_ptr<State> weak = state;
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<Http2Server> server,
      Http2Server::Create(
          std::move(bind_address), port,
          [weak](HttpRequest request,
                 std::shared_ptr<Http2ResponseWriter> response) {
            std::shared_ptr<State> state = weak.lock();
            if (state == nullptr) {
              return SendHttpStatus(
                  response, absl::UnavailableError("SSE server stopped"));
            }
            return HandleRequest(state, std::move(request),
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
  if (!state->options.cors_allow_origin.empty() &&
      request.method == "OPTIONS" &&
      (path == state->options.connect_endpoint ||
       MatchMessagePath(path, state->options.message_endpoint).ok())) {
    return StatusTask(response->SendResponse(204, CorsHeaders(state->options)));
  }
  if (request.method == "POST" && path == state->options.connect_endpoint) {
    return HandleConnect(state, std::move(request), std::move(response));
  }
  if (request.method == "POST") {
    absl::StatusOr<std::string> stream_id =
        MatchMessagePath(path, state->options.message_endpoint);
    if (stream_id.ok()) {
      return HandleMessage(state, std::move(*stream_id), std::move(request),
                           std::move(response));
    }
  }
  return SendHttpStatus(response,
                        absl::NotFoundError("No matching SSE endpoint"),
                        CorsHeaders(state->options));
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
                            CorsHeaders(state->options))
          .Await()
          .status();
    }
    const std::string id = NewSseId();
    auto base_state =
        std::make_shared<HttpSseWireStream::State>(id, state->options);
    {
      thread::MutexLock lock(&base_state->mu);
      base_state->request_headers = request.headers;
      base_state->response_headers = CorsHeaders(state->options);
    }
    std::weak_ptr<State> weak = state;
    auto remove = [weak](std::string stream_id) {
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
    {
      thread::MutexLock lock(&state->mu);
      if (state->stopped) {
        return SendHttpStatus(response,
                              absl::UnavailableError("SSE server stopped"),
                              CorsHeaders(state->options))
            .Await()
            .status();
      }
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
    if (on_connect) {
      absl::Status callback_status;
      try {
        callback_status = on_connect(stream).Await().status();
      } catch (const std::exception& error) {
        callback_status = absl::UnknownError(error.what());
      } catch (...) {
        callback_status = absl::UnknownError(
            "SSE on_connect callback raised a non-standard exception");
      }
      if (!callback_status.ok()) {
        stream->FailTransport(callback_status);
        return response->headers_sent()
                   ? callback_status
                   : SendHttpStatus(response, callback_status,
                                    CorsHeaders(state->options))
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
                                  CorsHeaders(state->options))
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
                            CorsHeaders(state->options))
          .Await()
          .status();
    }
    if (request.body.size() >
        state->options.stream_options.max_single_message_size) {
      const absl::Status status =
          absl::OutOfRangeError("Incoming SSE JSON WireMessage is too large");
      stream->FailTransport(status);
      return SendHttpStatus(response, status, CorsHeaders(state->options))
          .Await()
          .status();
    }
    absl::StatusOr<data::WireMessage> message =
        data::WireMessageFromJson(request.body);
    if (!message.ok()) {
      stream->FailTransport(message.status());
      return SendHttpStatus(response, message.status(),
                            CorsHeaders(state->options))
          .Await()
          .status();
    }
    absl::flat_hash_map<std::string, std::string> combined;
    for (const auto& [name, value] : request.headers) {
      auto [iterator, inserted] = combined.emplace(name, value);
      if (!inserted) {
        iterator->second = absl::StrCat(iterator->second, ", ", value);
      }
    }
    for (auto& [name, value] : combined) {
      // Framing headers describe the POST itself and are not application
      // metadata. Other HTTP headers are namespaced exactly once.
      if (name == "content-type" || name == "content-length" ||
          name == kSseStreamIdHeader) {
        continue;
      }
      std::string wire_name = absl::StartsWith(name, kSseHttpHeaderPrefix)
                                  ? name
                                  : absl::StrCat(kSseHttpHeaderPrefix, name);
      absl::Status valid_name = data::ValidateName(wire_name);
      if (!valid_name.ok()) {
        stream->FailTransport(valid_name);
        return SendHttpStatus(response, valid_name, CorsHeaders(state->options))
            .Await()
            .status();
      }
      message->headers.insert_or_assign(std::move(wire_name), std::move(value));
    }
    absl::Status received =
        stream->ReceiveTransportMessage(std::move(*message)).Await().status();
    if (!received.ok()) {
      stream->FailTransport(received);
      return SendHttpStatus(response, received, CorsHeaders(state->options))
          .Await()
          .status();
    }
    return response->SendResponse(204, CorsHeaders(state->options));
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
