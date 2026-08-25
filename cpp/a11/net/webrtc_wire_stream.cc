// Copyright 2026 The A11 Authors.

#include "a11/net/webrtc_wire_stream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
#include <rtc/rtc.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/channel_wire_stream.h"
#include "a11/net/internal/binary_channel.h"
#include "a11/net/internal/multiplexed_binary_channel.h"
#include "a11/net/internal/path_mtu.h"
#include "a11/net/signalling.h"
#include "a11/net/wire_stream.h"
#include "a11/uuid.h"
#include "thread/boost_primitives.h"

namespace a11::net {
namespace {

std::string NewDataChannelId() {
  return NewStreamId("a11-");
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
      // Max burst is how many MTUs SCTP may put on the wire per send
      // opportunity, and libdatachannel's default of 10 is worth ~10% at
      // 64 KiB: on the bare data channel, raising it took loopback throughput
      // from 133 to 146 MiB/s on Linux, monotonically across 10/32/64/128.
      // Unlike WebRtcConfiguration::mtu this is a property of the sender rather
      // than of the path, so there is no value at which a peer stops being
      // reachable and no reason to leave it at the smaller number. (The initial
      // congestion window, swept over 10/32/100 in the same run, moved nothing
      // -- these benchmarks are long enough that the window has long since
      // opened, so it is startup latency rather than throughput and is left
      // alone.)
      settings.maxBurst = 128;
      rtc::SetSctpSettings(settings);
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
  // Set once the stream exists, so the answer handler can hand it a probe
  // channel. Weak: the application owns the stream's lifetime, and a context
  // captured by libdatachannel callbacks must not extend it.
  std::weak_ptr<WebRtcWireStream> stream ABSL_GUARDED_BY(mu);
  std::weak_ptr<internal::MultiplexedBinaryChannel> multiplex
      ABSL_GUARDED_BY(mu);
  WebRtcConfiguration configuration;
  bool probe_channel_opened ABSL_GUARDED_BY(mu) = false;
};

// Opens the probe channel, once, after the peer has said it understands one.
//
// Deferred to the answer rather than done at dial time on purpose. A client
// cannot know at dial time whether the peer supports probing, and a probe channel
// opened unconditionally would be admitted into an older server's multiplex --
// which would then send application data over it that this side would silently
// drop as "not a probe frame". Creating a data channel after the association is
// up needs no renegotiation (DCEP carries it in band), which is the same
// mechanism the multiplex's own replenishment already relies on.

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
                             const SignallingMessage& message) {
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
  // 576 is the IPv4 minimum reassembly buffer; below it the headers
  // libdatachannel subtracts (12 + 48 + 8 + 40) leave no room for a chunk.
  if (path_mtu_discovery && max_discovered_mtu < kWebRtcBaseMtu) {
    return absl::InvalidArgumentError(absl::StrCat(
        "WebRTC max_discovered_mtu must be at least the base MTU of ",
        kWebRtcBaseMtu));
  }
  if (path_mtu_discovery && probe_timeout <= absl::ZeroDuration()) {
    return absl::InvalidArgumentError("WebRTC probe_timeout must be positive");
  }
  if (mtu.has_value() && *mtu < 576) {
    return absl::InvalidArgumentError(
        "WebRTC mtu must be at least 576 or absent");
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

internal::PathMtuOptions BuildPathMtuOptions(
    const WebRtcConfiguration& configuration) {
  internal::PathMtuOptions options;
  // The configured MTU is the *floor* once discovery is on: a caller that pinned
  // a value knows something about the path, and the search should start from that
  // knowledge rather than throw it away and re-derive 1280.
  options.base_mtu =
      std::max(configuration.mtu.value_or(kWebRtcBaseMtu), kWebRtcBaseMtu);
  options.min_mtu = kWebRtcMinMtu;
  options.max_mtu =
      std::max(configuration.max_discovered_mtu, options.base_mtu);
  options.probe_timeout = configuration.probe_timeout;
  options.raise_timer = configuration.path_mtu_raise_interval;
  options.startup_retry = configuration.path_mtu_startup_retry;
  return options;
}

absl::StatusOr<rtc::Configuration> BuildLibDataChannelConfiguration(
    const WebRtcConfiguration& configuration) {
  ABSL_RETURN_IF_ERROR(configuration.Validate());
  ABSL_RETURN_IF_ERROR(InitializeSctp());
  try {
    rtc::Configuration result;
    result.maxMessageSize = configuration.max_message_size;
    result.mtu = configuration.mtu;
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
    OpenOperation open_operation, size_t split_size, size_t configured_mtu) {
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
      std::move(connection), std::move(signalling_endpoint), configured_mtu);
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
WebRtcWireStream::CreateClient(
    std::string identity, std::string peer_identity,
    const std::shared_ptr<SignallingService>& signalling,
    const WebRtcConfiguration& configuration, WireStreamOptions options) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(identity));
  if (signalling == nullptr) {
    return absl::InvalidArgumentError(
        "WebRTC signalling service must not be null");
  }
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<SignallingEndpoint> endpoint,
      signalling->Connect(std::move(identity), [](const SignallingMessage&) {
        return a11::ReadyTask();
      }));
  return CreateClient(std::move(peer_identity), std::move(endpoint),
                      configuration, options);
}

absl::StatusOr<std::shared_ptr<WebRtcWireStream>>
WebRtcWireStream::CreateClient(
    std::string peer_identity,
    const std::shared_ptr<SignallingTransport>& signalling,
    const WebRtcConfiguration& configuration, WireStreamOptions options) {
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
  {
    thread::MutexLock lock(&context->mu);
    context->configuration = configuration;
  }
  std::weak_ptr<ClientPeerContext> weak = context;
  absl::Status validation =
      signalling->SetOnMessage([weak](const SignallingMessage& message) {
        std::shared_ptr<ClientPeerContext> held_context = weak.lock();
        if (held_context == nullptr) {
          return a11::ReadyTask();
        }
        return HandleClientSignal(held_context, message);
      });
  if (!validation.ok()) {
    (void)signalling->Close();
    return validation;
  }
  std::shared_ptr<rtc::DataChannel> data_channel;
  try {
    connection->onLocalDescription([weak](const rtc::Description& description) {
      std::shared_ptr<ClientPeerContext> held_context = weak.lock();
      if (held_context == nullptr) {
        return;
      }
      SignallingMessage message{.type = SignallingMessageType::kDescription,
                                .sender = held_context->identity,
                                .recipient = held_context->peer_identity,
                                .description = description.generateSdp("\r\n"),
                                .description_type = description.typeString()};
      absl::Status sent = held_context->endpoint->Send(std::move(message));
      if (!sent.ok()) {
        FailClient(held_context, sent);
      }
    });
    connection->onLocalCandidate([weak](const rtc::Candidate& candidate) {
      std::shared_ptr<ClientPeerContext> held_context = weak.lock();
      if (held_context == nullptr) {
        return;
      }
      SignallingMessage message{.type = SignallingMessageType::kCandidate,
                                .sender = held_context->identity,
                                .recipient = held_context->peer_identity,
                                .candidate = std::string(candidate),
                                .mid = candidate.mid()};
      absl::Status sent = held_context->endpoint->Send(std::move(message));
      if (!sent.ok()) {
        FailClient(held_context, sent);
      }
    });
    connection->onStateChange([weak](rtc::PeerConnection::State state) {
      if (state != rtc::PeerConnection::State::Failed &&
          state != rtc::PeerConnection::State::Closed) {
        return;
      }
      if (std::shared_ptr<ClientPeerContext> held_context = weak.lock();
          held_context != nullptr) {
        FailClient(held_context,
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
      std::shared_ptr<rtc::PeerConnection> held_connection =
          weak_connection.lock();
      if (held_connection == nullptr) {
        return absl::UnavailableError("WebRTC peer held_connection is gone");
      }
      std::shared_ptr<rtc::DataChannel> channel;
      try {
        channel = held_connection->createDataChannel(NewDataChannelId());
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
    // Kept for the probe path, which pauses it around each candidate. The
    // BuildMultiplexedStream call below moves the owning pointer away.
    const std::shared_ptr<internal::MultiplexedBinaryChannel>
        multiplex_for_probing = multiplex;
    OpenOperation open = [context]() {
      return ClientStatus(context);
    };
    absl::StatusOr<std::shared_ptr<WebRtcWireStream>> stream =
        BuildMultiplexedStream(
            std::move(multiplex), data_channel, connection, context->endpoint,
            std::move(id), ChannelEndpointRole::kClient, options,
            std::move(open), configuration.channel_split_size,
            configuration.mtu.value_or(kWebRtcBaseMtu));
    if (!stream.ok()) {
      FailClient(context, stream.status());
      return stream.status();
    }
    {
      thread::MutexLock lock(&context->mu);
      context->stream = *stream;
      context->multiplex = multiplex_for_probing;
    }
    // Each side discovers its own *send* direction: an MTU bounds what a sender
    // emits, so both ends search independently and neither needs the other to do
    // anything but be a conforming SCTP peer.
    if (configuration.path_mtu_discovery) {
      (*stream)->StartPathMtuDiscovery(configuration, multiplex_for_probing);
    }
    return stream;
  } catch (const std::exception& error) {
    absl::Status status = ExternalException(error, "Configuring WebRTC client");
    FailClient(context, status);
    return status;
  } catch (...) {
    absl::Status status =
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

// If libdatachannel's fallback ever stops being the IPv6 minimum, A11's idea of
// "the size that needs no evidence" has to move with it -- and silently
// disagreeing would mean probing upward from a base the peer never agreed to.
static_assert(kWebRtcBaseMtu == RTC_DEFAULT_MTU,
              "kWebRtcBaseMtu must match libdatachannel's RTC_DEFAULT_MTU");

absl::Status WebRtcWireStream::SetPathMtu(size_t mtu) {
  if (mtu < kWebRtcMinMtu) {
    return absl::InvalidArgumentError(absl::StrCat(
        "WebRTC path MTU must be at least ", kWebRtcMinMtu, ", got ", mtu));
  }
  if (connection_ == nullptr) {
    return absl::FailedPreconditionError(
        "WebRTC stream has no peer connection");
  }
  bool applied = false;
  try {
    applied = connection_->setPathMtu(mtu);
  } catch (const std::exception& error) {
    return ExternalException(error, "Setting the WebRTC path MTU");
  } catch (...) {
    return absl::UnknownError(
        "Setting the WebRTC path MTU raised a non-standard exception");
  }
  if (!applied) {
    // No association yet, or usrsctp refused. Deliberately not an error about
    // the *path*: a caller searching for a working MTU must be able to tell
    // "the stack would not take this" from "the path dropped this", and only
    // the second is evidence about the network.
    return absl::FailedPreconditionError(
        absl::StrCat("WebRTC association would not accept a path MTU of ", mtu,
                     " (not connected yet, or out of range)"));
  }
  {
    thread::MutexLock lock(&path_mtu_mu_);
    path_mtu_ = mtu;
  }
  return absl::OkStatus();
}

void WebRtcWireStream::StartPathMtuDiscovery(
    const WebRtcConfiguration& configuration,
    const std::shared_ptr<internal::MultiplexedBinaryChannel>& stream_data) {
  if (connection_ == nullptr) {
    return;
  }
  std::shared_ptr<internal::PathMtuDiscovery> discovery;
  {
    thread::MutexLock lock(&path_mtu_mu_);
    if (discovery_ != nullptr) {
      return;
    }
  }
  const std::shared_ptr<rtc::PeerConnection> connection = connection_;
  const auto next_probe_id = std::make_shared<std::atomic<std::uint32_t>>(1);
  internal::PathMtuProber prober{
      // Only ever called with a size a probe has *confirmed*, so this applies the
      // association MTU for real -- fragmentation and all. Probing itself changes
      // nothing: the probe packet carries its own ceiling for the instant it is
      // emitted (see the usrsctp patch), so application traffic never sees a size
      // the path has not acknowledged and there is nothing to hold back.
      .apply = [connection](size_t mtu) -> absl::Status {
        bool ok = false;
        try {
          ok = connection->setPathMtu(mtu);
        } catch (...) {
          ok = false;
        }
        return ok ? absl::OkStatus()
                  : absl::FailedPreconditionError(
                        "WebRTC association would not take a path MTU");
      },
      .probe_burst = [connection, next_probe_id](
                         size_t payload, int count,
                         absl::Time deadline) -> std::optional<int> {
        // `payload` is the chunk space the search wants on the wire. A padded
        // HEARTBEAT of that size is the probe: any conforming peer answers it,
        // and the stack needs no MTU change to send one, so nothing about the
        // stream is disturbed.
        //
        // Sent back to back without waiting for each answer, so the burst is in
        // flight together. That is the point: probes that wait for each other
        // are each isolated, and a size that survives only in isolation is
        // exactly the false positive this has to reject.
        const std::uint32_t before = connection->pathMtuProbeAckCount();
        std::uint32_t last = 0;
        int sent = 0;
        for (int index = 0; index < count; ++index) {
          const std::uint32_t id =
              next_probe_id->fetch_add(1, std::memory_order_relaxed);
          bool ok = false;
          try {
            ok = connection->sendPathMtuProbe(payload, id);
          } catch (...) {
            ok = false;
          }
          if (!ok) {
            break;
          }
          last = id;
          ++sent;
        }
        if (sent == 0) {
          return std::nullopt;  // No association yet: no evidence.
        }
        // Polled: the acknowledgement is consumed by SCTP itself, so there is
        // nothing to wait on. Requiring the *last* id as well as the count is
        // what stops a late answer from an earlier burst standing in for a
        // probe of this one.
        while (absl::Now() < deadline) {
          const std::uint32_t acked = connection->pathMtuProbeAckCount();
          if ((acked - before >= static_cast<std::uint32_t>(sent)) &&
              (connection->acknowledgedPathMtuProbe() == last)) {
            return sent;
          }
          thread::SleepFor(absl::Milliseconds(1));
        }
        const std::uint32_t acked = connection->pathMtuProbeAckCount();
        return static_cast<int>(std::min<std::uint32_t>(
            acked - before, static_cast<std::uint32_t>(sent)));
      },
  };
  discovery = std::make_shared<internal::PathMtuDiscovery>(
      BuildPathMtuOptions(configuration), std::move(prober));
  {
    thread::MutexLock lock(&path_mtu_mu_);
    if (discovery_ != nullptr) {
      return;
    }
    discovery_ = discovery;
  }
  // The safety net for a size that was confirmed and later stops working -- a path
  // that changed, or a burst of probes that was luckier than a stream of data. A run
  // of send failures drops the association back to the base MTU and re-searches;
  // without this the search has fall-back logic that nothing ever triggers.
  if (stream_data != nullptr) {
    std::weak_ptr<internal::PathMtuDiscovery> weak = discovery;
    stream_data->SetSendOutcomeObserver([weak](bool succeeded) {
      if (auto search = weak.lock(); search != nullptr) {
        if (succeeded) {
          search->ReportSendSuccess();
        } else {
          search->ReportSendFailure();
        }
      }
    });
  }
  a11::Schedule(
      [discovery = std::move(discovery)]() mutable { discovery->Run(); });
}

size_t WebRtcWireStream::discovered_path_mtu() const {
  std::shared_ptr<internal::PathMtuDiscovery> discovery;
  {
    thread::MutexLock lock(&path_mtu_mu_);
    discovery = discovery_;
  }
  return discovery != nullptr ? discovery->confirmed_mtu() : 0;
}

size_t WebRtcWireStream::current_path_mtu() const {
  thread::MutexLock lock(&path_mtu_mu_);
  return path_mtu_;
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
  std::weak_ptr<WebRtcWireStream> stream ABSL_GUARDED_BY(mu);
};

absl::Status ServerPeerStatus(
    const std::shared_ptr<ServerPeerContext>& context) {
  thread::MutexLock lock(&context->mu);
  return context->status;
}

}  // namespace

struct WebRtcWireServer::State {
  State(std::string value_identity, OnWebRtcStream stream_callback,
        WebRtcConfiguration value_configuration,
        WireStreamOptions value_stream_options)
      : identity(std::move(value_identity)),
        on_stream(std::move(stream_callback)),
        configuration(std::move(value_configuration)),
        stream_options(value_stream_options) {}

  mutable thread::Mutex mu;
  const std::string identity;
  // A transport, not an endpoint: an in-process service's endpoint is one, and
  // so is a signalling client connected to a remote rendezvous. Everything
  // after registration only ever sends and receives.
  std::shared_ptr<SignallingTransport> endpoint ABSL_GUARDED_BY(mu);
  const OnWebRtcStream on_stream;
  const WebRtcConfiguration configuration;
  const WireStreamOptions stream_options;
  bool running ABSL_GUARDED_BY(mu) = true;
  absl::flat_hash_map<std::string, std::shared_ptr<ServerPeerContext>> peers
      ABSL_GUARDED_BY(mu);
};

absl::StatusOr<std::shared_ptr<WebRtcWireServer>> WebRtcWireServer::Create(
    std::string identity, const std::shared_ptr<SignallingService>& signalling,
    OnWebRtcStream on_stream, WebRtcConfiguration configuration,
    WireStreamOptions stream_options) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(identity));
  if (signalling == nullptr) {
    return absl::InvalidArgumentError(
        "WebRTC signalling service must not be null");
  }
  // Registered with a callback that does nothing, then replaced below by the
  // transport overload's own. Nothing can be delivered in between: a message
  // for this identity could not have been addressed before it existed.
  ABSL_ASSIGN_OR_RETURN(
      std::shared_ptr<SignallingEndpoint> endpoint,
      signalling->Connect(std::move(identity), [](const SignallingMessage&) {
        return a11::ReadyTask();
      }));
  return Create(std::move(endpoint), std::move(on_stream),
                std::move(configuration), stream_options);
}

absl::StatusOr<std::shared_ptr<WebRtcWireServer>> WebRtcWireServer::Create(
    const std::shared_ptr<SignallingTransport>& signalling,
    OnWebRtcStream on_stream, WebRtcConfiguration configuration,
    WireStreamOptions stream_options) {
  if (signalling == nullptr) {
    return absl::InvalidArgumentError(
        "WebRTC signalling transport must not be null");
  }
  std::string identity = signalling->identity();
  ABSL_RETURN_IF_ERROR(data::ValidateName(identity));
  if (!on_stream) {
    return absl::InvalidArgumentError("WebRTC on_stream must be callable");
  }
  ABSL_RETURN_IF_ERROR(configuration.Validate());
  ABSL_RETURN_IF_ERROR(stream_options.Validate());
  auto state =
      std::make_shared<State>(std::move(identity), std::move(on_stream),
                              std::move(configuration), stream_options);
  std::weak_ptr<State> weak = state;
  {
    // Installing the callback makes the server externally reachable. Keep the
    // state lock held until the transport is published so an immediate offer
    // cannot observe a partially initialized server.
    thread::MutexLock lock(&state->mu);
    state->endpoint = signalling;
    absl::Status installed =
        signalling->SetOnMessage([weak](const SignallingMessage& message) {
          std::shared_ptr<State> held_state = weak.lock();
          if (held_state == nullptr) {
            return a11::ReadyTask();
          }
          return OnSignal(held_state, message);
        });
    if (!installed.ok()) {
      return installed;
    }
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
                                     const SignallingMessage& message) {
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
                                       const rtc::Description& description) {
      std::shared_ptr<State> held_state = weak_state.lock();
      if (held_state == nullptr) {
        return;
      }
      std::shared_ptr<SignallingTransport> endpoint;
      std::string identity;
      {
        thread::MutexLock lock(&held_state->mu);
        if (!held_state->running) {
          return;
        }
        endpoint = held_state->endpoint;
        identity = held_state->identity;
      }
      SignallingMessage reply{.type = SignallingMessageType::kDescription,
                              .sender = std::move(identity),
                              .recipient = peer,
                              .description = description.generateSdp("\r\n"),
                              .description_type = description.typeString()};
      absl::Status sent = endpoint->Send(std::move(reply));
      if (!sent.ok()) {
        ReportPeerError(held_state, peer, sent);
      }
    });
    connection->onLocalCandidate(
        [weak_state, peer = message.sender](const rtc::Candidate& candidate) {
          std::shared_ptr<State> held_state = weak_state.lock();
          if (held_state == nullptr) {
            return;
          }
          std::shared_ptr<SignallingTransport> endpoint;
          std::string identity;
          {
            thread::MutexLock lock(&held_state->mu);
            if (!held_state->running) {
              return;
            }
            endpoint = held_state->endpoint;
            identity = held_state->identity;
          }
          SignallingMessage reply{.type = SignallingMessageType::kCandidate,
                                  .sender = std::move(identity),
                                  .recipient = peer,
                                  .candidate = std::string(candidate),
                                  .mid = candidate.mid()};
          absl::Status sent = endpoint->Send(std::move(reply));
          if (!sent.ok()) {
            ReportPeerError(held_state, peer, sent);
          }
        });
    connection->onStateChange(
        [weak_state, peer = message.sender](rtc::PeerConnection::State status) {
          if (status == rtc::PeerConnection::State::Failed) {
            if (std::shared_ptr<State> held_state = weak_state.lock();
                held_state != nullptr) {
              ReportPeerError(
                  held_state, peer,
                  absl::UnavailableError("WebRTC peer connection failed"));
            }
          } else if (status == rtc::PeerConnection::State::Closed) {
            if (std::shared_ptr<State> held_state = weak_state.lock();
                held_state != nullptr) {
              thread::MutexLock lock(&held_state->mu);
              held_state->peers.erase(peer);
            }
          }
        });
    connection->onDataChannel([weak_state, weak_context](
                                  const std::shared_ptr<rtc::DataChannel>&
                                      channel) {
      std::shared_ptr<State> held_state = weak_state.lock();
      std::shared_ptr<ServerPeerContext> held_context = weak_context.lock();
      if (held_state == nullptr || held_context == nullptr ||
          channel == nullptr) {
        return;
      }
      std::shared_ptr<SignallingTransport> endpoint;
      OnWebRtcStream callback;
      WebRtcConfiguration wanted_configuration;
      WireStreamOptions options;
      {
        thread::MutexLock lock(&held_state->mu);
        if (!held_state->running) {
          try {
            channel->close();
          } catch (...) {}
          return;
        }
        endpoint = held_state->endpoint;
        callback = held_state->on_stream;
        wanted_configuration = held_state->configuration;
        options = held_state->stream_options;
      }
      absl::StatusOr<std::shared_ptr<internal::BinaryChannel>> wrapped =
          internal::MakeRtcBinaryChannel(channel);
      if (!wrapped.ok()) {
        ReportPeerError(held_state, held_context->identity, wrapped.status());
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
        thread::MutexLock lock(&held_context->mu);
        if (held_context->failed) {
          try {
            channel->close();
          } catch (...) {}
          return;
        }
        if (held_context->multiplex == nullptr) {
          const size_t max_channels = wanted_configuration.max_channels == 0
                                          ? 1
                                          : wanted_configuration.max_channels;
          held_context->multiplex = internal::MultiplexedBinaryChannel::Create(
              {*wrapped}, /*factory=*/{},
              internal::MultiplexedChannelOptions{.target_channels =
                                                      max_channels});
          held_context->data_channel = channel;
          multiplex = held_context->multiplex;
          first = true;
        } else {
          multiplex = held_context->multiplex;
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
      ChannelWireStream::OpenOperation open = [held_context]() {
        return ServerPeerStatus(held_context);
      };
      absl::StatusOr<std::shared_ptr<WebRtcWireStream>> stream =
          WebRtcWireStream::BuildMultiplexedStream(
              multiplex, channel, held_context->connection, std::move(endpoint),
              std::move(id), ChannelEndpointRole::kServer, options,
              std::move(open), wanted_configuration.channel_split_size,
              wanted_configuration.mtu.value_or(kWebRtcBaseMtu));
      if (!stream.ok()) {
        ReportPeerError(held_state, held_context->identity, stream.status());
        return;
      }
      {
        thread::MutexLock lock(&held_context->mu);
        held_context->stream = *stream;
      }
      // Discovery needs nothing from the peer: a probe is a padded SCTP
      // HEARTBEAT, which any conforming peer answers on its own. No channel, no
      // capability, no responder.
      if (wanted_configuration.path_mtu_discovery) {
        (*stream)->StartPathMtuDiscovery(wanted_configuration, multiplex);
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
  std::shared_ptr<SignallingTransport> endpoint;
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
  std::shared_ptr<SignallingTransport> endpoint;
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

std::shared_ptr<SignallingTransport> WebRtcWireServer::signalling_endpoint()
    const {
  thread::MutexLock lock(&state_->mu);
  return state_->endpoint;
}

}  // namespace a11::net
