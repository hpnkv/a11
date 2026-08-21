// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief WireStream transport over a WebRTC data channel (libdatachannel).
 *
 * A WebRtcWireStream carries A11 WireMessage traffic over a WebRTC SCTP data
 * channel, enabling peer-to-peer connections including NAT traversal via STUN
 * and TURN. Pick it when peers must reach each other directly without a shared
 * server on the data path. It relies on a signalling channel (see
 * signalling.h) to exchange SDP offers/answers and ICE candidates before the
 * peer connection is established. Large logical messages are fragmented by the
 * ChannelWireStream layer before reaching SCTP. Delivery carries no global
 * ordering guarantee but is synchronised on closure -- a reader observes every
 * delivered message before the stream completes.
 */

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

#include <absl/base/thread_annotations.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/net/channel_wire_stream.h"
#include "a11/net/signalling.h"
#include "a11/net/wire_stream.h"
#include "thread/boost_primitives.h"

namespace rtc {
class DataChannel;
class PeerConnection;
}  // namespace rtc

namespace a11::net {

namespace internal {
class MultiplexedBinaryChannel;
class PathMtuDiscovery;
}  // namespace internal

/** Transport used to reach a TURN relay server. */
enum class TurnRelayType { kUdp, kTcp, kTls };

/**
 * @brief The MTU a WebRTC association uses until something better is confirmed.
 *
 * The 1280-byte value follows the IPv6 minimum link MTU and RFC 8261's SCTP
 * fallback. Path MTU discovery may confirm a larger value.
 */
inline constexpr size_t kWebRtcBaseMtu = 1280;

/// Smallest MTU usrsctp will accept (`SCTP_SMALLEST_PMTU`).
inline constexpr size_t kWebRtcMinMtu = 512;

/**
 * @brief Address and credentials for a TURN relay used during ICE.
 *
 * TURN servers relay media when a direct peer-to-peer path cannot be
 * established through NATs or firewalls.
 */
struct TurnServer {
  /** @brief Parses a TURN server from a URL-like string.
   * @return The parsed server, or an error status. */
  static absl::StatusOr<TurnServer> FromString(std::string_view value);

  std::string hostname;       ///< TURN host name or address.
  std::uint16_t port = 3478;  ///< TURN service port.
  std::string username;       ///< Relay credential username.
  std::string password;       ///< Relay credential password.
  TurnRelayType relay_type = TurnRelayType::kUdp;  ///< Transport to the relay.

  friend bool operator==(const TurnServer&, const TurnServer&) = default;
};

/**
 * @brief ICE, fragmentation, and binding settings for a WebRTC connection.
 *
 * Lists the STUN and TURN servers used for ICE, the message size at which A11
 * fragments large logical messages, and optional local port/address binding.
 */
struct WebRtcConfiguration {
  // Large logical WireMessages are fragmented by A11 before reaching SCTP;
  // this remains the advertised local libdatachannel message ceiling.
  std::optional<size_t> max_message_size =
      64 * 1024;  ///< Advertised channel ceiling.
  size_t channel_split_size =
      48 * 1024;  ///< A11 packet size below SCTP limits.
  /**
   * @brief Network MTU SCTP builds packets to, in bytes. Absent = 1280.
   *
   * The 1280-byte default follows RFC 8261 and works without path information.
   * Set a larger value only for a path whose MTU is known; an oversized value
   * can prevent fragmented messages from arriving.
   */
  std::optional<size_t> mtu;
  bool enable_ice_udp_mux =
      false;  ///< Reuse one UDP socket for ICE candidates.
  std::vector<std::string> stun_servers;  ///< STUN URLs used for NAT discovery.
  std::vector<TurnServer> turn_servers;   ///< Fallback relay servers.
  /// Optional inclusive local UDP port range for ICE.
  std::optional<std::pair<std::uint16_t, std::uint16_t>> preferred_port_range;
  std::optional<std::string>
      bind_address;  ///< Optional local candidate address.
  // Stripe packets across data channels while preserving one reliable
  // WireStream. Clients maintain `desired_channels`; servers admit at most
  // `max_channels` per peer.
  size_t desired_channels = 8;  ///< Data channels a client opens and replenishes.
  size_t max_channels = 8;      ///< Data channels a server admits per peer.

  /**
   * @brief Discover the path MTU by probing, instead of assuming `mtu`.
   *
   * Discovery starts from `mtu` and raises it only after a burst of padded SCTP
   * HEARTBEAT probes is acknowledged. Application traffic waits while testing a
   * candidate above the confirmed size. Each peer discovers its own send path;
   * peers without discovery support keep the configured MTU.
   */
  bool path_mtu_discovery = true;
  /// Bounds and timers for the search. @see internal::PathMtuOptions
  size_t max_discovered_mtu = 9216;
  /// How long one probe has to be acknowledged before it counts as lost.
  absl::Duration probe_timeout = absl::Milliseconds(500);
  /// Delay before searching upward again after convergence.
  absl::Duration path_mtu_raise_interval = absl::Seconds(600);
  /// Delay before retrying while the SCTP association is unavailable.
  absl::Duration path_mtu_startup_retry = absl::Milliseconds(250);

  /** @return OK if the configuration is internally consistent. */
  absl::Status Validate() const;
};

/**
 * @brief A WireStream that carries A11 traffic over a WebRTC data channel.
 *
 * Created by dialing a named peer through a signalling service or transport
 * (CreateClient), or by adopting an already-negotiated libdatachannel channel
 * (Create, used internally by WebRtcWireServer). Framing and fragmentation are
 * handled by the ChannelWireStream base.
 */
class WebRtcWireStream final : public ChannelWireStream {
 private:
  struct ConstructorToken {};

 public:
  /**
   * @brief Dials a peer over a shared in-process signalling service.
   *
   * @param identity Local identity to register with the service.
   * @param peer_identity Identity of the peer to connect to.
   * @param signalling In-process signalling service both peers share.
   * @param configuration ICE and fragmentation settings.
   * @param options Transport-level WireStreamOptions.
   * @return The negotiated stream, or an error status. Drives the ICE/SDP
   *     handshake asynchronously before resolving.
   */
  static absl::StatusOr<std::shared_ptr<WebRtcWireStream>> CreateClient(
      std::string identity, std::string peer_identity,
      std::shared_ptr<SignallingService> signalling,
      WebRtcConfiguration configuration = {}, WireStreamOptions options = {});

  /**
   * @brief Dials a peer over an explicit signalling transport.
   *
   * Prefer this overload when reaching a peer across a network via a remote
   * signalling server (e.g. a WebSocketSignallingClient) rather than an
   * in-process service.
   *
   * @param peer_identity Identity of the peer to connect to.
   * @param signalling Transport that carries the SDP/ICE handshake.
   * @param configuration ICE and fragmentation settings.
   * @param options Transport-level WireStreamOptions.
   * @return The negotiated stream, or an error status.
   */
  static absl::StatusOr<std::shared_ptr<WebRtcWireStream>> CreateClient(
      std::string peer_identity,
      std::shared_ptr<SignallingTransport> signalling,
      WebRtcConfiguration configuration = {}, WireStreamOptions options = {});

  /**
   * @brief Adopts an already-established data channel as a WireStream.
   *
   * The low-level factory used once a peer connection and data channel exist;
   * WebRtcWireServer builds accepted streams through it.
   *
   * @param data_channel The negotiated libdatachannel data channel.
   * @param connection The owning peer connection.
   * @param signalling_endpoint Transport the handshake ran over.
   * @param role Whether this endpoint is the channel opener or acceptor.
   * @param options Transport-level WireStreamOptions.
   * @param open_operation Optional operation to await channel open.
   * @param split_size Size at which large messages are fragmented.
   * @return The stream, or an error status.
   */
  static absl::StatusOr<std::shared_ptr<WebRtcWireStream>> Create(
      std::shared_ptr<rtc::DataChannel> data_channel,
      std::shared_ptr<rtc::PeerConnection> connection,
      std::shared_ptr<SignallingTransport> signalling_endpoint,
      ChannelEndpointRole role, WireStreamOptions options = {},
      OpenOperation open_operation = {}, size_t split_size = 48 * 1024);

  /**
   * @brief Adopts an already-built binary channel (possibly multiplexed).
   *
   * Lower-level than Create: the caller owns the channel and its member data
   * channels. `primary_channel` is retained only for the data_channel()
   * accessor. Used by WebRtcWireServer and the client factory to wrap a
   * multiplexed channel that stripes across several data channels.
   */
  static absl::StatusOr<std::shared_ptr<WebRtcWireStream>> BuildMultiplexedStream(
      std::shared_ptr<internal::BinaryChannel> channel,
      std::shared_ptr<rtc::DataChannel> primary_channel,
      std::shared_ptr<rtc::PeerConnection> connection,
      std::shared_ptr<SignallingTransport> signalling_endpoint, std::string id,
      ChannelEndpointRole role, WireStreamOptions options = {},
      OpenOperation open_operation = {}, size_t split_size = 48 * 1024,
      size_t configured_mtu = kWebRtcBaseMtu);

  ~WebRtcWireStream() override;

  /** @return The underlying libdatachannel data channel (advanced interop). */
  [[nodiscard]] std::shared_ptr<rtc::DataChannel> data_channel() const;
  /** @return The underlying libdatachannel peer connection (advanced
   * interop). */
  [[nodiscard]] std::shared_ptr<rtc::PeerConnection> peer_connection() const;
  /** @return The signalling transport this stream negotiated over. */
  [[nodiscard]] std::shared_ptr<SignallingTransport> signalling_endpoint()
      const;

  /**
   * @brief Sets the SCTP path MTU on this already-connected association.
   *
   * The same units and meaning as WebRtcConfiguration::mtu, applied to a live
   * association rather than at construction. Packets built from now on use it;
   * usrsctp re-fragments already-queued data when the value shrinks.
   *
   * This is the primitive path MTU discovery is built on, and it is public
   * because the discovery loop is not the only legitimate caller: an application
   * that already knows its path (a fixed datacentre fabric, a tunnel with a
   * known overhead) can set it once and skip searching.
   *
   * @param mtu Network MTU in bytes.
   * @return OK if applied. FAILED_PRECONDITION when there is no connected
   *   association yet or libdatachannel refused the value -- which a discovery
   *   loop must read as "not now", *not* as a path that cannot carry this size,
   *   or it will mistake a startup race for a black hole.
   */
  absl::Status SetPathMtu(size_t mtu);

  /**
   * @return The MTU most recently applied by SetPathMtu(), or the configured
   *   value if none has been. What discovery has settled on, for observability.
   */
  [[nodiscard]] size_t current_path_mtu() const;

  /**
   * @brief Starts path MTU discovery on this stream. Idempotent.
   *
   * Needs no peer cooperation and no extra channel: a probe is a padded SCTP
   * HEARTBEAT, which every conforming peer answers by itself.
   */
  void StartPathMtuDiscovery(
      const WebRtcConfiguration& configuration,
      const std::shared_ptr<internal::MultiplexedBinaryChannel>& stream_data);

  /// @return What path MTU discovery has confirmed, or 0 when it is not running.
  [[nodiscard]] size_t discovered_path_mtu() const;

  WebRtcWireStream(ConstructorToken, std::shared_ptr<State> state,
                   std::shared_ptr<rtc::DataChannel> data_channel,
                   std::shared_ptr<rtc::PeerConnection> connection,
                   std::shared_ptr<SignallingTransport> signalling_endpoint,
                   size_t configured_mtu)
      : ChannelWireStream(std::move(state)),
        data_channel_(std::move(data_channel)),
        connection_(std::move(connection)),
        signalling_endpoint_(std::move(signalling_endpoint)),
        path_mtu_(configured_mtu) {}

 private:
  std::shared_ptr<rtc::DataChannel> data_channel_;
  std::shared_ptr<rtc::PeerConnection> connection_;
  std::shared_ptr<SignallingTransport> signalling_endpoint_;
  mutable thread::Mutex path_mtu_mu_;
  size_t path_mtu_ ABSL_GUARDED_BY(path_mtu_mu_);
  std::shared_ptr<internal::PathMtuDiscovery> discovery_
      ABSL_GUARDED_BY(path_mtu_mu_);
};

/** Callback invoked with each newly negotiated peer WireStream. */
using OnWebRtcStream =
    std::function<a11::Task(std::shared_ptr<WebRtcWireStream>)>;

/**
 * @brief Accepts incoming WebRTC peer connections under a fixed identity.
 *
 * Listens on a signalling service for offers addressed to `identity`,
 * completes the ICE/SDP handshake for each peer, and hands the resulting
 * WebRtcWireStream to a callback.
 */
class WebRtcWireServer : public std::enable_shared_from_this<WebRtcWireServer> {
 public:
  /**
   * @brief Creates a WebRTC server that accepts peer connections.
   *
   * Registers `identity` on an in-process signalling service. The overload
   * below is the general one; this is the convenience for the case where the
   * peers are negotiating through a service in this process.
   *
   * @param identity Local identity this server listens as.
   * @param signalling Signalling service used to negotiate with peers.
   * @param on_stream Callback invoked with each accepted stream.
   * @param configuration ICE and fragmentation settings.
   * @param stream_options Transport-level WireStreamOptions per stream.
   * @return The running server, or an error status.
   */
  static absl::StatusOr<std::shared_ptr<WebRtcWireServer>> Create(
      std::string identity, std::shared_ptr<SignallingService> signalling,
      OnWebRtcStream on_stream, WebRtcConfiguration configuration = {},
      WireStreamOptions stream_options = {});

  /**
   * @brief Creates a WebRTC server negotiating over an explicit transport.
   *
   * Prefer this overload when the peers are reached across a network through a
   * *remote* signalling server rather than a service in this process: connect a
   * WebSocketSignallingClient to it and hand the client over here. The server
   * listens as whatever identity the transport registered under, so there is no
   * separate identity to keep in step.
   *
   * The mirror of WebRtcWireStream::CreateClient's transport overload, and the
   * one that makes an agent behind a signalling rendezvous reachable at all.
   *
   * @param signalling Connected signalling transport; its identity is the
   *        server's. The server takes over its message callback, and closes it
   *        when stopped -- it is the server's rendezvous, not a side channel the
   *        caller keeps using.
   * @param on_stream Callback invoked with each accepted stream.
   * @param configuration ICE and fragmentation settings.
   * @param stream_options Transport-level WireStreamOptions per stream.
   * @return The running server, or an error status.
   */
  static absl::StatusOr<std::shared_ptr<WebRtcWireServer>> Create(
      std::shared_ptr<SignallingTransport> signalling, OnWebRtcStream on_stream,
      WebRtcConfiguration configuration = {},
      WireStreamOptions stream_options = {});

  ~WebRtcWireServer();

  /** Stops the server and stops accepting new peer connections. */
  absl::Status Stop();
  /** @return The local identity this server listens as. */
  [[nodiscard]] std::string identity() const;
  /** @return Whether the server is currently running. */
  [[nodiscard]] bool running() const;
  /** @return The number of peers still completing negotiation. */
  [[nodiscard]] size_t pending_peer_count() const;
  /** @return The signalling transport the server negotiates over. */
  [[nodiscard]] std::shared_ptr<SignallingTransport> signalling_endpoint()
      const;

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
