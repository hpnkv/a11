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

enum class ChannelEndpointRole { kClient, kServer, kEither };

struct ChannelFramingOptions {
  // Serialized byte packets never exceed split_size. Complete messages carry
  // a small suffix; larger messages use length-suffixed first chunks followed
  // by ordinary chunks, matching Action Engine's byte-chunking protocol.
  size_t split_size = 64 * 1024;
  size_t max_pending_messages = 64;
  size_t max_pending_bytes = 64 * 1024 * 1024;

  absl::Status Validate() const;
};

// WireStream implementation for libdatachannel WebRTC DataChannels.
// libdatachannel callbacks run on ordinary external threads; this adapter
// hands all protocol and user work to the bundled fiber runtime.
class ChannelWireStream
    : public WireStream,
      public std::enable_shared_from_this<ChannelWireStream> {
 public:
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
