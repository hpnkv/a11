// Copyright 2026 The A11 Authors.

#ifndef A11_NET_CHANNEL_WIRE_STREAM_H_
#define A11_NET_CHANNEL_WIRE_STREAM_H_

#include <cstddef>
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
#include "a11/net/byte_chunking.h"
#include "a11/net/wire_stream.h"

namespace a11::net {

namespace internal {
class BinaryChannel;
}  // namespace internal

/// Handshake role a binary channel endpoint is permitted to assume.
enum class ChannelEndpointRole { kClient, kServer, kEither };

/** Packetisation and incomplete-message bounds for a binary WireStream. */
struct ChannelFramingOptions {
  // Serialized byte packets never exceed split_size. Complete messages carry
  // a small suffix; larger messages use length-suffixed first chunks followed
  // by ordinary chunks in A11's byte-chunking protocol.
  size_t split_size = 64 * 1024;     ///< Maximum encoded channel packet size.
  size_t max_pending_messages = 64;  ///< Incomplete inbound message limit.
  size_t max_pending_bytes = 64 * 1024 * 1024;  ///< Pending inbound byte limit.

  /// Validate the framing limits before a transport starts.
  absl::Status Validate() const;
};

/**
 * @brief Shared WireStream lifecycle and framing for binary channels.
 *
 * WebRTC data channels and WebSocket channels supply a small BinaryChannel
 * adapter; this class supplies A11 packetisation, bounded reassembly,
 * backpressure, deadlines, half-close, and abort semantics. External channel
 * callbacks are handed into A11's scheduler before protocol or application
 * work runs.
 */
class ChannelWireStream
    : public WireStream,
      public std::enable_shared_from_this<ChannelWireStream> {
 public:
  /// Transport-specific operation that initiates or accepts the channel.
  using OpenOperation = std::function<absl::Status()>;

  using WireStream::HalfClose;
  using WireStream::SetDeadline;

  ~ChannelWireStream() override;

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

 protected:
  struct State;

  explicit ChannelWireStream(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  static absl::StatusOr<std::shared_ptr<State>> MakeState(
      std::shared_ptr<internal::BinaryChannel> channel, std::string id,
      ChannelEndpointRole role, OpenOperation open_operation,
      WireStreamOptions options, ChannelFramingOptions framing = {});

 private:
  a11::Task StartEndpoint(bool accept, OnMessage on_message, OnDone on_done);
  static void Sender(std::shared_ptr<State> state);
  /// Encodes, packetises and writes `message` on the calling thread. Called
  /// with the endpoint's send claim held; see Send.
  static void DeliverClaimed(const std::shared_ptr<State>& state,
                             data::WireMessage message,
                             std::uint64_t message_id);
  static void Receiver(std::shared_ptr<State> state);
  static void WatchTiming(std::shared_ptr<State> state);
  static void MarkActivity(const std::shared_ptr<State>& state);
  static void ForceAbort(const std::shared_ptr<State>& state,
                         absl::Status status, bool can_communicate = true);
  static void MaybeFinish(const std::shared_ptr<State>& state);
  static void Finish(const std::shared_ptr<State>& state,
                     std::optional<absl::Status> terminal_error = std::nullopt);
  static void Notify(const std::shared_ptr<State>& state);

  std::shared_ptr<State> state_;
};

}  // namespace a11::net

#endif  // A11_NET_CHANNEL_WIRE_STREAM_H_
