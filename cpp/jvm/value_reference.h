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

#ifndef A11_JVM_VALUE_REFERENCE_H_
#define A11_JVM_VALUE_REFERENCE_H_
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include <absl/container/flat_hash_map.h>

namespace a11::jvm {
enum class ValueMember {
  kMetadata,
  kPayload,
  kInputs,
  kOutputs,
  kFragments,
  kActions,
  kAttributes,
  kHeaders
};

struct ValueLifetime {
  std::array<std::uint64_t, 8> revisions{};
  absl::flat_hash_map<std::uint64_t, std::weak_ptr<ValueLifetime>> children;
  size_t visits = 0;

  std::shared_ptr<ValueLifetime> Child(ValueMember member, size_t index) {
    if (++visits >= std::max(children.size(), size_t{64})) {
      for (auto entry = children.begin(); entry != children.end();) {
        if (entry->second.expired()) {
          children.erase(entry++);
        } else {
          ++entry;
        }
      }
      visits = 0;
    }
    const auto key = (static_cast<std::uint64_t>(member) << 32) | index;
    auto& cached = children[key];
    auto child = cached.lock();
    if (child == nullptr) {
      child = std::make_shared<ValueLifetime>();
      cached = child;
    }
    return child;
  }
};

// A view resolves its field on each access, retaining the containing value.
// Reallocation of a containing vector never leaves a dangling native pointer.
template <typename T>
class ValueReference final {
 public:
  explicit ValueReference(T value)
      : owned_(std::make_unique<T>(std::move(value))) {}

  ValueReference(std::function<T&()> resolve,
                 std::shared_ptr<ValueLifetime> lifetime)
      : resolve_(std::move(resolve)), lifetime_(std::move(lifetime)) {}

  std::uint64_t Revision(ValueMember member) const {
    return lifetime_ == nullptr
               ? 0
               : lifetime_->revisions[static_cast<size_t>(member)];
  }

  void Invalidate(ValueMember member) {
    if (lifetime_ != nullptr) {
      ++lifetime_->revisions[static_cast<size_t>(member)];
    }
  }

  const std::shared_ptr<ValueLifetime>& lifetime() const {
    if (lifetime_ == nullptr) {
      lifetime_ = std::make_shared<ValueLifetime>();
    }
    return lifetime_;
  }

  T& get() const { return owned_ != nullptr ? *owned_ : resolve_(); }

 private:
  std::unique_ptr<T> owned_;
  std::function<T&()> resolve_;
  mutable std::shared_ptr<ValueLifetime> lifetime_;
};

}  // namespace a11::jvm
#endif  // A11_JVM_VALUE_REFERENCE_H_
