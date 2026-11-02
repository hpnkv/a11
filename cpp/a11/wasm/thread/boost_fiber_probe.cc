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

#include <cstdio>

#include <boost/fiber/all.hpp>

int main() {
  int steps = 0;
  boost::fibers::fiber child([&] {
    ++steps;
    boost::this_fiber::yield();
    ++steps;
  });
  boost::this_fiber::yield();
  if (steps != 1) {
    return 1;
  }
  child.join();
  if (steps != 2) {
    return 2;
  }
  std::puts("Boost.Fiber probe passed");
  return 0;
}
