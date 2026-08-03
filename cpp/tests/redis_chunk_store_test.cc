// Copyright 2026 The A11 Authors.

#include "a11/stores/redis_chunk_store.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/data/types.h"
#include "redis/client.h"
#include "redis/reply.h"

namespace a11::stores {
namespace {

absl::StatusOr<std::shared_ptr<redis::Client>> ConnectForTest() {
  redis::ClientOptions options;
  options.client_name = "a11-redis-chunk-store-test";
  options.connect_timeout = absl::Milliseconds(250);
  options.command_timeout = absl::Seconds(3);
  absl::StatusOr<std::shared_ptr<redis::Client>> client =
      redis::Client::Create(std::move(options));
  if (!client.ok())
    return client.status();
  absl::Status ready =
      (*client)->Ready().Await(absl::Now() + absl::Seconds(2)).status();
  if (!ready.ok())
    return ready;
  return *client;
}

data::NodeFragment Fragment(std::optional<std::uint32_t> seq,
                            std::string payload, bool final = false) {
  return data::NodeFragment{
      .data = data::Chunk{.data = std::move(payload)},
      .seq = seq,
      .continued = !final,
  };
}

absl::Status DeleteStore(const std::shared_ptr<redis::Client>& client,
                         const RedisChunkStoreKeys& keys) {
  std::vector<std::string> command{"DEL"};
  for (std::string key : keys.ScriptKeys())
    command.push_back(std::move(key));
  return client->Command(std::move(command)).Await().status();
}

TEST(RedisChunkStoreTest, ImplementsOrderingAtomicityAndMetadata) {
  absl::StatusOr<std::shared_ptr<redis::Client>> connected = ConnectForTest();
  if (!connected.ok())
    GTEST_SKIP() << "Redis is unavailable: " << connected.status();
  std::shared_ptr<redis::Client> client = *connected;
  RedisChunkStoreOptions options;
  options.key_prefix =
      "a11:test:" + std::to_string(absl::ToUnixNanos(absl::Now())) + ":";
  options.inline_data_threshold = 32;
  auto created = RedisChunkStore::Create("redis-store", client, options);
  ASSERT_TRUE(created.ok()) << created.status();
  std::shared_ptr<RedisChunkStore> store = *created;

  ASSERT_TRUE(store->Initialize().Await().ok());
  absl::StatusOr<RedisChunkStoreMetadata> empty = store->GetMetadata().Await();
  ASSERT_TRUE(empty.ok()) << empty.status();
  EXPECT_EQ(empty->id, "redis-store");
  EXPECT_EQ(empty->size, 0);
  EXPECT_FALSE(empty->closed);

  absl::StatusOr<std::vector<std::uint32_t>> duplicate =
      store->PutMany({Fragment(0, "first"), Fragment(0, "duplicate", true)})
          .Await();
  EXPECT_TRUE(absl::IsInvalidArgument(duplicate.status()));
  ASSERT_EQ(*store->Size().Await(), 0);

  absl::StatusOr<std::vector<std::uint32_t>> assigned =
      store
          ->PutMany({Fragment(2, std::string(1024, 'x'), true),
                     Fragment(0, "zero"), Fragment(1, "one")})
          .Await();
  ASSERT_TRUE(assigned.ok()) << assigned.status();
  EXPECT_EQ(*assigned, (std::vector<std::uint32_t>{2, 0, 1}));
  EXPECT_EQ(*store->GetSeqForArrivalOrder(0).Await(), 2);
  EXPECT_EQ(*store->GetSeqForArrivalOrder(1).Await(), 0);

  absl::StatusOr<redis::Reply> blob_count =
      client->Command({"HLEN", store->keys().blobs}).Await();
  ASSERT_TRUE(blob_count.ok()) << blob_count.status();
  EXPECT_EQ(*blob_count->AsInteger(), 1);

  absl::StatusOr<std::vector<std::optional<data::NodeFragment>>> batch =
      store->Next(absl::Now() + absl::Seconds(2), 3).Await();
  ASSERT_TRUE(batch.ok()) << batch.status();
  ASSERT_EQ(batch->size(), 4);
  EXPECT_EQ((*batch)[0]->seq, 0);
  EXPECT_EQ((*batch)[1]->seq, 1);
  EXPECT_EQ((*batch)[2]->seq, 2);
  EXPECT_FALSE((*batch)[3].has_value());

  absl::StatusOr<RedisChunkStoreMetadata> metadata =
      store->GetMetadata().Await();
  ASSERT_TRUE(metadata.ok()) << metadata.status();
  EXPECT_EQ(metadata->size, 3);
  EXPECT_EQ(metadata->total_chunks_put, 3);
  EXPECT_EQ(metadata->next_cursor, 3);
  EXPECT_EQ(metadata->final_seq, 2);
  EXPECT_EQ(metadata->max_seq, 2);
  EXPECT_GT(metadata->revision, 0);

  ASSERT_TRUE(DeleteStore(client, store->keys()).ok());
}

TEST(RedisChunkStoreTest, WaitsForWritesAndPropagatesCloseStatus) {
  absl::StatusOr<std::shared_ptr<redis::Client>> connected = ConnectForTest();
  if (!connected.ok())
    GTEST_SKIP() << "Redis is unavailable: " << connected.status();
  std::shared_ptr<redis::Client> client = *connected;
  RedisChunkStoreOptions options;
  options.key_prefix =
      "a11:test:" + std::to_string(absl::ToUnixNanos(absl::Now())) + ":";
  auto store = *RedisChunkStore::Create("redis-wait", client, options);

  a11::Future<data::NodeFragment> waiting =
      store->Get(1, absl::Now() + absl::Seconds(2));
  ASSERT_TRUE(store->Put(Fragment(0, "zero")).Await().ok());
  EXPECT_FALSE(waiting.IsReady());
  ASSERT_TRUE(store->Put(Fragment(1, "one", true)).Await().ok());
  absl::StatusOr<data::NodeFragment> fragment = waiting.Await();
  ASSERT_TRUE(fragment.ok()) << fragment.status();
  EXPECT_EQ(fragment->seq, 1);

  a11::Future<data::NodeFragment> missing =
      store->Get(7, absl::Now() + absl::Seconds(2));
  absl::Status terminal = absl::AbortedError("producer failed");
  absl::StatusOr<absl::Status> closed =
      store->CloseWritesWithStatus(terminal, false).Await();
  ASSERT_TRUE(closed.ok());
  EXPECT_EQ(closed->code(), absl::StatusCode::kAborted);
  EXPECT_EQ(missing.Await().status().code(), absl::StatusCode::kAborted);
  EXPECT_EQ(store->Put(Fragment(2, "late")).Await().status().code(),
            absl::StatusCode::kFailedPrecondition);
  absl::StatusOr<absl::Status> existing =
      store->CloseWritesWithStatus(absl::OkStatus(), true).Await();
  ASSERT_TRUE(existing.ok());
  EXPECT_EQ(existing->code(), absl::StatusCode::kAborted);

  ASSERT_TRUE(DeleteStore(client, store->keys()).ok());
}

TEST(RedisChunkStoreTest, ClearDataPreservesMetadataAndRemovesLargePayload) {
  absl::StatusOr<std::shared_ptr<redis::Client>> connected = ConnectForTest();
  if (!connected.ok())
    GTEST_SKIP() << "Redis is unavailable: " << connected.status();
  std::shared_ptr<redis::Client> client = *connected;
  RedisChunkStoreOptions options;
  options.key_prefix =
      "a11:test:" + std::to_string(absl::ToUnixNanos(absl::Now())) + ":";
  options.inline_data_threshold = 16;
  auto store = *RedisChunkStore::Create("redis-clear", client, options);
  data::ChunkMetadata metadata{
      .mimetype = "text/plain",
      .timestamp = std::nullopt,
      .attributes = {{"source", "test"}},
  };
  data::NodeFragment value{
      .data =
          data::Chunk{
              .metadata = metadata, .ref = {}, .data = std::string(512, 'a')},
      .seq = 0,
      .continued = false,
  };
  ASSERT_TRUE(store->Put(std::move(value)).Await().ok());

  absl::StatusOr<data::NodeFragment> original = store->ClearData(0).Await();
  ASSERT_TRUE(original.ok()) << original.status();
  EXPECT_EQ(std::get<data::Chunk>(original->data).data.size(), 512);
  absl::StatusOr<data::NodeFragment> cleared = store->Get(0).Await();
  ASSERT_TRUE(cleared.ok()) << cleared.status();
  const data::Chunk& tombstone = std::get<data::Chunk>(cleared->data);
  EXPECT_EQ(tombstone.ref, "__tombstone__");
  EXPECT_TRUE(tombstone.data.empty());
  EXPECT_EQ(tombstone.metadata, metadata);
  absl::StatusOr<redis::Reply> blob_count =
      client->Command({"HLEN", store->keys().blobs}).Await();
  ASSERT_TRUE(blob_count.ok()) << blob_count.status();
  EXPECT_EQ(*blob_count->AsInteger(), 0);

  ASSERT_TRUE(DeleteStore(client, store->keys()).ok());
}

TEST(RedisChunkStoreTest, UsesRawDataSizeForInlineThreshold) {
  absl::StatusOr<std::shared_ptr<redis::Client>> connected = ConnectForTest();
  if (!connected.ok())
    GTEST_SKIP() << "Redis is unavailable: " << connected.status();
  std::shared_ptr<redis::Client> client = *connected;
  RedisChunkStoreOptions options;
  options.key_prefix =
      "a11:test:" + std::to_string(absl::ToUnixNanos(absl::Now())) + ":";
  options.inline_data_threshold = 16;
  auto store = *RedisChunkStore::Create("redis-threshold", client, options);

  ASSERT_TRUE(store->Put(Fragment(0, std::string(16, 'a'))).Await().ok());
  ASSERT_TRUE(store->Put(Fragment(1, std::string(17, 'b'), true)).Await().ok());
  absl::StatusOr<redis::Reply> blob_count =
      client->Command({"HLEN", store->keys().blobs}).Await();
  ASSERT_TRUE(blob_count.ok()) << blob_count.status();
  EXPECT_EQ(*blob_count->AsInteger(), 1);

  ASSERT_TRUE(DeleteStore(client, store->keys()).ok());
}

TEST(RedisChunkStoreTest, MetadataRemainsReadableWhenChunkDataIsMissing) {
  absl::StatusOr<std::shared_ptr<redis::Client>> connected = ConnectForTest();
  if (!connected.ok())
    GTEST_SKIP() << "Redis is unavailable: " << connected.status();
  std::shared_ptr<redis::Client> client = *connected;
  RedisChunkStoreOptions options;
  options.key_prefix =
      "a11:test:" + std::to_string(absl::ToUnixNanos(absl::Now())) + ":";
  options.inline_data_threshold = 1;
  auto store = *RedisChunkStore::Create("redis-metadata", client, options);
  ASSERT_TRUE(store->Put(Fragment(0, "payload", true)).Await().ok());
  ASSERT_TRUE(client->Command({"HDEL", store->keys().blobs, "0"}).Await().ok());

  absl::StatusOr<RedisChunkStoreMetadata> metadata =
      store->GetMetadata().Await();
  ASSERT_TRUE(metadata.ok()) << metadata.status();
  EXPECT_EQ(metadata->size, 1);
  EXPECT_EQ(metadata->final_seq, 0);
  EXPECT_TRUE(absl::IsDataLoss(store->Get(0).Await().status()));

  ASSERT_TRUE(DeleteStore(client, store->keys()).ok());
}

TEST(RedisChunkStoreTest, ReservesS3ReferencesWithoutPretendingToReadThem) {
  absl::StatusOr<std::shared_ptr<redis::Client>> connected = ConnectForTest();
  if (!connected.ok())
    GTEST_SKIP() << "Redis is unavailable: " << connected.status();
  std::shared_ptr<redis::Client> client = *connected;
  RedisChunkStoreOptions options;
  options.key_prefix =
      "a11:test:" + std::to_string(absl::ToUnixNanos(absl::Now())) + ":";
  auto store = *RedisChunkStore::Create("redis-s3", client, options);
  ASSERT_TRUE(store->Put(Fragment(0, "placeholder")).Await().ok());

  absl::StatusOr<redis::Reply> old_id =
      client->Command({"HGET", store->keys().sequence_index, "0"}).Await();
  ASSERT_TRUE(old_id.ok()) << old_id.status();
  absl::StatusOr<std::string> old_id_text = old_id->AsString();
  ASSERT_TRUE(old_id_text.ok()) << old_id_text.status();
  absl::StatusOr<redis::Reply> new_id =
      client
          ->Command({"XADD", store->keys().stream, "*", "v", "1", "kind",
                     "chunk", "seq", "0", "arrival", "0", "storage", "s3",
                     "ref", "s3://bucket/object"})
          .Await();
  ASSERT_TRUE(new_id.ok()) << new_id.status();
  absl::StatusOr<std::string> new_id_text = new_id->AsString();
  ASSERT_TRUE(new_id_text.ok()) << new_id_text.status();
  ASSERT_TRUE(client
                  ->Command({"HSET", store->keys().sequence_index, "0",
                             std::move(*new_id_text)})
                  .Await()
                  .ok());
  ASSERT_TRUE(
      client->Command({"XDEL", store->keys().stream, std::move(*old_id_text)})
          .Await()
          .ok());

  EXPECT_TRUE(absl::IsUnimplemented(store->Get(0).Await().status()));
  EXPECT_TRUE(absl::IsUnimplemented(store->ClearData(0).Await().status()));
  EXPECT_TRUE(absl::IsUnimplemented(store->Next().Await().status()));
  absl::StatusOr<RedisChunkStoreMetadata> metadata =
      store->GetMetadata().Await();
  ASSERT_TRUE(metadata.ok()) << metadata.status();
  EXPECT_EQ(metadata->size, 1);

  ASSERT_TRUE(DeleteStore(client, store->keys()).ok());
}

TEST(RedisChunkStoreTest, RejectsCorruptMetadataBeforeWritingAnything) {
  absl::StatusOr<std::shared_ptr<redis::Client>> connected = ConnectForTest();
  if (!connected.ok())
    GTEST_SKIP() << "Redis is unavailable: " << connected.status();
  std::shared_ptr<redis::Client> client = *connected;
  RedisChunkStoreOptions options;
  options.key_prefix =
      "a11:test:" + std::to_string(absl::ToUnixNanos(absl::Now())) + ":";
  auto store = *RedisChunkStore::Create("redis-corrupt", client, options);
  ASSERT_TRUE(store->Initialize().Await().ok());
  ASSERT_TRUE(client->Command({"HSET", store->keys().metadata, "size", "-1"})
                  .Await()
                  .ok());

  EXPECT_TRUE(absl::IsDataLoss(
      store->Put(Fragment(0, "must-not-land")).Await().status()));
  absl::StatusOr<redis::Reply> stream_size =
      client->Command({"XLEN", store->keys().stream}).Await();
  absl::StatusOr<redis::Reply> index_size =
      client->Command({"HLEN", store->keys().sequence_index}).Await();
  ASSERT_TRUE(stream_size.ok()) << stream_size.status();
  ASSERT_TRUE(index_size.ok()) << index_size.status();
  EXPECT_EQ(*stream_size->AsInteger(), 0);
  EXPECT_EQ(*index_size->AsInteger(), 0);

  ASSERT_TRUE(DeleteStore(client, store->keys()).ok());
}

TEST(RedisChunkStoreTest, PreflightsRevisionAndStreamIdExhaustion) {
  absl::StatusOr<std::shared_ptr<redis::Client>> connected = ConnectForTest();
  if (!connected.ok())
    GTEST_SKIP() << "Redis is unavailable: " << connected.status();
  std::shared_ptr<redis::Client> client = *connected;
  RedisChunkStoreOptions options;
  options.key_prefix =
      "a11:test:" + std::to_string(absl::ToUnixNanos(absl::Now())) + ":";

  auto revision_store =
      *RedisChunkStore::Create("redis-revision", client, options);
  ASSERT_TRUE(revision_store->Initialize().Await().ok());
  ASSERT_TRUE(client
                  ->Command({"HSET", revision_store->keys().metadata,
                             "revision", "8589934593"})
                  .Await()
                  .ok());
  EXPECT_TRUE(absl::IsResourceExhausted(
      revision_store->CloseWritesWithStatus(absl::OkStatus(), false)
          .Await()
          .status()));
  absl::StatusOr<redis::Reply> closed =
      client->Command({"HGET", revision_store->keys().metadata, "closed"})
          .Await();
  absl::StatusOr<redis::Reply> revision_stream_size =
      client->Command({"XLEN", revision_store->keys().stream}).Await();
  ASSERT_TRUE(closed.ok()) << closed.status();
  ASSERT_TRUE(revision_stream_size.ok()) << revision_stream_size.status();
  EXPECT_EQ(*closed->AsString(), "0");
  EXPECT_EQ(*revision_stream_size->AsInteger(), 0);
  ASSERT_TRUE(DeleteStore(client, revision_store->keys()).ok());

  options.key_prefix += "stream:";
  auto stream_store =
      *RedisChunkStore::Create("redis-stream-id", client, options);
  ASSERT_TRUE(stream_store->Put(Fragment(0, "payload")).Await().ok());
  absl::StatusOr<redis::Reply> set_id =
      client
          ->Command(
              {"XSETID", stream_store->keys().stream, "18446744073709551615-0"})
          .Await();
  ASSERT_TRUE(set_id.ok()) << set_id.status();
  EXPECT_TRUE(
      absl::IsResourceExhausted(stream_store->ClearData(0).Await().status()));
  absl::StatusOr<data::NodeFragment> unchanged = stream_store->Get(0).Await();
  ASSERT_TRUE(unchanged.ok()) << unchanged.status();
  EXPECT_EQ(std::get<data::Chunk>(unchanged->data).data, "payload");
  ASSERT_TRUE(DeleteStore(client, stream_store->keys()).ok());
}

TEST(RedisChunkStoreTest, ConcurrentBatchesAndCloseCommitWholeBatches) {
  absl::StatusOr<std::shared_ptr<redis::Client>> connected = ConnectForTest();
  if (!connected.ok())
    GTEST_SKIP() << "Redis is unavailable: " << connected.status();
  std::shared_ptr<redis::Client> client = *connected;
  RedisChunkStoreOptions options;
  options.key_prefix =
      "a11:test:" + std::to_string(absl::ToUnixNanos(absl::Now())) + ":";
  auto store = *RedisChunkStore::Create("redis-race", client, options);

  constexpr size_t kBatchCount = 24;
  constexpr size_t kBatchSize = 4;
  std::vector<a11::Future<std::vector<std::uint32_t>>> writes;
  writes.reserve(kBatchCount);
  for (size_t batch = 0; batch < kBatchCount; ++batch) {
    std::vector<data::NodeFragment> fragments;
    fragments.reserve(kBatchSize);
    for (size_t index = 0; index < kBatchSize; ++index) {
      fragments.push_back(
          Fragment(std::nullopt, std::to_string(batch * kBatchSize + index)));
    }
    writes.push_back(store->PutMany(std::move(fragments)));
  }
  a11::Future<absl::Status> closing =
      store->CloseWritesWithStatus(absl::OkStatus(), false);

  size_t successful_chunks = 0;
  absl::flat_hash_set<std::uint32_t> assigned;
  for (const auto& write : writes) {
    absl::StatusOr<std::vector<std::uint32_t>> result = write.Await();
    if (!result.ok()) {
      EXPECT_TRUE(absl::IsFailedPrecondition(result.status()))
          << result.status();
      continue;
    }
    ASSERT_EQ(result->size(), kBatchSize);
    successful_chunks += result->size();
    for (std::uint32_t seq : *result)
      EXPECT_TRUE(assigned.insert(seq).second);
  }
  absl::StatusOr<absl::Status> closed_status = closing.Await();
  ASSERT_TRUE(closed_status.ok());
  EXPECT_TRUE(closed_status->ok());
  EXPECT_TRUE(absl::IsFailedPrecondition(
      store->Put(Fragment(std::nullopt, "late")).Await().status()));

  absl::StatusOr<RedisChunkStoreMetadata> metadata =
      store->GetMetadata().Await();
  ASSERT_TRUE(metadata.ok()) << metadata.status();
  EXPECT_TRUE(metadata->closed);
  EXPECT_EQ(metadata->size, successful_chunks);
  EXPECT_EQ(metadata->total_chunks_put, successful_chunks);
  EXPECT_EQ(assigned.size(), successful_chunks);
  for (std::uint64_t arrival = 0; arrival < successful_chunks; ++arrival) {
    absl::StatusOr<std::uint32_t> seq =
        store->GetSeqForArrivalOrder(arrival).Await();
    ASSERT_TRUE(seq.ok()) << seq.status();
    EXPECT_NE(assigned.find(*seq), assigned.end());
  }
  absl::StatusOr<redis::Reply> stream_size =
      client->Command({"XLEN", store->keys().stream}).Await();
  ASSERT_TRUE(stream_size.ok()) << stream_size.status();
  EXPECT_EQ(*stream_size->AsInteger(),
            static_cast<std::int64_t>(successful_chunks + 1));

  ASSERT_TRUE(DeleteStore(client, store->keys()).ok());
}

TEST(RedisChunkStoreTest, CancellingAWaitDoesNotConsumeFutureData) {
  absl::StatusOr<std::shared_ptr<redis::Client>> connected = ConnectForTest();
  if (!connected.ok())
    GTEST_SKIP() << "Redis is unavailable: " << connected.status();
  std::shared_ptr<redis::Client> client = *connected;
  RedisChunkStoreOptions options;
  options.key_prefix =
      "a11:test:" + std::to_string(absl::ToUnixNanos(absl::Now())) + ":";
  auto store = *RedisChunkStore::Create("redis-cancel", client, options);

  a11::Future<data::NodeFragment> waiting = store->Get(0);
  ASSERT_TRUE(waiting.Cancel().ok());
  absl::StatusOr<data::NodeFragment> cancelled =
      waiting.Await(absl::Now() + absl::Seconds(2));
  EXPECT_TRUE(absl::IsCancelled(cancelled.status())) << cancelled.status();

  ASSERT_TRUE(store->Put(Fragment(0, "available", true)).Await().ok());
  absl::StatusOr<data::NodeFragment> stored = store->Get(0).Await();
  ASSERT_TRUE(stored.ok()) << stored.status();
  EXPECT_EQ(std::get<data::Chunk>(stored->data).data, "available");

  ASSERT_TRUE(DeleteStore(client, store->keys()).ok());
}

}  // namespace
}  // namespace a11::stores
