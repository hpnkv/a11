// Copyright 2026 The A11 Authors.

#ifndef A11_NODES_ASYNC_NODE_H_
#define A11_NODES_ASYNC_NODE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/base/thread_annotations.h>
#include <absl/status/status.h>
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

namespace a11::net {
class WireStream;
}  // namespace a11::net

namespace a11::nodes {

class AsyncNode : public std::enable_shared_from_this<AsyncNode> {
 public:
  static absl::StatusOr<std::shared_ptr<AsyncNode>> Create(
      std::shared_ptr<stores::ChunkStore> store,
      std::shared_ptr<data::SerializationRegistry> serialization_registry =
          nullptr,
      stores::ChunkStoreReaderOptions reader_options = {},
      stores::ChunkStoreWriterOptions writer_options = {});

  ~AsyncNode() = default;

  AsyncNode(const AsyncNode&) = delete;
  AsyncNode& operator=(const AsyncNode&) = delete;

  absl::StatusOr<std::string> GetId() const;
  [[nodiscard]] std::shared_ptr<stores::ChunkStore> GetChunkStore() const;
  [[nodiscard]] std::shared_ptr<data::SerializationRegistry>
  serialization_registry() const;
  absl::Status SetSerializationRegistry(
      std::shared_ptr<data::SerializationRegistry> registry);

  absl::StatusOr<std::shared_ptr<stores::ChunkStoreReader>> reader();
  absl::StatusOr<std::shared_ptr<stores::ChunkStoreWriter>> writer();

  [[nodiscard]] stores::ChunkStoreReaderOptions GetReaderOptions() const;
  absl::Status SetReaderOptions(stores::ChunkStoreReaderOptions options);
  absl::Status ResetReader(
      std::optional<stores::ChunkStoreReaderOptions> options = std::nullopt);
  [[nodiscard]] stores::ChunkStoreWriterOptions GetWriterOptions() const;
  absl::Status SetWriterOptions(stores::ChunkStoreWriterOptions options);

  [[nodiscard]] absl::Status GetReaderStatus() const;
  [[nodiscard]] absl::Status GetWriterStatus() const;
  [[nodiscard]] std::optional<absl::Status> GetWriterAbortStatus() const;
  a11::Future<bool> IsWritable();

  a11::Future<std::uint32_t> PutChunk(
      data::Chunk chunk, std::optional<std::uint32_t> seq = std::nullopt,
      bool final = false);
  a11::Future<std::uint32_t> PutFragment(data::NodeFragment fragment);

  template <typename T>
  a11::Future<std::uint32_t> Put(
      const T& value, std::optional<std::uint32_t> seq = std::nullopt,
      bool final = false, std::string_view mimetype = {}) {
    std::shared_ptr<data::SerializationRegistry> registry;
    {
      thread::MutexLock lock(&mu_);
      registry = serialization_registry_;
    }
    absl::StatusOr<data::Chunk> chunk = registry->ToChunk<T>(value, mimetype);
    if (!chunk.ok()) {
      return a11::FailedFuture<std::uint32_t>(chunk.status());
    }
    return PutChunk(std::move(*chunk), seq, final);
  }

  a11::Future<std::uint32_t> PutNullFinal(
      std::optional<std::uint32_t> seq = std::nullopt);
  a11::Future<std::optional<data::NodeFragment>> NextFragment(
      absl::Duration timeout = absl::InfiniteDuration());
  a11::Future<std::optional<data::Chunk>> NextChunk(
      absl::Duration timeout = absl::InfiniteDuration());

  template <typename T>
  a11::Future<std::optional<T>> NextObject(
      absl::Duration timeout = absl::InfiniteDuration(),
      std::vector<std::string> mimetype_patterns = {}) {
    std::shared_ptr<AsyncNode> self = shared_from_this();
    return a11::Submit<std::optional<T>>(
        [self = std::move(self), timeout,
         mimetype_patterns = std::move(
             mimetype_patterns)]() mutable -> absl::StatusOr<std::optional<T>> {
          absl::StatusOr<std::optional<data::NodeFragment>> fragment =
              self->NextFragment(timeout).Await();
          if (!fragment.ok())
            return fragment.status();
          if (!fragment->has_value())
            return std::nullopt;
          absl::StatusOr<const data::Chunk*> chunk = (*fragment)->GetChunk();
          if (!chunk.ok())
            return chunk.status();
          if ((*chunk)->IsNull()) {
            if ((*fragment)->continued) {
              return absl::FailedPreconditionError(
                  "A null stream marker must be final");
            }
            return std::nullopt;
          }
          std::shared_ptr<data::SerializationRegistry> registry;
          {
            thread::MutexLock lock(&self->mu_);
            registry = self->serialization_registry_;
          }
          absl::StatusOr<T> value =
              registry->FromChunk<T>(**chunk, mimetype_patterns);
          if (!value.ok())
            return value.status();
          return std::optional<T>(std::move(*value));
        });
  }

  a11::Task WaitForBufferToDrain();
  a11::Task DrainAndClose();
  a11::Task AbortWithStatus(absl::Status status);
  absl::Status AttachStream(std::shared_ptr<net::WireStream> stream);
  absl::Status DetachStream(const std::shared_ptr<net::WireStream>& stream);
  void CancelReader();
  void CancelWriter();
  void Cancel();

 private:
  AsyncNode(std::shared_ptr<stores::ChunkStore> store,
            std::shared_ptr<data::SerializationRegistry> registry,
            stores::ChunkStoreReaderOptions reader_options,
            stores::ChunkStoreWriterOptions writer_options)
      : store_(std::move(store)),
        serialization_registry_(std::move(registry)),
        reader_options_(reader_options),
        writer_options_(writer_options) {}

  const std::shared_ptr<stores::ChunkStore> store_;
  mutable thread::Mutex mu_;
  std::shared_ptr<data::SerializationRegistry> serialization_registry_
      ABSL_GUARDED_BY(mu_);
  stores::ChunkStoreReaderOptions reader_options_ ABSL_GUARDED_BY(mu_);
  stores::ChunkStoreWriterOptions writer_options_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<stores::ChunkStoreReader> reader_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<stores::ChunkStoreWriter> writer_ ABSL_GUARDED_BY(mu_);
};

}  // namespace a11::nodes

#endif  // A11_NODES_ASYNC_NODE_H_
