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

#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#include <emscripten/fiber.h>
#include <ucontext.h>

namespace {

struct ContextState {
  emscripten_fiber_t fiber{};
  void (*entry)(void*) = nullptr;
  void* argument = nullptr;
};

static_assert(sizeof(ContextState) <= sizeof(ucontext_t::uc_mcontext));
static_assert(alignof(ContextState) <= alignof(ucontext_t));
static_assert(offsetof(ucontext_t, uc_mcontext) % alignof(ContextState) == 0);

constexpr unsigned long kInitialized = 0xa11f1b3UL;
constexpr size_t kRootAsyncifyStackSize = 64 * 1024;
constexpr size_t kMinimumFiberStackSize = 64 * 1024;
alignas(16) thread_local std::array<std::byte,
                                    kRootAsyncifyStackSize> root_asyncify_stack;
thread_local ucontext_t* active_context = nullptr;

struct WorkerDefaults {
  WorkerDefaults() {
    // Emscripten fiber continuations cannot move between pthread workers.
    setenv("A11_POOL_THREADS", "1", 1);
    setenv("A11_POOL_SPINNERS", "0", 1);
  }
};

WorkerDefaults worker_defaults;

ContextState* State(ucontext_t* context) {
  return std::launder(reinterpret_cast<ContextState*>(&context->uc_mcontext));
}

void Entry(void* opaque) {
  auto* context = static_cast<ucontext_t*>(opaque);
  State(context)->entry(State(context)->argument);
  if (context->uc_link != nullptr) {
    setcontext(context->uc_link);
  }
  std::abort();
}

}  // namespace

extern "C" int getcontext(ucontext_t* context) {
  if (context == nullptr) {
    errno = EINVAL;
    return -1;
  }
  new (&context->uc_mcontext) ContextState;
  emscripten_fiber_init_from_current_context(&State(context)->fiber,
                                             root_asyncify_stack.data(),
                                             root_asyncify_stack.size());
  context->uc_flags = kInitialized;
  return 0;
}

extern "C" void makecontext(ucontext_t* context, void (*function)(), int argc,
                            ...) {
  if (context == nullptr || context->uc_flags != kInitialized ||
      function == nullptr || argc != 1 || context->uc_stack.ss_sp == nullptr ||
      context->uc_stack.ss_size < kMinimumFiberStackSize) {
    std::abort();
  }
  va_list args;
  va_start(args, argc);
  void* argument = va_arg(args, void*);
  va_end(args);

  auto* state = State(context);
  state->entry = reinterpret_cast<void (*)(void*)>(function);
  state->argument = argument;

  // The caller owns this stack. Reserve its first quarter for Asyncify's
  // continuation and use the remainder as the fiber's C stack.
  auto* raw = static_cast<std::byte*>(context->uc_stack.ss_sp);
  const auto address = reinterpret_cast<std::uintptr_t>(raw);
  const size_t skip = (16 - address % 16) % 16;
  auto* base = raw + skip;
  const size_t usable_size = (context->uc_stack.ss_size - skip) & ~size_t{15};
  const size_t asyncify_size = (usable_size / 4) & ~size_t{15};
  const size_t c_size = usable_size - asyncify_size;
  emscripten_fiber_init(&state->fiber, Entry, context, base + asyncify_size,
                        c_size, base, asyncify_size);
}

extern "C" int swapcontext(ucontext_t* old_context,
                           const ucontext_t* new_context) {
  if (old_context == nullptr || new_context == nullptr ||
      old_context->uc_flags != kInitialized ||
      new_context->uc_flags != kInitialized) {
    errno = EINVAL;
    return -1;
  }
  active_context = const_cast<ucontext_t*>(new_context);
  emscripten_fiber_swap(&State(old_context)->fiber,
                        &State(active_context)->fiber);
  active_context = old_context;
  return 0;
}

extern "C" int setcontext(const ucontext_t* context) {
  if (context == nullptr || active_context == nullptr ||
      context->uc_flags != kInitialized) {
    errno = EINVAL;
    return -1;
  }
  emscripten_fiber_swap(&State(active_context)->fiber,
                        &State(const_cast<ucontext_t*>(context))->fiber);
  std::abort();
}
