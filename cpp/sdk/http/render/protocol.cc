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

#include "sdk/http/render/protocol.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <unistd.h>

#include "a11/json_codec.h"
#include "thread/boost_primitives.h"
#include "thread/fiber.h"

namespace a11::sdk::http::render_protocol {
namespace {

absl::Status IoError(std::string_view operation) {
  return absl::InternalError(
      absl::StrCat(operation, ": ", std::strerror(errno)));
}

absl::Status WaitReadable(int fd, absl::Time deadline) {
  while (true) {
    if (thread::Cancelled()) {
      return absl::CancelledError("web-render helper wait cancelled");
    }
    int timeout = -1;
    if (deadline != absl::InfiniteFuture()) {
      const absl::Duration remaining = deadline - absl::Now();
      if (remaining <= absl::ZeroDuration()) {
        return absl::DeadlineExceededError("web-render helper timed out");
      }
      timeout = static_cast<int>(std::min<std::int64_t>(
          INT_MAX, absl::ToInt64Milliseconds(remaining) + 1));
    }
    // A native Action runs in a fiber. Polling with a timeout there would park
    // its OS worker, so probe once and yield only the current fiber between
    // attempts. The helper process has no fiber scheduler and uses poll's
    // blocking timeout normally.
    const bool in_fiber = thread::GetPerThreadFiberPtr() != nullptr;
    pollfd entry{.fd = fd, .events = POLLIN, .revents = 0};
    if (in_fiber) {
      timeout = 0;
    }
    const int ready = ::poll(&entry, 1, timeout);
    if (ready > 0) {
      return absl::OkStatus();
    }
    if (ready == 0) {
      if (in_fiber) {
        const absl::Duration remaining = deadline - absl::Now();
        if (deadline != absl::InfiniteFuture() &&
            remaining <= absl::ZeroDuration()) {
          return absl::DeadlineExceededError("web-render helper timed out");
        }
        thread::SleepFor(deadline == absl::InfiniteFuture()
                             ? absl::Milliseconds(10)
                             : std::min(absl::Milliseconds(10), remaining));
        continue;
      }
      return absl::DeadlineExceededError("web-render helper timed out");
    }
    if (errno != EINTR) {
      return IoError("cannot wait for web-render helper output");
    }
  }
}

absl::Status ReadExact(int fd, char* data, size_t size, absl::Time deadline) {
  size_t offset = 0;
  while (offset < size) {
    ABSL_RETURN_IF_ERROR(WaitReadable(fd, deadline));
    const ssize_t count = ::read(fd, data + offset, size - offset);
    if (count == 0) {
      return absl::UnavailableError("web-render helper closed its output");
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return IoError("cannot read web-render helper output");
    }
    offset += static_cast<size_t>(count);
  }
  return absl::OkStatus();
}

absl::Status WriteExact(int fd, std::string_view bytes) {
  size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count =
        ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return IoError("cannot write web-render helper input");
    }
    offset += static_cast<size_t>(count);
  }
  return absl::OkStatus();
}

absl::Status WriteFrame(int fd, FrameKind kind, std::string_view payload,
                        size_t limit) {
  if (payload.size() > limit) {
    return absl::ResourceExhaustedError("web-render helper frame is too large");
  }
  const std::uint32_t length = static_cast<std::uint32_t>(payload.size() + 1);
  char header[5] = {static_cast<char>((length >> 24) & 0xff),
                    static_cast<char>((length >> 16) & 0xff),
                    static_cast<char>((length >> 8) & 0xff),
                    static_cast<char>(length & 0xff), static_cast<char>(kind)};
  ABSL_RETURN_IF_ERROR(
      WriteExact(fd, std::string_view(header, sizeof(header))));
  return WriteExact(fd, payload);
}

}  // namespace

absl::Status WriteControl(int fd, const nlohmann::json& value) {
  ABSL_ASSIGN_OR_RETURN(std::string encoded,
                        DumpJson(value, "a web-render control frame"));
  return WriteFrame(fd, FrameKind::kControl, encoded, kMaxControlBytes);
}

absl::Status WriteData(int fd, std::string_view bytes) {
  return WriteFrame(fd, FrameKind::kData, bytes, kMaxDataBytes);
}

absl::StatusOr<Frame> ReadFrame(int fd, absl::Time deadline) {
  char header[5];
  ABSL_RETURN_IF_ERROR(ReadExact(fd, header, sizeof(header), deadline));
  const std::uint32_t length =
      (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0]))
       << 24) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1]))
       << 16) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8) |
      static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
  if (length == 0) {
    return absl::InvalidArgumentError("web-render helper sent an empty frame");
  }
  const FrameKind kind =
      static_cast<FrameKind>(static_cast<unsigned char>(header[4]));
  const size_t payload_size = length - 1;
  const size_t limit =
      kind == FrameKind::kControl ? kMaxControlBytes : kMaxDataBytes;
  if ((kind != FrameKind::kControl && kind != FrameKind::kData) ||
      payload_size > limit) {
    return absl::InvalidArgumentError(
        "web-render helper sent an invalid frame header");
  }
  Frame frame{.kind = kind, .payload = std::string(payload_size, '\0')};
  ABSL_RETURN_IF_ERROR(
      ReadExact(fd, frame.payload.data(), payload_size, deadline));
  return frame;
}

absl::StatusOr<nlohmann::json> ParseControl(const Frame& frame) {
  if (frame.kind != FrameKind::kControl) {
    return absl::InvalidArgumentError("expected a web-render control frame");
  }
  return ParseJson(frame.payload, "a web-render control frame");
}

}  // namespace a11::sdk::http::render_protocol
