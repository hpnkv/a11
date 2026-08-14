// Copyright 2026 The A11 Authors.

#include "a11/stores/chunk_store_reader.h"

#include <memory>
#include <optional>
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

TEST(ChunkStoreReaderTest, StickyMimetypeExpandsOrderedChunkMetadata) {
  auto store = *LocalChunkStore::Create("reader-sticky-mimetype");
  ASSERT_TRUE(
      store
          ->PutMany({
              data::NodeFragment{.data = data::Chunk{.data = "before-anchor"},
                                 .seq = 0,
                                 .continued = true},
              data::NodeFragment{
                  .data = data::Chunk{.metadata =
                                          data::ChunkMetadata{.mimetype =
                                                                  "text/plain"},
                                      .data = "anchor"},
                  .seq = 1,
                  .continued = true},
              data::NodeFragment{
                  .data = data::Chunk{.metadata =
                                          data::ChunkMetadata{
                                              .attributes = {{"role",
                                                              "assistant"}}},
                                      .data = "attributes"},
                  .seq = 2,
                  .continued = true},
              data::NodeFragment{
                  .data = data::Chunk{.data = "missing-metadata"},
                  .seq = 3,
                  .continued = true},
              data::NodeFragment{
                  .data = data::Chunk{.metadata =
                                          data::ChunkMetadata{
                                              .mimetype = "application/json"},
                                      .data = "new-anchor"},
                  .seq = 4,
                  .continued = true},
              data::NodeFragment{
                  .data = data::Chunk{.data = "inherits-new-anchor"},
                  .seq = 5,
                  .continued = false},
          })
          .Await()
          .ok());
  auto reader = *ChunkStoreReader::Create(
      store, ChunkStoreReaderOptions{.sticky_mimetype = true});

  std::vector<data::Chunk> chunks;
  for (int index = 0; index < 6; ++index) {
    auto fragment = reader->Next(absl::Seconds(1)).Await();
    ASSERT_TRUE(fragment.ok()) << fragment.status();
    ASSERT_TRUE(fragment->has_value());
    chunks.push_back(std::get<data::Chunk>((*fragment)->data));
  }

  EXPECT_FALSE(chunks[0].metadata.has_value());
  EXPECT_EQ(chunks[1].GetMimetype(), "text/plain");
  EXPECT_EQ(chunks[2].GetMimetype(), "text/plain");
  ASSERT_TRUE(chunks[2].metadata.has_value());
  EXPECT_EQ(*chunks[2].metadata->GetAttribute("role"), "assistant");
  EXPECT_EQ(chunks[3].GetMimetype(), "text/plain");
  ASSERT_TRUE(chunks[3].metadata.has_value());
  EXPECT_EQ(chunks[4].GetMimetype(), "application/json");
  EXPECT_EQ(chunks[5].GetMimetype(), "application/json");
  EXPECT_FALSE(reader->Next(absl::Seconds(1)).Await()->has_value());
}

TEST(ChunkStoreReaderTest, UnorderedReaderDoesNotExpandStickyMimetype) {
  auto store = *LocalChunkStore::Create("reader-unordered-sticky");
  ASSERT_TRUE(store
                  ->PutMany({
                      data::NodeFragment{
                          .data = data::Chunk{.metadata =
                                                  data::ChunkMetadata{
                                                      .mimetype = "text/plain"},
                                              .data = "anchor"},
                          .seq = 0,
                          .continued = true},
                      data::NodeFragment{.data = data::Chunk{.data = "missing"},
                                         .seq = 1,
                                         .continued = false},
                  })
                  .Await()
                  .ok());
  auto reader = *ChunkStoreReader::Create(
      store,
      ChunkStoreReaderOptions{.ordered = false,
                              .max_chunks_to_read = 2,
                              .sticky_mimetype = true});

  ASSERT_TRUE(reader->Next(absl::Seconds(1)).Await()->has_value());
  auto second = reader->Next(absl::Seconds(1)).Await();
  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(second->has_value());
  EXPECT_FALSE(std::get<data::Chunk>((*second)->data).metadata.has_value());
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

// A fragment that arrived while the reader was already prefetching the next
// sequence must still reach the caller that asks for it afterwards. That
// prefetch waits for a value nobody has written yet, so a reader which only
// delivered from a completing fetch would hold the answer back forever.
TEST(ChunkStoreReaderTest, BufferedFragmentReachesALaterReader) {
  auto store = *LocalChunkStore::Create("reader-prefetch");
  auto reader = *ChunkStoreReader::Create(store);
  // Arms the first fetch, which waits on a store that is still empty.
  reader->EnsureStarted();
  thread::SleepFor(absl::Milliseconds(20));

  ASSERT_TRUE(store
                  ->Put(data::NodeFragment{.data = data::Chunk{.data = "one"},
                                           .seq = 0,
                                           .continued = true})
                  .Await()
                  .ok());
  // Long enough for the fetch to finish, the fragment to be buffered, and the
  // prefetch of seq 1 -- which nobody will write -- to be in flight.
  thread::SleepFor(absl::Milliseconds(50));
  EXPECT_EQ(reader->buffer_size(), 1);

  const absl::StatusOr<std::optional<data::NodeFragment>> fragment =
      reader->Next(absl::Seconds(2)).Await();
  ASSERT_TRUE(fragment.ok()) << fragment.status();
  ASSERT_TRUE(fragment->has_value());
  EXPECT_EQ(std::get<data::Chunk>((*fragment)->data).data, "one");
  reader->Cancel();
}

}  // namespace
}  // namespace a11::stores
