// Copyright 2026 The A11 Authors.

#include "a11/stores/chunk_store_reader.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <absl/base/no_destructor.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "a11/concurrency/callback_scheduler.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/stores/chunk_store.h"
#include "thread/boost_primitives.h"
#include "thread/executor.h"

namespace a11::stores {

absl::Status ChunkStoreReaderOptions::Validate() const {
  constexpr std::uint64_t kMaximum = std::uint64_t{1} << 32U;
  if (num_chunks_to_buffer > kMaximum) {
    return absl::OutOfRangeError("num_chunks_to_buffer exceeds 2^32");
  }
  if (max_chunks_to_read.has_value() && *max_chunks_to_read > kMaximum) {
    return absl::OutOfRangeError("max_chunks_to_read exceeds 2^32");
  }
  return absl::OkStatus();
}

struct ChunkStoreReader::State
    : public std::enable_shared_from_this<ChunkStoreReader::State> {
  using NextResult = std::optional<data::NodeFragment>;

  struct Request {
    [[nodiscard]] bool TryClaim() {
      return !completed.exchange(true, std::memory_order_acq_rel);
    }

    std::atomic<bool> completed = false;
    a11::Promise<NextResult> promise;
  };

  enum class CompletionKind { kFragment, kEnd, kError };

  struct Completion {
    std::shared_ptr<Request> request;
    CompletionKind kind;
    std::optional<data::NodeFragment> fragment;
    absl::Status status;
  };

  enum class Operation { kNone, kFetch, kClear };

  State(std::shared_ptr<ChunkStore> chunk_store,
        ChunkStoreReaderOptions reader_options)
      : store(std::move(chunk_store)),
        options(reader_options),
        position(reader_options.offset),
        done(done_promise.future()) {}

  static a11::internal::CallbackScheduler& Scheduler() {
    static absl::NoDestructor<a11::internal::CallbackScheduler> scheduler;
    return *scheduler;
  }

  void Wake() {
    bool schedule = false;
    {
      thread::MutexLock lock(&mu);
      if (!queued && operation == Operation::kNone) {
        queued = true;
        schedule = true;
      }
    }
    if (schedule) {
      Scheduler().Schedule([self = shared_from_this()] { self->Drive(); });
    }
  }

  void Cancel() {
    std::vector<Completion> completions;
    a11::Future<data::NodeFragment> active;
    {
      thread::MutexLock lock(&mu);
      if (!status.has_value()) {
        status = absl::AbortedError("ChunkStoreReader was cancelled");
      }
      CollectAvailableLocked(&completions);
      active = active_operation;
    }
    Complete(std::move(completions));
    if (active.valid()) {
      (void)active.Cancel();
    }
    MaybeCompleteDone();
  }

  a11::Future<NextResult> Next(absl::Duration timeout) {
    auto request = std::make_shared<Request>();
    a11::Future<NextResult> future = request->promise.future();
    const std::weak_ptr<State> weak_state = weak_from_this();
    const std::weak_ptr<Request> weak_request = request;
    const absl::Status cancellation_callback =
        request->promise.SetCancellationCallback([weak_state, weak_request] {
          const std::shared_ptr<Request> pending = weak_request.lock();
          if (pending == nullptr || !pending->TryClaim()) {
            return;
          }
          pending->promise
              .SetStatus(
                  absl::CancelledError("ChunkStoreReader Next was cancelled"))
              .IgnoreError();
          const std::shared_ptr<State> state = weak_state.lock();
          if (state != nullptr) {
            state->Wake();
          }
        });
    (void)cancellation_callback;

    {
      thread::MutexLock lock(&mu);
      pending_reads.push_back(request);
    }

    if (timeout != absl::InfiniteDuration()) {
      thread::PostAt(absl::Now() + timeout, [weak_state, weak_request] {
        const std::shared_ptr<Request> pending = weak_request.lock();
        if (pending == nullptr || !pending->TryClaim()) {
          return;
        }
        pending->promise
            .SetStatus(absl::DeadlineExceededError(
                "ChunkStoreReader Next timed out before a fragment was "
                "available"))
            .IgnoreError();
        const std::shared_ptr<State> state = weak_state.lock();
        if (state != nullptr) {
          state->Wake();
        }
      });
    }
    Wake();
    return future;
  }

  void Drive() {
    std::vector<Completion> completions;
    bool start_fetch = false;
    bool ordered = true;
    std::uint64_t requested_position = 0;
    std::uint64_t generation = 0;
    {
      thread::MutexLock lock(&mu);
      queued = false;
      CollectAvailableLocked(&completions);
      if (status.has_value() || operation != Operation::kNone) {
        // Terminal requests were collected above; an active operation will
        // arrange the next wake-up from its completion callback.
      } else if (options.max_chunks_to_read.has_value() &&
                 *options.max_chunks_to_read == 0) {
        status = absl::OkStatus();
        CollectAvailableLocked(&completions);
      } else if (!HasReadCapacityLocked()) {
        // Demand and prefetch capacity are both exhausted.
      } else if (position > std::numeric_limits<std::uint32_t>::max()) {
        status = absl::OutOfRangeError(
            "ChunkStoreReader position exceeds the sequence range");
        CollectAvailableLocked(&completions);
      } else {
        operation = Operation::kFetch;
        generation = ++operation_generation;
        requested_position = position;
        ordered = options.ordered;
        start_fetch = true;
      }
    }
    Complete(std::move(completions));
    MaybeCompleteDone();
    if (!start_fetch) {
      return;
    }

    a11::Future<data::NodeFragment> pending;
    try {
      pending = ordered
                    ? store->Get(static_cast<std::uint32_t>(requested_position),
                                 absl::InfiniteFuture())
                    : store->GetByArrivalOrder(requested_position,
                                               absl::InfiniteFuture());
    } catch (const std::exception& error) {
      pending = a11::FailedFuture<data::NodeFragment>(
          absl::UnknownError(error.what()));
    } catch (...) {
      pending = a11::FailedFuture<data::NodeFragment>(absl::UnknownError(
          "ChunkStore reader fetch raised a non-standard exception"));
    }
    InstallFetch(std::move(pending), generation);
  }

  void InstallFetch(a11::Future<data::NodeFragment> pending,
                    std::uint64_t generation) {
    bool cancel = false;
    {
      thread::MutexLock lock(&mu);
      if (operation != Operation::kFetch ||
          operation_generation != generation) {
        cancel = true;
      } else {
        active_operation = pending;
        cancel = status.has_value();
      }
    }
    pending.OnReady([self = shared_from_this(), generation](
                        const absl::StatusOr<data::NodeFragment>& result) {
      self->FetchDone(generation, result);
    });
    if (cancel) {
      (void)pending.Cancel();
    }
  }

  void FetchDone(std::uint64_t generation,
                 const absl::StatusOr<data::NodeFragment>& result) {
    std::vector<Completion> completions;
    bool start_clear = false;
    std::uint32_t clear_seq = 0;
    std::uint64_t clear_generation = 0;
    {
      thread::MutexLock lock(&mu);
      if (operation != Operation::kFetch ||
          operation_generation != generation) {
        return;
      }
      active_operation = {};
      operation = Operation::kNone;
      if (status.has_value()) {
        CollectAvailableLocked(&completions);
      } else if (!result.ok()) {
        if (absl::IsNotFound(result.status()) &&
            !options.max_chunks_to_read.has_value()) {
          status = absl::OkStatus();
        } else {
          status = result.status();
        }
        CollectAvailableLocked(&completions);
      } else if (!result->seq.has_value()) {
        status = absl::DataLossError(
            "ChunkStore returned a fragment without a sequence number");
        CollectAvailableLocked(&completions);
      } else if (options.pop_chunks) {
        operation = Operation::kClear;
        clear_generation = ++operation_generation;
        clear_seq = *result->seq;
        start_clear = true;
      } else {
        FinishFragmentLocked(*result, &completions);
      }
    }
    Complete(std::move(completions));
    MaybeCompleteDone();

    if (start_clear) {
      a11::Future<data::NodeFragment> pending;
      try {
        pending = store->ClearData(clear_seq);
      } catch (const std::exception& error) {
        pending = a11::FailedFuture<data::NodeFragment>(
            absl::UnknownError(error.what()));
      } catch (...) {
        pending = a11::FailedFuture<data::NodeFragment>(absl::UnknownError(
            "ChunkStore clear raised a non-standard exception"));
      }
      InstallClear(std::move(pending), clear_generation);
      return;
    }
    Wake();
  }

  void InstallClear(a11::Future<data::NodeFragment> pending,
                    std::uint64_t generation) {
    bool cancel = false;
    {
      thread::MutexLock lock(&mu);
      if (operation != Operation::kClear ||
          operation_generation != generation) {
        cancel = true;
      } else {
        active_operation = pending;
        cancel = status.has_value();
      }
    }
    pending.OnReady([self = shared_from_this(), generation](
                        const absl::StatusOr<data::NodeFragment>& result) {
      self->ClearDone(generation, result);
    });
    if (cancel) {
      (void)pending.Cancel();
    }
  }

  void ClearDone(std::uint64_t generation,
                 const absl::StatusOr<data::NodeFragment>& result) {
    std::vector<Completion> completions;
    {
      thread::MutexLock lock(&mu);
      if (operation != Operation::kClear ||
          operation_generation != generation) {
        return;
      }
      active_operation = {};
      operation = Operation::kNone;
      if (status.has_value()) {
        CollectAvailableLocked(&completions);
      } else if (!result.ok()) {
        status = result.status();
        CollectAvailableLocked(&completions);
      } else {
        FinishFragmentLocked(*result, &completions);
      }
    }
    Complete(std::move(completions));
    MaybeCompleteDone();
    Wake();
  }

  void MaybeCompleteDone() {
    bool complete = false;
    {
      thread::MutexLock lock(&mu);
      if (status.has_value() && !done_completed) {
        done_completed = true;
        complete = true;
      }
    }
    if (complete) {
      done_promise.SetValue(a11::Unit{}).IgnoreError();
    }
  }

  void FinishFragmentLocked(data::NodeFragment fragment,
                            std::vector<Completion>* completions)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu) {
    const bool continued = fragment.continued;
    // Fetches are serialised through the operation guard, so fragments finish
    // in strictly increasing position order and the freshly read one always
    // belongs at the back of the ordered buffer. Appending it here - rather
    // than handing it straight to the FIFO-front waiter - is what makes serial
    // reads deliver serially: a request that could not yet be matched against
    // an earlier buffered fragment (its wake-up was coalesced away while this
    // fetch was in flight) must not receive this later fragment ahead of it.
    // CollectAvailableLocked below drains the buffer front-to-back, so any
    // waiter is always paired with the earliest outstanding fragment. It also
    // covers the zero-buffer case, where a timed-out request may leave one
    // unavoidable in-flight result that is preserved for the next caller.
    buffer.push_back(std::move(fragment));

    ++chunks_read;
    ++position;
    if (options.max_chunks_to_read.has_value() &&
        chunks_read == *options.max_chunks_to_read) {
      status = absl::OkStatus();
    } else if (options.ordered && !continued) {
      if (!options.max_chunks_to_read.has_value()) {
        status = absl::OkStatus();
      } else {
        status = absl::OutOfRangeError(
            absl::StrCat("The final fragment arrived after ", chunks_read,
                         " chunks, before max_chunks_to_read=",
                         *options.max_chunks_to_read));
      }
    }
    CollectAvailableLocked(completions);
  }

  [[nodiscard]] size_t ActivePendingReadCountLocked() const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu) {
    size_t count = 0;
    for (const std::shared_ptr<Request>& request : pending_reads) {
      if (!request->completed.load(std::memory_order_acquire)) {
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] bool HasReadCapacityLocked() const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu) {
    const size_t pending_count = ActivePendingReadCountLocked();
    const size_t capacity =
        static_cast<size_t>(options.num_chunks_to_buffer) + pending_count;
    return buffer.size() < capacity;
  }

  std::shared_ptr<Request> PopPendingReadLocked()
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu) {
    while (!pending_reads.empty()) {
      std::shared_ptr<Request> request = std::move(pending_reads.front());
      pending_reads.pop_front();
      if (request->TryClaim()) {
        return request;
      }
    }
    return nullptr;
  }

  void CollectAvailableLocked(std::vector<Completion>* completions)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu) {
    while (!buffer.empty()) {
      std::shared_ptr<Request> request = PopPendingReadLocked();
      if (request == nullptr) {
        break;
      }
      data::NodeFragment fragment = std::move(buffer.front());
      buffer.pop_front();
      completions->push_back(Completion{
          .request = std::move(request),
          .kind = CompletionKind::kFragment,
          .fragment = std::move(fragment),
          .status = absl::OkStatus(),
      });
    }
    if (!status.has_value() || !buffer.empty()) {
      return;
    }
    while (std::shared_ptr<Request> request = PopPendingReadLocked()) {
      completions->push_back(Completion{
          .request = std::move(request),
          .kind = status->ok() ? CompletionKind::kEnd : CompletionKind::kError,
          .fragment = std::nullopt,
          .status = *status,
      });
    }
  }

  static void Complete(std::vector<Completion> completions) {
    for (Completion& completion : completions) {
      switch (completion.kind) {
        case CompletionKind::kFragment:
          completion.request->promise
              .SetValue(NextResult(std::move(*completion.fragment)))
              .IgnoreError();
          break;
        case CompletionKind::kEnd:
          completion.request->promise.SetValue(std::nullopt).IgnoreError();
          break;
        case CompletionKind::kError:
          completion.request->promise.SetStatus(completion.status)
              .IgnoreError();
          break;
      }
    }
  }

  const std::shared_ptr<ChunkStore> store;
  const ChunkStoreReaderOptions options;
  mutable thread::Mutex mu;
  std::uint64_t position ABSL_GUARDED_BY(mu);
  std::uint64_t chunks_read ABSL_GUARDED_BY(mu) = 0;
  std::optional<absl::Status> status ABSL_GUARDED_BY(mu);
  std::deque<data::NodeFragment> buffer ABSL_GUARDED_BY(mu);
  std::deque<std::shared_ptr<Request>> pending_reads ABSL_GUARDED_BY(mu);
  bool queued ABSL_GUARDED_BY(mu) = false;
  Operation operation ABSL_GUARDED_BY(mu) = Operation::kNone;
  std::uint64_t operation_generation ABSL_GUARDED_BY(mu) = 0;
  a11::Future<data::NodeFragment> active_operation ABSL_GUARDED_BY(mu);
  a11::Promise<a11::Unit> done_promise;
  const a11::Task done;
  bool done_completed ABSL_GUARDED_BY(mu) = false;
};

absl::StatusOr<std::shared_ptr<ChunkStoreReader>> ChunkStoreReader::Create(
    std::shared_ptr<ChunkStore> store, ChunkStoreReaderOptions options) {
  if (store == nullptr) {
    return absl::InvalidArgumentError("store must not be null");
  }
  absl::Status status = options.Validate();
  if (!status.ok()) {
    return status;
  }

  struct MakeSharedEnabler final : ChunkStoreReader {
    explicit MakeSharedEnabler(std::shared_ptr<State> state)
        : ChunkStoreReader(std::move(state)) {}
  };

  std::shared_ptr<ChunkStoreReader> reader =
      std::make_shared<MakeSharedEnabler>(
          std::make_shared<State>(std::move(store), options));
  reader->EnsureStarted();
  return reader;
}

void ChunkStoreReader::EnsureStarted() {
  state_->Wake();
}

void ChunkStoreReader::Cancel() {
  state_->Cancel();
}

absl::Status ChunkStoreReader::GetStatus() const {
  thread::MutexLock lock(&state_->mu);
  return state_->status.value_or(absl::OkStatus());
}

a11::Task ChunkStoreReader::Done() const {
  return state_->done;
}

a11::Future<std::optional<data::NodeFragment>> ChunkStoreReader::Next(
    absl::Duration timeout) {
  if (timeout < absl::ZeroDuration() && timeout != absl::InfiniteDuration()) {
    return a11::FailedFuture<std::optional<data::NodeFragment>>(
        absl::InvalidArgumentError("timeout must be non-negative or infinite"));
  }
  return state_->Next(timeout);
}

std::shared_ptr<ChunkStore> ChunkStoreReader::store() const {
  return state_->store;
}

ChunkStoreReaderOptions ChunkStoreReader::options() const {
  return state_->options;
}

size_t ChunkStoreReader::buffer_size() const {
  thread::MutexLock lock(&state_->mu);
  return state_->buffer.size();
}

}  // namespace a11::stores
