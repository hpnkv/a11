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

#include "completion_signal.h"

namespace a11::wasm {

void CompletionSignal::Complete() {
  thread::MutexLock lock(&mu_);
  done_ = true;
  cv_.Signal();
}

void CompletionSignal::Wait() {
  thread::MutexLock lock(&mu_);
  while (!done_) {
    cv_.Wait(&mu_);
  }
}

}  // namespace a11::wasm

extern "C" void a11_wasm_completion_notify(
    a11::wasm::CompletionSignal* completion) {
  if (completion != nullptr) {
    completion->Complete();
  }
}
