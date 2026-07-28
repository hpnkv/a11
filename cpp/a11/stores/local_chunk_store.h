// Copyright 2026 The A11 Authors.

#ifndef A11_STORES_LOCAL_CHUNK_STORE_H_
#define A11_STORES_LOCAL_CHUNK_STORE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/stores/chunk_store.h"

namespace a11::stores {

class LocalChunkStore final : public ChunkStore {
 private:
  struct State;

  struct ConstructorToken {};

 public:
  static absl::StatusOr<std::shared_ptr<LocalChunkStore>> Create(
      std::string node_id);

  ~LocalChunkStore() override = default;

  using ChunkStore::CloseWritesWithStatus;
  using ChunkStore::Get;
  using ChunkStore::GetByArrivalOrder;
  using ChunkStore::Next;

  a11::Future<data::NodeFragment> Get(std::uint32_t seq,
                                      absl::Time deadline) override;
  a11::Future<data::NodeFragment> GetByArrivalOrder(
      std::uint64_t arrival_order, absl::Time deadline) override;
  a11::Future<std::vector<std::optional<data::NodeFragment>>> Next(
      absl::Time deadline, size_t limit) override;

  a11::Future<std::uint32_t> Put(data::NodeFragment fragment) override;
  a11::Future<std::vector<std::uint32_t>> PutMany(
      std::vector<data::NodeFragment> fragments) override;
  a11::Future<data::NodeFragment> ClearData(std::uint32_t seq) override;
  a11::Future<std::uint32_t> GetSeqForArrivalOrder(
      std::uint64_t arrival_order) override;
  a11::Future<std::optional<std::uint32_t>> GetFinalSeq() override;
  a11::Future<absl::Status> CloseWritesWithStatus(
      absl::Status status, bool return_status_if_already_closed) override;
  a11::Future<size_t> Size() override;
  absl::StatusOr<std::string> GetId() const override;

  explicit LocalChunkStore(ConstructorToken, std::shared_ptr<State> state)
      : state_(std::move(state)) {}

 private:
  std::shared_ptr<State> state_;
};

}  // namespace a11::stores

#endif  // A11_STORES_LOCAL_CHUNK_STORE_H_
