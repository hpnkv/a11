// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sdk/audio/device.h"

#include <memory>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>
#include <portaudio.h>

#include "sdk/audio/internal/portaudio_session.h"

namespace a11::sdk::audio {
namespace {

DeviceInfo BuildDeviceInfo(int index, const PaDeviceInfo& info) {
  DeviceInfo device;
  device.index = index;
  device.name = info.name == nullptr ? "" : info.name;
  const PaHostApiInfo* host = Pa_GetHostApiInfo(info.hostApi);
  device.host_api = host != nullptr && host->name != nullptr ? host->name : "";
  device.max_input_channels = info.maxInputChannels;
  device.max_output_channels = info.maxOutputChannels;
  device.default_sample_rate = info.defaultSampleRate;
  device.default_low_input_latency = absl::Seconds(info.defaultLowInputLatency);
  device.default_high_input_latency =
      absl::Seconds(info.defaultHighInputLatency);
  device.is_default_input = index == Pa_GetDefaultInputDevice();
  device.is_default_output = index == Pa_GetDefaultOutputDevice();
  return device;
}

}  // namespace

absl::StatusOr<std::vector<DeviceInfo>> ListDevices() {
  ABSL_ASSIGN_OR_RETURN(
      const std::shared_ptr<internal::PortAudioSession> session,
      internal::PortAudioSession::Acquire());

  const PaDeviceIndex count = Pa_GetDeviceCount();
  if (count < 0) {
    return internal::PaErrorToStatus("Pa_GetDeviceCount", count);
  }

  std::vector<DeviceInfo> devices;
  devices.reserve(static_cast<size_t>(count));
  for (PaDeviceIndex index = 0; index < count; ++index) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
    if (info == nullptr) {
      continue;
    }
    devices.push_back(BuildDeviceInfo(index, *info));
  }
  return devices;
}

absl::StatusOr<DeviceInfo> DefaultInputDevice() {
  ABSL_ASSIGN_OR_RETURN(
      const std::shared_ptr<internal::PortAudioSession> session,
      internal::PortAudioSession::Acquire());

  const PaDeviceIndex index = Pa_GetDefaultInputDevice();
  if (index == paNoDevice) {
    return absl::NotFoundError("No default input device is available");
  }
  const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
  if (info == nullptr) {
    return absl::NotFoundError("Default input device metadata is unavailable");
  }
  return BuildDeviceInfo(index, *info);
}

absl::StatusOr<DeviceInfo> DeviceInfoAt(int index) {
  ABSL_ASSIGN_OR_RETURN(
      const std::shared_ptr<internal::PortAudioSession> session,
      internal::PortAudioSession::Acquire());

  const PaDeviceIndex count = Pa_GetDeviceCount();
  if (count < 0) {
    return internal::PaErrorToStatus("Pa_GetDeviceCount", count);
  }
  if (index < 0 || index >= count) {
    return absl::OutOfRangeError(absl::StrCat(
        "Audio device index ", index, " is out of range [0, ", count, ")"));
  }
  const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
  if (info == nullptr) {
    return absl::NotFoundError(
        absl::StrCat("Metadata for audio device ", index, " is unavailable"));
  }
  return BuildDeviceInfo(index, *info);
}

}  // namespace a11::sdk::audio
