// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Enumeration and metadata for the host's audio devices.
 */

#ifndef A11_SDK_AUDIO_DEVICE_H_
#define A11_SDK_AUDIO_DEVICE_H_

#include <string>
#include <vector>

#include <absl/status/statusor.h>

#include "a11/time.h"

namespace a11::sdk::audio {

/**
 * @brief Static metadata describing one audio device, as PortAudio reports it.
 *
 * Callers inspect this before subscribing to learn a device's channel count
 * and default sample rate, and to choose a device by index or name.
 */
struct DeviceInfo {
  /// PortAudio device index, stable within one process run.
  int index = -1;
  /// Human-readable device name.
  std::string name;
  /// Name of the host API (e.g. "Core Audio", "ALSA") backing the device.
  std::string host_api;
  /// Maximum number of input (capture) channels the device offers.
  int max_input_channels = 0;
  /// Maximum number of output (playback) channels the device offers.
  int max_output_channels = 0;
  /// Device's default sample rate, in hertz.
  double default_sample_rate = 0.0;
  /// Suggested latency for interactive input use.
  a11::Duration default_low_input_latency = a11::ZeroDuration();
  /// Suggested latency for robust (buffered) input use.
  a11::Duration default_high_input_latency = a11::ZeroDuration();
  /// Whether this is the host's default input device.
  bool is_default_input = false;
  /// Whether this is the host's default output device.
  bool is_default_output = false;
};

/// Return metadata for every device PortAudio can see, in index order.
absl::StatusOr<std::vector<DeviceInfo>> ListDevices();

/// Return metadata for the host's default input device.
absl::StatusOr<DeviceInfo> DefaultInputDevice();

/// Return metadata for the device at @p index.
absl::StatusOr<DeviceInfo> DeviceInfoAt(int index);

}  // namespace a11::sdk::audio

#endif  // A11_SDK_AUDIO_DEVICE_H_
