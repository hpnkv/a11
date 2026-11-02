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

#include <atomic>
#include <cstdio>

#include "thread/fiber.h"

int main() {
  std::atomic<int> steps = 0;
  auto fiber = thread::NewTree({.stack_size = 512 * 1024}, [&] {
    steps.fetch_add(1);
    thread::SleepFor(absl::Milliseconds(1));
    steps.fetch_add(1);
  });
  fiber->Join();
  if (steps.load() != 2) {
    return 1;
  }
  std::puts("A11 Thread probe passed");
  return 0;
}
