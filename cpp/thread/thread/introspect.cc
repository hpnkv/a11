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

#include "thread/introspect.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <absl/base/no_destructor.h>
#include <absl/container/flat_hash_map.h>
#include <absl/log/log.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <absl/strings/str_join.h>
#include <absl/strings/str_split.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <signal.h>

#include "thread/fiber_diagnostics.h"
#include "thread/internal/stack_walk.h"

// Debugger anchors. scripts/a11_fibers.py reads the registry and the record
// layout through these two symbols, so it needs no compiled-in copy of the
// struct and keeps working when a field is added. Bump `version` on a change
// that is not additive.
//
// Do not rename or make static.
extern "C" {

struct A11FiberLayout {
  std::uint32_t version;
  std::uint32_t record_size;
  std::uint32_t name_capacity;
  std::uint32_t max_selectables;
  std::uint32_t id;
  std::uint32_t parent_id;
  std::uint32_t context;
  std::uint32_t stack_lo;
  std::uint32_t stack_hi;
  std::uint32_t creation_pc;
  std::uint32_t name;
  std::uint32_t epoch;
  std::uint32_t wait_kind;
  std::uint32_t wait_object;
  std::uint32_t wait_owner_context;
  std::uint32_t wait_fp;
  std::uint32_t wait_started_nanos;
  std::uint32_t wait_deadline_nanos;
  std::uint32_t selectables;
  std::uint32_t selectable_count;
  std::uint32_t waits_completed;
  std::uint32_t reg_next;
  std::uint32_t reg_prev;
};

// Intrusive, so registration allocates nothing and a debugger can walk the same
// list from outside the process.
__attribute__((used)) thread::FiberDiagnostics* a11_fiber_registry_head =
    nullptr;

#define A11_FIBER_OFFSET(field) \
  static_cast<std::uint32_t>(offsetof(thread::FiberDiagnostics, field))

// `extern` on the definition, because a namespace-scope `const` would otherwise
// have internal linkage and never reach the symbol table.
extern const A11FiberLayout a11_fiber_layout;
__attribute__((used)) const A11FiberLayout a11_fiber_layout = {
    .version = 1,
    .record_size = static_cast<std::uint32_t>(sizeof(thread::FiberDiagnostics)),
    .name_capacity = static_cast<std::uint32_t>(thread::kFiberNameCapacity),
    .max_selectables =
        static_cast<std::uint32_t>(thread::kMaxRecordedSelectables),
    .id = A11_FIBER_OFFSET(id),
    .parent_id = A11_FIBER_OFFSET(parent_id),
    .context = A11_FIBER_OFFSET(context),
    .stack_lo = A11_FIBER_OFFSET(stack_lo),
    .stack_hi = A11_FIBER_OFFSET(stack_hi),
    .creation_pc = A11_FIBER_OFFSET(creation_pc),
    .name = A11_FIBER_OFFSET(name),
    .epoch = A11_FIBER_OFFSET(epoch),
    .wait_kind = A11_FIBER_OFFSET(wait_kind),
    .wait_object = A11_FIBER_OFFSET(wait_object),
    .wait_owner_context = A11_FIBER_OFFSET(wait_owner_context),
    .wait_fp = A11_FIBER_OFFSET(wait_fp),
    .wait_started_nanos = A11_FIBER_OFFSET(wait_started_nanos),
    .wait_deadline_nanos = A11_FIBER_OFFSET(wait_deadline_nanos),
    .selectables = A11_FIBER_OFFSET(selectables),
    .selectable_count = A11_FIBER_OFFSET(selectable_count),
    .waits_completed = A11_FIBER_OFFSET(waits_completed),
    .reg_next = A11_FIBER_OFFSET(reg_next),
    .reg_prev = A11_FIBER_OFFSET(reg_prev),
};

#undef A11_FIBER_OFFSET

}  // extern "C"

namespace thread {
namespace {

FiberDiagnostics*& RegistryHead() {
  return a11_fiber_registry_head;
}

}  // namespace
}  // namespace thread

namespace thread {

const char* absl_nonnull WaitKindName(WaitKind kind) {
  switch (kind) {
    case WaitKind::kRunning:
      return "running";
    case WaitKind::kCondVar:
      return "condvar";
    case WaitKind::kMutex:
      return "mutex";
    case WaitKind::kSelect:
      return "select";
    case WaitKind::kSleep:
      return "sleep";
    case WaitKind::kJoin:
      return "join";
    case WaitKind::kThreadPlaceholder:
      return "os-thread";
  }
  return "unknown";
}

std::int64_t DeadlineNanos(absl::Time deadline) {
  if (deadline >= absl::InfiniteFuture()) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return absl::ToUnixNanos(deadline);
}

namespace {

constexpr std::int64_t kNoDeadline = std::numeric_limits<std::int64_t>::max();

// A plain std::mutex: a report may be diagnosing a process whose fiber-aware
// locks are all held, so it cannot wait on one of them.
std::mutex& RegistryMutex() {
  static absl::NoDestructor<std::mutex> mu;
  return *mu;
}

FiberDiagnostics*& RegistryHead();

std::atomic<std::uint64_t>& RegistrySize() {
  static absl::NoDestructor<std::atomic<std::uint64_t>> size{0};
  return *size;
}

std::atomic<std::uint64_t>& FiberIdCounter() {
  static absl::NoDestructor<std::atomic<std::uint64_t>> counter{0};
  return *counter;
}

bool EnabledUnlessDisabled(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr || std::strcmp(value, "0") != 0;
}

absl::Time DeadlineFromNanos(std::int64_t nanos) {
  return nanos == kNoDeadline ? absl::InfiniteFuture()
                              : absl::FromUnixNanos(nanos);
}

}  // namespace

namespace internal {

std::uint64_t NextFiberId() {
  return FiberIdCounter().fetch_add(1, std::memory_order_relaxed) + 1;
}

bool OwnerTrackingEnabled() {
  static const bool enabled = EnabledUnlessDisabled("A11_FIBER_OWNER_TRACKING");
  return enabled;
}

void RegisterFiberDiagnostics(FiberDiagnostics* absl_nonnull record) {
  const std::lock_guard<std::mutex> lock(RegistryMutex());
  record->reg_prev = nullptr;
  record->reg_next = RegistryHead();
  if (RegistryHead() != nullptr) {
    RegistryHead()->reg_prev = record;
  }
  RegistryHead() = record;
  RegistrySize().fetch_add(1, std::memory_order_relaxed);
}

void UnregisterFiberDiagnostics(FiberDiagnostics* absl_nonnull record) {
  const std::lock_guard<std::mutex> lock(RegistryMutex());
  if (record->reg_prev == nullptr && RegistryHead() != record) {
    return;  // Never registered.
  }
  if (record->reg_prev != nullptr) {
    record->reg_prev->reg_next = record->reg_next;
  } else {
    RegistryHead() = record->reg_next;
  }
  if (record->reg_next != nullptr) {
    record->reg_next->reg_prev = record->reg_prev;
  }
  record->reg_next = nullptr;
  record->reg_prev = nullptr;
  RegistrySize().fetch_sub(1, std::memory_order_relaxed);
}

void SetWaitOwnerContext(FiberDiagnostics* absl_nonnull record,
                         const void* absl_nullable owner_context) {
  record->wait_owner_context.store(owner_context, std::memory_order_relaxed);
}

WaitScope::WaitScope(WaitKind kind, const void* absl_nullable object,
                     void* absl_nullable frame_pointer,
                     std::int64_t deadline_nanos)
    : record_(CurrentFiberDiagnostics()) {
  if (record_ == nullptr) {
    return;
  }
  // Outermost wins; see the header.
  if (record_->wait_kind.load(std::memory_order_relaxed) !=
      WaitKind::kRunning) {
    record_ = nullptr;
    return;
  }

  record_->wait_object.store(object, std::memory_order_relaxed);
  record_->wait_fp.store(frame_pointer, std::memory_order_relaxed);
  record_->wait_started_nanos.store(absl::GetCurrentTimeNanos(),
                                    std::memory_order_relaxed);
  record_->wait_deadline_nanos.store(
      deadline_nanos == 0 ? kNoDeadline : deadline_nanos,
      std::memory_order_relaxed);
  record_->wait_owner_context.store(nullptr, std::memory_order_relaxed);
  record_->selectable_count.store(0, std::memory_order_relaxed);
  record_->wait_kind.store(kind, std::memory_order_relaxed);
  // The epoch turns odd last, so an odd epoch implies the fields above are
  // visible.
  record_->epoch.fetch_add(1, std::memory_order_release);
}

WaitScope::~WaitScope() {
  if (record_ == nullptr) {
    return;
  }
  record_->wait_kind.store(WaitKind::kRunning, std::memory_order_relaxed);
  record_->waits_completed.fetch_add(1, std::memory_order_relaxed);
  record_->epoch.fetch_add(1, std::memory_order_release);
}

void WaitScope::RecordSelectables(
    const void* absl_nullable const* absl_nullable items, size_t count) {
  if (record_ == nullptr || items == nullptr) {
    return;
  }
  const size_t recorded = std::min(count, kMaxRecordedSelectables);
  for (size_t index = 0; index < recorded; ++index) {
    record_->selectables[index].store(items[index], std::memory_order_relaxed);
  }
  record_->selectable_count.store(static_cast<std::uint32_t>(recorded),
                                  std::memory_order_relaxed);
}

}  // namespace internal

namespace {

// One record read into a snapshot, with the parked stack unwound.
//
// REQUIRES: the registry lock is held, so `record` and its stack are alive.
FiberSnapshot Capture(const FiberDiagnostics& record, size_t max_frames,
                      absl::Time now) {
  FiberSnapshot out;
  out.id = record.id;
  out.parent_id = record.parent_id;
  const char* name_end = static_cast<const char*>(
      std::memchr(record.name, '\0', kFiberNameCapacity));
  out.name.assign(record.name,
                  name_end == nullptr
                      ? kFiberNameCapacity
                      : static_cast<size_t>(name_end - record.name));
  out.creation_pc = record.creation_pc;
  out.stack_lo = record.stack_lo;
  out.stack_hi = record.stack_hi;
  out.waits_completed = record.waits_completed.load(std::memory_order_relaxed);

  const std::uint32_t epoch_before =
      record.epoch.load(std::memory_order_acquire);
  out.kind = record.wait_kind.load(std::memory_order_relaxed);
  if (out.kind == WaitKind::kRunning) {
    return out;
  }

  out.wait_object = record.wait_object.load(std::memory_order_relaxed);
  out.waited = std::max(
      absl::ZeroDuration(),
      now - absl::FromUnixNanos(
                record.wait_started_nanos.load(std::memory_order_relaxed)));
  out.deadline = DeadlineFromNanos(
      record.wait_deadline_nanos.load(std::memory_order_relaxed));

  const std::uint32_t selectables = std::min<std::uint32_t>(
      record.selectable_count.load(std::memory_order_relaxed),
      kMaxRecordedSelectables);
  for (std::uint32_t index = 0; index < selectables; ++index) {
    out.selectables.push_back(
        record.selectables[index].load(std::memory_order_relaxed));
  }

  void* frames[internal::kMaxWalkedFrames];
  const size_t walked = internal::WalkFramePointers(
      record.wait_fp.load(std::memory_order_relaxed), record.stack_lo,
      record.stack_hi, frames,
      std::min(max_frames, internal::kMaxWalkedFrames));

  // An odd, unchanged epoch means the fiber stayed parked for the walk.
  const std::uint32_t epoch_after =
      record.epoch.load(std::memory_order_acquire);
  if (epoch_before != epoch_after || (epoch_before & 1U) == 0) {
    out.trace_raced = true;
    return out;
  }
  for (size_t index = 0; index < walked; ++index) {
    out.stack.push_back(frames[index]);
  }
  return out;
}

}  // namespace

std::vector<FiberSnapshot> SnapshotFibers(size_t max_frames) {
  std::vector<FiberSnapshot> snapshot;
  // Owner contexts and join targets, resolved to ids after the pass that
  // collects them: the fiber on the other end of an edge may appear anywhere in
  // the list.
  std::vector<const void*> owner_contexts;
  std::vector<const void*> join_targets;
  absl::flat_hash_map<const void*, std::uint64_t> id_by_context;
  absl::flat_hash_map<const void*, std::uint64_t> id_by_record;

  {
    const std::lock_guard<std::mutex> lock(RegistryMutex());
    const size_t live = RegistrySize().load(std::memory_order_relaxed);
    snapshot.reserve(live);
    owner_contexts.reserve(live);
    join_targets.reserve(live);
    const absl::Time now = absl::Now();

    for (const FiberDiagnostics* record = RegistryHead(); record != nullptr;
         record = record->reg_next) {
      snapshot.push_back(Capture(*record, max_frames, now));
      owner_contexts.push_back(
          record->wait_owner_context.load(std::memory_order_relaxed));
      join_targets.push_back(snapshot.back().kind == WaitKind::kJoin
                                 ? snapshot.back().wait_object
                                 : nullptr);
      if (record->context != nullptr) {
        id_by_context.emplace(record->context, record->id);
      }
      id_by_record.emplace(record, record->id);
    }
  }

  for (size_t index = 0; index < snapshot.size(); ++index) {
    if (const void* owner = owner_contexts[index]; owner != nullptr) {
      if (const auto found = id_by_context.find(owner);
          found != id_by_context.end()) {
        snapshot[index].blocking_fiber_id = found->second;
        continue;
      }
    }
    if (const void* target = join_targets[index]; target != nullptr) {
      if (const auto found = id_by_record.find(target);
          found != id_by_record.end()) {
        snapshot[index].blocking_fiber_id = found->second;
      }
    }
  }
  return snapshot;
}

std::vector<FiberSnapshot> SnapshotFibers() {
  return SnapshotFibers(internal::kMaxWalkedFrames);
}

std::uint64_t TotalCompletedWaits() {
  std::uint64_t total = 0;
  const std::lock_guard<std::mutex> lock(RegistryMutex());
  for (const FiberDiagnostics* record = RegistryHead(); record != nullptr;
       record = record->reg_next) {
    total += record->waits_completed.load(std::memory_order_relaxed);
  }
  return total;
}

std::uint64_t CurrentFiberId() {
  const FiberDiagnostics* record = internal::CurrentFiberDiagnostics();
  return record == nullptr ? 0 : record->id;
}

void SetCurrentFiberName(std::string_view name) {
  FiberDiagnostics* record = internal::CurrentFiberDiagnostics();
  if (record == nullptr) {
    return;
  }
  const size_t length = std::min(name.size(), kFiberNameCapacity - 1);
  std::memcpy(record->name, name.data(), length);
  std::memset(record->name + length, 0, kFiberNameCapacity - length);
}

std::vector<std::vector<std::uint64_t>> FindWaitCycles(
    const std::vector<FiberSnapshot>& snapshot) {
  absl::flat_hash_map<std::uint64_t, std::uint64_t> waits_for;
  for (const FiberSnapshot& fiber : snapshot) {
    if (fiber.blocking_fiber_id != 0 && fiber.blocking_fiber_id != fiber.id) {
      waits_for.emplace(fiber.id, fiber.blocking_fiber_id);
    }
  }

  std::vector<std::vector<std::uint64_t>> cycles;
  absl::flat_hash_map<std::uint64_t, int> visit_state;  // 1 in path, 2 done.
  for (const auto& [start, unused] : waits_for) {
    if (visit_state[start] != 0) {
      continue;
    }
    std::vector<std::uint64_t> path;
    absl::flat_hash_map<std::uint64_t, size_t> position;
    std::uint64_t node = start;
    while (true) {
      if (const auto seen = position.find(node); seen != position.end()) {
        cycles.emplace_back(
            path.begin() + static_cast<std::ptrdiff_t>(seen->second),
            path.end());
        break;
      }
      if (visit_state[node] == 2) {
        break;
      }
      const auto next = waits_for.find(node);
      if (next == waits_for.end()) {
        break;
      }
      position.emplace(node, path.size());
      path.push_back(node);
      node = next->second;
    }
    for (const std::uint64_t visited : path) {
      visit_state[visited] = 2;
    }
  }
  return cycles;
}

std::vector<std::vector<std::uint64_t>> FindWaitCycles() {
  return FindWaitCycles(SnapshotFibers());
}

namespace {

std::string FiberLabel(const FiberSnapshot& fiber) {
  if (fiber.name.empty()) {
    return absl::StrCat("F#", fiber.id);
  }
  return absl::StrCat("F#", fiber.id, " \"", fiber.name, "\"");
}

void AppendWaitSummary(const FiberSnapshot& fiber, std::string* out) {
  absl::StrAppend(out, WaitKindName(fiber.kind));
  if (fiber.wait_object != nullptr) {
    absl::StrAppendFormat(out, "(%p)", fiber.wait_object);
  }
  if (fiber.blocking_fiber_id != 0) {
    absl::StrAppend(out, " held-by=F#", fiber.blocking_fiber_id);
  }
  absl::StrAppend(out, " waited=", absl::FormatDuration(fiber.waited));
  if (fiber.deadline != absl::InfiniteFuture()) {
    absl::StrAppend(out, " deadline=in ",
                    absl::FormatDuration(fiber.deadline - absl::Now()));
  }
}

void AppendFiberEntry(const FiberSnapshot& fiber, size_t max_frames,
                      std::string* out) {
  absl::StrAppend(out, FiberLabel(fiber));
  if (fiber.parent_id != 0) {
    absl::StrAppend(out, "  parent=F#", fiber.parent_id);
  }
  if (fiber.creation_pc != nullptr) {
    absl::StrAppend(out, "  created-at ",
                    internal::DescribeProgramCounter(fiber.creation_pc));
  }
  absl::StrAppend(out, "\n     ");
  AppendWaitSummary(fiber, out);
  absl::StrAppend(out, "\n");

  for (const void* selectable : fiber.selectables) {
    absl::StrAppendFormat(out, "     case %p\n", selectable);
  }

  if (fiber.trace_raced) {
    absl::StrAppend(out,
                    "     (woke while its stack was read; frames discarded)\n");
    return;
  }
  if (fiber.stack.empty()) {
    absl::StrAppend(out, internal::FramePointerWalkSupported()
                             ? "     (no frames recovered)\n"
                             : "     (frame-pointer walk unsupported on this "
                               "architecture)\n");
    return;
  }
  const size_t shown = std::min(fiber.stack.size(), max_frames);
  for (size_t frame = 0; frame < shown; ++frame) {
    absl::StrAppendFormat(out, "     #%-2zu %s\n", frame,
                          internal::DescribeProgramCounter(fiber.stack[frame]));
  }
  if (shown < fiber.stack.size()) {
    absl::StrAppendFormat(out, "     ... %zu more frames\n",
                          fiber.stack.size() - shown);
  }
}

}  // namespace

std::string FormatFiberReport(const FiberReportOptions& options) {
  const std::vector<FiberSnapshot> snapshot =
      SnapshotFibers(options.max_frames);
  const std::vector<std::vector<std::uint64_t>> cycles =
      FindWaitCycles(snapshot);

  absl::flat_hash_map<std::uint64_t, const FiberSnapshot*> by_id;
  absl::flat_hash_map<WaitKind, size_t> census;
  for (const FiberSnapshot& fiber : snapshot) {
    by_id.emplace(fiber.id, &fiber);
    ++census[fiber.kind];
  }

  std::string out = absl::StrFormat(
      "=== A11 fiber dump: %zu live fibers at %s\n", snapshot.size(),
      absl::FormatTime(absl::Now(), absl::UTCTimeZone()));

  absl::StrAppend(&out, "census:");
  for (const WaitKind kind :
       {WaitKind::kRunning, WaitKind::kCondVar, WaitKind::kMutex,
        WaitKind::kSelect, WaitKind::kSleep, WaitKind::kJoin,
        WaitKind::kThreadPlaceholder}) {
    if (const auto found = census.find(kind); found != census.end()) {
      absl::StrAppend(&out, " ", WaitKindName(kind), "=", found->second);
    }
  }
  absl::StrAppend(&out, "\n");

  for (const std::vector<std::uint64_t>& cycle : cycles) {
    absl::StrAppendFormat(
        &out, "\n--- deadlock: wait cycle of %zu fibers ---\n", cycle.size());
    for (const std::uint64_t id : cycle) {
      const auto found = by_id.find(id);
      if (found == by_id.end()) {
        continue;
      }
      absl::StrAppend(&out, "  ", FiberLabel(*found->second), " waits ");
      AppendWaitSummary(*found->second, &out);
      absl::StrAppend(&out, "\n");
    }
  }

  // Grouped by what the waiters have in common. For a Select that is the
  // selectable, not the per-call selector.
  const auto shared_object = [](const FiberSnapshot& fiber) {
    return fiber.selectables.empty() ? fiber.wait_object
                                     : fiber.selectables.front();
  };
  absl::flat_hash_map<const void*, size_t> sharing;
  std::vector<const FiberSnapshot*> ordered;
  for (const FiberSnapshot& fiber : snapshot) {
    if (fiber.kind == WaitKind::kRunning && !options.include_running) {
      continue;
    }
    if (fiber.kind == WaitKind::kThreadPlaceholder) {
      continue;
    }
    if (fiber.waited < options.stall_threshold) {
      continue;
    }
    ++sharing[shared_object(fiber)];
    ordered.push_back(&fiber);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const FiberSnapshot* left, const FiberSnapshot* right) {
              return left->waited > right->waited;
            });

  absl::StrAppendFormat(&out, "\n--- %zu fibers waiting at least %s ---\n",
                        ordered.size(),
                        absl::FormatDuration(options.stall_threshold));
  for (const FiberSnapshot* fiber : ordered) {
    const void* shared = shared_object(*fiber);
    if (const size_t count = sharing[shared]; count > 1 && shared != nullptr) {
      absl::StrAppendFormat(&out, "[%zu fibers wait on %p] ", count, shared);
    }
    AppendFiberEntry(*fiber, options.max_frames, &out);
  }
  return out;
}

namespace {

// Log sinks bound lines; a full report is thousands of them.
constexpr size_t kMaxLogChunkLines = 40;

void LogInChunks(std::string_view report) {
  std::vector<std::string_view> lines = absl::StrSplit(report, '\n');
  for (size_t start = 0; start < lines.size(); start += kMaxLogChunkLines) {
    const size_t end = std::min(start + kMaxLogChunkLines, lines.size());
    LOG(ERROR) << absl::StrJoin(std::vector<std::string_view>(
                                    lines.begin() + start, lines.begin() + end),
                                "\n");
  }
}

struct WatchdogState {
  std::mutex mu;
  std::atomic<bool> running{false};
  std::atomic<std::int64_t> threshold_nanos{0};
  std::atomic<bool> abort_on_stall{false};
  std::atomic<bool> dump_requested{false};
};

WatchdogState& Watchdog() {
  static absl::NoDestructor<WatchdogState> state;
  return *state;
}

void WatchdogLoop() {
  WatchdogState& state = Watchdog();
  bool reported = false;
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    const absl::Duration threshold = absl::Nanoseconds(
        state.threshold_nanos.load(std::memory_order_relaxed));
    const bool on_request =
        state.dump_requested.exchange(false, std::memory_order_acq_rel);

    if (on_request) {
      LogInChunks(FormatFiberReport({.include_running = true}));
      continue;
    }
    if (threshold <= absl::ZeroDuration()) {
      continue;
    }

    const std::vector<FiberSnapshot> snapshot = SnapshotFibers(0);
    bool stalled = false;
    for (const FiberSnapshot& fiber : snapshot) {
      if (fiber.kind != WaitKind::kRunning &&
          fiber.kind != WaitKind::kThreadPlaceholder &&
          fiber.waited >= threshold) {
        stalled = true;
        break;
      }
    }
    if (!stalled) {
      reported = false;
      continue;
    }
    // Report the stall once; a fiber legitimately parked forever should not
    // produce a report every quarter second.
    if (reported) {
      continue;
    }
    reported = true;
    const std::string report =
        FormatFiberReport({.stall_threshold = threshold});
    LogInChunks(report);
    if (state.abort_on_stall.load(std::memory_order_relaxed)) {
      LOG(FATAL) << "A11_FIBER_WATCHDOG_ABORT: fibers stalled for "
                 << absl::FormatDuration(threshold);
    }
  }
}

void StartWatchdogThread() {
  WatchdogState& state = Watchdog();
  const std::lock_guard<std::mutex> lock(state.mu);
  if (state.running.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  std::thread(WatchdogLoop).detach();
}

std::atomic<int>& DumpSignalNumber() {
  static absl::NoDestructor<std::atomic<int>> number{0};
  return *number;
}

// Async-signal-safe: one atomic store, no lock, no allocation. The watchdog
// thread does the reporting.
void HandleDumpSignal(int /*signal_number*/) {
  Watchdog().dump_requested.store(true, std::memory_order_release);
}

}  // namespace

void InstallFiberWatchdog(absl::Duration stall_threshold, bool abort_on_stall) {
  WatchdogState& state = Watchdog();
  state.threshold_nanos.store(absl::ToInt64Nanoseconds(stall_threshold),
                              std::memory_order_relaxed);
  state.abort_on_stall.store(abort_on_stall, std::memory_order_relaxed);
  StartWatchdogThread();
}

void RequestFiberDump() {
  Watchdog().dump_requested.store(true, std::memory_order_release);
}

bool InstallFiberDumpSignalHandler(int signal_number) {
  if (signal_number == 0) {
    const char* configured = std::getenv("A11_FIBER_DUMP_SIGNAL");
    signal_number = SIGUSR2;
    if (configured != nullptr) {
      const int parsed = std::atoi(configured);
      if (parsed <= 0) {
        return false;
      }
      signal_number = parsed;
    }
  }
  StartWatchdogThread();
  struct sigaction action = {};
  action.sa_handler = HandleDumpSignal;
  action.sa_flags = SA_RESTART;
  sigemptyset(&action.sa_mask);
  if (sigaction(signal_number, &action, nullptr) != 0) {
    return false;
  }
  DumpSignalNumber().store(signal_number, std::memory_order_relaxed);
  return true;
}

void DumpFiberReport(const FiberReportOptions& options) {
  LogInChunks(FormatFiberReport(options));
}

namespace internal {

void InstallFiberDiagnosticsFromEnvironment() {
  const char* watchdog = std::getenv("A11_FIBER_WATCHDOG");
  if (watchdog != nullptr) {
    const double seconds = std::atof(watchdog);
    if (seconds > 0.0) {
      const char* abort_dial = std::getenv("A11_FIBER_WATCHDOG_ABORT");
      InstallFiberWatchdog(
          absl::Seconds(seconds),
          abort_dial != nullptr && std::strcmp(abort_dial, "0") != 0);
    }
  }
  if (std::getenv("A11_FIBER_DUMP_SIGNAL") != nullptr) {
    InstallFiberDumpSignalHandler();
  }
}

}  // namespace internal
}  // namespace thread
