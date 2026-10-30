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

#ifndef A11_FLOW_LIMITS_H_
#define A11_FLOW_LIMITS_H_

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace a11::flow {
/// Host-controlled bounds shared by parsing, linking and semantic analysis.
struct CompilationLimits {
  size_t max_modules = 256;
  size_t max_source_bytes = 8 * 1024 * 1024;
  size_t max_depth = 128;
  size_t max_nesting = 128;
  size_t max_tokens = 250000;
  size_t max_work = 2000000;
  size_t max_linked_name_bytes = 64 * 1024 * 1024;
  size_t max_duration_ms = 5000;
};

/// One cooperative deadline and work counter for a compilation request.
class CompilationBudget {
 public:
  explicit CompilationBudget(const CompilationLimits& limits)
      : remaining_(limits.max_work),
        remaining_tokens_(limits.max_tokens),
        deadline_(std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(std::min<size_t>(
                      limits.max_duration_ms, 24 * 60 * 60 * 1000))) {}

  bool Check(size_t work = 1) {
    if (exhausted_) {
      return false;
    }
    if (work > remaining_ || std::chrono::steady_clock::now() >= deadline_) {
      exhausted_ = true;
      return false;
    }
    remaining_ -= work;
    return true;
  }

  /// Charge optional work without invalidating an already valid baseline.
  bool TryOptional(size_t work = 1) {
    if (exhausted_ || work > remaining_ ||
        std::chrono::steady_clock::now() >= deadline_) {
      return false;
    }
    remaining_ -= work;
    return true;
  }

  bool ConsumeTokens(size_t count) {
    if (count > remaining_tokens_) {
      exhausted_ = true;
    } else {
      remaining_tokens_ -= count;
    }
    return Check(0);
  }

  bool exhausted() const { return exhausted_; }

 private:
  size_t remaining_;
  size_t remaining_tokens_;
  std::chrono::steady_clock::time_point deadline_;
  bool exhausted_ = false;
};
}  // namespace a11::flow
#endif  // A11_FLOW_LIMITS_H_
