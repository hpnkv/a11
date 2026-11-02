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

#ifndef A11_WASM_HOST_STREAM_H_
#define A11_WASM_HOST_STREAM_H_

#include <memory>
#include <string>

#include <absl/status/statusor.h>

#include "a11/net/in_process_wire_stream.h"

namespace a11::wasm {

struct HostStreamPair {
  std::string id;
  std::shared_ptr<net::InProcessWireStream> session_side;
  std::shared_ptr<net::InProcessWireStream> host_side;

  static absl::StatusOr<std::shared_ptr<HostStreamPair>> Create(std::string id);
};

}  // namespace a11::wasm

#endif  // A11_WASM_HOST_STREAM_H_
