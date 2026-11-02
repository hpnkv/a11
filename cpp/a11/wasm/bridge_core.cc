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

#include "bridge_core.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/data/json.h"
#include "a11/data/types.h"
#include "a11/flow/service.h"
#include "a11/json_codec.h"
#include "a11/net/byte_chunking.h"
#include "a11/status.h"
#include "runtime.h"

Result* Failure(const absl::Status& status) {
  auto encoded = a11::DumpJson(a11::StatusToJsonOrEmptyDetails(status),
                               "WASM status");
  return new Result{static_cast<int>(status.code()),
                    encoded.ok() ? std::move(*encoded)
                                 : std::string(status.message())};
}

std::string_view View(const std::uint8_t* data, size_t size) {
  return {reinterpret_cast<const char*>(data), size};
}

std::string_view View(const char* data, size_t size) {
  return {data, size};
}

Result* Success() {
  return new Result{};
}

Result* FromStatus(const absl::Status& status) {
  return status.ok() ? Success() : Failure(status);
}

extern "C" {
const std::uint8_t* a11_wasm_result_data(const Result* result) {
  return reinterpret_cast<const std::uint8_t*>(result->bytes.data());
}

size_t a11_wasm_result_size(const Result* result) {
  return result->bytes.size();
}

int a11_wasm_result_code(const Result* result) {
  return result->code;
}

int a11_wasm_result_ready(const Result* result) {
  return result->ready ? 1 : 0;
}

void a11_wasm_result_free(Result* result) {
  delete result;
}

}  // extern "C"
