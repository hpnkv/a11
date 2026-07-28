// Copyright 2026 The A11 Authors.

#ifndef A11_NET_IN_PROCESS_WIRE_STREAM_H_
#define A11_NET_IN_PROCESS_WIRE_STREAM_H_

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/wire_stream.h"

namespace a11::net {

class InProcessWireStream final : public WireStream {
 private:
  struct State;

  struct ConstructorToken {};

 public:
  using Pair = std::pair<std::shared_ptr<InProcessWireStream>,
                         std::shared_ptr<InProcessWireStream>>;

  static absl::StatusOr<Pair> CreatePair(
      std::optional<WireStreamOptions> options = std::nullopt,
      std::optional<WireStreamOptions> first_options = std::nullopt,
      std::optional<WireStreamOptions> second_options = std::nullopt);

  ~InProcessWireStream() override = default;

  using WireStream::HalfClose;
  using WireStream::SetDeadline;

  absl::Status Send(data::WireMessage message) override;
  a11::Task Start(OnMessage on_message, OnDone on_done) override;
  a11::Task Accept(OnMessage on_message, OnDone on_done) override;
  absl::Status HalfClose(data::ByteMap trailers) override;
  a11::Task DrainOutgoingMessages() override;
  absl::Status Abort(absl::Status status) override;
  absl::Status SetDeadline(absl::Time deadline) override;
  a11::Task Done() const;

  [[nodiscard]] absl::Time deadline() const override;
  [[nodiscard]] absl::Status GetStatus() const override;
  [[nodiscard]] std::optional<data::ByteMap> GetTrailers() const override;
  [[nodiscard]] std::string GetId() const override;
  [[nodiscard]] void* absl_nullable GetImpl() const override;

  explicit InProcessWireStream(ConstructorToken, std::shared_ptr<State> state)
      : state_(std::move(state)) {}

 private:
  a11::Task StartEndpoint(OnMessage on_message, OnDone on_done);
  static void Sender(std::shared_ptr<State> state);
  static void Receiver(std::shared_ptr<State> state);
  static void WatchTiming(std::shared_ptr<State> state);
  static void MarkActivity(const std::shared_ptr<State>& first,
                           const std::shared_ptr<State>& second);
  static bool ForceAbort(const std::shared_ptr<State>& state,
                         absl::Status status);
  static void MaybeFinish(const std::shared_ptr<State>& state);
  static void Finish(const std::shared_ptr<State>& state);

  std::shared_ptr<State> state_;
};

}  // namespace a11::net

#endif  // A11_NET_IN_PROCESS_WIRE_STREAM_H_
