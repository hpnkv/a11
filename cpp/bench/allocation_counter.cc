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

#include "bench/allocation_counter.h"

#include <atomic>
#include <cstdlib>
#include <new>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif

namespace {
std::atomic<bool> sampling{false};
std::atomic<size_t> calls{0};
std::atomic<size_t> requested_bytes{0};

void Record(size_t size) {
  if (sampling.load(std::memory_order_relaxed)) {
    calls.fetch_add(1, std::memory_order_relaxed);
    requested_bytes.fetch_add(size, std::memory_order_relaxed);
  }
}
}  // namespace

#if defined(A11_BENCH_ALLOCATION_SAMPLES)
// The benchmark's system-allocator build counts C++ new calls, including workers.
// No per-allocation headers or runtime library changes accompany this sampling.
void* operator new(size_t size) {
  void* result = std::malloc(size == 0 ? 1 : size);
  if (result == nullptr) {
    std::abort();
  }
  Record(size);
  return result;
}

void* operator new[](size_t size) {
  return ::operator new(size);
}

void operator delete(void* pointer) noexcept {
  std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
  std::free(pointer);
}

void operator delete(void* pointer, size_t) noexcept {
  std::free(pointer);
}

void operator delete[](void* pointer, size_t) noexcept {
  std::free(pointer);
}

void* operator new(size_t size, std::align_val_t alignment) {
  void* result = nullptr;
  if (posix_memalign(&result, static_cast<size_t>(alignment),
                     size == 0 ? 1 : size) != 0) {
    std::abort();
  }
  Record(size);
  return result;
}

void* operator new[](size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}

void operator delete(void* pointer, std::align_val_t) noexcept {
  std::free(pointer);
}

void operator delete[](void* pointer, std::align_val_t) noexcept {
  std::free(pointer);
}

void operator delete(void* pointer, size_t, std::align_val_t) noexcept {
  std::free(pointer);
}

void operator delete[](void* pointer, size_t, std::align_val_t) noexcept {
  std::free(pointer);
}
#endif

namespace a11::bench {
void BeginAllocationSample() {
  calls.store(0, std::memory_order_relaxed);
  requested_bytes.store(0, std::memory_order_relaxed);
  sampling.store(true, std::memory_order_relaxed);
}

AllocationSample EndAllocationSample() {
  sampling.store(false, std::memory_order_relaxed);
  return {calls.load(std::memory_order_relaxed),
          requested_bytes.load(std::memory_order_relaxed)};
}

HeapSample CurrentHeapSample() {
#if defined(__APPLE__)
  malloc_statistics_t stats{};
  malloc_zone_statistics(nullptr, &stats);
  return {stats.blocks_in_use, stats.size_in_use};
#else
  return {};
#endif
}

bool AllocationSamplesAvailable() {
#if defined(A11_BENCH_ALLOCATION_SAMPLES)
  return true;
#else
  return false;
#endif
}
}  // namespace a11::bench
