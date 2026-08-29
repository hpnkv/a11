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

#ifndef THREAD_INTERNAL_STACK_WALK_H_
#define THREAD_INTERNAL_STACK_WALK_H_

#include <cstddef>
#include <string>

#include <absl/base/nullability.h>

namespace thread::internal {

/// Frames past this are dropped; a corrupt chain reaches the cap.
inline constexpr size_t kMaxWalkedFrames = 128;

/**
 * @brief Unwinds a parked fiber's stack from a frame pointer recorded at its
 *        blocking call.
 *
 * `absl::GetStackTrace` unwinds the calling stack, which for a fiber deadlock
 * is not the interesting one.
 *
 * Each candidate frame is checked against `[stack_lo, stack_hi)`, alignment and
 * strict monotonicity, so a stale `frame_pointer` yields fewer frames instead
 * of a read of unmapped memory.
 *
 * REQUIRES: the fiber is parked and its stack stays mapped for the call.
 * `thread::SnapshotFibers` holds the fiber registry lock to guarantee both.
 *
 * @param frame_pointer
 *   From `FiberDiagnostics::wait_fp`.
 * @param stack_lo, stack_hi
 *   The fiber's stack region. A null bound falls back to a span limit, which is
 *   a weaker check.
 * @param out_pcs
 *   Receives return addresses, innermost frame first.
 * @param max_frames
 *   Capacity of `out_pcs`, capped at `kMaxWalkedFrames`.
 * @return
 *   Frames written. Zero when this architecture has no frame-record layout
 *   here, or the first candidate frame fails validation.
 */
size_t WalkFramePointers(void* absl_nullable frame_pointer,
                         const void* absl_nullable stack_lo,
                         const void* absl_nullable stack_hi,
                         void* absl_nullable* absl_nonnull out_pcs,
                         size_t max_frames);

/// Whether this build knows its architecture's frame-record layout. A report
/// states this instead of presenting an empty stack as a fact.
bool FramePointerWalkSupported();

/// Idempotent symbolizer bootstrap. Locates this executable itself
/// (`_NSGetExecutablePath`, `/proc/self/exe`), because the caller is often a
/// signal-driven watchdog with no argv.
void InitializeSymbolizerOnce();

/// `pc` as "Symbol (0xaddress)", or the address alone when nothing names it.
/// Never returns empty.
std::string DescribeProgramCounter(const void* absl_nullable pc);

}  // namespace thread::internal

#endif  // THREAD_INTERNAL_STACK_WALK_H_
