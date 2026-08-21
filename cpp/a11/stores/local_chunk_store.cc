// Copyright 2026 The A11 Authors.

#include "a11/stores/local_chunk_store.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/stores/chunk_store.h"
#include "a11/stores/internal/chunk_store_common.h"
#include "thread/boost_primitives.h"
#include "thread/executor.h"
#include "thread/fiber.h"
#include "thread/select.h"
#include "thread/selectables.h"

namespace a11::stores {
namespace {

using internal::CompleteInline;
using internal::WaitForChange;

}  // namespace

struct LocalChunkStore::State
    : public std::enable_shared_from_this<LocalChunkStore::State> {
  enum class ReadKind { kSequence, kArrivalOrder };

  struct ReadRequest {
    ReadRequest(ReadKind read_kind, std::uint64_t read_value)
        : kind(read_kind), value(read_value) {}

    [[nodiscard]] bool TryClaim() {
      return !completed.exchange(true, std::memory_order_acq_rel);
    }

    ReadKind kind;
    std::uint64_t value;
    std::atomic<bool> completed = false;
    a11::Promise<data::NodeFragment> promise;
  };

  struct ReadCompletion {
    std::shared_ptr<ReadRequest> request;
    absl::StatusOr<data::NodeFragment> result;
  };

  explicit State(std::string id) : node_id(std::move(id)) {}

  a11::Future<data::NodeFragment> Read(ReadKind kind, std::uint64_t value,
                                       absl::Time deadline) {
    auto request = std::make_shared<ReadRequest>(kind, value);
    a11::Future<data::NodeFragment> future = request->promise.future();
    const std::weak_ptr<State> weak_state = weak_from_this();
    const std::weak_ptr<ReadRequest> weak_request = request;
    const absl::Status callback_status =
        request->promise.SetCancellationCallback([weak_state, weak_request] {
          const std::shared_ptr<ReadRequest> pending = weak_request.lock();
          if (pending == nullptr || !pending->TryClaim()) {
            return;
          }
          pending->promise
              .SetStatus(absl::CancelledError("Chunk store read was cancelled"))
              .IgnoreError();
          const std::shared_ptr<State> state = weak_state.lock();
          if (state != nullptr) {
            state->RemoveRead(pending.get());
          }
        });
    callback_status.IgnoreError();

    std::vector<ReadCompletion> completions;
    {
      thread::MutexLock lock(&mu);
      std::optional<absl::StatusOr<data::NodeFragment>> result =
          LookupReadLocked(*request);
      if (result.has_value() && request->TryClaim()) {
        completions.push_back(ReadCompletion{
            .request = request,
            .result = std::move(*result),
        });
      } else {
        pending_reads.push_back(request);
      }
    }
    CompleteReads(std::move(completions));

    if (!request->completed.load(std::memory_order_acquire) &&
        deadline != absl::InfiniteFuture()) {
      thread::PostAt(deadline, [weak_state, weak_request] {
        const std::shared_ptr<ReadRequest> pending = weak_request.lock();
        if (pending == nullptr || !pending->TryClaim()) {
          return;
        }
        pending->promise
            .SetStatus(absl::DeadlineExceededError(
                "Chunk store fragment was not available before the deadline"))
            .IgnoreError();
        const std::shared_ptr<State> state = weak_state.lock();
        if (state != nullptr) {
          state->RemoveRead(pending.get());
        }
      });
    }
    return future;
  }

  std::optional<absl::StatusOr<data::NodeFragment>> LookupReadLocked(
      const ReadRequest& request) const ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu) {
    std::optional<std::uint32_t> seq;
    if (request.kind == ReadKind::kSequence) {
      const std::uint32_t requested_seq =
          static_cast<std::uint32_t>(request.value);
      if (chunks.find(requested_seq) != chunks.end()) {
        seq = requested_seq;
      }
    } else {
      const auto arrival = arrival_order_to_seq.find(request.value);
      if (arrival != arrival_order_to_seq.end()) {
        seq = arrival->second;
        if (chunks.find(*seq) == chunks.end()) {
          return absl::DataLossError(
              "Chunk store index references a missing chunk");
        }
      }
    }

    if (seq.has_value()) {
      const auto chunk = chunks.find(*seq);
      return data::NodeFragment{
          .id = node_id,
          .data = chunk->second,
          .seq = *seq,
          .continued = !final_seq.has_value() || *seq < *final_seq,
      };
    }
    if (!status.has_value()) {
      return std::nullopt;
    }
    if (!status->ok()) {
      return *status;
    }
    return absl::NotFoundError(
        request.kind == ReadKind::kSequence
            ? absl::StrCat("Chunk store closed without seq ", request.value)
            : absl::StrCat("Chunk store closed without arrival order ",
                           request.value));
  }

  void CollectReadsLocked(std::vector<ReadCompletion>* completions)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu) {
    for (auto request = pending_reads.begin();
         request != pending_reads.end();) {
      if ((*request)->completed.load(std::memory_order_acquire)) {
        request = pending_reads.erase(request);
        continue;
      }
      std::optional<absl::StatusOr<data::NodeFragment>> result =
          LookupReadLocked(**request);
      if (!result.has_value() || !(*request)->TryClaim()) {
        ++request;
        continue;
      }
      completions->push_back(ReadCompletion{
          .request = *request,
          .result = std::move(*result),
      });
      request = pending_reads.erase(request);
    }
  }

  void RemoveRead(const ReadRequest* absl_nonnull request) {
    thread::MutexLock lock(&mu);
    std::erase_if(pending_reads,
                  [request](const std::shared_ptr<ReadRequest>& pending) {
                    return pending.get() == request;
                  });
  }

  static void CompleteReads(std::vector<ReadCompletion> completions) {
    for (ReadCompletion& completion : completions) {
      completion.request->promise.SetResult(std::move(completion.result))
          .IgnoreError();
    }
  }

  /**
   * @brief
   *   Append whatever `Next` can return right now, without ever blocking.
   *
   * Split out of Next() so the common case -- the fragments are already here --
   * can run on the caller's thread. Wrapping the whole loop in a Submit() would
   * spawn a fiber even when nothing needs waiting for, and a fiber spawn costs
   * more than an order of magnitude what the read itself does.
   *
   * @param fragments
   *   Accumulator, appended to. Carries across waits, so a partial batch
   *   collected before a wait is still there after it.
   * @param limit
   *   Maximum fragments to accumulate, counting what is already there.
   * @param waiter
   *   Set to the generation event only when the caller must wait. It is
   *   snapshotted *inside* the lock, which is what closes the lost-wakeup
   *   window -- see WaitForChange().
   * @return
   *   An engaged status when the call has its answer: OK means `fragments` is
   *   that answer, and a non-OK status is the terminal one to fail with.
   *   `std::nullopt` means the caller must wait on `*waiter` and try again.
   */
  std::optional<absl::Status> CollectNext(
      std::vector<std::optional<data::NodeFragment>>& fragments, size_t limit,
      std::shared_ptr<thread::PermanentEvent>* waiter)
      ABSL_LOCKS_EXCLUDED(mu) {
    thread::MutexLock lock(&mu);
    while (true) {
      if (total_chunks_read > std::numeric_limits<std::uint32_t>::max()) {
        fragments.emplace_back(std::nullopt);
        return absl::OkStatus();
      }
      const auto expected_seq = static_cast<std::uint32_t>(total_chunks_read);
      const bool final_was_read =
          final_seq.has_value() && total_chunks_read > *final_seq;
      if (final_was_read) {
        if (status.has_value() && !status->ok()) {
          if (!fragments.empty()) {
            return absl::OkStatus();
          }
          return *status;
        }
        fragments.emplace_back(std::nullopt);
        return absl::OkStatus();
      }

      const auto found = chunks.find(expected_seq);
      if (found == chunks.end() && status.has_value()) {
        if (status->ok()) {
          fragments.emplace_back(std::nullopt);
          return absl::OkStatus();
        }
        if (!fragments.empty()) {
          return absl::OkStatus();
        }
        return *status;
      }
      if (fragments.size() == limit) {
        return absl::OkStatus();
      }
      if (found == chunks.end()) {
        *waiter = changed;
        return std::nullopt;
      }
      ++total_chunks_read;
      fragments.emplace_back(data::NodeFragment{
          .id = node_id,
          .data = found->second,
          .seq = expected_seq,
          .continued = !final_seq.has_value() || expected_seq < *final_seq,
      });
    }
  }

  thread::Mutex mu;
  const std::string node_id;
  absl::flat_hash_map<std::uint32_t, std::uint64_t> seq_to_arrival_order
      ABSL_GUARDED_BY(mu);
  absl::flat_hash_map<std::uint64_t, std::uint32_t> arrival_order_to_seq
      ABSL_GUARDED_BY(mu);
  absl::flat_hash_map<std::uint32_t, data::Chunk> chunks ABSL_GUARDED_BY(mu);
  std::optional<std::uint32_t> final_seq ABSL_GUARDED_BY(mu);
  std::uint64_t total_chunks_put ABSL_GUARDED_BY(mu) = 0;
  std::uint64_t total_chunks_read ABSL_GUARDED_BY(mu) = 0;
  std::optional<absl::Status> status ABSL_GUARDED_BY(mu);
  std::vector<std::shared_ptr<ReadRequest>> pending_reads ABSL_GUARDED_BY(mu);
  std::shared_ptr<thread::PermanentEvent> changed ABSL_GUARDED_BY(mu) =
      std::make_shared<thread::PermanentEvent>();
};

absl::StatusOr<std::shared_ptr<LocalChunkStore>> LocalChunkStore::Create(
    std::string node_id) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(node_id));
  return std::make_shared<LocalChunkStore>(
      ConstructorToken{}, std::make_shared<State>(std::move(node_id)));
}

a11::Future<data::NodeFragment> LocalChunkStore::Get(std::uint32_t seq,
                                                     absl::Time deadline) {
  return state_->Read(State::ReadKind::kSequence, seq, deadline);
}

a11::Future<data::NodeFragment> LocalChunkStore::GetByArrivalOrder(
    std::uint64_t arrival_order, absl::Time deadline) {
  return state_->Read(State::ReadKind::kArrivalOrder, arrival_order, deadline);
}

a11::Future<std::vector<std::optional<data::NodeFragment>>>
LocalChunkStore::Next(absl::Time deadline, size_t limit) {
  if (limit == 0) {
    return a11::FailedFuture<std::vector<std::optional<data::NodeFragment>>>(
        absl::InvalidArgumentError("limit must be positive"));
  }
  std::shared_ptr<State> state = state_;

  // The overwhelmingly common case is that the fragments are already here, and
  // it is answered on the caller's thread. Only a call that genuinely has to
  // wait pays for a fiber.
  std::vector<std::optional<data::NodeFragment>> fragments;
  fragments.reserve(limit + 1);
  std::shared_ptr<thread::PermanentEvent> changed;
  std::optional<absl::Status> settled =
      state->CollectNext(fragments, limit, &changed);
  if (settled.has_value()) {
    if (!settled->ok()) {
      return a11::FailedFuture<std::vector<std::optional<data::NodeFragment>>>(
          *settled);
    }
    return a11::CompletedFuture<std::vector<std::optional<data::NodeFragment>>>(
        std::move(fragments));
  }

  return a11::Submit<std::vector<std::optional<data::NodeFragment>>>(
      [state = std::move(state), deadline, limit,
       fragments = std::move(fragments), changed = std::move(changed)]() mutable
          -> absl::StatusOr<std::vector<std::optional<data::NodeFragment>>> {
        while (true) {
          absl::Status wait = WaitForChange(
              changed, deadline,
              absl::StrCat("Expected seq was not available before the "
                           "deadline"));
          if (!wait.ok()) {
            if (!fragments.empty()) {
              return fragments;
            }
            return wait;
          }
          std::optional<absl::Status> settled =
              state->CollectNext(fragments, limit, &changed);
          if (settled.has_value()) {
            if (!settled->ok()) {
              return *settled;
            }
            return fragments;
          }
        }
      });
}

a11::Future<std::uint32_t> LocalChunkStore::Put(data::NodeFragment fragment) {
  return internal::PutOneViaPutMany(
      [this](std::vector<data::NodeFragment> batch) {
        return PutMany(std::move(batch));
      },
      std::move(fragment), "LocalChunkStore");
}

a11::Future<std::vector<std::uint32_t>> LocalChunkStore::PutMany(
    std::vector<data::NodeFragment> fragments) {
  std::shared_ptr<State> state = state_;
  return CompleteInline<std::vector<std::uint32_t>>(
      [state = std::move(state), fragments = std::move(fragments)]() mutable
          -> absl::StatusOr<std::vector<std::uint32_t>> {
        bool any_explicit = false;
        bool all_explicit = true;
        absl::flat_hash_set<std::uint32_t> explicit_sequences;
        for (const data::NodeFragment& fragment : fragments) {
          ABSL_RETURN_IF_ERROR(fragment.Validate());
          any_explicit = any_explicit || fragment.seq.has_value();
          all_explicit = all_explicit && fragment.seq.has_value();
          if (fragment.seq.has_value() &&
              !explicit_sequences.insert(*fragment.seq).second) {
            return absl::InvalidArgumentError(absl::StrCat(
                "Explicit seq ", *fragment.seq, " occurs more than once"));
          }
          if (!std::holds_alternative<data::Chunk>(fragment.data)) {
            return absl::UnimplementedError(
                "LocalChunkStore supports Chunk payloads, not NodeRef");
          }
        }
        if (any_explicit != all_explicit) {
          return absl::InvalidArgumentError(
              "Sequence numbers must be set on every fragment or none");
        }

        std::vector<std::uint32_t> assigned;
        std::vector<State::ReadCompletion> read_completions;
        std::shared_ptr<thread::PermanentEvent> notify;
        {
          thread::MutexLock lock(&state->mu);
          if (state->status.has_value()) {
            return absl::FailedPreconditionError(absl::StrCat(
                "Chunk store ", state->node_id, " is closed for writes"));
          }
          if (fragments.empty()) {
            return std::vector<std::uint32_t>{};
          }

          assigned.reserve(fragments.size());
          if (all_explicit) {
            for (const data::NodeFragment& fragment : fragments) {
              assigned.push_back(*fragment.seq);
            }
          } else {
            std::uint64_t candidate = state->total_chunks_put;
            for (size_t index = 0; index < fragments.size(); ++index) {
              while (candidate <= std::numeric_limits<std::uint32_t>::max() &&
                     state->chunks.find(static_cast<std::uint32_t>(
                         candidate)) != state->chunks.end()) {
                ++candidate;
              }
              if (candidate > std::numeric_limits<std::uint32_t>::max()) {
                return absl::ResourceExhaustedError(
                    "Maximum implicit sequence number exceeded");
              }
              assigned.push_back(static_cast<std::uint32_t>(candidate++));
            }
          }
          for (std::uint32_t seq : assigned) {
            if (state->chunks.find(seq) != state->chunks.end()) {
              return absl::AlreadyExistsError(
                  absl::StrCat("A fragment with seq ", seq, " already exists"));
            }
          }

          std::optional<std::uint32_t> batch_final;
          bool saw_final = false;
          for (size_t index = 0; index < fragments.size(); ++index) {
            if (fragments[index].continued) {
              if (saw_final && !all_explicit) {
                return absl::InvalidArgumentError(
                    "The final implicit fragment must be last");
              }
              continue;
            }
            if (saw_final) {
              return absl::InvalidArgumentError(
                  "More than one fragment in the batch is marked final");
            }
            saw_final = true;
            batch_final = assigned[index];
          }
          if (batch_final.has_value() && state->final_seq.has_value() &&
              batch_final != state->final_seq) {
            return absl::FailedPreconditionError(
                "The chunk store already has a different final sequence");
          }
          const std::optional<std::uint32_t> pending_final =
              batch_final.has_value() ? batch_final : state->final_seq;
          if (pending_final.has_value()) {
            for (std::uint32_t seq : assigned) {
              if (seq > *pending_final) {
                return absl::InvalidArgumentError(
                    "A fragment sequence exceeds the final sequence");
              }
            }
            const bool has_later =
                std::any_of(state->chunks.begin(), state->chunks.end(),
                            [pending_final](const auto& entry) {
                              return entry.first > *pending_final;
                            });
            if (has_later) {
              return absl::InvalidArgumentError(
                  "An existing fragment exceeds the proposed final sequence");
            }
          }

          // Stage all values and reserve the destination tables before
          // publishing the batch. This keeps the commit compact and avoids
          // growth allocations once mutation begins.
          absl::flat_hash_map<std::uint32_t, data::Chunk> chunk_nodes;
          absl::flat_hash_map<std::uint64_t, std::uint32_t> arrival_nodes;
          absl::flat_hash_map<std::uint32_t, std::uint64_t> reverse_nodes;
          for (size_t index = 0; index < fragments.size(); ++index) {
            const std::uint64_t arrival = state->total_chunks_put + index;
            chunk_nodes.emplace(
                assigned[index],
                std::get<data::Chunk>(std::move(fragments[index].data)));
            arrival_nodes.emplace(arrival, assigned[index]);
            reverse_nodes.emplace(assigned[index], arrival);
          }
          state->chunks.reserve(state->chunks.size() + chunk_nodes.size());
          state->arrival_order_to_seq.reserve(
              state->arrival_order_to_seq.size() + arrival_nodes.size());
          state->seq_to_arrival_order.reserve(
              state->seq_to_arrival_order.size() + reverse_nodes.size());
          state->chunks.merge(chunk_nodes);
          state->arrival_order_to_seq.merge(arrival_nodes);
          state->seq_to_arrival_order.merge(reverse_nodes);
          state->total_chunks_put += fragments.size();
          state->final_seq = pending_final;
          notify = std::exchange(state->changed,
                                 std::make_shared<thread::PermanentEvent>());
          state->CollectReadsLocked(&read_completions);
        }
        State::CompleteReads(std::move(read_completions));
        notify->Notify();
        return assigned;
      });
}

a11::Future<data::NodeFragment> LocalChunkStore::ClearData(std::uint32_t seq) {
  std::shared_ptr<State> state = state_;
  return CompleteInline<data::NodeFragment>(
      [state = std::move(state), seq]() -> absl::StatusOr<data::NodeFragment> {
        thread::MutexLock lock(&state->mu);
        const auto found = state->chunks.find(seq);
        if (found == state->chunks.end()) {
          return absl::NotFoundError(
              absl::StrCat("No fragment with seq ", seq, " exists"));
        }
        data::Chunk original = found->second;
        found->second.data.clear();
        found->second.ref = "__tombstone__";
        return data::NodeFragment{
            .id = state->node_id,
            .data = std::move(original),
            .seq = seq,
            .continued =
                !state->final_seq.has_value() || seq < *state->final_seq,
        };
      });
}

a11::Future<std::uint32_t> LocalChunkStore::GetSeqForArrivalOrder(
    std::uint64_t arrival_order) {
  std::shared_ptr<State> state = state_;
  return CompleteInline<std::uint32_t>(
      [state = std::move(state),
       arrival_order]() -> absl::StatusOr<std::uint32_t> {
        thread::MutexLock lock(&state->mu);
        const auto found = state->arrival_order_to_seq.find(arrival_order);
        if (found == state->arrival_order_to_seq.end()) {
          return absl::NotFoundError(
              absl::StrCat("No fragment has arrival order ", arrival_order));
        }
        return found->second;
      });
}

a11::Future<std::optional<std::uint32_t>> LocalChunkStore::GetFinalSeq() {
  std::shared_ptr<State> state = state_;
  return CompleteInline<std::optional<std::uint32_t>>(
      [state =
           std::move(state)]() -> absl::StatusOr<std::optional<std::uint32_t>> {
        thread::MutexLock lock(&state->mu);
        return state->final_seq;
      });
}

a11::Future<absl::Status> LocalChunkStore::CloseWritesWithStatus(
    absl::Status status, bool return_status_if_already_closed) {
  std::shared_ptr<State> state = state_;
  return CompleteInline<absl::Status>(
      [state = std::move(state), status = std::move(status),
       return_status_if_already_closed]() mutable
          -> absl::StatusOr<absl::Status> {
        absl::Status published;
        std::vector<State::ReadCompletion> read_completions;
        std::shared_ptr<thread::PermanentEvent> notify;
        {
          thread::MutexLock lock(&state->mu);
          if (state->status.has_value()) {
            if (return_status_if_already_closed) {
              return absl::StatusOr<absl::Status>(std::in_place,
                                                  *state->status);
            }
            absl::StatusOr<absl::Status> result;
            result.AssignStatus(absl::FailedPreconditionError(
                "Chunk store is already closed for writes"));
            return result;
          }
          state->status = std::move(status);
          published = *state->status;
          notify = std::exchange(state->changed,
                                 std::make_shared<thread::PermanentEvent>());
          state->CollectReadsLocked(&read_completions);
        }
        State::CompleteReads(std::move(read_completions));
        notify->Notify();
        return absl::StatusOr<absl::Status>(std::in_place,
                                            std::move(published));
      });
}

a11::Future<size_t> LocalChunkStore::Size() {
  std::shared_ptr<State> state = state_;
  return CompleteInline<size_t>(
      [state = std::move(state)]() -> absl::StatusOr<size_t> {
        thread::MutexLock lock(&state->mu);
        return state->chunks.size();
      });
}

absl::StatusOr<std::string> LocalChunkStore::GetId() const {
  return state_->node_id;
}

}  // namespace a11::stores
