// Copyright 2026 The A11 Authors.

#include "a11/stores/local_chunk_store.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/data/types.h"
#include "thread/boost_primitives.h"

namespace a11::stores {
namespace {

data::NodeFragment Fragment(std::uint32_t seq, bool final = false) {
  return data::NodeFragment{.data = data::Chunk{.data = std::to_string(seq)},
                            .seq = seq,
                            .continued = !final};
}

TEST(LocalChunkStoreTest, PreservesSequenceAndArrivalOrder) {
  absl::StatusOr<std::shared_ptr<LocalChunkStore>> created =
      LocalChunkStore::Create("test");
  ASSERT_TRUE(created.ok());
  std::shared_ptr<LocalChunkStore> store = *created;
  absl::StatusOr<std::vector<std::uint32_t>> sequences =
      store->PutMany({Fragment(2, true), Fragment(0), Fragment(1)})
          .Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(sequences.ok()) << sequences.status();
  EXPECT_EQ(*sequences, (std::vector<std::uint32_t>{2, 0, 1}));
  EXPECT_EQ(*store->GetSeqForArrivalOrder(0).Await(), 2);
  EXPECT_EQ(*store->GetSeqForArrivalOrder(1).Await(), 0);

  const auto first = store->Next(absl::Now() + absl::Seconds(5), 3).Await();
  ASSERT_TRUE(first.ok()) << first.status();
  ASSERT_EQ(first->size(), 4);
  EXPECT_EQ((*first)[0]->seq, 0);
  EXPECT_EQ((*first)[1]->seq, 1);
  EXPECT_EQ((*first)[2]->seq, 2);
  EXPECT_FALSE((*first)[3].has_value());
  const auto end = store->Next().Await();
  ASSERT_TRUE(end.ok());
  ASSERT_EQ(end->size(), 1);
  EXPECT_FALSE(end->front().has_value());
}

TEST(LocalChunkStoreTest, CloseWakesBlockedRead) {
  auto created = LocalChunkStore::Create("test");
  ASSERT_TRUE(created.ok());
  std::shared_ptr<LocalChunkStore> store = *created;
  auto pending = store->Get(7, absl::Now() + absl::Seconds(5));
  auto closed = store->CloseWritesWithStatus(absl::DataLossError("failed"));
  ASSERT_TRUE(closed.Await().ok());
  EXPECT_EQ(pending.Await().status().code(), absl::StatusCode::kDataLoss);
}

TEST(LocalChunkStoreTest, CancellationWakesBlockedReadAndStoreRemainsUsable) {
  auto store = *LocalChunkStore::Create("cancel-read");
  a11::Future<data::NodeFragment> pending = store->Get(0);
  thread::SleepFor(absl::Milliseconds(1));
  ASSERT_TRUE(pending.Cancel().ok());
  absl::StatusOr<data::NodeFragment> cancelled =
      pending.Await(absl::Now() + absl::Seconds(5));
  EXPECT_TRUE(absl::IsCancelled(cancelled.status()));

  ASSERT_TRUE(store->Put(Fragment(0, true)).Await().ok());
  auto recovered = store->Get(0).Await(absl::Now() + absl::Seconds(5));
  ASSERT_TRUE(recovered.ok()) << recovered.status();
  EXPECT_EQ(recovered->seq, 0);
}

TEST(LocalChunkStoreTest, ClearLeavesTombstone) {
  auto created = LocalChunkStore::Create("test");
  ASSERT_TRUE(created.ok());
  std::shared_ptr<LocalChunkStore> store = *created;
  ASSERT_TRUE(store->Put(Fragment(0, true)).Await().ok());
  auto original = store->ClearData(0).Await();
  ASSERT_TRUE(original.ok());
  EXPECT_EQ(std::get<data::Chunk>(original->data).data, "0");
  auto tombstone = store->Get(0).Await();
  ASSERT_TRUE(tombstone.ok());
  EXPECT_EQ(std::get<data::Chunk>(tombstone->data).ref, "__tombstone__");
}

}  // namespace
}  // namespace a11::stores
