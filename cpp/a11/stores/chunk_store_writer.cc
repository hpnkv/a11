// Copyright 2026 The A11 Authors.

#include "a11/stores/chunk_store_writer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/base/no_destructor.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/concurrency/callback_scheduler.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/wire_stream.h"
#include "a11/stores/chunk_store.h"
#include "thread/boost_primitives.h"

namespace a11::stores {
namespace {

void CompleteTask(const std::shared_ptr<a11::Promise<a11::Unit>>& promise,
                  const absl::Status& status) {
  if (promise == nullptr) {
    return;
  }
  if (status.ok()) {
    promise->SetValue(a11::Unit{}).IgnoreError();
  } else {
    promise->SetStatus(status).IgnoreError();
  }
}

}  // namespace

absl::Status ChunkStoreWriterOptions::Validate() const {
  constexpr std::uint64_t kMaximum = std::uint64_t{1} << 32U;
  if (max_chunks_to_write_at_once == 0) {
    return absl::InvalidArgumentError(
        "max_chunks_to_write_at_once must be positive");
  }
  if (max_chunks_to_write_at_once > kMaximum) {
    return absl::OutOfRangeError("max_chunks_to_write_at_once exceeds 2^32");
  }
  if (num_chunks_to_buffer.has_value() && *num_chunks_to_buffer == 0) {
    return absl::InvalidArgumentError("num_chunks_to_buffer must be positive");
  }
  if (num_chunks_to_buffer.has_value() && *num_chunks_to_buffer > kMaximum) {
    return absl::OutOfRangeError("num_chunks_to_buffer exceeds 2^32");
  }
  return absl::OkStatus();
}

struct ChunkStoreWriter::State
    : public std::enable_shared_from_this<ChunkStoreWriter::State> {
  struct Element {
    data::Chunk chunk;
    std::optional<std::uint32_t> seq;
    bool continued;
    std::shared_ptr<a11::Promise<a11::Unit>> admission;
    a11::Promise<std::uint32_t> result;

    Element(data::Chunk value, std::optional<std::uint32_t> sequence,
            bool is_continued,
            std::shared_ptr<a11::Promise<a11::Unit>> admission_promise,
            a11::Promise<std::uint32_t> promise)
        : chunk(std::move(value)),
          seq(sequence),
          continued(is_continued),
          admission(std::move(admission_promise)),
          result(std::move(promise)) {}

    Element(Element&&) noexcept = default;
    Element& operator=(Element&&) noexcept = default;
    Element(const Element&) = delete;
    Element& operator=(const Element&) = delete;
  };

  struct Batch {
    std::vector<Element> elements;
    std::string store_id;
    std::vector<std::shared_ptr<net::WireStream>> streams;
  };

  enum class Lifecycle { kNone, kClose, kAbort, kCancel };
  enum class Operation { kNone, kWrite, kClose };

  State(std::shared_ptr<ChunkStore> chunk_store,
        ChunkStoreWriterOptions writer_options)
      : store(std::move(chunk_store)),
        options(writer_options),
        next_offset_seq(writer_options.offset),
        next_sticky_seq(writer_options.offset) {}

  void ApplyStickyMimetypeLocked(data::Chunk& chunk, bool explicit_sequence_gap)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu) {
    const std::string mimetype = chunk.GetMimetype();
    if (explicit_sequence_gap || mimetype != current_mimetype) {
      current_mimetype = mimetype;
      return;
    }
    if (!chunk.metadata.has_value()) {
      return;
    }
    chunk.metadata->mimetype.clear();
    if (!chunk.metadata->timestamp.has_value() &&
        chunk.metadata->attributes.empty()) {
      chunk.metadata.reset();
    }
  }

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

  void Drive() {
    std::vector<Element> rejected;
    std::vector<Element> pending_rejected;
    std::vector<std::shared_ptr<a11::Promise<a11::Unit>>> completed_waiters;
    std::shared_ptr<a11::Promise<a11::Unit>> failed_lifecycle;
    std::shared_ptr<a11::Promise<a11::Unit>> cancelled_lifecycle;
    absl::Status rejected_status = absl::OkStatus();
    absl::Status lifecycle_status = absl::OkStatus();
    bool start_write = false;
    bool start_close = false;
    std::uint64_t generation = 0;
    absl::Status requested_close_status;
    {
      thread::MutexLock lock(&mu);
      queued = false;
      if (operation != Operation::kNone) {
        return;
      }

      if (stop_status.has_value() && !status.has_value()) {
        rejected_status = *stop_status;
        while (!queue.empty()) {
          rejected.push_back(std::move(queue.front()));
          queue.pop_front();
        }
        outstanding -= rejected.size();
        while (!pending_queue.empty()) {
          pending_rejected.push_back(std::move(pending_queue.front()));
          pending_queue.pop_front();
        }
        status = *stop_status;
      }

      if (status.has_value() && !status->ok() &&
          lifecycle == Lifecycle::kClose && !lifecycle_completed) {
        lifecycle_status = *status;
        failed_lifecycle = lifecycle_promise;
        lifecycle_completed = true;
        closing = false;
      }

      if (outstanding == 0 && !drain_waiters.empty()) {
        completed_waiters = std::move(drain_waiters);
        drain_waiters.clear();
      }

      if (!lifecycle_completed && lifecycle == Lifecycle::kCancel &&
          stop_status.has_value() && outstanding == 0) {
        cancelled_lifecycle = lifecycle_promise;
        lifecycle_completed = true;
        closing = false;
      } else if (!lifecycle_completed && lifecycle == Lifecycle::kAbort &&
                 stop_status.has_value() && outstanding == 0) {
        operation = Operation::kClose;
        generation = ++operation_generation;
        requested_close_status = *stop_status;
        start_close = true;
      } else if (!lifecycle_completed && lifecycle == Lifecycle::kClose &&
                 !status.has_value() && outstanding == 0) {
        operation = Operation::kClose;
        generation = ++operation_generation;
        requested_close_status = absl::OkStatus();
        start_close = true;
      } else if (!status.has_value() && !stop_status.has_value() &&
                 !queue.empty()) {
        const bool implicit = !queue.front().seq.has_value();
        std::vector<Element> elements;
        elements.reserve(options.max_chunks_to_write_at_once);
        while (!queue.empty() &&
               elements.size() < options.max_chunks_to_write_at_once &&
               (!queue.front().seq.has_value()) == implicit) {
          elements.push_back(std::move(queue.front()));
          queue.pop_front();
        }
        in_flight_batch.emplace(Batch{
            .elements = std::move(elements),
            .store_id = {},
            .streams = attached_streams,
        });
        operation = Operation::kWrite;
        generation = ++operation_generation;
        start_write = true;
      }
    }

    CompleteElements(std::move(rejected), {}, rejected_status);
    CompleteElements(std::move(pending_rejected), {}, rejected_status);
    for (const auto& waiter : completed_waiters) {
      CompleteTask(waiter, rejected_status);
    }
    CompleteTask(failed_lifecycle, lifecycle_status);
    CompleteTask(cancelled_lifecycle, absl::OkStatus());

    if (start_write) {
      StartWrite(generation);
    } else if (start_close) {
      StartClose(generation, std::move(requested_close_status));
    }
  }

  void StartWrite(std::uint64_t generation) {
    absl::StatusOr<std::string> id;
    try {
      id = store->GetId();
    } catch (const std::exception& error) {
      id = absl::UnknownError(error.what());
    } catch (...) {
      id = absl::UnknownError(
          "ChunkStore GetId raised a non-standard exception");
    }
    if (!id.ok()) {
      InstallWrite(a11::FailedFuture<std::vector<std::uint32_t>>(id.status()),
                   generation);
      return;
    }

    std::vector<data::NodeFragment> fragments;
    // The batch is exclusively owned by this state-machine operation until its
    // completion callback moves it out under mu.
    Batch& batch = *in_flight_batch;
    batch.store_id = std::move(*id);
    fragments.reserve(batch.elements.size());
    for (Element& element : batch.elements) {
      data::Chunk chunk =
          batch.streams.empty() ? std::move(element.chunk) : element.chunk;
      fragments.push_back(data::NodeFragment{
          .id = batch.store_id,
          .data = std::move(chunk),
          .seq = element.seq,
          .continued = element.continued,
      });
    }

    a11::Future<std::vector<std::uint32_t>> pending;
    try {
      pending = store->PutMany(std::move(fragments));
    } catch (const std::exception& error) {
      pending = a11::FailedFuture<std::vector<std::uint32_t>>(
          absl::UnknownError(error.what()));
    } catch (...) {
      pending =
          a11::FailedFuture<std::vector<std::uint32_t>>(absl::UnknownError(
              "ChunkStore PutMany raised a non-standard exception"));
    }
    InstallWrite(std::move(pending), generation);
  }

  void InstallWrite(a11::Future<std::vector<std::uint32_t>> pending,
                    std::uint64_t generation) {
    bool cancel = false;
    {
      thread::MutexLock lock(&mu);
      if (operation != Operation::kWrite ||
          operation_generation != generation) {
        cancel = true;
      } else {
        active_write = pending;
        cancel = stop_status.has_value();
      }
    }
    pending.OnReady(
        [self = shared_from_this(),
         generation](const absl::StatusOr<std::vector<std::uint32_t>>& result) {
          self->WriteDone(generation, result);
        });
    if (cancel) {
      (void)pending.Cancel();
    }
  }

  void WriteDone(std::uint64_t generation,
                 const absl::StatusOr<std::vector<std::uint32_t>>& result) {
    {
      thread::MutexLock lock(&mu);
      if (operation != Operation::kWrite ||
          operation_generation != generation) {
        return;
      }
    }

    absl::Status operation_status = result.status();
    Batch& active_batch = *in_flight_batch;
    if (result.ok()) {
      if (result->size() != active_batch.elements.size()) {
        operation_status = absl::DataLossError(
            "ChunkStore PutMany returned the wrong number of sequences");
      } else {
        for (size_t index = 0; index < active_batch.elements.size(); ++index) {
          if (active_batch.elements[index].seq.has_value() &&
              active_batch.elements[index].seq != (*result)[index]) {
            operation_status = absl::DataLossError(
                "ChunkStore changed an explicit sequence number");
            break;
          }
        }
      }
    }

    absl::Status tee_status;
    if (operation_status.ok() && !active_batch.streams.empty()) {
      data::WireMessage message;
      message.node_fragments.reserve(active_batch.elements.size());
      for (size_t index = 0; index < active_batch.elements.size(); ++index) {
        message.node_fragments.push_back(data::NodeFragment{
            .id = active_batch.store_id,
            .data = active_batch.elements[index].chunk,
            .seq = (*result)[index],
            .continued = active_batch.elements[index].continued,
        });
      }
      for (const std::shared_ptr<net::WireStream>& stream :
           active_batch.streams) {
        try {
          tee_status = stream->Send(message);
        } catch (const std::exception& error) {
          tee_status = absl::UnknownError(error.what());
        } catch (...) {
          tee_status = absl::UnknownError(
              "WireStream Send raised a non-standard exception");
        }
        if (!tee_status.ok()) {
          break;
        }
      }
    }

    std::vector<Element> completed;
    std::vector<Element> rejected;
    std::vector<Element> pending_rejected;
    std::vector<std::shared_ptr<a11::Promise<a11::Unit>>> admitted;
    std::vector<std::shared_ptr<a11::Promise<a11::Unit>>> completed_waiters;
    std::shared_ptr<a11::Promise<a11::Unit>> failed_lifecycle;
    std::optional<absl::Status> stop;
    absl::Status later_status =
        operation_status.ok() ? tee_status : operation_status;
    {
      thread::MutexLock lock(&mu);
      if (operation != Operation::kWrite ||
          operation_generation != generation) {
        return;
      }
      stop = stop_status;
      completed = std::move(in_flight_batch->elements);
      in_flight_batch.reset();
      active_write = {};
      operation = Operation::kNone;
      outstanding -= completed.size();

      if (stop.has_value()) {
        status = *stop;
      } else if (!later_status.ok()) {
        status = later_status;
      }
      if (stop.has_value() || !later_status.ok()) {
        const size_t rejected_admitted = queue.size();
        while (!queue.empty()) {
          rejected.push_back(std::move(queue.front()));
          queue.pop_front();
        }
        outstanding -= rejected_admitted;
        while (!pending_queue.empty()) {
          pending_rejected.push_back(std::move(pending_queue.front()));
          pending_queue.pop_front();
        }
      } else {
        AdmitPendingLocked(&admitted);
      }

      if (outstanding == 0 && !drain_waiters.empty()) {
        completed_waiters = std::move(drain_waiters);
        drain_waiters.clear();
      }
      if (!stop.has_value() && !later_status.ok() &&
          lifecycle == Lifecycle::kClose && !lifecycle_completed) {
        failed_lifecycle = lifecycle_promise;
        lifecycle_completed = true;
        closing = false;
      }
    }

    // A tee error occurs after durable store confirmation. It fails later
    // writes, while the current batch still receives its sequence values.
    const absl::Status completion_status = stop.value_or(operation_status);
    const absl::Status remaining_status = stop.value_or(later_status);
    std::vector<std::uint32_t> sequences;
    if (result.ok()) {
      sequences = *result;
    }
    CompleteElements(std::move(completed), std::move(sequences),
                     completion_status);
    CompleteElements(std::move(rejected), {}, remaining_status);
    CompleteElements(std::move(pending_rejected), {}, remaining_status);
    for (const auto& admission : admitted) {
      CompleteTask(admission, absl::OkStatus());
    }
    for (const auto& waiter : completed_waiters) {
      CompleteTask(waiter, remaining_status);
    }
    CompleteTask(failed_lifecycle, remaining_status);
    Wake();
  }

  // Closing a writer is a lifecycle fact bound peers cannot otherwise observe:
  // a remote reader ends a node on a not-continued fragment, and closing
  // appends none. The graceful path therefore tees one closure marker -- a
  // status chunk carrying data::kCloseAttribute -- so a mirror on the far side
  // closes its own write half. Draining is already synchronised with the tee:
  // the close operation only starts once every batch has been sent, so the
  // marker is the last thing a peer sees. The abort path sends nothing here;
  // Action::SendNodeAbortStatuses already fans failures out.
  absl::Status TeeClose(const absl::Status& close_status) {
    std::vector<std::shared_ptr<net::WireStream>> streams;
    {
      thread::MutexLock lock(&mu);
      if (lifecycle != Lifecycle::kClose) {
        return absl::OkStatus();
      }
      streams = attached_streams;
    }
    if (streams.empty()) {
      return absl::OkStatus();
    }

    absl::StatusOr<std::string> id;
    try {
      id = store->GetId();
    } catch (const std::exception& error) {
      id = absl::UnknownError(error.what());
    } catch (...) {
      id = absl::UnknownError(
          "ChunkStore GetId raised a non-standard exception");
    }
    if (!id.ok()) {
      return id.status();
    }
    absl::StatusOr<data::Chunk> chunk =
        data::MakeStatusChunk(close_status, true);
    if (!chunk.ok()) {
      return chunk.status();
    }
    data::WireMessage message;
    message.node_fragments.push_back(data::NodeFragment{
        .id = *std::move(id),
        .data = *std::move(chunk),
        .seq = 0,
        .continued = false,
    });
    for (const std::shared_ptr<net::WireStream>& stream : streams) {
      absl::Status sent;
      try {
        sent = stream->Send(message);
      } catch (const std::exception& error) {
        sent = absl::UnknownError(error.what());
      } catch (...) {
        sent = absl::UnknownError(
            "WireStream Send raised a non-standard exception");
      }
      if (!sent.ok()) {
        return sent;
      }
    }
    return absl::OkStatus();
  }

  void StartClose(std::uint64_t generation, absl::Status requested_status) {
    absl::Status tee_status = TeeClose(requested_status);
    a11::Future<absl::Status> pending;
    try {
      pending = store->CloseWritesWithStatus(requested_status);
    } catch (const std::exception& error) {
      pending =
          a11::FailedFuture<absl::Status>(absl::UnknownError(error.what()));
    } catch (...) {
      pending = a11::FailedFuture<absl::Status>(absl::UnknownError(
          "ChunkStore close raised a non-standard exception"));
    }
    bool cancel = false;
    {
      thread::MutexLock lock(&mu);
      if (operation != Operation::kClose ||
          operation_generation != generation) {
        cancel = true;
      } else {
        active_close = pending;
      }
    }
    pending.OnReady([self = shared_from_this(), generation,
                     requested_status = std::move(requested_status),
                     tee_status = std::move(tee_status)](
                        const absl::StatusOr<absl::Status>& result) {
      self->CloseDone(generation, requested_status, tee_status, result);
    });
    if (cancel) {
      (void)pending.Cancel();
    }
  }

  void CloseDone(std::uint64_t generation, const absl::Status& requested_status,
                 const absl::Status& tee_status,
                 const absl::StatusOr<absl::Status>& result) {
    absl::Status operation_status = result.status();
    if (result.ok() && result->code() != requested_status.code()) {
      operation_status = absl::DataLossError(
          "ChunkStore closed with a different status than requested");
    }
    // A failed closure marker cannot un-close the store, exactly as a failed
    // data tee cannot revoke store confirmations. The store still closes and
    // the send error becomes the writer's terminal status, so the producer
    // learns its peer was never told.
    if (operation_status.ok()) {
      operation_status = tee_status;
    }

    std::shared_ptr<a11::Promise<a11::Unit>> promise;
    {
      thread::MutexLock lock(&mu);
      if (operation != Operation::kClose ||
          operation_generation != generation) {
        return;
      }
      active_close = {};
      operation = Operation::kNone;
      if (operation_status.ok()) {
        status = requested_status;
      } else if (lifecycle == Lifecycle::kClose) {
        status = operation_status;
      }
      closing = false;
      lifecycle_completed = true;
      promise = lifecycle_promise;
    }
    CompleteTask(promise, operation_status);
  }

  static void CompleteElements(std::vector<Element> elements,
                               std::vector<std::uint32_t> sequences,
                               const absl::Status& status) {
    if (status.ok() && sequences.size() != elements.size()) {
      const absl::Status mismatch = absl::InternalError(
          "Writer completion has the wrong number of sequences");
      for (Element& element : elements) {
        element.result.SetStatus(mismatch).IgnoreError();
      }
      return;
    }
    for (size_t index = 0; index < elements.size(); ++index) {
      if (!status.ok()) {
        CompleteTask(elements[index].admission, status);
      }
      if (status.ok()) {
        elements[index].result.SetValue(sequences[index]).IgnoreError();
      } else {
        elements[index].result.SetStatus(status).IgnoreError();
      }
    }
  }

  void AdmitPendingLocked(
      std::vector<std::shared_ptr<a11::Promise<a11::Unit>>>* admitted)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu) {
    while (!pending_queue.empty() &&
           (!options.num_chunks_to_buffer.has_value() ||
            outstanding < *options.num_chunks_to_buffer)) {
      Element element = std::move(pending_queue.front());
      pending_queue.pop_front();
      admitted->push_back(element.admission);
      queue.push_back(std::move(element));
      ++outstanding;
    }
  }

  const std::shared_ptr<ChunkStore> store;
  const ChunkStoreWriterOptions options;
  mutable thread::Mutex mu;
  std::uint64_t next_offset_seq ABSL_GUARDED_BY(mu);
  std::uint64_t next_sticky_seq ABSL_GUARDED_BY(mu);
  std::string current_mimetype ABSL_GUARDED_BY(mu);
  std::deque<Element> queue ABSL_GUARDED_BY(mu);
  std::deque<Element> pending_queue ABSL_GUARDED_BY(mu);
  size_t outstanding ABSL_GUARDED_BY(mu) = 0;
  std::optional<absl::Status> status ABSL_GUARDED_BY(mu);
  bool closing ABSL_GUARDED_BY(mu) = false;
  std::optional<absl::Status> stop_status ABSL_GUARDED_BY(mu);
  Lifecycle lifecycle ABSL_GUARDED_BY(mu) = Lifecycle::kNone;
  bool lifecycle_completed ABSL_GUARDED_BY(mu) = false;
  std::shared_ptr<a11::Promise<a11::Unit>> lifecycle_promise
      ABSL_GUARDED_BY(mu);
  a11::Task lifecycle_future ABSL_GUARDED_BY(mu);
  std::vector<std::shared_ptr<a11::Promise<a11::Unit>>> drain_waiters
      ABSL_GUARDED_BY(mu);
  std::vector<std::shared_ptr<net::WireStream>> attached_streams
      ABSL_GUARDED_BY(mu);
  bool queued ABSL_GUARDED_BY(mu) = false;
  Operation operation ABSL_GUARDED_BY(mu) = Operation::kNone;
  std::uint64_t operation_generation ABSL_GUARDED_BY(mu) = 0;
  a11::Future<std::vector<std::uint32_t>> active_write ABSL_GUARDED_BY(mu);
  a11::Future<absl::Status> active_close ABSL_GUARDED_BY(mu);

  // Only the current stackless state-machine turn and its one completion
  // callback access this value. operation_generation serialises those accesses;
  // API threads only inspect the guarded operation marker.
  std::optional<Batch> in_flight_batch;
};

absl::StatusOr<std::shared_ptr<ChunkStoreWriter>> ChunkStoreWriter::Create(
    std::shared_ptr<ChunkStore> store, ChunkStoreWriterOptions options) {
  if (store == nullptr) {
    return absl::InvalidArgumentError("store must not be null");
  }
  absl::Status status = options.Validate();
  if (!status.ok()) {
    return status;
  }

  struct MakeSharedEnabler final : ChunkStoreWriter {
    explicit MakeSharedEnabler(std::shared_ptr<State> state)
        : ChunkStoreWriter(std::move(state)) {}
  };

  return std::make_shared<MakeSharedEnabler>(
      std::make_shared<State>(std::move(store), options));
}

void ChunkStoreWriter::EnsureStarted() {
  state_->Wake();
}

ChunkStoreWrite ChunkStoreWriter::EnqueueChunk(data::Chunk chunk,
                                               std::optional<std::uint32_t> seq,
                                               bool final,
                                               bool ensure_started) {
  const auto failed_write = [](const absl::Status& status) {
    return ChunkStoreWrite{
        .admitted = a11::FailedTask(status),
        .confirmation = a11::FailedFuture<std::uint32_t>(status),
    };
  };

  if (const absl::Status validation = chunk.Validate(); !validation.ok()) {
    return failed_write(validation);
  }

  const auto admission = std::make_shared<a11::Promise<a11::Unit>>();
  a11::Task admitted = admission->future();
  a11::Promise<std::uint32_t> promise;
  a11::Future<std::uint32_t> confirmation = promise.future();
  bool admitted_immediately = false;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->status.has_value()) {
      return failed_write(
          state_->status->ok()
              ? absl::FailedPreconditionError("ChunkStoreWriter is closed")
              : *state_->status);
    }
    if (state_->closing) {
      return failed_write(state_->stop_status.value_or(
          absl::FailedPreconditionError("ChunkStoreWriter is closing")));
    }
    const std::optional<std::uint32_t> requested_seq = seq;
    if (!seq.has_value() && state_->options.offset != 0) {
      if (state_->next_offset_seq > std::numeric_limits<std::uint32_t>::max()) {
        return failed_write(absl::ResourceExhaustedError(
            "Maximum writer sequence number exceeded"));
      }
      seq = static_cast<std::uint32_t>(state_->next_offset_seq++);
    }
    if (state_->options.sticky_mimetype) {
      const bool explicit_sequence_gap =
          requested_seq.has_value() &&
          static_cast<std::uint64_t>(*requested_seq) != state_->next_sticky_seq;
      state_->ApplyStickyMimetypeLocked(chunk, explicit_sequence_gap);
      state_->next_sticky_seq =
          requested_seq.has_value()
              ? static_cast<std::uint64_t>(*requested_seq) + 1
              : state_->next_sticky_seq + 1;
    }
    State::Element element(std::move(chunk), seq, !final, admission,
                           std::move(promise));
    if (!state_->options.num_chunks_to_buffer.has_value() ||
        state_->outstanding < *state_->options.num_chunks_to_buffer) {
      state_->queue.push_back(std::move(element));
      ++state_->outstanding;
      admitted_immediately = true;
    } else {
      state_->pending_queue.push_back(std::move(element));
    }
  }
  if (admitted_immediately) {
    CompleteTask(admission, absl::OkStatus());
    if (ensure_started) {
      state_->Wake();
    }
  }
  return ChunkStoreWrite{
      .admitted = std::move(admitted),
      .confirmation = std::move(confirmation),
  };
}

a11::Future<std::uint32_t> ChunkStoreWriter::PutChunk(
    data::Chunk chunk, std::optional<std::uint32_t> seq, bool final) {
  return EnqueueChunk(std::move(chunk), seq, final, true).confirmation;
}

std::optional<absl::Status> ChunkStoreWriter::GetStatus() const {
  thread::MutexLock lock(&state_->mu);
  return state_->status;
}

std::optional<absl::Status> ChunkStoreWriter::GetAbortStatus() const {
  thread::MutexLock lock(&state_->mu);
  return state_->stop_status;
}

bool ChunkStoreWriter::IsWritable() const {
  thread::MutexLock lock(&state_->mu);
  return !state_->status.has_value() && !state_->closing;
}

a11::Task ChunkStoreWriter::Cancel() {
  a11::Task lifecycle;
  a11::Future<std::vector<std::uint32_t>> active;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->lifecycle != State::Lifecycle::kNone) {
      if (state_->lifecycle != State::Lifecycle::kCancel) {
        return a11::FailedTask(absl::FailedPreconditionError(
            "ChunkStoreWriter is already stopping"));
      }
      return state_->lifecycle_future;
    }
    if (state_->status.has_value()) {
      return a11::FailedTask(state_->status->ok()
                                 ? absl::FailedPreconditionError(
                                       "ChunkStoreWriter has already stopped")
                                 : *state_->status);
    }
    state_->closing = true;
    state_->stop_status = absl::AbortedError("ChunkStoreWriter was cancelled");
    state_->lifecycle = State::Lifecycle::kCancel;
    state_->lifecycle_promise = std::make_shared<a11::Promise<a11::Unit>>();
    state_->lifecycle_future = state_->lifecycle_promise->future();
    lifecycle = state_->lifecycle_future;
    active = state_->active_write;
  }
  if (active.valid()) {
    (void)active.Cancel();
  }
  state_->Wake();
  return lifecycle;
}

a11::Task ChunkStoreWriter::DrainAndClose() {
  a11::Task lifecycle;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->lifecycle != State::Lifecycle::kNone) {
      if (state_->lifecycle != State::Lifecycle::kClose) {
        return a11::FailedTask(absl::FailedPreconditionError(
            "ChunkStoreWriter is already being aborted"));
      }
      return state_->lifecycle_future;
    }
    if (state_->status.has_value()) {
      return a11::FailedTask(state_->status->ok()
                                 ? absl::FailedPreconditionError(
                                       "ChunkStoreWriter has already stopped")
                                 : *state_->status);
    }
    state_->closing = true;
    state_->lifecycle = State::Lifecycle::kClose;
    state_->lifecycle_promise = std::make_shared<a11::Promise<a11::Unit>>();
    state_->lifecycle_future = state_->lifecycle_promise->future();
    lifecycle = state_->lifecycle_future;
  }
  state_->Wake();
  return lifecycle;
}

a11::Task ChunkStoreWriter::AbortWithStatus(absl::Status status) {
  if (status.ok()) {
    return a11::FailedTask(
        absl::InvalidArgumentError("Abort status must be non-OK"));
  }
  a11::Task lifecycle;
  a11::Future<std::vector<std::uint32_t>> active;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->lifecycle != State::Lifecycle::kNone) {
      if (state_->lifecycle == State::Lifecycle::kAbort) {
        return state_->lifecycle_future;
      }
      const bool retry_failed_close =
          state_->lifecycle == State::Lifecycle::kClose &&
          state_->lifecycle_completed && state_->status.has_value() &&
          !state_->status->ok();
      if (!retry_failed_close) {
        return a11::FailedTask(absl::FailedPreconditionError(
            "ChunkStoreWriter is already being closed"));
      }
    } else if (state_->status.has_value()) {
      return a11::FailedTask(state_->status->ok()
                                 ? absl::FailedPreconditionError(
                                       "ChunkStoreWriter has already stopped")
                                 : *state_->status);
    }
    state_->closing = true;
    state_->stop_status = std::move(status);
    state_->lifecycle = State::Lifecycle::kAbort;
    state_->lifecycle_completed = false;
    state_->lifecycle_promise = std::make_shared<a11::Promise<a11::Unit>>();
    state_->lifecycle_future = state_->lifecycle_promise->future();
    lifecycle = state_->lifecycle_future;
    active = state_->active_write;
  }
  if (active.valid()) {
    active.Cancel().IgnoreError();
  }
  state_->Wake();
  return lifecycle;
}

a11::Task ChunkStoreWriter::WaitForBufferToDrain() {
  const auto promise = std::make_shared<a11::Promise<a11::Unit>>();
  a11::Task future = promise->future();
  std::optional<absl::Status> immediate;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->status.has_value() && !state_->status->ok()) {
      immediate = *state_->status;
    } else if (state_->outstanding == 0) {
      immediate = absl::OkStatus();
    } else {
      state_->drain_waiters.push_back(promise);
    }
  }
  if (immediate.has_value()) {
    CompleteTask(promise, *immediate);
  }
  return future;
}

absl::Status ChunkStoreWriter::AttachStream(
    std::shared_ptr<net::WireStream> stream) {
  if (stream == nullptr) {
    return absl::InvalidArgumentError("stream must not be null");
  }
  try {
    (void)stream->GetId();
  } catch (const std::exception& error) {
    return absl::UnknownError(error.what());
  } catch (...) {
    return absl::UnknownError("WireStream GetId raised an exception");
  }
  thread::MutexLock lock(&state_->mu);
  for (const auto& attached : state_->attached_streams) {
    if (attached == stream) {
      return absl::OkStatus();
    }
  }
  state_->attached_streams.push_back(std::move(stream));
  return absl::OkStatus();
}

absl::Status ChunkStoreWriter::DetachStream(
    const std::shared_ptr<net::WireStream>& stream) {
  if (stream == nullptr) {
    return absl::InvalidArgumentError("stream must not be null");
  }
  thread::MutexLock lock(&state_->mu);
  std::erase(state_->attached_streams, stream);
  return absl::OkStatus();
}

std::shared_ptr<ChunkStore> ChunkStoreWriter::store() const {
  return state_->store;
}

ChunkStoreWriterOptions ChunkStoreWriter::options() const {
  return state_->options;
}

size_t ChunkStoreWriter::queue_size() const {
  thread::MutexLock lock(&state_->mu);
  return state_->outstanding;
}

}  // namespace a11::stores
