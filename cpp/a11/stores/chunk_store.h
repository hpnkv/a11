// Copyright 2026 The A11 Authors.

#ifndef A11_STORES_CHUNK_STORE_H_
#define A11_STORES_CHUNK_STORE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"

namespace a11::stores {

class ChunkStore {
 public:
  virtual ~ChunkStore() = default;

  a11::Future<data::NodeFragment> Get(std::uint32_t seq) {
    return Get(seq, absl::InfiniteFuture());
  }

  virtual a11::Future<data::NodeFragment> Get(std::uint32_t seq,
                                              absl::Time deadline) = 0;

  a11::Future<data::NodeFragment> GetByArrivalOrder(
      std::uint64_t arrival_order) {
    return GetByArrivalOrder(arrival_order, absl::InfiniteFuture());
  }

  virtual a11::Future<data::NodeFragment> GetByArrivalOrder(
      std::uint64_t arrival_order, absl::Time deadline) = 0;

  a11::Future<std::vector<std::optional<data::NodeFragment>>> Next() {
    return Next(absl::InfiniteFuture(), 1);
  }

  a11::Future<std::vector<std::optional<data::NodeFragment>>> Next(
      absl::Time deadline) {
    return Next(deadline, 1);
  }

  virtual a11::Future<std::vector<std::optional<data::NodeFragment>>> Next(
      absl::Time deadline, size_t limit) = 0;

  virtual a11::Future<std::uint32_t> Put(data::NodeFragment fragment) = 0;
  virtual a11::Future<std::vector<std::uint32_t>> PutMany(
      std::vector<data::NodeFragment> fragments) = 0;

  virtual a11::Future<data::NodeFragment> ClearData(std::uint32_t seq) = 0;
  virtual a11::Future<std::uint32_t> GetSeqForArrivalOrder(
      std::uint64_t arrival_order) = 0;
  virtual a11::Future<std::optional<std::uint32_t>> GetFinalSeq() = 0;

  a11::Future<absl::Status> CloseWritesWithStatus(absl::Status status) {
    return CloseWritesWithStatus(std::move(status), false);
  }

  virtual a11::Future<absl::Status> CloseWritesWithStatus(
      absl::Status status, bool return_status_if_already_closed) = 0;
  virtual a11::Future<size_t> Size() = 0;
  virtual absl::StatusOr<std::string> GetId() const = 0;
};

}  // namespace a11::stores

#endif  // A11_STORES_CHUNK_STORE_H_
