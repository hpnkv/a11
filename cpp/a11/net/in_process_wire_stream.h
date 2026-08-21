// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief In-memory paired WireStream that connects two endpoints without a
 * network.
 *
 * An InProcessWireStream carries A11 WireMessage traffic between two endpoints
 * living in the same process, with no sockets or serialization on the wire.
 * Pick it when both peers run in one interpreter -- for tests, for composing
 * agents locally, and as the internal bridge other transports layer on top of.
 * Like every WireStream it delivers messages without a global ordering
 * guarantee, but it is synchronised on closure: a reader observes every
 * delivered message before the stream completes.
 */

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

/**
 * @brief A WireStream endpoint wired directly to a peer in the same process.
 *
 * The two endpoints share in-memory queues, so a Send() on one is delivered to
 * the OnMessage callback of the other with no network hop. One endpoint drives
 * Start() and the other Accept(); either may HalfClose() or Abort(). Construct
 * a connected pair with CreatePair() rather than instantiating directly.
 */
class InProcessWireStream final : public WireStream {
 private:
  struct State;

  struct ConstructorToken {};

 public:
  /** A connected pair of endpoints returned by CreatePair(). */
  using Pair = std::pair<std::shared_ptr<InProcessWireStream>,
                         std::shared_ptr<InProcessWireStream>>;

  /**
   * @brief Creates a connected pair of in-process endpoints.
   *
   * @param options Shared WireStreamOptions applied to both endpoints.
   * @param first_options Optional overrides for the first endpoint.
   * @param second_options Optional overrides for the second endpoint.
   * @param preassigned_id When non-empty, fixes the shared stream id (and hence
   *     the tracing trace id) instead of generating one. This is the
   *     implementation-level hook for preassigning a stream's trace id without
   *     widening the WireStream interface.
   * @return The two connected endpoints, or an error status.
   */
  static absl::StatusOr<Pair> CreatePair(
      std::optional<WireStreamOptions> options = std::nullopt,
      std::optional<WireStreamOptions> first_options = std::nullopt,
      std::optional<WireStreamOptions> second_options = std::nullopt,
      std::string preassigned_id = {});

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

  /** @return An awaitable that resolves when this endpoint has fully finished
   * (its peer half-closed or the stream aborted). */
  [[nodiscard]] a11::Task Done() const;

  [[nodiscard]] absl::Time deadline() const override;
  [[nodiscard]] absl::Status GetStatus() const override;
  [[nodiscard]] std::optional<data::ByteMap> GetTrailers() const override;
  [[nodiscard]] std::string GetId() const override;
  [[nodiscard]] void* absl_nullable GetImpl() const override;

  explicit InProcessWireStream(ConstructorToken, std::shared_ptr<State> state)
      : state_(std::move(state)) {}

 private:
  a11::Task StartEndpoint(OnMessage on_message, OnDone on_done);
  static void Sender(const std::shared_ptr<State>& state);
  /// Delivers `message` on the calling thread, or queues it for Sender if the
  /// peer has no room. Called with the endpoint's send claim held.
  static void DeliverClaimed(const std::shared_ptr<State>& state,
                             data::WireMessage message);
  static void Receiver(const std::shared_ptr<State>& state);
  static void WatchTiming(const std::shared_ptr<State>& state);
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
