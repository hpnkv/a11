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
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/flow/runtime.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "openssl/rand.h"
#include "openssl/sha.h"

std::string HashHex(const std::string& input) {
  unsigned char digest[SHA256_DIGEST_LENGTH]{};
  SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(),
         digest);
  constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(SHA256_DIGEST_LENGTH * 2);
  for (unsigned char byte : digest) {
    hex.push_back(kHex[byte >> 4]);
    hex.push_back(kHex[byte & 15]);
  }
  return hex;
}

int main() {
  const char* workers = std::getenv("A11_POOL_THREADS");
  if (workers == nullptr || std::strcmp(workers, "1") != 0) {
    return 10;
  }
  unsigned char digest[SHA256_DIGEST_LENGTH]{};
  SHA256(reinterpret_cast<const unsigned char*>("abc"), 3, digest);
  const unsigned char expected[] = {
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
      0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
      0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  if (std::memcmp(digest, expected, sizeof(expected)) != 0) {
    return 1;
  }
  if (HashHex("") !=
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ||
      HashHex(std::string(64, 'a')) !=
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" ||
      HashHex(std::string(1000, 'a')) !=
          "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3") {
    return 1;
  }
  unsigned char random[32]{};
  if (RAND_bytes(random, sizeof(random)) != 1) {
    return 2;
  }
  auto program = a11::flow::CompiledProgram::Compile(R"(
flow hello {
  out result: string
  "hi" -> result
}
)");
  if (!program.ok()) {
    return 3;
  }
  const auto* flow = (*program)->Flow("hello");
  if (flow == nullptr) {
    return 4;
  }
  auto schema = a11::flow::FlowSchema(flow->plan);
  auto handler = a11::flow::MakeHandler(*program, "hello");
  auto nodes = a11::nodes::NodeMap::Create();
  if (!schema.ok() || !handler.ok() || !nodes.ok()) {
    return 5;
  }
  auto registry = std::make_shared<a11::actions::ActionRegistry>();
  auto action = a11::actions::Action::Create(*schema, "flow", *handler, *nodes,
                                             nullptr, nullptr, registry);
  if (!action.ok()) {
    return 6;
  }
  auto output = (*action)->GetOutput("result", false);
  if (!output.ok() || !(*action)->Run().ok()) {
    return 7;
  }
  if (!(*action)->Wait(absl::Seconds(5)).Await().ok()) {
    return 8;
  }
  auto chunk = (*output)->NextChunk().Await();
  if (!chunk.ok() || !chunk->has_value() || chunk->value().data != "\"hi\"") {
    return 9;
  }
  std::puts("A11 native action and Flow runtime linked");
  return 0;
}
