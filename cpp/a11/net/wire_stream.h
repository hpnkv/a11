// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief A11's transport abstraction: the bidirectional WireStream channel and
 *        the options/callbacks that drive it.
 *
 * A `WireStream` carries `data::WireMessage` values between two endpoints.
 * Everything above it -- AsyncNode mirroring, Session multiplexing, remote
 * action dispatch -- is written against this one interface, so the concrete
 * transport (in-process, WebSocket/HTTP2, HTTP SSE, WebRTC) is a pluggable
 * detail and an extension point for custom transports.
 */

#ifndef A11_NET_WIRE_STREAM_H_
#define A11_NET_WIRE_STREAM_H_

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"

namespace a11::net {

/// Trailer key under which an aborting endpoint reports its terminal status.
inline constexpr std::string_view kAbortStatusHeader = "x-a11-abort-status";
/// Hard ceiling on the size of a single reassembled inbound WireMessage.
inline constexpr size_t kMaxSingleMessageSize = 32 * 1024 * 1024;

/**
 * @brief Buffering, sizing, and deadline limits for a WireStream endpoint.
 *
 * These bound how much inbound data the stream will hold before applying
 * backpressure or failing, and when it gives up. Concrete transports validate
 * and apply them at construction.
 */
struct WireStreamOptions {
  /// Maximum inbound messages buffered before backpressure is applied.
  size_t max_buffered_incoming_messages = 1000;
  /// Reject any single reassembled message larger than this.
  size_t max_single_message_size = kMaxSingleMessageSize;
  /// Maximum total bytes of buffered inbound messages.
  size_t max_buffered_incoming_bytes = 32 * 1024 * 1024;
  /// Fail a message that cannot be delivered within this duration (infinite =
  /// no per-message timeout).
  absl::Duration message_timeout = absl::InfiniteDuration();
  /// Absolute deadline after which the stream is aborted.
  absl::Time deadline = absl::InfiniteFuture();

  /// Validate the option values, returning a non-OK status if inconsistent.
  absl::Status Validate() const;
};

/// Called for each inbound message; `std::nullopt` signals the peer
/// half-closed. Returns an awaitable the transport awaits before delivering the
/// next message (this is where a consumer applies backpressure).
using OnMessage =
    std::function<a11::Task(std::optional<data::WireMessage> message)>;
/// Called once, when the stream has fully finished (cleanly or via abort).
using OnDone = std::function<a11::Task()>;

/**
 * @brief A bidirectional, message-oriented channel between two A11 endpoints.
 *
 * A WireStream is A11's transport abstraction: an ordered *neither* -- delivery
 * carries **no global ordering guarantee**. Messages may be observed by the
 * reader in an order different from how the sender enqueued them, and different
 * transports (e.g. an unreliable WebRTC data channel) make that explicit. The
 * one synchronisation point the interface does promise is **closure**: every
 * message accepted for delivery is observed by the reader before the stream
 * reports done, and the half-close marker follows messages already queued by
 * that endpoint. HalfClose() queues that transition; DrainOutgoingMessages()
 * is the explicit local delivery barrier. Callers that need ordering must
 * impose it above the transport (an AsyncNode/ChunkStore log, which *is*
 * ordered by sequence number, is the usual way).
 *
 * Implement this interface to carry A11 traffic over custom transports;
 * the runtime treats every implementation identically.
 *
 * @headerfile a11/net/wire_stream.h
 */
class WireStream {
 public:
  virtual ~WireStream() = default;

  /**
   * @brief Enqueue a message for delivery to the peer. Non-blocking.
   *
   * The message is admitted to this endpoint's outbound queue and delivered
   * asynchronously by the transport task, which is also where backpressure is
   * applied; there is no delivery-order guarantee across messages (see the
   * class comment).
   * @param message The message to send.
   * @return OK once the message is queued, or a non-OK status if the stream is
   *   not writable (e.g. already half-closed or aborted).
   */
  virtual absl::Status Send(data::WireMessage message) = 0;

  /**
   * @brief Begin the stream as the initiating ("start") side.
   * @param on_message Invoked for each inbound message (nullopt = peer
   *   half-closed).
   * @param on_done Invoked once when the stream has finished.
   * @return An awaitable that resolves once the startup handshake completes.
   */
  virtual a11::Task Start(OnMessage on_message, OnDone on_done) = 0;

  /**
   * @brief Begin the stream as the accepting ("accept") side.
   * @param on_message Invoked for each inbound message (nullopt = peer
   *   half-closed).
   * @param on_done Invoked once when the stream has finished.
   * @return An awaitable that resolves once the stream is accepted.
   */
  virtual a11::Task Accept(OnMessage on_message, OnDone on_done) = 0;

  /// Half-close with no trailers. @see HalfClose(data::ByteMap)
  absl::Status HalfClose() { return HalfClose(data::ByteMap{}); }

  /**
   * @brief Signal that this side will send no more messages.
   *
   * The terminal marker is queued after buffered outbound messages and, once
   * the peer also half-closes, the stream completes. Inbound messages continue
   * to be delivered until then. Call DrainOutgoingMessages() to await local
   * transport delivery.
   * @param trailers Optional closing metadata delivered to the peer.
   * @return OK, or a non-OK status if the stream cannot be half-closed.
   */
  virtual absl::Status HalfClose(data::ByteMap trailers) = 0;

  /**
   * @brief Await delivery of all buffered outbound messages.
   * @return An awaitable that resolves once this endpoint's queued messages
   *   have been flushed to the transport (requires a prior HalfClose).
   */
  virtual a11::Task DrainOutgoingMessages() = 0;

  /**
   * @brief Abort the stream, discarding buffered work.
   * @param status A non-OK status reported to the peer as the abort reason.
   * @return OK if the abort was accepted.
   */
  virtual absl::Status Abort(absl::Status status) = 0;

  /// Clear any deadline (equivalent to an infinite deadline).
  absl::Status SetDeadline() { return SetDeadline(absl::InfiniteFuture()); }

  /**
   * @brief Set the absolute deadline after which the stream is aborted.
   * @param deadline The deadline; `absl::InfiniteFuture()` disables it.
   */
  virtual absl::Status SetDeadline(absl::Time deadline) = 0;

  /// @return The stream's current deadline.
  [[nodiscard]] virtual absl::Time deadline() const = 0;
  /// @return The stream's terminal status (OK unless it failed or was aborted).
  [[nodiscard]] virtual absl::Status GetStatus() const = 0;
  /// @return The peer's closing trailers, if the stream has received them.
  [[nodiscard]] virtual std::optional<data::ByteMap> GetTrailers() const = 0;
  /// @return This stream's transport-assigned identifier.
  [[nodiscard]] virtual std::string GetId() const = 0;
  /// @return An opaque handle to the underlying transport object (advanced;
  ///   may be null).
  [[nodiscard]] virtual void* absl_nullable GetImpl() const = 0;
};

/// Validate and case-normalise a wire header map, returning the normalised copy
/// or a non-OK status if a header name/value is invalid.
absl::StatusOr<data::ByteMap> NormalizeWireHeaders(data::ByteMap headers);

}  // namespace a11::net

#endif  // A11_NET_WIRE_STREAM_H_
