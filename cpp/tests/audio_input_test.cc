// Copyright 2026 The A11 Authors.

#include "sdk/audio/audio_input.h"

#include <limits>
#include <memory>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <gtest/gtest.h>

#include "sdk/audio/device.h"

namespace a11::sdk::audio {
namespace {

TEST(AudioInputOptionsTest, RejectsTooSmallBlock) {
  AudioInputOptions options;
  options.block_frames = 8;
  EXPECT_EQ(options.Validate().code(), absl::StatusCode::kInvalidArgument);
}

TEST(AudioInputOptionsTest, RejectsTooShallowRing) {
  AudioInputOptions options;
  options.ring_blocks = 1;
  EXPECT_EQ(options.Validate().code(), absl::StatusCode::kInvalidArgument);
}

TEST(AudioInputOptionsTest, AcceptsDefaults) {
  EXPECT_TRUE(AudioInputOptions{}.Validate().ok());
}

TEST(AudioInputOptionsTest, RejectsNonFiniteSampleRate) {
  AudioInputOptions options;
  options.sample_rate = std::numeric_limits<double>::infinity();
  EXPECT_EQ(options.Validate().code(), absl::StatusCode::kInvalidArgument);

  options.sample_rate = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(options.Validate().code(), absl::StatusCode::kInvalidArgument);
}

TEST(DeviceTest, ListDevicesSucceeds) {
  // Tolerates a host with zero devices; it must still not error.
  const absl::StatusOr<std::vector<DeviceInfo>> devices = ListDevices();
  ASSERT_TRUE(devices.ok()) << devices.status();
}

TEST(DeviceTest, NegativeIndexIsOutOfRange) {
  EXPECT_EQ(DeviceInfoAt(-1).status().code(), absl::StatusCode::kOutOfRange);
}

TEST(AudioInputTest, OpenExposesDeviceMetadata) {
  const absl::StatusOr<DeviceInfo> default_input = DefaultInputDevice();
  if (!default_input.ok()) {
    GTEST_SKIP() << "No default input device on this host";
  }

  const absl::StatusOr<std::shared_ptr<AudioInput>> input =
      AudioInput::Open(AudioInputOptions{});
  ASSERT_TRUE(input.ok()) << input.status();
  EXPECT_GT((*input)->channels(), 0);
  EXPECT_GT((*input)->sample_rate(), 0.0);
  EXPECT_FALSE((*input)->capturing());

  // Below-minimum buffer sizes are rejected before any stream is opened.
  EXPECT_EQ((*input)->Subscribe(kMinBufferSize - 1).status().code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_FALSE((*input)->capturing());
}

}  // namespace
}  // namespace a11::sdk::audio
