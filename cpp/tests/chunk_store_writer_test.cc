// Copyright 2026 The A11 Authors.

#include "a11/stores/chunk_store_writer.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <gtest/gtest.h>

#include "a11/data/types.h"
#include "a11/net/in_process_wire_stream.h"
#include "a11/net/wire_stream_with_recv.h"
#include "a11/stores/chunk_store.h"
#include "a11/stores/local_chunk_store.h"
#include "thread/fiber.h"

namespace a11::stores {
namespace {

class FailFirstCloseStore final : public ChunkStore {
 public:
  explicit FailFirstCloseStore(std::shared_ptr<ChunkStore> store,
                               absl::Status failure)
      : store_(std::move(store)), failure_(std::move(failure)) {}

  a11::Future<data::NodeFragment> Get(std::uint32_t seq,
                                      absl::Time deadline) override {
    return store_->Get(seq, deadline);
  }

  a11::Future<data::NodeFragment> GetByArrivalOrder(
      std::uint64_t arrival_order, absl::Time deadline) override {
    return store_->GetByArrivalOrder(arrival_order, deadline);
  }

  a11::Future<std::vector<std::optional<data::NodeFragment>>> Next(
      absl::Time deadline, size_t limit) override {
    return store_->Next(deadline, limit);
  }

  a11::Future<std::uint32_t> Put(data::NodeFragment fragment) override {
    return store_->Put(std::move(fragment));
  }

  a11::Future<std::vector<std::uint32_t>> PutMany(
      std::vector<data::NodeFragment> fragments) override {
    return store_->PutMany(std::move(fragments));
  }

  a11::Future<data::NodeFragment> ClearData(std::uint32_t seq) override {
    return store_->ClearData(seq);
  }

  a11::Future<std::uint32_t> GetSeqForArrivalOrder(
      std::uint64_t arrival_order) override {
    return store_->GetSeqForArrivalOrder(arrival_order);
  }

  a11::Future<std::optional<std::uint32_t>> GetFinalSeq() override {
    return store_->GetFinalSeq();
  }

  a11::Future<absl::Status> CloseWritesWithStatus(
      absl::Status status, bool return_status_if_already_closed) override {
    close_statuses_.push_back(status);
    if (close_statuses_.size() == 1) {
      return a11::FailedFuture<absl::Status>(failure_);
    }
    return store_->CloseWritesWithStatus(std::move(status),
                                         return_status_if_already_closed);
  }

  a11::Future<size_t> Size() override { return store_->Size(); }

  absl::StatusOr<std::string> GetId() const override { return store_->GetId(); }

  const std::vector<absl::Status>& close_statuses() const {
    return close_statuses_;
  }

 private:
  const std::shared_ptr<ChunkStore> store_;
  const absl::Status failure_;
  std::vector<absl::Status> close_statuses_;
};

TEST(ChunkStoreWriterTest, BatchesAndClosesStore) {
  auto store = *LocalChunkStore::Create("writer");
  auto writer = *ChunkStoreWriter::Create(
      store, ChunkStoreWriterOptions{.max_chunks_to_write_at_once = 3});
  auto first = writer->PutChunk(data::Chunk{.data = "a"});
  auto second = writer->PutChunk(data::Chunk{.data = "b"});
  auto third = writer->PutChunk(data::Chunk{.data = "c"}, std::nullopt, true);
  EXPECT_EQ(*first.Await(), 0);
  EXPECT_EQ(*second.Await(), 1);
  EXPECT_EQ(*third.Await(), 2);
  EXPECT_TRUE(writer->DrainAndClose().Await().ok());
  ASSERT_TRUE(writer->GetStatus().has_value());
  EXPECT_TRUE(writer->GetStatus()->ok());
}

TEST(ChunkStoreWriterTest, OffsetAssignsExplicitSequences) {
  auto store = *LocalChunkStore::Create("writer-offset");
  auto writer =
      *ChunkStoreWriter::Create(store, ChunkStoreWriterOptions{.offset = 7});
  auto first = writer->PutChunk(data::Chunk{.data = "a"});
  auto second = writer->PutChunk(data::Chunk{.data = "b"}, std::nullopt, true);
  EXPECT_EQ(*first.Await(), 7);
  EXPECT_EQ(*second.Await(), 8);
  EXPECT_TRUE(writer->DrainAndClose().Await().ok());
}

TEST(ChunkStoreWriterTest, AbortFailsQueuedWritesAndClosesProducer) {
  auto store = *LocalChunkStore::Create("writer-abort");
  auto writer = *ChunkStoreWriter::Create(store);
  writer->EnsureStarted();
  EXPECT_TRUE(
      writer->AbortWithStatus(absl::DataLossError("bad source")).Await().ok());
  ASSERT_TRUE(writer->GetStatus().has_value());
  EXPECT_EQ(writer->GetStatus()->code(), absl::StatusCode::kDataLoss);
  EXPECT_EQ(
      writer->PutChunk(data::Chunk{.data = "late"}).Await().status().code(),
      absl::StatusCode::kDataLoss);
}

TEST(ChunkStoreWriterTest, FailedGracefulCloseCanBeRetriedAsAbort) {
  const absl::Status close_failure =
      absl::InternalError("graceful close failed");
  const absl::Status abort_status = absl::DataLossError("producer failed");
  auto local = *LocalChunkStore::Create("writer-close-retry");
  auto store = std::make_shared<FailFirstCloseStore>(local, close_failure);
  auto writer = *ChunkStoreWriter::Create(store);

  EXPECT_EQ(writer->DrainAndClose().Await().status(), close_failure);
  EXPECT_TRUE(writer->AbortWithStatus(abort_status).Await().ok());

  ASSERT_EQ(store->close_statuses().size(), 2);
  EXPECT_TRUE(store->close_statuses()[0].ok());
  EXPECT_EQ(store->close_statuses()[1], abort_status);
  ASSERT_TRUE(writer->GetStatus().has_value());
  EXPECT_EQ(*writer->GetStatus(), abort_status);
}

TEST(ChunkStoreWriterTest, TeesOnlyStoreConfirmedFragments) {
  auto pair = *net::InProcessWireStream::CreatePair();
  auto receiver = *net::WireStreamWithRecv::Create(pair.second);
  ASSERT_TRUE(
      pair.first
          ->Start(
              [](std::optional<data::WireMessage>) { return a11::ReadyTask(); },
              [] { return a11::ReadyTask(); })
          .Await()
          .ok());
  ASSERT_TRUE(receiver->Accept().Await().ok());

  auto store = *LocalChunkStore::Create("writer-tee");
  auto writer = *ChunkStoreWriter::Create(store);
  ASSERT_TRUE(writer->AttachStream(pair.first).ok());
  auto confirmation =
      writer->PutChunk(data::Chunk{.data = "sent"}, std::nullopt, true);
  ASSERT_EQ(*confirmation.Await(), 0);
  auto received = receiver->Receive().Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(received.ok()) << received.status();
  ASSERT_TRUE(received->has_value());
  ASSERT_EQ((**received).node_fragments.size(), 1);
  EXPECT_EQ((**received).node_fragments.front().id, "writer-tee");
  EXPECT_EQ((**received).node_fragments.front().seq, 0);

  ASSERT_TRUE(pair.first->HalfClose().ok());
  ASSERT_TRUE(receiver->HalfClose().ok());
  const absl::Time shutdown_deadline = absl::Now() + absl::Seconds(5);
  ASSERT_TRUE(pair.first->Done().Await(shutdown_deadline).ok());
  ASSERT_TRUE(pair.second->Done().Await(shutdown_deadline).ok());
}

TEST(ChunkStoreWriterTest, ManyWritersShareStacklessPump) {
  (void)thread::Fiber::Current();
  const size_t created = thread::internal::CreatedFiberCountForTesting();
  std::vector<a11::Future<std::uint32_t>> writes;
  writes.reserve(256);
  for (int index = 0; index < 256; ++index) {
    auto store =
        *LocalChunkStore::Create("writer-pool-" + std::to_string(index));
    auto writer = *ChunkStoreWriter::Create(store);
    writes.push_back(
        writer->PutChunk(data::Chunk{.data = "value"}, std::nullopt, true));
  }
  for (const auto& write : writes) {
    ASSERT_TRUE(write.Await(absl::Now() + absl::Seconds(5)).ok());
  }
  EXPECT_EQ(thread::internal::CreatedFiberCountForTesting(), created);
}

}  // namespace
}  // namespace a11::stores
