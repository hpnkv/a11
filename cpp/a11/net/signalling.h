// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Out-of-band signalling used to negotiate WebRTC peer connections.
 *
 * Signalling is the handshake that lets two peers find each other and agree on
 * how to connect before a data transport exists -- chiefly to exchange the SDP
 * offers/answers and ICE candidates a WebRtcWireStream needs. A
 * SignallingService routes SignallingMessage values between identity-bound
 * SignallingEndpoints in one process; SignallingTransport is the abstract
 * channel a WebRTC stream negotiates over, with a WebSocket-backed
 * implementation in websocket_signalling.h for peers on different machines.
 * This is not itself a WireStream transport -- it carries control messages,
 * not A11 WireMessage traffic.
 */

#ifndef A11_NET_SIGNALLING_H_
#define A11_NET_SIGNALLING_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/concurrency/future.h"

namespace a11::net {

/** The kind of payload a SignallingMessage carries. */
enum class SignallingMessageType {
  kDescription,
  kCandidate,
  kError,
};

/**
 * @brief One signalling payload: an SDP description, an ICE candidate, or an
 * error.
 *
 * Addressed from `sender` to `recipient`. Which fields are meaningful depends
 * on `type`; ToJson()/FromJson() move it over a network transport.
 */
struct SignallingMessage {
  SignallingMessageType type = SignallingMessageType::kDescription;
  std::string sender;
  std::string recipient;
  std::string description;
  std::string description_type;
  std::string candidate;
  std::string mid;
  absl::Status error;

  /** @return OK if the fields are consistent for this message's type. */
  absl::Status Validate() const;
  /** @return The JSON wire representation, or an error status. */
  absl::StatusOr<std::string> ToJson() const;
  /** @brief Parses a message from its JSON wire representation.
   * @return The parsed message, or an error status. */
  static absl::StatusOr<SignallingMessage> FromJson(std::string_view json);
};

class SignallingService;

/** Async callback invoked for each inbound SignallingMessage. */
using OnSignallingMessage = std::function<a11::Task(SignallingMessage message)>;

/**
 * @brief Abstract identity-bound channel over which signalling flows.
 *
 * The interface a WebRtcWireStream negotiates over; concrete implementations
 * include the in-process SignallingEndpoint and the WebSocket-backed
 * WebSocketSignallingClient.
 */
class SignallingTransport {
 public:
  virtual ~SignallingTransport() = default;
  /** Sends a signalling message to the peer (non-blocking). */
  virtual absl::Status Send(SignallingMessage message) = 0;
  /** Registers the async callback invoked for each inbound message. */
  virtual absl::Status SetOnMessage(OnSignallingMessage on_message) = 0;
  /** Closes the transport and releases its resources. */
  virtual absl::Status Close() = 0;
  /** @return The local identity bound to this transport. */
  [[nodiscard]] virtual std::string identity() const = 0;
  /** @return Whether the transport is currently connected. */
  [[nodiscard]] virtual bool connected() const = 0;
  /** @return The current transport status. */
  [[nodiscard]] virtual absl::Status GetStatus() const = 0;
};

/**
 * @brief An identity-bound endpoint into an in-process SignallingService.
 *
 * Send() is non-blocking but ordered: the service invokes each recipient
 * callback serially on A11 fibers. This makes it safe to use directly from
 * libdatachannel's ordinary callback threads. Obtain one from
 * SignallingService::Connect().
 */
class SignallingEndpoint
    : public SignallingTransport,
      public std::enable_shared_from_this<SignallingEndpoint> {
 public:
  ~SignallingEndpoint();

  absl::Status Send(SignallingMessage message) override;
  absl::Status SetOnMessage(OnSignallingMessage on_message) override;
  absl::Status Close() override;
  [[nodiscard]] std::string identity() const override;
  [[nodiscard]] bool connected() const override;
  [[nodiscard]] absl::Status GetStatus() const override;

 private:
  struct State;

  explicit SignallingEndpoint(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;

  friend class SignallingService;
};

/**
 * @brief An in-process broker that routes signalling messages by identity.
 *
 * Each peer registers an identity and inbound callback via Connect(); a
 * message's recipient field selects the endpoint it is delivered to. Used to
 * bootstrap WebRTC peers in the same process, and fronted by
 * WebSocketSignallingServer when peers are remote.
 */
class SignallingService
    : public std::enable_shared_from_this<SignallingService> {
 public:
  /** @return A new, empty in-process signalling service. */
  static std::shared_ptr<SignallingService> Create();
  ~SignallingService();

  /**
   * @brief Registers an identity and its inbound-message callback.
   *
   * @param identity Identity other peers address messages to.
   * @param on_message Async callback invoked for each inbound message.
   * @return The connected endpoint, or an error status.
   */
  absl::StatusOr<std::shared_ptr<SignallingEndpoint>> Connect(
      std::string identity, OnSignallingMessage on_message);
  /** @return Whether the given identity is currently connected. */
  [[nodiscard]] bool Contains(std::string_view identity) const;
  /** @return The list of currently connected identities. */
  [[nodiscard]] std::vector<std::string> Identities() const;
  /** Stops the service and disconnects all endpoints. */
  absl::Status Stop();

 private:
  struct State;

  explicit SignallingService(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  absl::Status Route(const std::shared_ptr<SignallingEndpoint::State>& sender,
                     SignallingMessage message);
  void Disconnect(const std::shared_ptr<SignallingEndpoint::State>& endpoint,
                  absl::Status status);
  static void Pump(const std::shared_ptr<SignallingEndpoint::State>& endpoint);

  std::shared_ptr<State> state_;

  friend class SignallingEndpoint;
};

}  // namespace a11::net

#endif  // A11_NET_SIGNALLING_H_
