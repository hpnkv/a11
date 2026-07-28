// Copyright 2026 The A11 Authors.

#include "a11/stores/chunk_store_reader.h"

#include <memory>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/data/types.h"
#include "a11/stores/local_chunk_store.h"
#include "thread/boost_primitives.h"
#include "thread/fiber.h"

namespace a11::stores {
namespace {

TEST(ChunkStoreReaderTest, StopsAfterOrderedFinalFragment) {
  auto store_result = LocalChunkStore::Create("reader");
  ASSERT_TRUE(store_result.ok());
  auto store = *store_result;
  ASSERT_TRUE(store
                  ->PutMany({
                      data::NodeFragment{.data = data::Chunk{.data = "a"},
                                         .seq = 0,
                                         .continued = true},
                      data::NodeFragment{.data = data::Chunk{.data = "b"},
                                         .seq = 1,
                                         .continued = false},
                  })
                  .Await()
                  .ok());
  auto reader_result = ChunkStoreReader::Create(store);
  ASSERT_TRUE(reader_result.ok());
  auto reader = *reader_result;
  EXPECT_TRUE(reader->Next(absl::Seconds(1)).Await()->has_value());
  EXPECT_TRUE(reader->Next(absl::Seconds(1)).Await()->has_value());
  EXPECT_FALSE(reader->Next(absl::Seconds(1)).Await()->has_value());
  EXPECT_TRUE(reader->GetStatus().ok());
}

TEST(ChunkStoreReaderTest, PopReplacesStoredPayloadWithTombstone) {
  auto store = *LocalChunkStore::Create("reader-pop");
  ASSERT_TRUE(
      store
          ->Put(data::NodeFragment{.data = data::Chunk{.data = "payload"},
                                   .seq = 0,
                                   .continued = false})
          .Await()
          .ok());
  auto reader = *ChunkStoreReader::Create(
      store, ChunkStoreReaderOptions{.pop_chunks = true});
  auto fragment = reader->Next(absl::Seconds(1)).Await();
  ASSERT_TRUE(fragment.ok());
  EXPECT_EQ(std::get<data::Chunk>((*fragment)->data).data, "payload");
  auto stored = store->Get(0).Await();
  ASSERT_TRUE(stored.ok());
  EXPECT_EQ(std::get<data::Chunk>(stored->data).ref, "__tombstone__");
}

TEST(ChunkStoreReaderTest, CancelInterruptsPendingStoreRead) {
  auto store = *LocalChunkStore::Create("reader-cancel");
  auto reader = *ChunkStoreReader::Create(store);
  a11::Future<std::optional<data::NodeFragment>> pending = reader->Next();
  thread::SleepFor(absl::Milliseconds(1));
  reader->Cancel();

  auto result = pending.Await(absl::Now() + absl::Seconds(5));
  EXPECT_TRUE(absl::IsAborted(result.status())) << result.status();
  EXPECT_TRUE(absl::IsAborted(reader->GetStatus()));
}

TEST(ChunkStoreReaderTest, CancellingOneNextFutureDoesNotPoisonReader) {
  auto store = *LocalChunkStore::Create("reader-request-cancel");
  auto reader = *ChunkStoreReader::Create(store);
  a11::Future<std::optional<data::NodeFragment>> pending = reader->Next();
  thread::SleepFor(absl::Milliseconds(1));
  ASSERT_TRUE(pending.Cancel().ok());
  auto cancelled = pending.Await(absl::Now() + absl::Seconds(5));
  EXPECT_TRUE(absl::IsCancelled(cancelled.status()));
  EXPECT_TRUE(reader->GetStatus().ok());

  ASSERT_TRUE(
      store
          ->Put(data::NodeFragment{.data = data::Chunk{.data = "after-cancel"},
                                   .seq = 0,
                                   .continued = false})
          .Await()
          .ok());
  auto recovered = reader->Next(absl::Seconds(5)).Await();
  ASSERT_TRUE(recovered.ok()) << recovered.status();
  ASSERT_TRUE(recovered->has_value());
  EXPECT_EQ(std::get<data::Chunk>((*recovered)->data).data, "after-cancel");
}

TEST(ChunkStoreReaderTest, ConcurrentRequestsUsePrefetchedResultsInOrder) {
  auto store = *LocalChunkStore::Create("reader-concurrent");
  auto reader = *ChunkStoreReader::Create(
      store, ChunkStoreReaderOptions{.num_chunks_to_buffer = 0});
  auto first = reader->Next(absl::Seconds(5));
  auto second = reader->Next(absl::Seconds(5));
  auto end = reader->Next(absl::Seconds(5));

  ASSERT_TRUE(store
                  ->PutMany({
                      data::NodeFragment{.data = data::Chunk{.data = "a"},
                                         .seq = 0,
                                         .continued = true},
                      data::NodeFragment{.data = data::Chunk{.data = "b"},
                                         .seq = 1,
                                         .continued = false},
                  })
                  .Await()
                  .ok());
  ASSERT_EQ(std::get<data::Chunk>((**first.Await()).data).data, "a");
  ASSERT_EQ(std::get<data::Chunk>((**second.Await()).data).data, "b");
  EXPECT_FALSE(end.Await()->has_value());
}

TEST(ChunkStoreReaderTest, ManyReadersShareStacklessPump) {
  (void)thread::Fiber::Current();
  const size_t created = thread::internal::CreatedFiberCountForTesting();
  std::vector<std::shared_ptr<ChunkStoreReader>> readers;
  readers.reserve(256);
  for (int index = 0; index < 256; ++index) {
    readers.push_back(*ChunkStoreReader::Create(
        *LocalChunkStore::Create("reader-pool-" + std::to_string(index))));
  }
  thread::SleepFor(absl::Milliseconds(2));
  EXPECT_EQ(thread::internal::CreatedFiberCountForTesting(), created);
  for (const auto& reader : readers) {
    reader->Cancel();
  }
  EXPECT_EQ(thread::internal::CreatedFiberCountForTesting(), created);
}

}  // namespace
}  // namespace a11::stores
