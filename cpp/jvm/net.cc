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

#include "jvm/net.h"

#include "a11/net/in_process_wire_stream.h"
#include "a11/net/websocket_wire_stream.h"
#include "a11/service/session.h"
#include "jvm/bridge.h"
#include "jvm/values.h"

namespace a11::jvm {
namespace {
absl::StatusOr<data::ByteMap> BytesMap(const nlohmann::json& args,
                                       std::string_view name) {
  ABSL_ASSIGN_OR_RETURN(
      nlohmann::json fields,
      DefaultArg<nlohmann::json>(args, name, nlohmann::json::object()));
  if (!fields.is_object()) {
    return absl::InvalidArgumentError("Expected byte map");
  }
  data::ByteMap result;
  for (const auto& [key, unused] : fields.items()) {
    ABSL_ASSIGN_OR_RETURN(std::string value, Arg<std::string>(fields, key));
    result.emplace(key, std::move(value));
  }
  return result;
}

absl::StatusOr<net::HttpHeaders> Headers(const nlohmann::json& args) {
  ABSL_ASSIGN_OR_RETURN(
      nlohmann::json raw,
      DefaultArg<nlohmann::json>(args, "headers", nlohmann::json::array()));
  if (!raw.is_array()) {
    return absl::InvalidArgumentError("Expected header pairs");
  }
  net::HttpHeaders headers;
  for (const auto& pair : raw) {
    if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string() ||
        !pair[1].is_string()) {
      return absl::InvalidArgumentError("Invalid HTTP header");
    }
    headers.emplace_back(pair[0].get<std::string>(),
                         pair[1].get<std::string>());
  }
  return headers;
}

absl::StatusOr<net::Http2Options> HttpOptions(const nlohmann::json& args) {
  ABSL_ASSIGN_OR_RETURN(auto fields,
                        DefaultArg<nlohmann::json>(args, "http2Options",
                                                   nlohmann::json::object()));
  if (!fields.is_object()) {
    return absl::InvalidArgumentError("Expected HTTP options");
  }
  net::Http2Options options;
#define A11_HTTP_OPTION(field)                                              \
  ABSL_ASSIGN_OR_RETURN(options.field, DefaultArg<decltype(options.field)>( \
                                           fields, #field, options.field))
  A11_HTTP_OPTION(max_request_body_size);
  A11_HTTP_OPTION(max_response_body_size);
  A11_HTTP_OPTION(max_buffered_request_bytes);
  A11_HTTP_OPTION(max_buffered_response_bytes);
  A11_HTTP_OPTION(enable_h2);
  A11_HTTP_OPTION(enable_h2c);
  A11_HTTP_OPTION(enable_http1);
  A11_HTTP_OPTION(enable_push);
  A11_HTTP_OPTION(client_allow_downgrade);
#undef A11_HTTP_OPTION
  ABSL_ASSIGN_OR_RETURN(
      auto preference,
      DefaultArg<int>(fields, "client_preference",
                      static_cast<int>(options.client_preference)));
  if (preference < 0 || preference > 2) {
    return absl::InvalidArgumentError("Invalid HTTP preference");
  }
  options.client_preference =
      static_cast<net::Http2Options::ProtocolPreference>(preference);
  ABSL_ASSIGN_OR_RETURN(auto deadline,
                        ObjectArg<absl::Time>(fields, "deadline", true));
  if (deadline != nullptr) {
    options.deadline = *deadline;
  }
  ABSL_ASSIGN_OR_RETURN(auto tls, DefaultArg<nlohmann::json>(
                                      fields, "tls", nlohmann::json::object()));
  if (!tls.is_object()) {
    return absl::InvalidArgumentError("Expected TLS options");
  }
#define A11_TLS_OPTION(field) \
  ABSL_ASSIGN_OR_RETURN(      \
      options.tls.field,      \
      DefaultArg<decltype(options.tls.field)>(tls, #field, options.tls.field))
  A11_TLS_OPTION(enabled);
  A11_TLS_OPTION(verify_peer);
  A11_TLS_OPTION(certificate_pem_file);
  A11_TLS_OPTION(key_pem_file);
  A11_TLS_OPTION(ca_certificate_pem_file);
#undef A11_TLS_OPTION
  ABSL_RETURN_IF_ERROR(options.Validate());
  return options;
}
}  // namespace

void BindNet(absl::flat_hash_map<std::string, Operation>* operations) {
  (*operations)["http.defaults"] =
      [](std::int64_t,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    const net::Http2Options options;
    nlohmann::json fields;
#define A11_HTTP_FIELD(field) fields[#field] = options.field
    A11_HTTP_FIELD(max_request_body_size);
    A11_HTTP_FIELD(max_response_body_size);
    A11_HTTP_FIELD(max_buffered_request_bytes);
    A11_HTTP_FIELD(max_buffered_response_bytes);
    A11_HTTP_FIELD(enable_h2);
    A11_HTTP_FIELD(enable_h2c);
    A11_HTTP_FIELD(enable_http1);
    A11_HTTP_FIELD(enable_push);
    A11_HTTP_FIELD(client_allow_downgrade);
#undef A11_HTTP_FIELD
    fields["client_preference"] = static_cast<int>(options.client_preference);
#define A11_TLS_FIELD(field) fields["tls"][#field] = options.tls.field
    A11_TLS_FIELD(enabled);
    A11_TLS_FIELD(verify_peer);
    A11_TLS_FIELD(certificate_pem_file);
    A11_TLS_FIELD(key_pem_file);
    A11_TLS_FIELD(ca_certificate_pem_file);
#undef A11_TLS_FIELD
    return fields;
  };
  (*operations)["http.validateOptions"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto options, HttpOptions(args));
    return Result(options.Validate());
  };
  (*operations)["stream.inProcessPair"] =
      [](std::int64_t,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto pair, net::InProcessWireStream::CreatePair());
    return nlohmann::json::array(
        {Save(std::make_shared<BoundStream>(pair.first)),
         Save(std::make_shared<BoundStream>(pair.second))});
  };
  (*operations)["websocket.connect"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto url, Arg<std::string>(args, "url"));
    net::WebSocketClientOptions options;
    ABSL_ASSIGN_OR_RETURN(options.http2_options, HttpOptions(args));
    ABSL_ASSIGN_OR_RETURN(options.headers, Headers(args));
    ABSL_ASSIGN_OR_RETURN(
        options.framing.split_size,
        DefaultArg<size_t>(args, "splitSize", options.framing.split_size));
    ABSL_ASSIGN_OR_RETURN(auto stream, net::WebSocketWireStream::CreateClient(
                                           url, {}, std::move(options)));
    return Save(std::make_shared<BoundStream>(std::move(stream)));
  };
  (*operations)["websocket.serve"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto callback, ObjectArg<Callback>(args, "callback"));
    net::WebSocketServerOptions options;
    ABSL_ASSIGN_OR_RETURN(options.http2_options, HttpOptions(args));
    ABSL_ASSIGN_OR_RETURN(options.path,
                          DefaultArg<std::string>(args, "path", options.path));
    ABSL_ASSIGN_OR_RETURN(
        options.bind_address,
        DefaultArg<std::string>(args, "bindAddress", options.bind_address));
    ABSL_ASSIGN_OR_RETURN(
        options.port, DefaultArg<std::uint16_t>(args, "port", options.port));
    ABSL_ASSIGN_OR_RETURN(
        auto server,
        net::WebSocketWireServer::Create(
            [callback](std::shared_ptr<net::WebSocketWireStream> stream) {
              auto bound = std::make_shared<BoundStream>(std::move(stream));
              const auto id = Retain(std::move(bound));
              return callback->Invoke(Handle(id), {id});
            },
            std::move(options)));
    return Save(std::move(server));
  };
  (*operations)["websocket.serverPort"] =
      [](std::int64_t self,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto server, Object<net::WebSocketWireServer>(self));
    ABSL_ASSIGN_OR_RETURN(auto port, server->port());
    return nlohmann::json(port);
  };
  (*operations)["websocket.serverStop"] =
      [](std::int64_t self,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto server, Object<net::WebSocketWireServer>(self));
    return Result(server->Stop());
  };
  (*operations)["stream.drive"] =
      [](std::int64_t self,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto bound, Object<BoundStream>(self));
    ABSL_ASSIGN_OR_RETURN(auto on_message,
                          ObjectArg<Callback>(args, "onMessage", true));
    ABSL_ASSIGN_OR_RETURN(auto on_done,
                          ObjectArg<Callback>(args, "onDone", true));
    ABSL_ASSIGN_OR_RETURN(bool accept, DefaultArg<bool>(args, "accept", false));
    net::OnMessage message_callback = [](std::optional<data::WireMessage>) {
      return ReadyTask();
    };
    if (on_message != nullptr) {
      message_callback =
          [on_message](std::optional<data::WireMessage> message) {
            if (!message.has_value()) {
              return on_message->Invoke(nullptr);
            }
            const auto handle = KeepValue(std::move(*message));
            return on_message->Invoke(Handle(handle), {handle});
          };
    }
    auto done_callback = [on_done, done = bound->done]() {
      Task task = on_done != nullptr ? on_done->Invoke(nullptr) : ReadyTask();
      task.OnReady([done](const absl::StatusOr<Unit>& result) {
        done->SetResult(result).IgnoreError();
      });
      return task;
    };
    return Async(accept ? bound->stream->Accept(std::move(message_callback),
                                                std::move(done_callback))
                        : bound->stream->Start(std::move(message_callback),
                                               std::move(done_callback)));
  };
  (*operations)["stream.id"] =
      [](std::int64_t self,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto bound, Object<BoundStream>(self));
    return nlohmann::json(bound->stream->GetId());
  };
  (*operations)["stream.status"] =
      [](std::int64_t self,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto bound, Object<BoundStream>(self));
    return StatusToJson(bound->stream->GetStatus());
  };
  (*operations)["stream.trailers"] =
      [](std::int64_t self,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto bound, Object<BoundStream>(self));
    auto trailers = bound->stream->GetTrailers();
    if (!trailers.has_value()) {
      return Null();
    }
    nlohmann::json fields = nlohmann::json::object();
    for (const auto& [key, bytes] : *trailers) {
      fields[key] = Binary(bytes);
    }
    return fields;
  };
  (*operations)["stream.halfClose"] =
      [](std::int64_t self,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto bound, Object<BoundStream>(self));
    ABSL_ASSIGN_OR_RETURN(auto trailers, BytesMap(args, "trailers"));
    return Result(bound->stream->HalfClose(std::move(trailers)));
  };
  (*operations)["stream.abort"] =
      [](std::int64_t self,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto bound, Object<BoundStream>(self));
    ABSL_ASSIGN_OR_RETURN(auto status, StatusArg(args, "status"));
    return Result(bound->stream->Abort(std::move(status)));
  };
  (*operations)["stream.drain"] =
      [](std::int64_t self,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto bound, Object<BoundStream>(self));
    return Async(bound->stream->DrainOutgoingMessages());
  };
  (*operations)["stream.done"] =
      [](std::int64_t self,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto bound, Object<BoundStream>(self));
    return Async(bound->done->future());
  };
  (*operations)["sessionWithRecv.create"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto id, DefaultArg<std::string>(args, "id", ""));
    ABSL_ASSIGN_OR_RETURN(auto headers, BytesMap(args, "headers"));
    ABSL_ASSIGN_OR_RETURN(auto session, service::SessionWithRecv::Create(
                                            std::move(id), std::move(headers)));
    return Save(std::move(session));
  };
  (*operations)["sessionWithRecv.addStream"] =
      [](std::int64_t self,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto session, Object<service::SessionWithRecv>(self));
    ABSL_ASSIGN_OR_RETURN(auto bound, ObjectArg<BoundStream>(args, "stream"));
    ABSL_ASSIGN_OR_RETURN(auto accept, DefaultArg<bool>(args, "accept", false));
    ABSL_ASSIGN_OR_RETURN(
        auto task, session->AddStream(bound->stream,
                                      accept ? service::StreamMode::kAccept
                                             : service::StreamMode::kStart));
    return Async(task);
  };
  (*operations)["sessionWithRecv.receive"] =
      [](std::int64_t self,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto session, Object<service::SessionWithRecv>(self));
    ABSL_ASSIGN_OR_RETURN(auto deadline, Deadline(args));
    return Async(session->Receive(deadline));
  };
  (*operations)["sessionWithRecv.halfClose"] =
      [](std::int64_t self,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto session, Object<service::SessionWithRecv>(self));
    return Result(session->HalfClose());
  };
  (*operations)["sessionWithRecv.done"] =
      [](std::int64_t self,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto session, Object<service::SessionWithRecv>(self));
    return Async(session->Done());
  };
  (*operations)["sessionWithRecv.abort"] =
      [](std::int64_t self,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto session, Object<service::SessionWithRecv>(self));
    ABSL_ASSIGN_OR_RETURN(auto status, StatusArg(args, "status"));
    return Result(session->Abort(std::move(status)));
  };
  (*operations)["sessionWithRecv.status"] =
      [](std::int64_t self,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto session, Object<service::SessionWithRecv>(self));
    return StatusToJson(session->GetStatus());
  };
}
}  // namespace a11::jvm
