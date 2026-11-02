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
#include "bridge_core.h"
#include "runtime.h"

extern "C" {
Result* a11_wasm_flow_request(const std::uint8_t* data, size_t size) {
  const auto request = a11::ParseJson(View(data, size), "Flow request");
  if (!request.ok()) {
    return Failure(request.status());
  }
  const auto answer =
      a11::DumpJson(a11::flow::Handle(*request), "Flow response");
  if (!answer.ok()) {
    return Failure(answer.status());
  }
  return new Result{0, std::move(*answer)};
}

}  // extern "C"
