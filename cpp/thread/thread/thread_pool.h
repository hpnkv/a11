// Copyright 2026 The Action Engine Authors.
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

#ifndef THREAD_FIBER_THREAD_POOL_H_
#define THREAD_FIBER_THREAD_POOL_H_

#include <vector>

#include <absl/base/nullability.h>

namespace thread {

class Fiber;

namespace internal {

// Returns the A11 Fiber associated with the active scheduler context. A null
// result means the context belongs to the calling OS thread rather than to a
// Fiber created by Thread.
Fiber* absl_nullable GetScheduledFiberPtr();

// The CPUs this process is allowed to run on, in ascending order. Empty when
// the platform has no way to ask -- which is every platform except Linux, and
// notably includes Apple arm64; see the affinity block in thread_pool.cc.
std::vector<int> ProcessAllowedCpus();

// Parses an A11_POOL_PIN spec against `allowed`. Exposed for testing: the
// grammar has more corners than the one line of code that consumes it, and
// getting it wrong means a pool confined to the wrong cores rather than an
// error. See ParseAffinitySpec in thread_pool.cc for the grammar.
std::vector<int> ParsePoolAffinitySpec(const char* absl_nullable spec,
                                       const std::vector<int>& allowed);

// The CPU the calling pool worker was pinned to, or -1 when this thread is not
// a pool worker or the pool is not pinning. Diagnostics and tests only.
int ThisWorkerAffinityCpu();

}  // namespace internal
}  // namespace thread

#endif  // THREAD_FIBER_THREAD_POOL_H_
