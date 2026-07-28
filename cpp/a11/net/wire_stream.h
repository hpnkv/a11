// Copyright 2026 The A11 Authors.

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

inline constexpr std::string_view kAbortStatusHeader = "x-a11-abort-status";
inline constexpr size_t kMaxSingleMessageSize = 32 * 1024 * 1024;

struct WireStreamOptions {
  size_t max_buffered_incoming_messages = 100;
  size_t max_single_message_size = kMaxSingleMessageSize;
  size_t max_buffered_incoming_bytes = 32 * 1024 * 1024;
  absl::Duration message_timeout = absl::InfiniteDuration();
  absl::Time deadline = absl::InfiniteFuture();

  absl::Status Validate() const;
};

using OnMessage =
    std::function<a11::Task(std::optional<data::WireMessage> message)>;
using OnDone = std::function<a11::Task()>;

class WireStream {
 public:
  virtual ~WireStream() = default;

  // Send is non-blocking. Backpressure is applied by the transport task after
  // messages have entered this endpoint's ordered outbound queue.
  virtual absl::Status Send(data::WireMessage message) = 0;
  virtual a11::Task Start(OnMessage on_message, OnDone on_done) = 0;
  virtual a11::Task Accept(OnMessage on_message, OnDone on_done) = 0;

  absl::Status HalfClose() { return HalfClose(data::ByteMap{}); }

  virtual absl::Status HalfClose(data::ByteMap trailers) = 0;
  virtual a11::Task DrainOutgoingMessages() = 0;
  virtual absl::Status Abort(absl::Status status) = 0;

  absl::Status SetDeadline() { return SetDeadline(absl::InfiniteFuture()); }

  virtual absl::Status SetDeadline(absl::Time deadline) = 0;

  [[nodiscard]] virtual absl::Time deadline() const = 0;
  [[nodiscard]] virtual absl::Status GetStatus() const = 0;
  [[nodiscard]] virtual std::optional<data::ByteMap> GetTrailers() const = 0;
  [[nodiscard]] virtual std::string GetId() const = 0;
  [[nodiscard]] virtual void* absl_nullable GetImpl() const = 0;
};

absl::StatusOr<data::ByteMap> NormalizeWireHeaders(data::ByteMap headers);

}  // namespace a11::net

#endif  // A11_NET_WIRE_STREAM_H_
