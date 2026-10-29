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

#include "a11/time.h"

#include <absl/time/clock.h>

#include "jvm/bridge.h"

namespace a11::jvm {
absl::StatusOr<absl::Duration> ReadDuration(const nlohmann::json& fields) {
  if (!fields.is_object()) {
    return absl::InvalidArgumentError("Expected a duration");
  }
  if (fields.find("handle") != fields.end()) {
    ABSL_ASSIGN_OR_RETURN(auto id, Arg<std::int64_t>(fields, "handle"));
    ABSL_ASSIGN_OR_RETURN(auto value, Object<absl::Duration>(id));
    return *value;
  }
  ABSL_ASSIGN_OR_RETURN(auto infinity, DefaultArg<int>(fields, "infinity", 0));
  if (infinity == 1) {
    return absl::InfiniteDuration();
  }
  if (infinity == -1) {
    return -absl::InfiniteDuration();
  }
  if (infinity != 0) {
    return absl::InvalidArgumentError("Invalid duration infinity");
  }
  ABSL_ASSIGN_OR_RETURN(auto seconds, Arg<std::int64_t>(fields, "seconds"));
  ABSL_ASSIGN_OR_RETURN(auto nanos, DefaultArg<int>(fields, "nanoseconds", 0));
  if (nanos <= -1000000000 || nanos >= 1000000000) {
    return absl::InvalidArgumentError("Duration nanoseconds exceed a second");
  }
  return absl::Seconds(seconds) + absl::Nanoseconds(nanos);
}

absl::StatusOr<absl::Time> ReadTime(const nlohmann::json& fields) {
  if (!fields.is_object()) {
    return absl::InvalidArgumentError("Expected an instant");
  }
  if (fields.find("handle") != fields.end()) {
    ABSL_ASSIGN_OR_RETURN(auto id, Arg<std::int64_t>(fields, "handle"));
    ABSL_ASSIGN_OR_RETURN(auto value, Object<absl::Time>(id));
    return *value;
  }
  ABSL_ASSIGN_OR_RETURN(auto infinity, DefaultArg<int>(fields, "infinity", 0));
  if (infinity == 1) {
    return absl::InfiniteFuture();
  }
  if (infinity == -1) {
    return absl::InfinitePast();
  }
  if (infinity != 0) {
    return absl::InvalidArgumentError("Invalid instant infinity");
  }
  ABSL_ASSIGN_OR_RETURN(auto seconds, Arg<std::int64_t>(fields, "seconds"));
  ABSL_ASSIGN_OR_RETURN(auto nanos, DefaultArg<int>(fields, "nanoseconds", 0));
  if (nanos < 0 || nanos >= 1000000000) {
    return absl::InvalidArgumentError(
        "Instant nanoseconds must be within a second");
  }
  return absl::FromUnixSeconds(seconds) + absl::Nanoseconds(nanos);
}

}  // namespace a11::jvm
