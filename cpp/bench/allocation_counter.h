/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef A11_BENCH_ALLOCATION_COUNTER_H_
#define A11_BENCH_ALLOCATION_COUNTER_H_

#include <cstddef>

namespace a11::bench {
struct AllocationSample {
  size_t calls = 0;
  size_t requested_bytes = 0;
};

struct HeapSample {
  size_t live_blocks = 0;
  size_t live_bytes = 0;
};

void BeginAllocationSample();
AllocationSample EndAllocationSample();
HeapSample CurrentHeapSample();
bool AllocationSamplesAvailable();
}  // namespace a11::bench
#endif  // A11_BENCH_ALLOCATION_COUNTER_H_
