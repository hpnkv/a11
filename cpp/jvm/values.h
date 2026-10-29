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

#ifndef A11_JVM_VALUES_H_
#define A11_JVM_VALUES_H_

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <absl/log/check.h>

#include "jvm/bridge.h"
#include "jvm/value_reference.h"

namespace a11::jvm {
template <typename T>
std::shared_ptr<ValueReference<T>> Value(std::int64_t id) {
  auto result = Object<ValueReference<T>>(id);
  CHECK_OK(result.status()) << "Invalid native value reference";
  return *result;
}

template <typename T>
std::int64_t KeepValue(T value) {
  return Retain(std::make_shared<ValueReference<T>>(std::move(value)));
}

template <typename T, typename Parent, typename Resolve>
std::int64_t KeepView(std::shared_ptr<ValueReference<Parent>> parent,
                      ValueMember member, Resolve resolve, size_t index = 0) {
  const auto revision = parent->Revision(member);
  auto lifetime = parent->lifetime()->Child(member, index);
  return Retain(std::make_shared<ValueReference<T>>(
      [parent = std::move(parent), member, revision,
       resolve = std::move(resolve)]() -> T& {
        CHECK_EQ(parent->Revision(member), revision)
            << "Native member view was invalidated by replacement or removal";
        return resolve(parent->get());
      },
      std::move(lifetime)));
}

}  // namespace a11::jvm
#endif  // A11_JVM_VALUES_H_
