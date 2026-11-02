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

#include <array>
#include <cstddef>
#include <cstdio>

#include <ucontext.h>

namespace {
ucontext_t main_context{};
ucontext_t child_context{};
std::array<std::byte, 256 * 1024> child_stack{};
int steps = 0;

void Child(void* ignored) {
  (void)ignored;
  ++steps;
  swapcontext(&child_context, &main_context);
  ++steps;
  swapcontext(&child_context, &main_context);
}
}  // namespace

int main() {
  if (getcontext(&main_context) != 0 || getcontext(&child_context) != 0) {
    return 1;
  }
  child_context.uc_stack.ss_sp = child_stack.data();
  child_context.uc_stack.ss_size = child_stack.size();
  makecontext(&child_context, reinterpret_cast<void (*)()>(Child), 1, nullptr);
  if (swapcontext(&main_context, &child_context) != 0 || steps != 1) {
    return 2;
  }
  if (swapcontext(&main_context, &child_context) != 0 || steps != 2) {
    return 3;
  }
  std::puts("fiber context probe passed");
  return 0;
}
