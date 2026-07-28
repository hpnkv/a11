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
  std::function<void()> on_open;
  std::function<void(std::string)> on_message;
  std::function<void(absl::Status)> on_error;
  std::function<void()> on_closed;
  std::function<void()> on_buffered_amount_low;
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
  virtual absl::Status Close() = 0;
  [[nodiscard]] virtual void* absl_nullable GetImpl() const = 0;
};

absl::StatusOr<std::shared_ptr<BinaryChannel>> MakeRtcBinaryChannel(
    std::shared_ptr<rtc::Channel> channel);

}  // namespace a11::net::internal

#endif  // A11_NET_INTERNAL_BINARY_CHANNEL_H_
