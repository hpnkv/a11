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

#ifndef A11_SDK_HTTP_RENDER_PROTOCOL_H_
#define A11_SDK_HTTP_RENDER_PROTOCOL_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <nlohmann/json_fwd.hpp>

namespace a11::sdk::http::render_protocol {

inline constexpr std::string_view kVersion = "a11.web-render-helper/v1";
inline constexpr size_t kMaxControlBytes = 1024 * 1024;
inline constexpr size_t kMaxDataBytes = 64 * 1024;

enum class FrameKind : std::uint8_t {
  kControl = 1,
  kData = 2,
};

struct Frame {
  FrameKind kind = FrameKind::kControl;
  std::string payload;
};

absl::Status WriteControl(int fd, const nlohmann::json& value);
absl::Status WriteData(int fd, std::string_view bytes);
absl::StatusOr<Frame> ReadFrame(int fd, absl::Time deadline);
absl::StatusOr<nlohmann::json> ParseControl(const Frame& frame);

}  // namespace a11::sdk::http::render_protocol

#endif  // A11_SDK_HTTP_RENDER_PROTOCOL_H_
