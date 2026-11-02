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
#include <utility>

#include <boost/context/fiber.hpp>

int main() {
  int steps = 0;
  boost::context::fiber child([&](boost::context::fiber&& parent) {
    ++steps;
    parent = std::move(parent).resume();
    ++steps;
    return std::move(parent);
  });
  child = std::move(child).resume();
  if (steps != 1) {
    return 1;
  }
  child = std::move(child).resume();
  if (steps != 2) {
    return 2;
  }
  std::puts("Boost.Context fiber probe passed");
  return 0;
}
