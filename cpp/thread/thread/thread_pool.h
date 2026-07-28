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

#include <absl/base/nullability.h>

namespace thread {

class Fiber;

namespace internal {

// Returns the A11 Fiber associated with the active scheduler context. A null
// result means the context belongs to the calling OS thread rather than to a
// Fiber created by Thread.
Fiber* absl_nullable GetScheduledFiberPtr();

}  // namespace internal
}  // namespace thread

#endif  // THREAD_FIBER_THREAD_POOL_H_
