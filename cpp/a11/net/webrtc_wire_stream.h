// Copyright 2026 The A11 Authors.

#ifndef A11_NET_WEBRTC_WIRE_STREAM_H_
#define A11_NET_WEBRTC_WIRE_STREAM_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/concurrency/future.h"
#include "a11/net/channel_wire_stream.h"
#include "a11/net/signalling.h"
#include "a11/net/wire_stream.h"

namespace rtc {
class DataChannel;
class PeerConnection;
}  // namespace rtc

namespace a11::net {

enum class TurnRelayType { kUdp, kTcp, kTls };

struct TurnServer {
  static absl::StatusOr<TurnServer> FromString(std::string_view value);

  std::string hostname;
  std::uint16_t port = 3478;
  std::string username;
  std::string password;
  TurnRelayType relay_type = TurnRelayType::kUdp;

  friend bool operator==(const TurnServer&, const TurnServer&) = default;
};

struct WebRtcConfiguration {
  // Large logical WireMessages are fragmented by A11 before reaching SCTP;
  // this remains the advertised local libdatachannel message ceiling.
  std::optional<size_t> max_message_size = 64 * 1024;
  size_t channel_split_size = 48 * 1024;
  bool enable_ice_udp_mux = false;
  std::vector<std::string> stun_servers;
  std::vector<TurnServer> turn_servers;
  std::optional<std::pair<std::uint16_t, std::uint16_t>> preferred_port_range;
  std::optional<std::string> bind_address;

  absl::Status Validate() const;
};

class WebRtcWireStream final : public ChannelWireStream {
 private:
  struct ConstructorToken {};

 public:
  static absl::StatusOr<std::shared_ptr<WebRtcWireStream>> CreateClient(
      std::string identity, std::string peer_identity,
      std::shared_ptr<SignallingService> signalling,
      WebRtcConfiguration configuration = {}, WireStreamOptions options = {});

  static absl::StatusOr<std::shared_ptr<WebRtcWireStream>> CreateClient(
      std::string peer_identity,
      std::shared_ptr<SignallingTransport> signalling,
      WebRtcConfiguration configuration = {}, WireStreamOptions options = {});

  static absl::StatusOr<std::shared_ptr<WebRtcWireStream>> Create(
      std::shared_ptr<rtc::DataChannel> data_channel,
      std::shared_ptr<rtc::PeerConnection> connection,
      std::shared_ptr<SignallingTransport> signalling_endpoint,
      ChannelEndpointRole role, WireStreamOptions options = {},
      OpenOperation open_operation = {}, size_t split_size = 48 * 1024);

  ~WebRtcWireStream() override;

  [[nodiscard]] std::shared_ptr<rtc::DataChannel> data_channel() const;
  [[nodiscard]] std::shared_ptr<rtc::PeerConnection> peer_connection() const;
  [[nodiscard]] std::shared_ptr<SignallingTransport> signalling_endpoint()
      const;

  WebRtcWireStream(ConstructorToken, std::shared_ptr<State> state,
                   std::shared_ptr<rtc::DataChannel> data_channel,
                   std::shared_ptr<rtc::PeerConnection> connection,
                   std::shared_ptr<SignallingTransport> signalling_endpoint)
      : ChannelWireStream(std::move(state)),
        data_channel_(std::move(data_channel)),
        connection_(std::move(connection)),
        signalling_endpoint_(std::move(signalling_endpoint)) {}

 private:
  std::shared_ptr<rtc::DataChannel> data_channel_;
  std::shared_ptr<rtc::PeerConnection> connection_;
  std::shared_ptr<SignallingTransport> signalling_endpoint_;
};

using OnWebRtcStream =
    std::function<a11::Task(std::shared_ptr<WebRtcWireStream>)>;

class WebRtcWireServer : public std::enable_shared_from_this<WebRtcWireServer> {
 public:
  static absl::StatusOr<std::shared_ptr<WebRtcWireServer>> Create(
      std::string identity, std::shared_ptr<SignallingService> signalling,
      OnWebRtcStream on_stream, WebRtcConfiguration configuration = {},
      WireStreamOptions stream_options = {});

  ~WebRtcWireServer();

  absl::Status Stop();
  [[nodiscard]] std::string identity() const;
  [[nodiscard]] bool running() const;
  [[nodiscard]] size_t pending_peer_count() const;
  [[nodiscard]] std::shared_ptr<SignallingEndpoint> signalling_endpoint() const;

 private:
  struct State;

  explicit WebRtcWireServer(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  static a11::Task OnSignal(const std::shared_ptr<State>& state,
                            SignallingMessage message);
  static absl::Status HandleOffer(const std::shared_ptr<State>& state,
                                  const SignallingMessage& message);
  static absl::Status HandleCandidate(const std::shared_ptr<State>& state,
                                      const SignallingMessage& message);
  static void ReportPeerError(const std::shared_ptr<State>& state,
                              std::string peer_identity, absl::Status status);

  std::shared_ptr<State> state_;
};

}  // namespace a11::net

#endif  // A11_NET_WEBRTC_WIRE_STREAM_H_
