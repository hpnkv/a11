// Copyright 2026 The A11 Authors.

#ifndef A11_NET_INTERNAL_BINARY_CHANNEL_H_
#define A11_NET_INTERNAL_BINARY_CHANNEL_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

namespace rtc {
class Channel;
}  // namespace rtc

namespace a11::net::internal {

struct BinaryChannelCallbacks {
  std::function<void()> on_open = {};

  std::function<void(std::string)> on_message = {};

  std::function<void(absl::Status)> on_error = {};

  std::function<void()> on_closed = {};

  std::function<void()> on_buffered_amount_low = {};
};

// A small dependency-neutral boundary around message-oriented transports.
// Implementations translate their native callbacks and exception model here;
// protocol and fiber scheduling remain in ChannelWireStream.
class BinaryChannel {
 public:
  virtual ~BinaryChannel() = default;

  virtual absl::Status SetCallbacks(BinaryChannelCallbacks callbacks) = 0;
  virtual absl::Status ResetCallbacks() = 0;
  virtual absl::Status Open() = 0;
  virtual absl::Status Send(std::string bytes) = 0;
  virtual absl::StatusOr<size_t> BufferedAmount() const = 0;
  virtual absl::StatusOr<bool> IsOpen() const = 0;
  /// Graceful close: shuts this side down and lets the peer finish reading.
  virtual absl::Status Close() = 0;

  /**
   * Abortive close: release the transport now, without waiting for the peer.
   *
   * `Close()` is not enough for this, and the difference cost a file descriptor
   * per connection. Over HTTP it ends the *request* -- END_STREAM, or a
   * WebSocket close frame plus `Http2DuplexStream::Finish()` -- which is a
   * half-close of a connection whose other half the peer still holds open, so
   * the socket survives on both ends. Measured on Linux against a real Service:
   * a client that half-closed and then aborted left one ESTABLISHED descriptor
   * per connection on *each* side, released only at process exit
   * (`bench/fdprobe.py`; `FINDINGS.md` item 0).
   *
   * So a stream that finishes with a non-OK status calls this instead, and an
   * implementation whose `Close()` is already abortive -- a libdatachannel data
   * channel is -- needs no override, which is why this defaults to it rather
   * than being pure.
   */
  virtual absl::Status Abort(absl::Status status) {
    (void)status;
    return Close();
  }

  [[nodiscard]] virtual void* absl_nullable GetImpl() const = 0;
};

absl::StatusOr<std::shared_ptr<BinaryChannel>> MakeRtcBinaryChannel(
    std::shared_ptr<rtc::Channel> channel);

}  // namespace a11::net::internal

#endif  // A11_NET_INTERNAL_BINARY_CHANNEL_H_
