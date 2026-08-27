// Copyright 2026 The A11 Authors.

#include "a11/nodes/async_node.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/stores/chunk_store.h"
#include "a11/stores/chunk_store_reader.h"
#include "a11/stores/chunk_store_writer.h"
#include "thread/boost_primitives.h"

namespace a11::nodes {
namespace {

std::shared_ptr<data::SerializationRegistry> GlobalRegistryPointer() {
  return {&data::GlobalSerializationRegistry(),
          [](data::SerializationRegistry*) {}};
}

/// The terminator Finalize() writes when the producer has no last value.
data::Chunk NullChunk() {
  return data::Chunk{
      .metadata = data::ChunkMetadata{.mimetype = "application/octet-stream"}};
}

// A producer that does not wait is not a producer that does not care. Nobody
// holds the awaitable, so a rejected write or a store that refuses to close
// would otherwise be visible only to whoever thinks to read GetWriterStatus().
template <typename T>
void LogIfFailed(const a11::Future<T>& pending, std::string_view what) {
  pending.OnReady([what](const absl::StatusOr<T>& result) {
    if (!result.ok()) {
      LOG(WARNING) << "AsyncNode " << what << " failed: " << result.status();
    }
  });
}

}  // namespace

absl::StatusOr<std::shared_ptr<AsyncNode>> AsyncNode::Create(
    std::shared_ptr<stores::ChunkStore> store,
    std::shared_ptr<data::SerializationRegistry> serialization_registry,
    stores::ChunkStoreReaderOptions reader_options,
    stores::ChunkStoreWriterOptions writer_options) {
  if (store == nullptr) {
    return absl::InvalidArgumentError("store must not be null");
  }
  ABSL_RETURN_IF_ERROR(reader_options.Validate());
  ABSL_RETURN_IF_ERROR(writer_options.Validate());
  if (serialization_registry == nullptr) {
    serialization_registry = GlobalRegistryPointer();
  }

  struct MakeSharedEnabler final : AsyncNode {
    MakeSharedEnabler(
        std::shared_ptr<stores::ChunkStore> store,
        std::shared_ptr<data::SerializationRegistry> serialization_registry,
        stores::ChunkStoreReaderOptions reader_options,
        stores::ChunkStoreWriterOptions writer_options)
        : AsyncNode(std::move(store), std::move(serialization_registry),
                    reader_options, writer_options) {}
  };

  return std::make_shared<MakeSharedEnabler>(std::move(store),
                                             std::move(serialization_registry),
                                             reader_options, writer_options);
}

absl::StatusOr<std::string> AsyncNode::GetId() const {
  return store_->GetId();
}

std::shared_ptr<stores::ChunkStore> AsyncNode::GetChunkStore() const {
  return store_;
}

std::shared_ptr<data::SerializationRegistry> AsyncNode::serialization_registry()
    const {
  thread::MutexLock lock(&mu_);
  return serialization_registry_;
}

absl::Status AsyncNode::SetSerializationRegistry(
    std::shared_ptr<data::SerializationRegistry> registry) {
  if (registry == nullptr) {
    return absl::InvalidArgumentError("registry must not be null");
  }
  thread::MutexLock lock(&mu_);
  serialization_registry_ = std::move(registry);
  return absl::OkStatus();
}

absl::StatusOr<std::shared_ptr<stores::ChunkStoreReader>> AsyncNode::reader() {
  thread::MutexLock lock(&mu_);
  if (reader_ == nullptr) {
    absl::StatusOr<std::shared_ptr<stores::ChunkStoreReader>> created =
        stores::ChunkStoreReader::Create(store_, reader_options_);
    if (!created.ok()) {
      return created.status();
    }
    reader_ = std::move(*created);
  }
  return reader_;
}

absl::StatusOr<std::shared_ptr<stores::ChunkStoreWriter>> AsyncNode::writer() {
  thread::MutexLock lock(&mu_);
  if (writer_ == nullptr) {
    absl::StatusOr<std::shared_ptr<stores::ChunkStoreWriter>> created =
        stores::ChunkStoreWriter::Create(store_, writer_options_);
    if (!created.ok()) {
      return created.status();
    }
    writer_ = std::move(*created);
  }
  return writer_;
}

stores::ChunkStoreReaderOptions AsyncNode::GetReaderOptions() const {
  thread::MutexLock lock(&mu_);
  return reader_options_;
}

absl::Status AsyncNode::SetReaderOptions(
    stores::ChunkStoreReaderOptions options) {
  ABSL_RETURN_IF_ERROR(options.Validate());
  thread::MutexLock lock(&mu_);
  if (reader_ != nullptr) {
    return absl::FailedPreconditionError(
        "Reader options cannot change after reader creation");
  }
  reader_options_ = options;
  return absl::OkStatus();
}

absl::Status AsyncNode::ResetReader(
    std::optional<stores::ChunkStoreReaderOptions> options) {
  if (options.has_value()) {
    ABSL_RETURN_IF_ERROR(options->Validate());
  }
  std::shared_ptr<stores::ChunkStoreReader> reader;
  {
    thread::MutexLock lock(&mu_);
    reader = std::move(reader_);
    if (options.has_value()) {
      reader_options_ = *options;
    }
  }
  if (reader != nullptr) {
    reader->Cancel();
  }
  return absl::OkStatus();
}

stores::ChunkStoreWriterOptions AsyncNode::GetWriterOptions() const {
  thread::MutexLock lock(&mu_);
  return writer_options_;
}

absl::Status AsyncNode::SetWriterOptions(
    stores::ChunkStoreWriterOptions options) {
  ABSL_RETURN_IF_ERROR(options.Validate());
  thread::MutexLock lock(&mu_);
  if (writer_ != nullptr) {
    return absl::FailedPreconditionError(
        "Writer options cannot change after writer creation");
  }
  writer_options_ = options;
  return absl::OkStatus();
}

absl::Status AsyncNode::GetReaderStatus() const {
  std::shared_ptr<stores::ChunkStoreReader> reader;
  {
    thread::MutexLock lock(&mu_);
    reader = reader_;
  }
  return reader != nullptr ? reader->GetStatus() : absl::OkStatus();
}

absl::Status AsyncNode::GetWriterStatus() const {
  std::shared_ptr<stores::ChunkStoreWriter> writer;
  {
    thread::MutexLock lock(&mu_);
    writer = writer_;
  }
  if (writer == nullptr) {
    return absl::OkStatus();
  }
  const std::optional<absl::Status> status = writer->GetStatus();
  return status.value_or(absl::OkStatus());
}

std::optional<absl::Status> AsyncNode::GetWriterAbortStatus() const {
  std::shared_ptr<stores::ChunkStoreWriter> writer;
  {
    thread::MutexLock lock(&mu_);
    writer = writer_;
  }
  return writer != nullptr ? writer->GetAbortStatus() : std::nullopt;
}

a11::Future<bool> AsyncNode::IsWritable() {
  // The writer is snapshotted *here*, on the caller's thread, rather than
  // inside the continuation.
  std::shared_ptr<stores::ChunkStoreWriter> writer;
  {
    thread::MutexLock lock(&mu_);
    writer = writer_;
  }
  return a11::Then(
      store_->GetFinalSeq(),
      [writer = std::move(writer)](
          const absl::StatusOr<std::optional<std::uint32_t>>& final)
          -> absl::StatusOr<bool> {
        if (!final.ok()) {
          return final.status();
        }
        if (final->has_value()) {
          return false;
        }
        return writer == nullptr || writer->IsWritable();
      });
}

a11::Future<std::uint32_t> AsyncNode::PutChunk(data::Chunk chunk,
                                               std::optional<std::uint32_t> seq,
                                               bool final) {
  absl::StatusOr<std::shared_ptr<stores::ChunkStoreWriter>> output = writer();
  if (!output.ok()) {
    return a11::FailedFuture<std::uint32_t>(output.status());
  }
  return (*output)->PutChunk(std::move(chunk), seq, final);
}

a11::Future<std::uint32_t> AsyncNode::PutFragment(data::NodeFragment fragment) {
  absl::StatusOr<data::Chunk*> chunk = fragment.GetChunk();
  if (!chunk.ok()) {
    return a11::FailedFuture<std::uint32_t>(absl::UnimplementedError(
        "AsyncNode does not resolve NodeRef payloads"));
  }
  return PutChunk(std::move(**chunk), fragment.seq, !fragment.continued);
}

a11::Task AsyncNode::Finalize(FinalizeOptions options) {
  return Finalize(NullChunk(), options);
}

a11::Task AsyncNode::Finalize(data::Chunk chunk, FinalizeOptions options) {
  absl::StatusOr<std::shared_ptr<stores::ChunkStoreWriter>> output = writer();
  if (!output.ok()) {
    return a11::FailedTask(output.status());
  }

  // The final chunk is enqueued before closure is asked for, and that order is
  // the whole of the synchronisation: the writer's state machine only starts
  // its close once nothing is outstanding, and admits whatever its bounded.
  stores::ChunkStoreWrite write =
      (*output)->EnqueueChunk(std::move(chunk), options.seq, /*final=*/true,
                              /*ensure_started=*/!options.wait);
  a11::Task closed =
      options.close ? (*output)->DrainAndClose() : a11::ReadyTask();

  if (!options.wait) {
    LogIfFailed(write.confirmation, "final write");
    if (options.close) {
      LogIfFailed(closed, "close");
    }
    return a11::ReadyTask();
  }

  // Flushing here rather than on the pump is what lets a store that answers
  // inline confirm in this frame; see ChunkStoreWriter::PutChunk.
  (*output)->Flush();
  if (options.close) {
    // A close cannot complete before the final chunk is confirmed, and fails
    // if that write fails, so it is the only awaitable this needs.
    return closed;
  }
  return a11::Then(write.confirmation,
                   [](const absl::StatusOr<std::uint32_t>& stored)
                       -> absl::StatusOr<a11::Unit> {
                     if (!stored.ok()) {
                       return stored.status();
                     }
                     return a11::Unit{};
                   });
}

a11::Future<std::vector<std::optional<data::NodeFragment>>>
AsyncNode::NextFragments(size_t limit, absl::Duration timeout) {
  using Batch = std::vector<std::optional<data::NodeFragment>>;
  absl::StatusOr<std::shared_ptr<stores::ChunkStoreReader>> input = reader();
  if (!input.ok()) {
    return a11::FailedFuture<Batch>(input.status());
  }
  return (*input)->NextMany(limit, timeout);
}

a11::Future<std::optional<data::NodeFragment>> AsyncNode::NextFragmentRaw(
    absl::Duration timeout) {
  absl::StatusOr<std::shared_ptr<stores::ChunkStoreReader>> input = reader();
  if (!input.ok()) {
    return a11::FailedFuture<std::optional<data::NodeFragment>>(input.status());
  }
  return (*input)->Next(timeout);
}

a11::Future<std::optional<data::NodeFragment>> AsyncNode::NextFragment(
    absl::Duration timeout) {
  // Materialised here, which is what keeps the local fast path invisible to
  // every existing caller: a fragment leaving this node has its bytes, exactly
  // as it did before a chunk could carry a value instead.
  return a11::Then(
      NextFragmentRaw(timeout),
      [](const absl::StatusOr<std::optional<data::NodeFragment>>& fragment)
          -> absl::StatusOr<std::optional<data::NodeFragment>> {
        if (!fragment.ok() || !fragment->has_value()) {
          return fragment;
        }
        std::optional<data::NodeFragment> materialised = *fragment;
        absl::StatusOr<data::Chunk*> chunk = materialised->GetChunk();
        if (!chunk.ok()) {
          return materialised;  // a NodeRef, which has no bytes to produce
        }
        ABSL_RETURN_IF_ERROR((*chunk)->Materialize());
        return materialised;
      });
}

a11::Future<std::optional<data::Chunk>> AsyncNode::NextChunk(
    absl::Duration timeout) {
  // `Then`, not `Submit`: this only reshapes what NextFragment produces, and a
  // fibre whose whole life is "await one future, unwrap it" is a fibre spent
  // on nothing.
  return a11::Then(
      NextFragment(timeout),
      [](const absl::StatusOr<std::optional<data::NodeFragment>>& fragment)
          -> absl::StatusOr<std::optional<data::Chunk>> {
        if (!fragment.ok()) {
          return fragment.status();
        }
        if (!fragment->has_value()) {
          return std::nullopt;
        }
        absl::StatusOr<const data::Chunk*> chunk = (*fragment)->GetChunk();
        if (!chunk.ok()) {
          return chunk.status();
        }
        return std::optional<data::Chunk>(**chunk);
      });
}

a11::Task AsyncNode::WaitForBufferToDrain() {
  std::shared_ptr<stores::ChunkStoreWriter> writer;
  {
    thread::MutexLock lock(&mu_);
    writer = writer_;
  }
  return writer != nullptr ? writer->WaitForBufferToDrain() : a11::ReadyTask();
}

a11::Task AsyncNode::Close() {
  absl::StatusOr<std::shared_ptr<stores::ChunkStoreWriter>> output = writer();
  return output.ok() ? (*output)->DrainAndClose()
                     : a11::FailedTask(output.status());
}

a11::Task AsyncNode::AbortWithStatus(absl::Status status) {
  absl::StatusOr<std::shared_ptr<stores::ChunkStoreWriter>> output = writer();
  return output.ok() ? (*output)->AbortWithStatus(std::move(status))
                     : a11::FailedTask(output.status());
}

absl::Status AsyncNode::AttachStream(std::shared_ptr<net::WireStream> stream) {
  absl::StatusOr<std::shared_ptr<stores::ChunkStoreWriter>> value = writer();
  if (!value.ok()) {
    return value.status();
  }
  return (*value)->AttachStream(std::move(stream));
}

absl::Status AsyncNode::DetachStream(
    const std::shared_ptr<net::WireStream>& stream) {
  absl::StatusOr<std::shared_ptr<stores::ChunkStoreWriter>> value = writer();
  if (!value.ok()) {
    return value.status();
  }
  return (*value)->DetachStream(stream);
}

void AsyncNode::CancelReader() {
  std::shared_ptr<stores::ChunkStoreReader> reader;
  {
    thread::MutexLock lock(&mu_);
    reader = reader_;
  }
  if (reader != nullptr) {
    reader->Cancel();
  }
}

void AsyncNode::CancelWriter() {
  std::shared_ptr<stores::ChunkStoreWriter> writer;
  {
    thread::MutexLock lock(&mu_);
    writer = writer_;
  }
  if (writer != nullptr) {
    writer->Cancel();
  }
}

void AsyncNode::Cancel() {
  CancelReader();
  CancelWriter();
}

}  // namespace a11::nodes
