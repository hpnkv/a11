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

#include "thread/internal/stack_walk.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include <absl/base/call_once.h>
#include <absl/debugging/symbolize.h>
#include <absl/strings/str_format.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(__linux__)
#include <unistd.h>
#endif

#if defined(__aarch64__) || defined(_M_ARM64) || defined(__x86_64__) || \
    defined(__amd64__)
#define THREAD_HAVE_FRAME_RECORD_LAYOUT 1
#else
#define THREAD_HAVE_FRAME_RECORD_LAYOUT 0
#endif

namespace thread::internal {
namespace {

// AArch64 and x86-64 share one frame-record shape: the saved frame pointer at
// [fp] and the return address at [fp + 8]. -fomit-frame-pointer removes this
// record, which is what A11_FRAME_POINTERS keeps enabled.
constexpr size_t kFrameRecordSize = 2 * sizeof(void*);

// AArch64 top-byte-ignore can leave tag bits in a pointer; masking them keeps a
// tagged frame pointer inside the bounds check.
constexpr std::uintptr_t kAddressMask =
#if defined(__aarch64__) || defined(_M_ARM64)
    (std::uintptr_t{1} << 56) - 1;
#else
    ~std::uintptr_t{0};
#endif

// The alignment the ABI guarantees for a frame record.
constexpr std::uintptr_t kFrameAlignment =
#if defined(__aarch64__) || defined(_M_ARM64)
    16;
#else
    8;
#endif

// Used when the caller has no stack bounds. A11 fiber stacks are a few hundred
// KiB, so a chain spanning more than this is corrupt.
constexpr std::uintptr_t kFallbackSpanLimit = 16 * 1024 * 1024;

std::uintptr_t Untag(const void* pointer) {
  return reinterpret_cast<std::uintptr_t>(pointer) & kAddressMask;
}

// A frame record sits wholly inside the fiber's stack, is aligned, and is above
// the frame that pointed at it. A corrupt chain fails one of the three and
// terminates the walk.
bool FrameIsPlausible(std::uintptr_t frame, std::uintptr_t previous_frame,
                      std::uintptr_t low, std::uintptr_t high) {
  if (frame == 0 || (frame & (kFrameAlignment - 1)) != 0) {
    return false;
  }
  if (frame <= previous_frame) {
    return false;
  }
  if (low != 0 || high != 0) {
    return frame >= low && frame + kFrameRecordSize <= high;
  }
  return frame - previous_frame < kFallbackSpanLimit;
}

std::string& SymbolizerProgramPath() {
  static std::string path;
  return path;
}

void ResolveOwnExecutablePath() {
  std::string& path = SymbolizerProgramPath();
#if defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  if (size == 0) {
    return;
  }
  path.resize(size);
  if (_NSGetExecutablePath(path.data(), &size) != 0) {
    path.clear();
    return;
  }
  path.resize(std::strlen(path.c_str()));
#elif defined(__linux__)
  char buffer[4096];
  const ssize_t written = ::readlink("/proc/self/exe", buffer, sizeof(buffer));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(buffer)) {
    return;
  }
  path.assign(buffer, static_cast<size_t>(written));
#endif
}

}  // namespace

bool FramePointerWalkSupported() {
  return THREAD_HAVE_FRAME_RECORD_LAYOUT != 0;
}

size_t WalkFramePointers(void* absl_nullable frame_pointer,
                         const void* absl_nullable stack_lo,
                         const void* absl_nullable stack_hi,
                         void* absl_nullable* absl_nonnull out_pcs,
                         size_t max_frames) {
#if !THREAD_HAVE_FRAME_RECORD_LAYOUT
  (void)frame_pointer;
  (void)stack_lo;
  (void)stack_hi;
  (void)out_pcs;
  (void)max_frames;
  return 0;
#else
  const size_t capacity = std::min(max_frames, kMaxWalkedFrames);
  if (capacity == 0) {
    return 0;
  }

  const std::uintptr_t low = Untag(stack_lo);
  const std::uintptr_t high = Untag(stack_hi);
  std::uintptr_t previous = 0;
  std::uintptr_t frame = Untag(frame_pointer);

  size_t written = 0;
  while (written < capacity && FrameIsPlausible(frame, previous, low, high)) {
    // Read before advancing, so an implausible next frame still yields this
    // frame's return address.
    void* return_address = nullptr;
    std::uintptr_t next = 0;
    std::memcpy(&next, reinterpret_cast<const void*>(frame), sizeof(next));
    std::memcpy(&return_address,
                reinterpret_cast<const void*>(frame + sizeof(void*)),
                sizeof(return_address));

    if (return_address == nullptr) {
      break;
    }
    out_pcs[written++] = return_address;

    previous = frame;
    frame = next & kAddressMask;
  }
  return written;
#endif
}

void InitializeSymbolizerOnce() {
  static absl::once_flag once;
  absl::call_once(once, [] {
    ResolveOwnExecutablePath();
    // Abseil uses the path to name the main binary. Darwin symbolizes through
    // dladdr regardless, so an empty path is not fatal.
    absl::InitializeSymbolizer(SymbolizerProgramPath().c_str());
  });
}

std::string DescribeProgramCounter(const void* absl_nullable pc) {
  if (pc == nullptr) {
    return "(null)";
  }
  InitializeSymbolizerOnce();
  char symbol[1024];
  if (absl::Symbolize(pc, symbol, sizeof(symbol))) {
    return absl::StrFormat("%s (%p)", symbol, pc);
  }
  return absl::StrFormat("(unsymbolized) (%p)", pc);
}

}  // namespace thread::internal
