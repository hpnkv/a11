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

#ifndef A11_JVM_NET_H_
#define A11_JVM_NET_H_
#include <memory>

#include "a11/concurrency/future.h"
#include "a11/net/wire_stream.h"

namespace a11::jvm {
struct BoundStream {
  explicit BoundStream(std::shared_ptr<net::WireStream> value)
      : stream(std::move(value)), done(std::make_shared<Promise<Unit>>()) {}

  std::shared_ptr<net::WireStream> stream;
  std::shared_ptr<Promise<Unit>> done;
};

}  // namespace a11::jvm
#endif  // A11_JVM_NET_H_
