// Copyright 2026 The A11 Authors.

#ifndef A11_STORES_CHUNK_STORE_READER_H_
#define A11_STORES_CHUNK_STORE_READER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/stores/chunk_store.h"

namespace a11::stores {

struct ChunkStoreReaderOptions {
  bool ordered = true;
  bool pop_chunks = false;
  std::uint64_t num_chunks_to_buffer = 32;
  std::uint32_t offset = 0;
  std::optional<std::uint64_t> max_chunks_to_read;

  absl::Status Validate() const;
};

class ChunkStoreReader {
 public:
  static absl::StatusOr<std::shared_ptr<ChunkStoreReader>> Create(
      std::shared_ptr<ChunkStore> store, ChunkStoreReaderOptions options = {});

  ~ChunkStoreReader() = default;

  void EnsureStarted();
  void Cancel();
  absl::Status GetStatus() const;
  a11::Task Done() const;
  a11::Future<std::optional<data::NodeFragment>> Next(
      absl::Duration timeout = absl::InfiniteDuration());

  [[nodiscard]] std::shared_ptr<ChunkStore> store() const;
  [[nodiscard]] ChunkStoreReaderOptions options() const;
  [[nodiscard]] size_t buffer_size() const;

 private:
  struct State;

  explicit ChunkStoreReader(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;
};

}  // namespace a11::stores

#endif  // A11_STORES_CHUNK_STORE_READER_H_
