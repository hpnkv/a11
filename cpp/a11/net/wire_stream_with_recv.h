// Copyright 2026 The A11 Authors.

#ifndef A11_NET_WIRE_STREAM_WITH_RECV_H_
#define A11_NET_WIRE_STREAM_WITH_RECV_H_

#include <memory>
#include <optional>
#include <string>

#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/wire_stream.h"

namespace a11::net {

/**
 * @brief Pull-oriented adapter for a callback-driven WireStream.
 *
 * Wrap a stream when an agent loop would rather await Receive() than implement
 * callbacks. Its single message slot preserves transport backpressure. A
 * remote abort takes priority over buffered data and is observed by every
 * current and future receiver.
 */
class WireStreamWithRecv final
    : public WireStream,
      public std::enable_shared_from_this<WireStreamWithRecv> {
 private:
  struct State;

  struct ConstructorToken {};

 public:
  /// Wrap @p stream, preserving its id, lifecycle, and transport handle.
  static absl::StatusOr<std::shared_ptr<WireStreamWithRecv>> Create(
      std::shared_ptr<WireStream> stream);

  using WireStream::HalfClose;
  using WireStream::SetDeadline;

  absl::Status Send(data::WireMessage message) override;
  a11::Task Start(OnMessage on_message, OnDone on_done) override;
  a11::Task Accept(OnMessage on_message, OnDone on_done) override;
  /// Start the wrapped stream and route inbound messages to Receive().
  a11::Task Start();
  /// Accept the wrapped stream and route inbound messages to Receive().
  a11::Task Accept();
  absl::Status HalfClose(data::ByteMap trailers) override;
  a11::Task DrainOutgoingMessages() override;
  absl::Status Abort(absl::Status status) override;
  absl::Status SetDeadline(absl::Time deadline) override;

  [[nodiscard]] absl::Time deadline() const override;
  [[nodiscard]] absl::Status GetStatus() const override;
  [[nodiscard]] std::optional<data::ByteMap> GetTrailers() const override;
  [[nodiscard]] std::string GetId() const override;
  [[nodiscard]] void* absl_nullable GetImpl() const override;

  /**
   * @brief Await one inbound message, or nullopt after the peer half-closes.
   * @param timeout Maximum duration to wait for the next lifecycle event.
   */
  a11::Future<std::optional<data::WireMessage>> Receive(
      absl::Duration timeout = absl::InfiniteDuration());

  /// Return the underlying callback-oriented stream.
  [[nodiscard]] std::shared_ptr<WireStream> wrapped_stream() const {
    return stream_;
  }

  WireStreamWithRecv(ConstructorToken, std::shared_ptr<WireStream> stream,
                     std::string id, std::shared_ptr<State> state)
      : stream_(std::move(stream)),
        id_(std::move(id)),
        state_(std::move(state)) {}

 private:
  a11::Task StartImpl(bool accept, OnMessage on_message, OnDone on_done);
  a11::Task HandleMessage(OnMessage observer,
                          std::optional<data::WireMessage> message);
  a11::Task HandleDone(OnDone observer);
  void RecordCurrentStatus() const;
  void SignalError(absl::Status status) const;

  std::shared_ptr<WireStream> stream_;
  std::string id_;
  std::shared_ptr<State> state_;
};

}  // namespace a11::net

#endif  // A11_NET_WIRE_STREAM_WITH_RECV_H_
