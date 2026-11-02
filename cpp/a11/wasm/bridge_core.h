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

#ifndef A11_WASM_BRIDGE_CORE_H_
#define A11_WASM_BRIDGE_CORE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <absl/status/status.h>

struct Result {
  int code = 0;
  std::string bytes;
  bool ready = true;
};

Result* Failure(const absl::Status& status);
Result* Success();
Result* FromStatus(const absl::Status& status);
std::string_view View(const std::uint8_t* data, size_t size);
std::string_view View(const char* data, size_t size);

#endif  // A11_WASM_BRIDGE_CORE_H_
