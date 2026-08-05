// Copyright 2026 The A11 Authors.

#include "a11/net/webrtc_wire_stream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/base/call_once.h>
#include <absl/base/thread_annotations.h>
#include <absl/container/flat_hash_map.h>
#include <absl/random/random.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <absl/strings/strip.h>
#include <rtc/candidate.hpp>
#include <rtc/configuration.hpp>
#include <rtc/datachannel.hpp>
#include <rtc/description.hpp>
#include <rtc/global.hpp>
#include <rtc/peerconnection.hpp>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/channel_wire_stream.h"
#include "a11/net/internal/binary_channel.h"
#include "a11/net/internal/multiplexed_binary_channel.h"
#include "a11/net/signalling.h"
#include "a11/net/wire_stream.h"
#include "thread/boost_primitives.h"

namespace a11::net {
namespace {

std::string NewDataChannelId() {
  absl::BitGen generator;
  return absl::StrFormat("a11-%016x%016x",
                         absl::Uniform<std::uint64_t>(generator),
                         absl::Uniform<std::uint64_t>(generator));
}

absl::Status ExternalException(const std::exception& error,
                               std::string_view operation) {
  return absl::UnknownError(
      absl::StrCat(operation, " raised an exception: ", error.what()));
}

absl::Status InitializeSctp() {
  static absl::once_flag once;
  static absl::Status status;
  absl::call_once(once, []() {
    try {
      rtc::SctpSettings settings;
      settings.recvBufferSize = 32 * 1024 * 1024;
      settings.sendBufferSize = 32 * 1024 * 1024;
      rtc::SetSctpSettings(std::move(settings));
      status = absl::OkStatus();
    } catch (const std::exception& error) {
      status = ExternalException(error, "Configuring WebRTC SCTP");
    } catch (...) {
      status = absl::UnknownError(
          "Configuring WebRTC SCTP raised a non-standard exception");
    }
  });
  return status;
}

struct ClientPeerContext {
  ClientPeerContext(std::string value_identity, std::string value_peer_identity,
                    std::shared_ptr<SignallingTransport> value_endpoint,
                    std::shared_ptr<rtc::PeerConnection> value_connection)
      : identity(std::move(value_identity)),
        peer_identity(std::move(value_peer_identity)),
        endpoint(std::move(value_endpoint)),
        connection(std::move(value_connection)) {}

  mutable thread::Mutex mu;
  const std::string identity;
  const std::string peer_identity;
  const std::shared_ptr<SignallingTransport> endpoint;
  const std::shared_ptr<rtc::PeerConnection> connection;
  std::shared_ptr<rtc::DataChannel> data_channel ABSL_GUARDED_BY(mu);
  absl::Status status ABSL_GUARDED_BY(mu);
  bool failed ABSL_GUARDED_BY(mu) = false;
};

void FailClient(const std::shared_ptr<ClientPeerContext>& context,
                absl::Status status) {
  if (status.ok()) {
    status = absl::UnknownError("WebRTC establishment failed");
  }
  std::shared_ptr<rtc::DataChannel> channel;
  std::shared_ptr<rtc::PeerConnection> connection;
  {
    thread::MutexLock lock(&context->mu);
    if (context->failed) {
      return;
    }
    context->failed = true;
    context->status = status;
    channel = context->data_channel;
    connection = context->connection;
  }
  try {
    if (channel != nullptr) {
      channel->close();
    }
  } catch (...) {}
  try {
    if (connection != nullptr) {
      connection->close();
    }
  } catch (...) {}
}

absl::Status ClientStatus(const std::shared_ptr<ClientPeerContext>& context) {
  thread::MutexLock lock(&context->mu);
  return context->status;
}

a11::Task HandleClientSignal(const std::shared_ptr<ClientPeerContext>& context,
                             SignallingMessage message) {
  absl::Status status;
  std::shared_ptr<rtc::PeerConnection> connection;
  {
    thread::MutexLock lock(&context->mu);
    if (context->failed) {
      return a11::FailedTask(context->status);
    }
    connection = context->connection;
  }
  if (connection == nullptr) {
    status = absl::FailedPreconditionError(
        "WebRTC client received signalling before peer initialization");
  } else {
    try {
      switch (message.type) {
        case SignallingMessageType::kDescription:
          if (message.description_type != "answer" &&
              message.description_type != "pranswer") {
            status = absl::InvalidArgumentError(
                "WebRTC client expected an answer description");
          } else {
            connection->setRemoteDescription(rtc::Description(
                message.description, message.description_type));
          }
          break;
        case SignallingMessageType::kCandidate:
          connection->addRemoteCandidate(
              rtc::Candidate(message.candidate, message.mid));
          break;
        case SignallingMessageType::kError:
          status = message.error;
          break;
      }
    } catch (const std::exception& error) {
      status = ExternalException(error, "Applying WebRTC client signalling");
    } catch (...) {
      status = absl::UnknownError(
          "Applying WebRTC client signalling raised an exception");
    }
  }
  if (!status.ok()) {
    FailClient(context, status);
    return a11::FailedTask(status);
  }
  return a11::ReadyTask();
}

}  // namespace

absl::StatusOr<TurnServer> TurnServer::FromString(std::string_view value) {
  if (absl::ConsumePrefix(&value, "turn:")) {
    // TURN over UDP is the default.
  } else if (absl::ConsumePrefix(&value, "turns:")) {
    // Relay type is assigned below after parsing.
  }
  const size_t query = value.find('?');
  std::string_view query_value;
  if (query != std::string_view::npos) {
    query_value = value.substr(query + 1);
    value = value.substr(0, query);
  }
  std::string_view credentials;
  std::string_view address = value;
  if (const size_t at = value.rfind('@'); at != std::string_view::npos) {
    credentials = value.substr(0, at);
    address = value.substr(at + 1);
  }
  if (address.empty()) {
    return absl::InvalidArgumentError("TURN server requires a hostname");
  }
  TurnServer result;
  if (!credentials.empty()) {
    const size_t colon = credentials.find(':');
    result.username = std::string(credentials.substr(0, colon));
    if (colon != std::string_view::npos) {
      result.password = std::string(credentials.substr(colon + 1));
    }
  }
  if (address.front() == '[') {
    const size_t closing = address.find(']');
    if (closing == std::string_view::npos) {
      return absl::InvalidArgumentError("TURN server has an invalid IPv6 host");
    }
    result.hostname = std::string(address.substr(1, closing - 1));
    if (closing + 1 < address.size() &&
        (address[closing + 1] != ':' ||
         !absl::SimpleAtoi(address.substr(closing + 2), &result.port))) {
      return absl::InvalidArgumentError("TURN server has an invalid port");
    }
  } else {
    const size_t colon = address.rfind(':');
    if (colon == std::string_view::npos) {
      result.hostname = std::string(address);
    } else {
      result.hostname = std::string(address.substr(0, colon));
      if (!absl::SimpleAtoi(address.substr(colon + 1), &result.port)) {
        return absl::InvalidArgumentError("TURN server has an invalid port");
      }
    }
  }
  if (result.hostname.empty()) {
    return absl::InvalidArgumentError("TURN server requires a hostname");
  }
  if (query_value == "transport=tcp") {
    result.relay_type = TurnRelayType::kTcp;
  } else if (query_value == "transport=tls") {
    result.relay_type = TurnRelayType::kTls;
  } else if (!query_value.empty() && query_value != "transport=udp") {
    return absl::InvalidArgumentError("TURN server has an invalid transport");
  }
  return result;
}

absl::Status WebRtcConfiguration::Validate() const {
  if (max_message_size.has_value() && *max_message_size == 0) {
    return absl::InvalidArgumentError(
        "WebRTC max_message_size must be positive or absent");
  }
  if (channel_split_size < 18 || channel_split_size > 1024 * 1024) {
    return absl::InvalidArgumentError(
        "WebRTC channel_split_size must be in [18, 1048576]");
  }
  if (max_message_size.has_value() && channel_split_size > *max_message_size) {
    return absl::InvalidArgumentError(
        "WebRTC channel_split_size exceeds max_message_size");
  }
  if (preferred_port_range.has_value() &&
      preferred_port_range->first > preferred_port_range->second) {
    return absl::InvalidArgumentError(
        "WebRTC preferred_port_range is reversed");
  }
  if (desired_channels == 0) {
    return absl::InvalidArgumentError(
        "WebRTC desired_channels must be at least 1");
  }
  if (max_channels == 0) {
    return absl::InvalidArgumentError("WebRTC max_channels must be at least 1");
  }
  for (const TurnServer& server : turn_servers) {
    if (server.hostname.empty()) {
      return absl::InvalidArgumentError(
          "WebRTC TURN server hostname must not be empty");
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<rtc::Configuration> BuildLibDataChannelConfiguration(
    const WebRtcConfiguration& configuration) {
  ABSL_RETURN_IF_ERROR(configuration.Validate());
  ABSL_RETURN_IF_ERROR(InitializeSctp());
  try {
    rtc::Configuration result;
    result.maxMessageSize = configuration.max_message_size;
    result.enableIceUdpMux = configuration.enable_ice_udp_mux;
    result.bindAddress = configuration.bind_address;
    if (configuration.preferred_port_range.has_value()) {
      result.portRangeBegin = configuration.preferred_port_range->first;
      result.portRangeEnd = configuration.preferred_port_range->second;
    }
    for (const std::string& server : configuration.stun_servers) {
      result.iceServers.emplace_back(server);
    }
    for (const TurnServer& server : configuration.turn_servers) {
      rtc::IceServer::RelayType relay_type = rtc::IceServer::RelayType::TurnUdp;
      if (server.relay_type == TurnRelayType::kTcp) {
        relay_type = rtc::IceServer::RelayType::TurnTcp;
      } else if (server.relay_type == TurnRelayType::kTls) {
        relay_type = rtc::IceServer::RelayType::TurnTls;
      }
      result.iceServers.emplace_back(server.hostname, server.port,
                                     server.username, server.password,
                                     relay_type);
    }
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid WebRTC configuration: ", error.what()));
  } catch (...) {
    return absl::InvalidArgumentError(
        "Invalid WebRTC configuration raised a non-standard exception");
  }
}

absl::StatusOr<std::shared_ptr<WebRtcWireStream>>
WebRtcWireStream::BuildMultiplexedStream(
    std::shared_ptr<internal::BinaryChannel> channel,
    std::shared_ptr<rtc::DataChannel> primary_channel,
    std::shared_ptr<rtc::PeerConnection> connection,
    std::shared_ptr<SignallingTransport> signalling_endpoint, std::string id,
    ChannelEndpointRole role, WireStreamOptions options,
    OpenOperation open_operation, size_t split_size) {
  if (channel == nullptr) {
    return absl::InvalidArgumentError("WebRTC channel must not be null");
  }
  if (connection == nullptr) {
    return absl::InvalidArgumentError(
        "WebRTC peer connection must not be null");
  }
  if (id.empty()) {
    id = NewDataChannelId();
  }
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<State> state,
      MakeState(std::move(channel), std::move(id), role,
                std::move(open_operation), options,
                ChannelFramingOptions{.split_size = split_size}));
  return std::make_shared<WebRtcWireStream>(
      ConstructorToken{}, std::move(state), std::move(primary_channel),
      std::move(connection), std::move(signalling_endpoint));
}

absl::StatusOr<std::shared_ptr<WebRtcWireStream>> WebRtcWireStream::Create(
    std::shared_ptr<rtc::DataChannel> data_channel,
    std::shared_ptr<rtc::PeerConnection> connection,
    std::shared_ptr<SignallingTransport> signalling_endpoint,
    ChannelEndpointRole role, WireStreamOptions options,
    OpenOperation open_operation, size_t split_size) {
  if (data_channel == nullptr) {
    return absl::InvalidArgumentError("WebRTC data channel must not be null");
  }
  if (connection == nullptr) {
    return absl::InvalidArgumentError(
        "WebRTC peer connection must not be null");
  }
  std::string id;
  try {
    id = data_channel->label();
  } catch (const std::exception& error) {
    return ExternalException(error, "Reading WebRTC data channel label");
  } catch (...) {
    return absl::UnknownError(
        "Reading WebRTC data channel label raised an exception");
  }
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<internal::BinaryChannel> channel,
                        internal::MakeRtcBinaryChannel(data_channel));
  // Adopt a single data channel as a one-member multiplex with no
  // replenishment; the multi-channel factories build wider ones directly.
  std::shared_ptr<internal::MultiplexedBinaryChannel> multiplex =
      internal::MultiplexedBinaryChannel::Create(
          {std::move(channel)}, /*factory=*/{},
          internal::MultiplexedChannelOptions{.target_channels = 1});
  return BuildMultiplexedStream(
      std::move(multiplex), std::move(data_channel), std::move(connection),
      std::move(signalling_endpoint), std::move(id), role, options,
      std::move(open_operation), split_size);
}

absl::StatusOr<std::shared_ptr<WebRtcWireStream>>
WebRtcWireStream::CreateClient(std::string identity, std::string peer_identity,
                               std::shared_ptr<SignallingService> signalling,
                               WebRtcConfiguration configuration,
                               WireStreamOptions options) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(identity));
  if (signalling == nullptr) {
    return absl::InvalidArgumentError(
        "WebRTC signalling service must not be null");
  }
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<SignallingEndpoint> endpoint,
      signalling->Connect(std::move(identity),
                          [](SignallingMessage) { return a11::ReadyTask(); }));
  return CreateClient(std::move(peer_identity), std::move(endpoint),
                      std::move(configuration), options);
}

absl::StatusOr<std::shared_ptr<WebRtcWireStream>>
WebRtcWireStream::CreateClient(std::string peer_identity,
                               std::shared_ptr<SignallingTransport> signalling,
                               WebRtcConfiguration configuration,
                               WireStreamOptions options) {
  if (signalling == nullptr) {
    return absl::InvalidArgumentError(
        "WebRTC signalling transport must not be null");
  }
  const std::string identity = signalling->identity();
  ABSL_RETURN_IF_ERROR(data::ValidateName(identity));
  ABSL_RETURN_IF_ERROR(data::ValidateName(peer_identity));
  if (identity == peer_identity) {
    return absl::InvalidArgumentError(
        "WebRTC identity and peer_identity must differ");
  }
  ABSL_RETURN_IF_ERROR(options.Validate());
  ABSL_ASSIGN_OR_RETURN(rtc::Configuration rtc_configuration,
                        BuildLibDataChannelConfiguration(configuration));

  std::shared_ptr<rtc::PeerConnection> connection;
  try {
    connection =
        std::make_shared<rtc::PeerConnection>(std::move(rtc_configuration));
  } catch (const std::exception& error) {
    (void)signalling->Close();
    return ExternalException(error, "Creating WebRTC peer connection");
  } catch (...) {
    (void)signalling->Close();
    return absl::UnknownError(
        "Creating WebRTC peer connection raised an exception");
  }
  auto context = std::make_shared<ClientPeerContext>(
      identity, std::move(peer_identity), signalling, connection);
  std::weak_ptr<ClientPeerContext> weak = context;
  absl::Status validation =
      signalling->SetOnMessage([weak](SignallingMessage message) {
        std::shared_ptr<ClientPeerContext> context = weak.lock();
        if (context == nullptr) {
          return a11::ReadyTask();
        }
        return HandleClientSignal(context, std::move(message));
      });
  if (!validation.ok()) {
    (void)signalling->Close();
    return validation;
  }
  std::shared_ptr<rtc::DataChannel> data_channel;
  try {
    connection->onLocalDescription([weak](rtc::Description description) {
      std::shared_ptr<ClientPeerContext> context = weak.lock();
      if (context == nullptr) {
        return;
      }
      SignallingMessage message{.type = SignallingMessageType::kDescription,
                                .sender = context->identity,
                                .recipient = context->peer_identity,
                                .description = description.generateSdp("\r\n"),
                                .description_type = description.typeString()};
      absl::Status sent = context->endpoint->Send(std::move(message));
      if (!sent.ok()) {
        FailClient(context, sent);
      }
    });
    connection->onLocalCandidate([weak](rtc::Candidate candidate) {
      std::shared_ptr<ClientPeerContext> context = weak.lock();
      if (context == nullptr) {
        return;
      }
      SignallingMessage message{.type = SignallingMessageType::kCandidate,
                                .sender = context->identity,
                                .recipient = context->peer_identity,
                                .candidate = std::string(candidate),
                                .mid = candidate.mid()};
      absl::Status sent = context->endpoint->Send(std::move(message));
      if (!sent.ok()) {
        FailClient(context, sent);
      }
    });
    connection->onStateChange([weak](rtc::PeerConnection::State state) {
      if (state != rtc::PeerConnection::State::Failed &&
          state != rtc::PeerConnection::State::Closed) {
        return;
      }
      if (std::shared_ptr<ClientPeerContext> context = weak.lock();
          context != nullptr) {
        FailClient(context,
                   state == rtc::PeerConnection::State::Failed
                       ? absl::UnavailableError("WebRTC peer connection failed")
                       : absl::CancelledError("WebRTC peer connection closed"));
      }
    });
    // Open the desired number of data channels up front so packets can stripe
    // across them from the first send; the multiplex replenishes losses later.
    const size_t desired = configuration.desired_channels == 0
                               ? 1
                               : configuration.desired_channels;
    std::vector<std::shared_ptr<internal::BinaryChannel>> members;
    members.reserve(desired);
    for (size_t index = 0; index < desired; ++index) {
      std::shared_ptr<rtc::DataChannel> channel =
          connection->createDataChannel(NewDataChannelId());
      if (index == 0) {
        data_channel = channel;
      }
      absl::StatusOr<std::shared_ptr<internal::BinaryChannel>> wrapped =
          internal::MakeRtcBinaryChannel(std::move(channel));
      if (!wrapped.ok()) {
        FailClient(context, wrapped.status());
        return wrapped.status();
      }
      members.push_back(std::move(*wrapped));
    }
    {
      thread::MutexLock lock(&context->mu);
      context->data_channel = data_channel;
    }
    // Replenishment reopens a data channel on the same peer connection.
    std::weak_ptr<rtc::PeerConnection> weak_connection = connection;
    internal::MemberChannelFactory factory = [weak_connection]()
        -> absl::StatusOr<std::shared_ptr<internal::BinaryChannel>> {
      std::shared_ptr<rtc::PeerConnection> connection = weak_connection.lock();
      if (connection == nullptr) {
        return absl::UnavailableError("WebRTC peer connection is gone");
      }
      std::shared_ptr<rtc::DataChannel> channel;
      try {
        channel = connection->createDataChannel(NewDataChannelId());
      } catch (const std::exception& error) {
        return ExternalException(error, "Creating replenishment data channel");
      } catch (...) {
        return absl::UnknownError(
            "Creating replenishment data channel raised an exception");
      }
      return internal::MakeRtcBinaryChannel(std::move(channel));
    };
    std::string id = data_channel->label();
    std::shared_ptr<internal::MultiplexedBinaryChannel> multiplex =
        internal::MultiplexedBinaryChannel::Create(
            std::move(members), std::move(factory),
            internal::MultiplexedChannelOptions{.target_channels = desired});
    OpenOperation open = [context]() {
      return ClientStatus(context);
    };
    absl::StatusOr<std::shared_ptr<WebRtcWireStream>> stream =
        BuildMultiplexedStream(
            std::move(multiplex), data_channel, connection, context->endpoint,
            std::move(id), ChannelEndpointRole::kClient, options,
            std::move(open), configuration.channel_split_size);
    if (!stream.ok()) {
      FailClient(context, stream.status());
      return stream.status();
    }
    return stream;
  } catch (const std::exception& error) {
    absl::Status status = ExternalException(error, "Configuring WebRTC client");
    FailClient(context, status);
    return status;
  } catch (...) {
    const absl::Status status =
        absl::UnknownError("Configuring WebRTC client raised an exception");
    FailClient(context, status);
    return status;
  }
}

WebRtcWireStream::~WebRtcWireStream() {
  try {
    if (connection_ != nullptr) {
      connection_->resetCallbacks();
      connection_->close();
    }
  } catch (...) {}
}

std::shared_ptr<rtc::DataChannel> WebRtcWireStream::data_channel() const {
  return data_channel_;
}

std::shared_ptr<rtc::PeerConnection> WebRtcWireStream::peer_connection() const {
  return connection_;
}

std::shared_ptr<SignallingTransport> WebRtcWireStream::signalling_endpoint()
    const {
  return signalling_endpoint_;
}

namespace {

struct ServerPeerContext {
  ServerPeerContext(std::string value_identity,
                    std::shared_ptr<rtc::PeerConnection> value_connection)
      : identity(std::move(value_identity)),
        connection(std::move(value_connection)) {}

  mutable thread::Mutex mu;
  const std::string identity;
  const std::shared_ptr<rtc::PeerConnection> connection;
  std::shared_ptr<rtc::DataChannel> data_channel ABSL_GUARDED_BY(mu);
  // All data channels a peer opens belong to one logical stream; the first
  // creates the multiplex and the rest are admitted into it up to max_channels.
  std::shared_ptr<internal::MultiplexedBinaryChannel> multiplex
      ABSL_GUARDED_BY(mu);
  absl::Status status ABSL_GUARDED_BY(mu);
  bool failed ABSL_GUARDED_BY(mu) = false;
};

absl::Status ServerPeerStatus(
    const std::shared_ptr<ServerPeerContext>& context) {
  thread::MutexLock lock(&context->mu);
  return context->status;
}

}  // namespace

struct WebRtcWireServer::State {
  State(std::string value_identity,
        std::shared_ptr<SignallingService> value_signalling,
        OnWebRtcStream stream_callback, WebRtcConfiguration value_configuration,
        WireStreamOptions value_stream_options)
      : identity(std::move(value_identity)),
        signalling(std::move(value_signalling)),
        on_stream(std::move(stream_callback)),
        configuration(std::move(value_configuration)),
        stream_options(value_stream_options) {}

  mutable thread::Mutex mu;
  const std::string identity;
  const std::shared_ptr<SignallingService> signalling;
  std::shared_ptr<SignallingEndpoint> endpoint ABSL_GUARDED_BY(mu);
  const OnWebRtcStream on_stream;
  const WebRtcConfiguration configuration;
  const WireStreamOptions stream_options;
  bool running ABSL_GUARDED_BY(mu) = true;
  absl::flat_hash_map<std::string, std::shared_ptr<ServerPeerContext>> peers
      ABSL_GUARDED_BY(mu);
};

absl::StatusOr<std::shared_ptr<WebRtcWireServer>> WebRtcWireServer::Create(
    std::string identity, std::shared_ptr<SignallingService> signalling,
    OnWebRtcStream on_stream, WebRtcConfiguration configuration,
    WireStreamOptions stream_options) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(identity));
  if (signalling == nullptr) {
    return absl::InvalidArgumentError(
        "WebRTC signalling service must not be null");
  }
  if (!on_stream) {
    return absl::InvalidArgumentError("WebRTC on_stream must be callable");
  }
  ABSL_RETURN_IF_ERROR(configuration.Validate());
  ABSL_RETURN_IF_ERROR(stream_options.Validate());
  auto state = std::make_shared<State>(
      std::move(identity), std::move(signalling), std::move(on_stream),
      std::move(configuration), stream_options);
  std::weak_ptr<State> weak = state;
  {
    // Registration makes the callback externally reachable. Keep the state
    // lock held until endpoint publication so an immediate offer cannot
    // observe a partially initialized server.
    thread::MutexLock lock(&state->mu);
    ABSL_ASSIGN_OR_RETURN(
        std::shared_ptr<SignallingEndpoint> endpoint,
        state->signalling->Connect(state->identity,
                                   [weak](SignallingMessage message) {
                                     std::shared_ptr<State> state = weak.lock();
                                     if (state == nullptr) {
                                       return a11::ReadyTask();
                                     }
                                     return OnSignal(state, std::move(message));
                                   }));
    state->endpoint = std::move(endpoint);
  }

  struct MakeSharedEnabler final : WebRtcWireServer {
    explicit MakeSharedEnabler(std::shared_ptr<State> state)
        : WebRtcWireServer(std::move(state)) {}
  };

  return std::make_shared<MakeSharedEnabler>(std::move(state));
}

WebRtcWireServer::~WebRtcWireServer() {
  (void)Stop();
}

a11::Task WebRtcWireServer::OnSignal(const std::shared_ptr<State>& state,
                                     SignallingMessage message) {
  absl::Status status;
  if (message.type == SignallingMessageType::kDescription &&
      message.description_type == "offer") {
    status = HandleOffer(state, message);
  } else if (message.type == SignallingMessageType::kCandidate) {
    status = HandleCandidate(state, message);
  } else if (message.type == SignallingMessageType::kError) {
    ReportPeerError(state, message.sender, message.error);
    status = absl::OkStatus();
  } else {
    status = absl::InvalidArgumentError(
        "WebRTC server expected an offer or ICE candidate");
  }
  if (!status.ok()) {
    ReportPeerError(state, message.sender, status);
    // One malformed peer must not disconnect the server identity from the
    // shared signalling service.
    return a11::ReadyTask();
  }
  return a11::ReadyTask();
}

absl::Status WebRtcWireServer::HandleOffer(const std::shared_ptr<State>& state,
                                           const SignallingMessage& message) {
  {
    thread::MutexLock lock(&state->mu);
    if (!state->running) {
      return absl::FailedPreconditionError("WebRTC server is stopped");
    }
    if (state->peers.find(message.sender) != state->peers.end()) {
      return absl::AlreadyExistsError(
          absl::StrCat("WebRTC peer already exists: ", message.sender));
    }
  }
  ABSL_ASSIGN_OR_RETURN(rtc::Configuration configuration,
                        BuildLibDataChannelConfiguration(state->configuration));
  std::shared_ptr<rtc::PeerConnection> connection;
  try {
    connection =
        std::make_shared<rtc::PeerConnection>(std::move(configuration));
  } catch (const std::exception& error) {
    return ExternalException(error, "Creating WebRTC server peer");
  } catch (...) {
    return absl::UnknownError(
        "Creating WebRTC server peer raised an exception");
  }
  auto context =
      std::make_shared<ServerPeerContext>(message.sender, connection);
  {
    thread::MutexLock lock(&state->mu);
    if (!state->running) {
      try {
        context->connection->close();
      } catch (...) {}
      return absl::FailedPreconditionError("WebRTC server is stopped");
    }
    auto [iterator, inserted] = state->peers.emplace(message.sender, context);
    if (!inserted) {
      return absl::AlreadyExistsError(
          absl::StrCat("WebRTC peer already exists: ", message.sender));
    }
    (void)iterator;
  }
  std::weak_ptr<State> weak_state = state;
  std::weak_ptr<ServerPeerContext> weak_context = context;
  try {
    connection->onLocalDescription([weak_state, peer = message.sender](
                                       rtc::Description description) {
      std::shared_ptr<State> state = weak_state.lock();
      if (state == nullptr) {
        return;
      }
      std::shared_ptr<SignallingEndpoint> endpoint;
      std::string identity;
      {
        thread::MutexLock lock(&state->mu);
        if (!state->running) {
          return;
        }
        endpoint = state->endpoint;
        identity = state->identity;
      }
      SignallingMessage reply{.type = SignallingMessageType::kDescription,
                              .sender = std::move(identity),
                              .recipient = peer,
                              .description = description.generateSdp("\r\n"),
                              .description_type = description.typeString()};
      absl::Status sent = endpoint->Send(std::move(reply));
      if (!sent.ok()) {
        ReportPeerError(state, peer, sent);
      }
    });
    connection->onLocalCandidate(
        [weak_state, peer = message.sender](rtc::Candidate candidate) {
          std::shared_ptr<State> state = weak_state.lock();
          if (state == nullptr) {
            return;
          }
          std::shared_ptr<SignallingEndpoint> endpoint;
          std::string identity;
          {
            thread::MutexLock lock(&state->mu);
            if (!state->running) {
              return;
            }
            endpoint = state->endpoint;
            identity = state->identity;
          }
          SignallingMessage reply{.type = SignallingMessageType::kCandidate,
                                  .sender = std::move(identity),
                                  .recipient = peer,
                                  .candidate = std::string(candidate),
                                  .mid = candidate.mid()};
          absl::Status sent = endpoint->Send(std::move(reply));
          if (!sent.ok()) {
            ReportPeerError(state, peer, sent);
          }
        });
    connection->onStateChange(
        [weak_state, peer = message.sender](rtc::PeerConnection::State status) {
          if (status == rtc::PeerConnection::State::Failed) {
            if (std::shared_ptr<State> state = weak_state.lock();
                state != nullptr) {
              ReportPeerError(
                  state, peer,
                  absl::UnavailableError("WebRTC peer connection failed"));
            }
          } else if (status == rtc::PeerConnection::State::Closed) {
            if (std::shared_ptr<State> state = weak_state.lock();
                state != nullptr) {
              thread::MutexLock lock(&state->mu);
              state->peers.erase(peer);
            }
          }
        });
    connection->onDataChannel([weak_state, weak_context](
                                  std::shared_ptr<rtc::DataChannel> channel) {
      std::shared_ptr<State> state = weak_state.lock();
      std::shared_ptr<ServerPeerContext> context = weak_context.lock();
      if (state == nullptr || context == nullptr || channel == nullptr) {
        return;
      }
      std::shared_ptr<SignallingEndpoint> endpoint;
      OnWebRtcStream callback;
      WebRtcConfiguration configuration;
      WireStreamOptions options;
      {
        thread::MutexLock lock(&state->mu);
        if (!state->running) {
          try {
            channel->close();
          } catch (...) {}
          return;
        }
        endpoint = state->endpoint;
        callback = state->on_stream;
        configuration = state->configuration;
        options = state->stream_options;
      }
      absl::StatusOr<std::shared_ptr<internal::BinaryChannel>> wrapped =
          internal::MakeRtcBinaryChannel(channel);
      if (!wrapped.ok()) {
        ReportPeerError(state, context->identity, wrapped.status());
        return;
      }
      std::string id;
      try {
        id = channel->label();
      } catch (...) {
        id.clear();
      }
      // The first data channel builds the multiplex and the stream; later
      // channels join the same multiplex (rejected past max_channels).
      std::shared_ptr<internal::MultiplexedBinaryChannel> multiplex;
      bool first = false;
      {
        thread::MutexLock lock(&context->mu);
        if (context->failed) {
          try {
            channel->close();
          } catch (...) {}
          return;
        }
        if (context->multiplex == nullptr) {
          const size_t max_channels =
              configuration.max_channels == 0 ? 1 : configuration.max_channels;
          context->multiplex = internal::MultiplexedBinaryChannel::Create(
              {*wrapped}, /*factory=*/{},
              internal::MultiplexedChannelOptions{.target_channels =
                                                      max_channels});
          context->data_channel = channel;
          multiplex = context->multiplex;
          first = true;
        } else {
          multiplex = context->multiplex;
        }
      }
      if (!first) {
        if (!multiplex->AddMember(*wrapped)) {
          // Past the server's per-peer channel cap; refuse the surplus.
          try {
            channel->close();
          } catch (...) {}
        }
        return;
      }
      ChannelWireStream::OpenOperation open = [context]() {
        return ServerPeerStatus(context);
      };
      absl::StatusOr<std::shared_ptr<WebRtcWireStream>> stream =
          WebRtcWireStream::BuildMultiplexedStream(
              std::move(multiplex), channel, context->connection,
              std::move(endpoint), std::move(id), ChannelEndpointRole::kServer,
              options, std::move(open), configuration.channel_split_size);
      if (!stream.ok()) {
        ReportPeerError(state, context->identity, stream.status());
        return;
      }
      a11::Schedule(
          [callback = std::move(callback), stream = std::move(*stream)]() {
            absl::Status status;
            try {
              status = callback(stream).Await().status();
            } catch (const std::exception& error) {
              status = ExternalException(error, "WebRTC on_stream callback");
            } catch (...) {
              status = absl::UnknownError(
                  "WebRTC on_stream callback raised an exception");
            }
            if (!status.ok()) {
              (void)stream->Abort(status);
            }
          });
    });
    connection->setRemoteDescription(
        rtc::Description(message.description, message.description_type));
  } catch (const std::exception& error) {
    return ExternalException(error, "Configuring WebRTC server peer");
  } catch (...) {
    return absl::UnknownError(
        "Configuring WebRTC server peer raised an exception");
  }
  return absl::OkStatus();
}

absl::Status WebRtcWireServer::HandleCandidate(
    const std::shared_ptr<State>& state, const SignallingMessage& message) {
  std::shared_ptr<ServerPeerContext> context;
  {
    thread::MutexLock lock(&state->mu);
    const auto iterator = state->peers.find(message.sender);
    if (iterator == state->peers.end()) {
      return absl::NotFoundError(absl::StrCat(
          "ICE candidate belongs to unknown peer: ", message.sender));
    }
    context = iterator->second;
  }
  try {
    context->connection->addRemoteCandidate(
        rtc::Candidate(message.candidate, message.mid));
    return absl::OkStatus();
  } catch (const std::exception& error) {
    return ExternalException(error, "Applying WebRTC server ICE candidate");
  } catch (...) {
    return absl::UnknownError(
        "Applying WebRTC server ICE candidate raised an exception");
  }
}

void WebRtcWireServer::ReportPeerError(const std::shared_ptr<State>& state,
                                       std::string peer_identity,
                                       absl::Status status) {
  if (status.ok()) {
    status = absl::UnknownError("WebRTC peer failed");
  }
  std::shared_ptr<ServerPeerContext> context;
  std::shared_ptr<SignallingEndpoint> endpoint;
  std::string identity;
  {
    thread::MutexLock lock(&state->mu);
    const auto iterator = state->peers.find(peer_identity);
    if (iterator != state->peers.end()) {
      context = iterator->second;
      state->peers.erase(iterator);
    }
    if (state->running) {
      endpoint = state->endpoint;
      identity = state->identity;
    }
  }
  if (context != nullptr) {
    std::shared_ptr<rtc::DataChannel> channel;
    std::shared_ptr<rtc::PeerConnection> connection;
    {
      thread::MutexLock lock(&context->mu);
      if (!context->failed) {
        context->failed = true;
        context->status = status;
      }
      channel = context->data_channel;
      connection = context->connection;
    }
    try {
      if (channel != nullptr) {
        channel->close();
      }
    } catch (...) {}
    try {
      if (connection != nullptr) {
        connection->close();
      }
    } catch (...) {}
  }
  if (endpoint != nullptr && !peer_identity.empty()) {
    SignallingMessage error{.type = SignallingMessageType::kError,
                            .sender = std::move(identity),
                            .recipient = std::move(peer_identity),
                            .error = std::move(status)};
    (void)endpoint->Send(std::move(error));
  }
}

absl::Status WebRtcWireServer::Stop() {
  absl::flat_hash_map<std::string, std::shared_ptr<ServerPeerContext>> peers;
  std::shared_ptr<SignallingEndpoint> endpoint;
  {
    thread::MutexLock lock(&state_->mu);
    if (!state_->running) {
      return absl::OkStatus();
    }
    state_->running = false;
    peers.swap(state_->peers);
    endpoint = std::move(state_->endpoint);
  }
  if (endpoint != nullptr) {
    (void)endpoint->Close();
  }
  for (auto& [identity, context] : peers) {
    (void)identity;
    std::shared_ptr<rtc::DataChannel> data_channel;
    {
      thread::MutexLock lock(&context->mu);
      data_channel = std::exchange(context->data_channel, nullptr);
    }
    try {
      if (data_channel != nullptr) {
        data_channel->close();
      }
    } catch (...) {}
    try {
      if (context->connection != nullptr) {
        context->connection->resetCallbacks();
        context->connection->close();
      }
    } catch (...) {}
  }
  return absl::OkStatus();
}

std::string WebRtcWireServer::identity() const {
  thread::MutexLock lock(&state_->mu);
  return state_->identity;
}

bool WebRtcWireServer::running() const {
  thread::MutexLock lock(&state_->mu);
  return state_->running;
}

size_t WebRtcWireServer::pending_peer_count() const {
  thread::MutexLock lock(&state_->mu);
  return state_->peers.size();
}

std::shared_ptr<SignallingEndpoint> WebRtcWireServer::signalling_endpoint()
    const {
  thread::MutexLock lock(&state_->mu);
  return state_->endpoint;
}

}  // namespace a11::net
