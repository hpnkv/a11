// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "a11/stores/chunk_store_reader.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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
  const auto& store = *store_result;
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
  const auto& reader = *reader_result;
  EXPECT_TRUE(reader->Next(absl::Seconds(1)).Await()->has_value());
  EXPECT_TRUE(reader->Next(absl::Seconds(1)).Await()->has_value());
  EXPECT_FALSE(reader->Next(absl::Seconds(1)).Await()->has_value());
  EXPECT_TRUE(reader->GetStatus().ok());
}

TEST(ChunkStoreReaderTest, LogicalReadCompletesBufferedChunksInline) {
  auto store = *LocalChunkStore::Create("logical-ready");
  std::vector<data::NodeFragment> fragments;
  fragments.push_back(data::NodeFragment{
      .data =
          data::Chunk{.metadata = data::ChunkMetadata{.mimetype = "text/plain"},
                      .data = "first"},
      .seq = 0,
      .continued = true});
  fragments.push_back(data::NodeFragment{
      .data = data::Chunk{.metadata =
                              data::ChunkMetadata{
                                  .mimetype = "application/octet-stream"}},
      .seq = 1,
      .continued = true});
  for (std::uint32_t index = 0; index < 32; ++index) {
    fragments.push_back(data::NodeFragment{
        .data = data::Chunk{.data = index == 0 ? "second" : "more"},
        .seq = index + 2,
        .continued = index != 31});
  }
  ASSERT_TRUE(store->PutMany(std::move(fragments)).Await().ok());
  auto reader = *ChunkStoreReader::Create(
      store, ChunkStoreReaderOptions{.sticky_mimetype = true});
  auto first = reader->NextLogical().Await(absl::Now() + absl::Seconds(2));
  ASSERT_TRUE(first.ok()) << first.status();
  ASSERT_TRUE(first->has_value());
  EXPECT_EQ(std::get<data::Chunk>((*first)->data).data, "first");

  bool saw_inline = false;
  for (int index = 0; index < 32; ++index) {
    auto next = reader->NextLogical();
    saw_inline |= next.IsReady();
    auto value = next.Await(absl::Now() + absl::Seconds(2));
    ASSERT_TRUE(value.ok()) << value.status();
    ASSERT_TRUE(value->has_value());
    EXPECT_EQ(std::get<data::Chunk>((*value)->data).data,
              index == 0 ? "second" : "more");
    EXPECT_EQ(std::get<data::Chunk>((*value)->data).GetMimetype(),
              "text/plain");
  }
  EXPECT_TRUE(saw_inline);
  EXPECT_FALSE(reader->NextLogical().Await()->has_value());
}

TEST(ChunkStoreReaderTest, ConcurrentLogicalReadsDoNotLoseValues) {
  auto store = *LocalChunkStore::Create("logical-order");
  auto reader = *ChunkStoreReader::Create(store);
  auto first = reader->NextLogical();
  auto second = reader->NextLogical();
  EXPECT_FALSE(first.IsReady());
  EXPECT_FALSE(second.IsReady());
  ASSERT_TRUE(store
                  ->PutMany({
                      data::NodeFragment{.data = data::Chunk{.data = "first"},
                                         .seq = 0,
                                         .continued = true},
                      data::NodeFragment{.data = data::Chunk{.data = "second"},
                                         .seq = 1,
                                         .continued = false},
                  })
                  .Await()
                  .ok());
  auto one = first.Await(absl::Now() + absl::Seconds(2));
  auto two = second.Await(absl::Now() + absl::Seconds(2));
  ASSERT_TRUE(one.ok()) << one.status();
  ASSERT_TRUE(two.ok()) << two.status();
  ASSERT_TRUE(one->has_value());
  ASSERT_TRUE(two->has_value());
  const std::string a = std::get<data::Chunk>((*one)->data).data;
  const std::string b = std::get<data::Chunk>((*two)->data).data;
  EXPECT_TRUE((a == "first" && b == "second") ||
              (a == "second" && b == "first"));
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
      store,
      ChunkStoreReaderOptions{.pop_chunks = true, .follow_node_refs = false});
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
      store, ChunkStoreReaderOptions{.ordered = false,
                                     .max_chunks_to_read = 2,
                                     .sticky_mimetype = true,
                                     .follow_node_refs = false});

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
// sequence must still reach the caller that asks for it afterwards.
TEST(ChunkStoreReaderTest, BufferedFragmentReachesALaterReader) {
  auto store = *LocalChunkStore::Create("reader-prefetch");
  auto reader = *ChunkStoreReader::Create(store);
  // Arms the first fetch, which waits on a store that is still empty.
  reader->EnsureStarted();
  thread::SleepFor(absl::Milliseconds(20));

  ASSERT_TRUE(
      store
          ->Put(data::NodeFragment{
              .data = data::Chunk{.data = "one"}, .seq = 0, .continued = true})
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
