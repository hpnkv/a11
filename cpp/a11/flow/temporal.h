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

#ifndef A11_FLOW_TEMPORAL_H_
#define A11_FLOW_TEMPORAL_H_

#include <optional>
#include <string_view>

#include <absl/time/time.h>

namespace a11::flow {
/// A duration of total seconds, including negative and infinite values.
absl::Duration SecondsDuration(double total);
/// Parse duration text, including compound units and bare seconds.
std::optional<absl::Duration> ParseDuration(std::string_view text);
/// Parse an instant as RFC 3339 text or a UTC calendar date.
std::optional<absl::Time> ParseTime(std::string_view text);
}  // namespace a11::flow
#endif  // A11_FLOW_TEMPORAL_H_
