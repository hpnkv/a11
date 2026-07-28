// Copyright 2026 The A11 Authors.

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <rtc/channel.hpp>
#include <rtc/common.hpp>

#include "a11/net/internal/binary_channel.h"

namespace a11::net::internal {
namespace {

absl::Status ExternalError(const std::exception& error) {
  return absl::UnknownError(error.what());
}

rtc::binary ToRtcBinary(std::string_view bytes) {
  rtc::binary result(bytes.size());
  std::transform(bytes.begin(), bytes.end(), result.begin(), [](char value) {
    return static_cast<std::byte>(static_cast<unsigned char>(value));
  });
  return result;
}

std::string FromRtcBinary(const rtc::binary& bytes) {
  std::string result(bytes.size(), '\0');
  std::transform(
      bytes.begin(), bytes.end(), result.begin(), [](std::byte value) {
        return static_cast<char>(std::to_integer<unsigned char>(value));
      });
  return result;
}

class RtcBinaryChannel final : public BinaryChannel {
 public:
  explicit RtcBinaryChannel(std::shared_ptr<rtc::Channel> channel)
      : channel_(std::move(channel)) {}

  absl::Status SetCallbacks(BinaryChannelCallbacks callbacks) override {
    try {
      channel_->onOpen(std::move(callbacks.on_open));
      auto on_error = callbacks.on_error;
      channel_->onMessage(
          [on_message = std::move(callbacks.on_message),
           on_error = std::move(on_error)](rtc::message_variant message) {
            if (const rtc::binary* binary = std::get_if<rtc::binary>(&message);
                binary != nullptr) {
              on_message(FromRtcBinary(*binary));
            } else {
              on_error(absl::InvalidArgumentError(
                  "A11 WireStream requires binary channel messages"));
            }
          });
      channel_->onError(
          [on_error = std::move(callbacks.on_error)](std::string error) {
            on_error(absl::UnavailableError(std::move(error)));
          });
      channel_->onClosed(std::move(callbacks.on_closed));
      channel_->setBufferedAmountLowThreshold(0);
      channel_->onBufferedAmountLow(
          std::move(callbacks.on_buffered_amount_low));
      return absl::OkStatus();
    } catch (const std::exception& error) {
      return ExternalError(error);
    } catch (...) {
      return absl::UnknownError(
          "Configuring libdatachannel callbacks raised an exception");
    }
  }

  absl::Status ResetCallbacks() override {
    try {
      channel_->resetCallbacks();
      return absl::OkStatus();
    } catch (const std::exception& error) {
      return ExternalError(error);
    } catch (...) {
      return absl::UnknownError(
          "Resetting libdatachannel callbacks raised an exception");
    }
  }

  absl::Status Open() override { return absl::OkStatus(); }

  absl::Status Send(std::string bytes) override {
    try {
      (void)channel_->send(ToRtcBinary(bytes));
      return absl::OkStatus();
    } catch (const std::exception& error) {
      return ExternalError(error);
    } catch (...) {
      return absl::UnknownError("libdatachannel send raised an exception");
    }
  }

  absl::StatusOr<size_t> BufferedAmount() const override {
    try {
      return channel_->bufferedAmount();
    } catch (const std::exception& error) {
      return ExternalError(error);
    } catch (...) {
      return absl::UnknownError(
          "Reading libdatachannel buffered amount raised an exception");
    }
  }

  absl::StatusOr<bool> IsOpen() const override {
    try {
      return channel_->isOpen();
    } catch (const std::exception& error) {
      return ExternalError(error);
    } catch (...) {
      return absl::UnknownError(
          "Reading libdatachannel open state raised an exception");
    }
  }

  absl::Status Close() override {
    try {
      channel_->close();
      return absl::OkStatus();
    } catch (const std::exception& error) {
      return ExternalError(error);
    } catch (...) {
      return absl::UnknownError("Closing libdatachannel raised an exception");
    }
  }

  void* absl_nullable GetImpl() const override { return channel_.get(); }

 private:
  std::shared_ptr<rtc::Channel> channel_;
};

}  // namespace

absl::StatusOr<std::shared_ptr<BinaryChannel>> MakeRtcBinaryChannel(
    std::shared_ptr<rtc::Channel> channel) {
  if (channel == nullptr)
    return absl::InvalidArgumentError("channel must not be null");
  return std::make_shared<RtcBinaryChannel>(std::move(channel));
}

}  // namespace a11::net::internal
