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
#include <memory>
#include <string>

#include "a11/data/types.h"
#include "a11/nodes/async_node.h"
#include "a11/stores/local_chunk_store.h"

int main() {
  auto store = a11::stores::LocalChunkStore::Create("probe");
  if (!store.ok()) {
    return 1;
  }
  auto node = a11::nodes::AsyncNode::Create(*store);
  if (!node.ok()) {
    return 2;
  }
  a11::data::Chunk chunk;
  chunk.data = "hello";
  if (!(*node)->Finalize(std::move(chunk)).Await().ok()) {
    return 3;
  }
  auto received = (*node)->NextChunk().Await();
  if (!received.ok() || !received->has_value() ||
      received->value().data != "hello") {
    return 4;
  }
  std::puts("A11 native AsyncNode probe passed");
  return 0;
}
