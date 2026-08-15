// Copyright 2026 The A11 Authors.

#include "a11/stores/chunk_store_reader.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/base/no_destructor.h>
#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "a11/concurrency/callback_scheduler.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/concurrency/inline_pump.h"
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
        next_fetch_position(reader_options.offset),
        done(done_promise.future()) {}

  static a11::internal::CallbackScheduler& Scheduler() {
    static absl::NoDestructor<a11::internal::CallbackScheduler> scheduler;
    return *scheduler;
  }

  void Wake() {
    bool schedule = false;
    {
      thread::MutexLock lock(&mu);
      if (!queued) {
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
    std::vector<a11::Future<data::NodeFragment>> outstanding;
    {
      thread::MutexLock lock(&mu);
      if (!status.has_value()) {
        status = absl::AbortedError("ChunkStoreReader was cancelled");
      }
      CollectAvailableLocked(&completions);
      active = active_operation;
      // Bumping the generation makes every outstanding fetch's completion a
      // no-op, so a late arrival cannot resurrect a cancelled reader.
      ++fetch_generation;
      fetches_in_flight = 0;
      arrived.clear();
      outstanding.swap(active_fetches);
    }
    Complete(std::move(completions));
    if (active.valid()) {
      (void)active.Cancel();
    }
    for (a11::Future<data::NodeFragment>& fetch : outstanding) {
      if (fetch.valid()) {
        (void)fetch.Cancel();
      }
    }
    MaybeCompleteDone();
  }

  /**
   * @brief
   *   Pop up to `limit` already-prefetched fragments, without ever waiting.
   *
   * Only takes anything when nobody is queued ahead: `pending_reads` is served
   * strictly in order, and a batch read that jumped that queue would reorder
   * two concurrent readers. Everything it returns is a fragment
   * CollectAvailableLocked() would have handed out next, in the same order, so
   * this adds no semantics of its own -- which is the point. Terminal state
   * (end of stream, error) is deliberately *not* handled here; NextMany falls
   * back to the single-fragment path for that, so there is exactly one place
   * that decides what the end of a stream looks like.
   */
  std::vector<data::NodeFragment> TakeBuffered(size_t limit) {
    // Fill on the caller's thread before taking, rather than waking the pump
    // and taking whatever happens to be there. A caller that comes straight
    // back for more outruns an asynchronous refill -- the buffer fills from
    // inside Drive(), so a poller that returns the moment its own fragment
    // lands arrives again mid-fill -- and batches alternate between full and
    // one. The reads cost the caller nothing it was not going to wait for, and
    // a store that answers inline answers them without waiting at all.
    Drive();
    std::vector<data::NodeFragment> taken;
    {
      thread::MutexLock lock(&mu);
      if (!pending_reads.empty()) {
        return taken;
      }
      taken.reserve(std::min(limit, buffer.size()));
      while (taken.size() < limit && !buffer.empty()) {
        taken.push_back(std::move(buffer.front()));
        buffer.pop_front();
      }
    }
    // The buffer just lost fragments; let the pump refill it.
    if (!taken.empty()) {
      Wake();
    }
    return taken;
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

    // Serve from the buffer on the caller's thread when it can be served.
    //
    // Drive() would do exactly this, but only after Wake() has put it through
    // the scheduler, and that hop costs more than an order of magnitude what
    // the read itself does -- which for a reader draining a node that is
    // already full would be a scheduler round trip per fragment.
    //
    // CollectAvailableLocked() pops `pending_reads` from the front, so a request
    // that arrives behind an earlier waiter still queues behind it; FIFO order is
    // unchanged. The request is claimed by PopPendingReadLocked(), so the timeout
    // below and the cancellation callback both find it taken and cannot
    // double-resolve.
    std::vector<Completion> completions;
    {
      thread::MutexLock lock(&mu);
      pending_reads.push_back(request);
      CollectAvailableLocked(&completions);
    }
    Complete(std::move(completions));

    // Then fetch on the caller's thread rather than handing the work to the
    // pump: Wake() only schedules, so a read the buffer cannot answer would pay
    // a scheduler hop before the store is even asked, and a caller reading one
    // fragment at a time never gets ahead of that. Driving here fetches
    // everything the store can answer without waiting, so this request is
    // usually resolved by the time it is returned and the buffer is refilled
    // for the one after it. A store that cannot answer inline leaves its
    // fetches outstanding.
    Drive();

    // Only a request that is still outstanding needs a timer. Arming one for a
    // read that has already been answered is pure cost, and it is the common
    // case now.
    const bool settled = request->completed.load(std::memory_order_acquire);
    if (!settled && timeout != absl::InfiniteDuration()) {
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
    if (settled) {
      // Served from the buffer, which Drive() above has already refilled; a
      // wake would only re-enter an idle pump.
      return future;
    }
    Wake();
    return future;
  }

  /**
   * @brief
   *   Pump until there is nothing left to do without waiting.
   *
   * A store that already holds the fragment resolves Get() inline, which is the
   * normal case, so this keeps going for as long as that holds. Handing each
   * such fragment back to the scheduler instead would cost a scheduler hop per
   * fragment and leave the buffer no more than one fragment ahead, which is
   * what would make `NextMany`/`next_fragments` return batches of one.
   *
   * A loop and not recursion, deliberately: A11 runs these callbacks on pooled
   * fibers with small stacks, and recursing once per fragment would overflow
   * them.
   */
  void Drive() {
    DriveInline(&mu, &pump, "ChunkStoreReader", [this] {
      while (DriveOnce()) {
      }
    });
  }

  /**
   * @brief How many reads the pump keeps outstanding at once.
   *
   * With one, a store whose Get() has any latency delivers at one fragment per
   * round trip and the prefetch buffer never fills; the reader spends its life
   * waiting. Beyond about this many the extra concurrency stops buying
   * anything and starts costing memory in the reorder map.
   */
  static constexpr size_t kMaxFetchesInFlight = 16;

  /** @return Whether the caller should drive again without yielding. */
  bool DriveOnce() {
    std::vector<Completion> completions;
    struct Wanted {
      std::uint64_t position;
      std::uint64_t generation;
    };
    std::vector<Wanted> to_issue;
    bool ordered = true;
    {
      thread::MutexLock lock(&mu);
      queued = false;
      CollectAvailableLocked(&completions);
      ordered = options.ordered;
      // `pop_chunks` clears each fragment after reading it, which is a second
      // operation per fragment sequenced behind the first. Keep that path on
      // one-at-a-time; overlapping reads with clears is a different problem
      // and not one this buys anything on.
      const size_t ceiling =
          options.pop_chunks ? 1 : kMaxFetchesInFlight;
      if (status.has_value() || operation != Operation::kNone) {
        // Terminal requests were collected above; a clear in progress will
        // arrange the next wake-up from its completion callback.
      } else if (options.max_chunks_to_read.has_value() &&
                 *options.max_chunks_to_read == 0) {
        status = absl::OkStatus();
        CollectAvailableLocked(&completions);
      } else {
        while (fetches_in_flight + to_issue.size() < ceiling &&
               HasReadCapacityLocked(to_issue.size())) {
          if (next_fetch_position >
              std::numeric_limits<std::uint32_t>::max()) {
            if (to_issue.empty() && fetches_in_flight == 0) {
              status = absl::OutOfRangeError(
                  "ChunkStoreReader position exceeds the sequence range");
              CollectAvailableLocked(&completions);
            }
            break;
          }
          // A read this reader has asked for but not yet delivered. Counted
          // against capacity so the prefetch window is the buffer size, not
          // the buffer size plus everything in flight.
          to_issue.push_back(
              Wanted{.position = next_fetch_position++,
                     .generation = fetch_generation});
        }
        fetches_in_flight += to_issue.size();
      }
    }
    Complete(std::move(completions));
    MaybeCompleteDone();
    if (to_issue.empty()) {
      return false;
    }

    bool drive_again = false;
    for (const Wanted& wanted : to_issue) {
      a11::Future<data::NodeFragment> pending;
      try {
        pending = ordered
                      ? store->Get(static_cast<std::uint32_t>(wanted.position),
                                   absl::InfiniteFuture())
                      : store->GetByArrivalOrder(wanted.position,
                                                 absl::InfiniteFuture());
      } catch (const std::exception& error) {
        pending = a11::FailedFuture<data::NodeFragment>(
            absl::UnknownError(error.what()));
      } catch (...) {
        pending = a11::FailedFuture<data::NodeFragment>(absl::UnknownError(
            "ChunkStore reader fetch raised a non-standard exception"));
      }
      if (pending.IsReady()) {
        // The store had it. Account for it here rather than paying a whole
        // scheduler pass to do the same thing.
        drive_again =
            FetchArrived(wanted.position, wanted.generation, pending.Await(),
                         /*inline_drive=*/true) ||
            drive_again;
      } else {
        InstallFetch(std::move(pending), wanted.position, wanted.generation);
      }
    }
    return drive_again;
  }

  void InstallFetch(a11::Future<data::NodeFragment> pending,
                    std::uint64_t position_wanted, std::uint64_t generation) {
    bool cancel = false;
    {
      thread::MutexLock lock(&mu);
      if (generation != fetch_generation) {
        cancel = true;
      } else {
        active_fetches.push_back(pending);
        cancel = status.has_value();
      }
    }
    pending.OnReady([self = shared_from_this(), position_wanted, generation](
                        const absl::StatusOr<data::NodeFragment>& result) {
      (void)self->FetchArrived(position_wanted, generation, result, false);
    });
    if (cancel) {
      (void)pending.Cancel();
    }
  }

  /**
   * @brief Record a completed fetch, in order.
   *
   * With several reads outstanding they can land in any order, so a result is
   * parked in `arrived` until it is the one at `position`. Only then is it
   * applied -- which is what preserves ordered delivery now that the old
   * "one fetch at a time" serialisation is gone. Errors wait their turn too: a
   * NotFound at position+2 is not end-of-stream while position is still
   * outstanding.
   *
   * @param inline_drive
   *   True when the fetch resolved without waiting and DriveOnce() is still on
   *   the stack; then this reports back whether to keep driving instead of
   *   waking the scheduler.
   * @return Whether the caller should drive again immediately.
   */
  bool FetchArrived(std::uint64_t arrived_position, std::uint64_t generation,
                    const absl::StatusOr<data::NodeFragment>& result,
                    bool inline_drive = false) {
    std::vector<Completion> completions;
    bool start_clear = false;
    std::uint32_t clear_seq = 0;
    std::uint64_t clear_generation = 0;
    {
      thread::MutexLock lock(&mu);
      if (generation != fetch_generation) {
        return false;
      }
      if (fetches_in_flight > 0) {
        --fetches_in_flight;
      }
      arrived.insert_or_assign(arrived_position, result);
      DrainArrivedLocked(&completions, &start_clear, &clear_seq,
                         &clear_generation);
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
      return false;
    }
    if (inline_drive) {
      return true;
    }
    Wake();
    return false;
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

  /**
   * @brief
   *   Apply arrived results that are now at the head, in position order.
   *
   * The decision for each one is exactly what it was when fetches were
   * serialised -- end of stream, error, clear-then-buffer, or buffer -- only
   * now it is taken when the result reaches the front of the queue rather than
   * when it happens to come back. Stops at the first gap, and stops as soon as
   * a terminal status is set, leaving anything later in `arrived` to be
   * discarded.
   */
  void DrainArrivedLocked(std::vector<Completion>* completions,
                          bool* start_clear, std::uint32_t* clear_seq,
                          std::uint64_t* clear_generation)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu) {
    while (!status.has_value()) {
      const auto head = arrived.find(position);
      if (head == arrived.end()) {
        break;
      }
      const absl::StatusOr<data::NodeFragment> result = std::move(head->second);
      arrived.erase(head);

      if (!result.ok()) {
        if (absl::IsNotFound(result.status()) &&
            !options.max_chunks_to_read.has_value()) {
          status = absl::OkStatus();
        } else {
          status = result.status();
        }
        break;
      }
      if (!result->seq.has_value()) {
        status = absl::DataLossError(
            "ChunkStore returned a fragment without a sequence number");
        break;
      }
      if (options.pop_chunks) {
        // One clear at a time, sequenced ahead of any further delivery; the
        // fetch ceiling is 1 in this mode so there is nothing queued behind.
        operation = Operation::kClear;
        *clear_generation = ++operation_generation;
        *clear_seq = *result->seq;
        *start_clear = true;
        return;
      }
      // Advances `position`, which is what lets the next arrival be the head.
      FinishFragmentLocked(*result, completions);
    }
    CollectAvailableLocked(completions);
    if (status.has_value()) {
      // Nothing after a terminal status can be delivered, and holding it only
      // keeps fragments alive.
      arrived.clear();
    }
  }

  void FinishFragmentLocked(data::NodeFragment fragment,
                            std::vector<Completion>* completions)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu) {
    if (options.ordered && options.sticky_mimetype) {
      data::Chunk* chunk = std::get_if<data::Chunk>(&fragment.data);
      if (chunk != nullptr) {
        const std::string mimetype = chunk->GetMimetype();
        if (!mimetype.empty()) {
          if (mimetype != current_mimetype) {
            current_mimetype = mimetype;
          }
        } else if (!current_mimetype.empty()) {
          if (!chunk->metadata.has_value()) {
            chunk->metadata.emplace();
          }
          chunk->metadata->mimetype = current_mimetype;
        }
      }
    }
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

  [[nodiscard]] bool HasReadCapacityLocked(size_t about_to_issue = 0) const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu) {
    const size_t pending_count = ActivePendingReadCountLocked();
    const size_t capacity =
        static_cast<size_t>(options.num_chunks_to_buffer) + pending_count;
    // Reads already in flight, and results parked waiting for their turn, are
    // fragments this reader has committed to holding. Counting them keeps the
    // prefetch window equal to the configured buffer size rather than that
    // plus everything outstanding.
    const size_t committed = buffer.size() + fetches_in_flight +
                             arrived.size() + about_to_issue;
    return committed < capacity;
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
  std::string current_mimetype ABSL_GUARDED_BY(mu);
  std::optional<absl::Status> status ABSL_GUARDED_BY(mu);
  std::deque<data::NodeFragment> buffer ABSL_GUARDED_BY(mu);
  std::deque<std::shared_ptr<Request>> pending_reads ABSL_GUARDED_BY(mu);
  bool queued ABSL_GUARDED_BY(mu) = false;
  // Re-entry bookkeeping for Drive(); read and written only under mu.
  InlinePumpState pump;
  Operation operation ABSL_GUARDED_BY(mu) = Operation::kNone;
  std::uint64_t operation_generation ABSL_GUARDED_BY(mu) = 0;
  a11::Future<data::NodeFragment> active_operation ABSL_GUARDED_BY(mu);
  // Multi-fetch state. `position` is the next fragment to *deliver*;
  // `next_fetch_position` is the next one to *ask for*, and runs ahead of it by
  // however many fetches are outstanding. Results that arrive out of order wait
  // in `arrived` until they are at the head, which is what preserves ordered
  // delivery across fetches that are in flight together.
  std::uint64_t next_fetch_position ABSL_GUARDED_BY(mu);
  size_t fetches_in_flight ABSL_GUARDED_BY(mu) = 0;
  std::uint64_t fetch_generation ABSL_GUARDED_BY(mu) = 0;
  std::map<std::uint64_t, absl::StatusOr<data::NodeFragment>> arrived
      ABSL_GUARDED_BY(mu);
  std::vector<a11::Future<data::NodeFragment>> active_fetches
      ABSL_GUARDED_BY(mu);
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

a11::Future<std::vector<std::optional<data::NodeFragment>>>
ChunkStoreReader::NextMany(size_t limit, absl::Duration timeout) {
  using Batch = std::vector<std::optional<data::NodeFragment>>;
  if (limit == 0) {
    return a11::FailedFuture<Batch>(
        absl::InvalidArgumentError("limit must be positive"));
  }
  if (timeout < absl::ZeroDuration() && timeout != absl::InfiniteDuration()) {
    return a11::FailedFuture<Batch>(
        absl::InvalidArgumentError("timeout must be non-negative or infinite"));
  }

  std::vector<data::NodeFragment> buffered = state_->TakeBuffered(limit);
  if (!buffered.empty()) {
    Batch batch;
    batch.reserve(buffered.size());
    for (data::NodeFragment& fragment : buffered) {
      batch.emplace_back(std::move(fragment));
    }
    return a11::CompletedFuture<Batch>(std::move(batch));
  }

  // Nothing prefetched. Fall through to exactly one ordinary read, so end of
  // stream, errors, timeouts and cancellation all behave as they always have,
  // then sweep up anything that landed while that read was in flight.
  std::shared_ptr<State> state = state_;
  return a11::Submit<Batch>(
      [state = std::move(state), limit, timeout]() -> absl::StatusOr<Batch> {
        absl::StatusOr<State::NextResult> first = state->Next(timeout).Await();
        if (!first.ok()) {
          return first.status();
        }
        Batch batch;
        batch.push_back(std::move(*first));
        if (!batch.back().has_value()) {
          return batch;
        }
        for (data::NodeFragment& fragment : state->TakeBuffered(limit - 1)) {
          batch.emplace_back(std::move(fragment));
        }
        return batch;
      });
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
