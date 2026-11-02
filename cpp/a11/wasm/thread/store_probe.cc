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
#include <string>

#include "a11/data/types.h"
#include "a11/stores/local_chunk_store.h"

int main() {
  auto store = a11::stores::LocalChunkStore::Create("probe");
  if (!store.ok()) {
    return 1;
  }
  auto pending = (*store)->Get(0);
  a11::data::NodeFragment fragment;
  fragment.id = "probe";
  fragment.data = a11::data::Chunk{.data = "hello"};
  fragment.continued = false;
  auto sequence = (*store)->Put(std::move(fragment)).Await();
  if (!sequence.ok() || *sequence != 0) {
    return 2;
  }
  auto received = pending.Await();
  if (!received.ok() || received->seq != 0) {
    return 3;
  }
  auto chunk = received->GetChunk();
  if (!chunk.ok() || (*chunk)->data != "hello") {
    return 4;
  }
  std::puts("A11 native LocalChunkStore probe passed");
  return 0;
}
