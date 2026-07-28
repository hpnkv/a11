// Copyright 2026 The A11 Authors.

#ifndef A11_STORES_CHUNK_STORE_WRITER_H_
#define A11_STORES_CHUNK_STORE_WRITER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/stores/chunk_store.h"

namespace a11::net {
class WireStream;
}  // namespace a11::net

namespace a11::stores {

struct ChunkStoreWriterOptions {
  std::uint32_t offset = 0;
  std::uint64_t max_chunks_to_write_at_once = 8;
  std::optional<std::uint64_t> num_chunks_to_buffer;

  absl::Status Validate() const;
};

struct ChunkStoreWrite {
  // The Python API distinguishes admission to the bounded native queue from
  // durable store confirmation. Native callers that only need confirmation
  // can continue to use ChunkStoreWriter::PutChunk.
  a11::Task admitted;
  a11::Future<std::uint32_t> confirmation;
};

class ChunkStoreWriter {
 public:
  static absl::StatusOr<std::shared_ptr<ChunkStoreWriter>> Create(
      std::shared_ptr<ChunkStore> store, ChunkStoreWriterOptions options = {});

  ~ChunkStoreWriter() = default;

  void EnsureStarted();
  ChunkStoreWrite EnqueueChunk(data::Chunk chunk,
                               std::optional<std::uint32_t> seq = std::nullopt,
                               bool final = false, bool ensure_started = true);
  a11::Future<std::uint32_t> PutChunk(
      data::Chunk chunk, std::optional<std::uint32_t> seq = std::nullopt,
      bool final = false);

  [[nodiscard]] std::optional<absl::Status> GetStatus() const;
  [[nodiscard]] std::optional<absl::Status> GetAbortStatus() const;
  [[nodiscard]] bool IsWritable() const;
  a11::Task Cancel();

  a11::Task DrainAndClose();
  a11::Task AbortWithStatus(absl::Status status);
  a11::Task WaitForBufferToDrain();

  // Persisted fragments are copied to every attached stream. A transport
  // failure stops subsequent writes but cannot revoke store confirmations
  // already returned for the current batch.
  absl::Status AttachStream(std::shared_ptr<net::WireStream> stream);
  absl::Status DetachStream(const std::shared_ptr<net::WireStream>& stream);

  [[nodiscard]] std::shared_ptr<ChunkStore> store() const;
  [[nodiscard]] ChunkStoreWriterOptions options() const;
  [[nodiscard]] size_t queue_size() const;

 private:
  struct State;

  explicit ChunkStoreWriter(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;
};

}  // namespace a11::stores

#endif  // A11_STORES_CHUNK_STORE_WRITER_H_
