// Copyright 2026 The A11 Authors.

#include "a11/stores/sqlite_chunk_store.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/data/types.h"
#include "temp_dir.h"
#include "thread/boost_primitives.h"

namespace a11::stores {
namespace {

using a11::testing::TempDir;

constexpr absl::Duration kTimeout = absl::Seconds(10);

absl::Time Soon() {
  return absl::Now() + kTimeout;
}

data::NodeFragment Fragment(std::uint32_t seq, bool final = false) {
  return data::NodeFragment{.data = data::Chunk{.data = std::to_string(seq)},
                            .seq = seq,
                            .continued = !final};
}

std::shared_ptr<SQLiteChunkStore> OpenStore(
    const TempDir& root, std::string node_id,
    SQLiteChunkStoreOptions options = {}) {
  absl::StatusOr<std::shared_ptr<SQLiteChunkStore>> created =
      SQLiteChunkStore::Create(std::move(node_id), root.string(),
                               std::move(options));
  EXPECT_TRUE(created.ok()) << created.status();
  return created.ok() ? *created : nullptr;
}

std::string ChunkData(const data::NodeFragment& fragment) {
  const auto* chunk = std::get_if<data::Chunk>(&fragment.data);
  return chunk == nullptr ? std::string() : chunk->data;
}

TEST(SQLiteChunkStoreTest, PreservesSequenceAndArrivalOrder) {
  TempDir root("a11-sqlite");
  std::shared_ptr<SQLiteChunkStore> store = OpenStore(root, "test");
  ASSERT_NE(store, nullptr);

  absl::StatusOr<std::vector<std::uint32_t>> sequences =
      store->PutMany({Fragment(2, true), Fragment(0), Fragment(1)})
          .Await(Soon());
  ASSERT_TRUE(sequences.ok()) << sequences.status();
  EXPECT_EQ(*sequences, (std::vector<std::uint32_t>{2, 0, 1}));
  EXPECT_EQ(*store->GetSeqForArrivalOrder(0).Await(), 2);
  EXPECT_EQ(*store->GetSeqForArrivalOrder(1).Await(), 0);

  const auto first = store->Next(Soon(), 3).Await();
  ASSERT_TRUE(first.ok()) << first.status();
  ASSERT_EQ(first->size(), 4);
  EXPECT_EQ((*first)[0]->seq, 0);
  EXPECT_EQ((*first)[1]->seq, 1);
  EXPECT_EQ((*first)[2]->seq, 2);
  // The final fragment is reported as not continued; the earlier two are.
  EXPECT_TRUE((*first)[0]->continued);
  EXPECT_FALSE((*first)[2]->continued);
  EXPECT_FALSE((*first)[3].has_value());

  const auto end = store->Next(Soon()).Await();
  ASSERT_TRUE(end.ok());
  ASSERT_EQ(end->size(), 1);
  EXPECT_FALSE(end->front().has_value());
}

TEST(SQLiteChunkStoreTest, PersistsAcrossReopen) {
  TempDir root("a11-sqlite");
  {
    std::shared_ptr<SQLiteChunkStore> store = OpenStore(root, "durable");
    ASSERT_NE(store, nullptr);
    ASSERT_TRUE(
        store->PutMany({Fragment(0), Fragment(1, true)}).Await(Soon()).ok());
  }
  // A fresh store object over the same root sees the committed rows, including
  // the shared cursor position and the declared final sequence.
  std::shared_ptr<SQLiteChunkStore> reopened = OpenStore(root, "durable");
  ASSERT_NE(reopened, nullptr);
  EXPECT_EQ(*reopened->Size().Await(), 2);
  EXPECT_EQ(*reopened->GetFinalSeq().Await(), 1);
  const auto fragment = reopened->Get(1, Soon()).Await();
  ASSERT_TRUE(fragment.ok()) << fragment.status();
  EXPECT_EQ(ChunkData(*fragment), "1");
  EXPECT_FALSE(fragment->continued);
}

TEST(SQLiteChunkStoreTest, ExternalizesPayloadsAboveTheInlineThreshold) {
  TempDir root("a11-sqlite");
  SQLiteChunkStoreOptions options;
  options.inline_data_threshold = 128 * 1024;
  std::shared_ptr<SQLiteChunkStore> store = OpenStore(root, "blobs", options);
  ASSERT_NE(store, nullptr);

  const std::string exactly_at(options.inline_data_threshold, 'a');
  const std::string just_over(options.inline_data_threshold + 1, 'b');
  ASSERT_TRUE(
      store
          ->PutMany({data::NodeFragment{.data = data::Chunk{.data = exactly_at},
                                        .seq = 0,
                                        .continued = true},
                     data::NodeFragment{.data = data::Chunk{.data = just_over},
                                        .seq = 1,
                                        .continued = true}})
          .Await(Soon())
          .ok());

  // The threshold is exclusive: only the larger payload leaves the row.
  size_t blob_count = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(root.path() / "blobs")) {
    if (entry.is_regular_file()) {
      ++blob_count;
    }
  }
  EXPECT_EQ(blob_count, 1);

  // Both round-trip identically regardless of where the bytes ended up.
  EXPECT_EQ(ChunkData(*store->Get(0, Soon()).Await()), exactly_at);
  EXPECT_EQ(ChunkData(*store->Get(1, Soon()).Await()), just_over);
}

TEST(SQLiteChunkStoreTest, ClearDataTombstonesAndRemovesTheBlobFile) {
  TempDir root("a11-sqlite");
  SQLiteChunkStoreOptions options;
  options.inline_data_threshold = 16;
  std::shared_ptr<SQLiteChunkStore> store = OpenStore(root, "clear", options);
  ASSERT_NE(store, nullptr);

  const std::string payload(1024, 'x');
  ASSERT_TRUE(store
                  ->Put(data::NodeFragment{.data = data::Chunk{.data = payload},
                                           .seq = 0,
                                           .continued = true})
                  .Await(Soon())
                  .ok());
  ASSERT_EQ(
      std::distance(std::filesystem::directory_iterator(root.path() / "blobs"),
                    std::filesystem::directory_iterator()),
      1);

  const auto cleared = store->ClearData(0).Await();
  ASSERT_TRUE(cleared.ok()) << cleared.status();
  // ClearData returns the fragment as it was, not the tombstone.
  EXPECT_EQ(ChunkData(*cleared), payload);

  // The slot survives, so Size() is unchanged, but the payload is gone.
  EXPECT_EQ(*store->Size().Await(), 1);
  EXPECT_EQ(
      std::distance(std::filesystem::directory_iterator(root.path() / "blobs"),
                    std::filesystem::directory_iterator()),
      0);
  const auto after = store->Get(0, Soon()).Await();
  ASSERT_TRUE(after.ok()) << after.status();
  EXPECT_TRUE(ChunkData(*after).empty());
}

TEST(SQLiteChunkStoreTest, StoresNodeReferencesAndFindsReferrersByIndex) {
  TempDir root("a11-sqlite");
  std::shared_ptr<SQLiteChunkStore> target = OpenStore(root, "target");
  std::shared_ptr<SQLiteChunkStore> referrer = OpenStore(root, "referrer");
  ASSERT_NE(target, nullptr);
  ASSERT_NE(referrer, nullptr);

  // Unlike the in-memory and Redis backends, a NodeRef is accepted here.
  ASSERT_TRUE(
      referrer
          ->Put(data::NodeFragment{
              .data = data::NodeRef{.id = "target", .offset = 8, .length = 64},
              .seq = 0,
              .continued = true})
          .Await(Soon())
          .ok());

  const auto stored = referrer->Get(0, Soon()).Await();
  ASSERT_TRUE(stored.ok()) << stored.status();
  const auto* node_ref = std::get_if<data::NodeRef>(&stored->data);
  ASSERT_NE(node_ref, nullptr);
  EXPECT_EQ(node_ref->id, "target");
  EXPECT_EQ(node_ref->offset, 8);
  ASSERT_TRUE(node_ref->length.has_value());
  EXPECT_EQ(*node_ref->length, 64);

  // The traversal the relational layout exists for.
  const auto referrers = target->FindReferrers(10).Await();
  ASSERT_TRUE(referrers.ok()) << referrers.status();
  ASSERT_EQ(referrers->size(), 1);
  EXPECT_EQ(referrers->front().id, "referrer");

  // A tombstone is Chunk-shaped, so clearing a NodeRef is refused rather than
  // silently changing the payload's type.
  EXPECT_EQ(referrer->ClearData(0).Await().status().code(),
            absl::StatusCode::kUnimplemented);
}

TEST(SQLiteChunkStoreTest, RoundTripsMetadataExactly) {
  TempDir root("a11-sqlite");
  std::shared_ptr<SQLiteChunkStore> store = OpenStore(root, "metadata");
  ASSERT_NE(store, nullptr);

  const absl::Time stamp = absl::FromUnixMicros(1'700'000'000'000'123);
  data::ChunkMetadata with_stamp{.mimetype = "audio/wav", .timestamp = stamp};
  with_stamp.attributes.emplace("binary", std::string("\x00\x01\xff", 3));
  data::ChunkMetadata without_stamp{.mimetype = "text/plain"};

  ASSERT_TRUE(store
                  ->PutMany({data::NodeFragment{
                                 .data = data::Chunk{.metadata = with_stamp,
                                                     .data = "one"},
                                 .seq = 0,
                                 .continued = true},
                             data::NodeFragment{
                                 .data = data::Chunk{.metadata = without_stamp,
                                                     .data = "two"},
                                 .seq = 1,
                                 .continued = true},
                             data::NodeFragment{
                                 .data = data::Chunk{.data = "three"},
                                 .seq = 2,
                                 .continued = true}})
                  .Await(Soon())
                  .ok());

  const auto first = store->Get(0, Soon()).Await();
  ASSERT_TRUE(first.ok()) << first.status();
  const auto* chunk = std::get_if<data::Chunk>(&first->data);
  ASSERT_NE(chunk, nullptr);
  ASSERT_TRUE(chunk->metadata.has_value());
  EXPECT_EQ(*chunk->metadata, with_stamp);

  // A metadata block without a timestamp must not gain one, even though the
  // timestamp column is always populated so it can be indexed.
  const auto second = store->Get(1, Soon()).Await();
  ASSERT_TRUE(second.ok());
  const auto* second_chunk = std::get_if<data::Chunk>(&second->data);
  ASSERT_TRUE(second_chunk->metadata.has_value());
  EXPECT_FALSE(second_chunk->metadata->timestamp.has_value());
  EXPECT_EQ(*second_chunk->metadata, without_stamp);

  // Absent metadata stays absent rather than becoming an empty block.
  const auto third = store->Get(2, Soon()).Await();
  ASSERT_TRUE(third.ok());
  EXPECT_FALSE(std::get_if<data::Chunk>(&third->data)->metadata.has_value());
}

TEST(SQLiteChunkStoreTest, EnforcesPutManyValidationOrder) {
  TempDir root("a11-sqlite");
  std::shared_ptr<SQLiteChunkStore> store = OpenStore(root, "validate");
  ASSERT_NE(store, nullptr);

  EXPECT_EQ(store->PutMany({Fragment(0), Fragment(0)}).Await().status().code(),
            absl::StatusCode::kInvalidArgument);

  std::vector<data::NodeFragment> mixed{Fragment(0), Fragment(1)};
  mixed[1].seq.reset();
  EXPECT_EQ(store->PutMany(std::move(mixed)).Await().status().code(),
            absl::StatusCode::kInvalidArgument);

  ASSERT_TRUE(store->PutMany({Fragment(0)}).Await().ok());
  EXPECT_EQ(store->PutMany({Fragment(0)}).Await().status().code(),
            absl::StatusCode::kAlreadyExists);

  EXPECT_EQ(store->PutMany({Fragment(1, true), Fragment(2, true)})
                .Await()
                .status()
                .code(),
            absl::StatusCode::kInvalidArgument);

  // Implicit sequences continue from put_count rather than from max_seq.
  const auto implicit =
      store
          ->PutMany({data::NodeFragment{.data = data::Chunk{.data = "next"},
                                        .continued = true}})
          .Await(Soon());
  ASSERT_TRUE(implicit.ok()) << implicit.status();
  EXPECT_EQ(implicit->front(), 1);
}

TEST(SQLiteChunkStoreTest, RejectsWritesAfterCloseAndPreservesTheStatus) {
  TempDir root("a11-sqlite");
  std::shared_ptr<SQLiteChunkStore> store = OpenStore(root, "closing");
  ASSERT_NE(store, nullptr);

  const auto closed =
      store->CloseWritesWithStatus(absl::DataLossError("producer failed"))
          .Await();
  ASSERT_TRUE(closed.ok()) << closed.status();
  EXPECT_EQ(closed->code(), absl::StatusCode::kDataLoss);

  EXPECT_EQ(store->PutMany({Fragment(0)}).Await().status().code(),
            absl::StatusCode::kFailedPrecondition);
  // An empty batch reports INVALID_ARGUMENT rather than FAILED_PRECONDITION,
  // because the all-or-none sequence rule is evaluated before any state is
  // consulted and an empty batch trivially fails it (no seq is set, yet
  // "every" seq vacuously is). LocalChunkStore behaves identically, which
  // makes its own empty-batch branch unreachable. Matching that exactly
  // matters more than the rule being pretty: the two backends share a
  // conformance suite.
  EXPECT_EQ(store->PutMany({}).Await().status().code(),
            absl::StatusCode::kInvalidArgument);

  // A second close reports the recorded status as a value...
  const auto again =
      store->CloseWritesWithStatus(absl::OkStatus(), true).Await();
  ASSERT_TRUE(again.ok()) << again.status();
  EXPECT_EQ(again->code(), absl::StatusCode::kDataLoss);
  // ...or fails outright when the caller did not ask for it.
  EXPECT_EQ(store->CloseWritesWithStatus(absl::OkStatus(), false)
                .Await()
                .status()
                .code(),
            absl::StatusCode::kFailedPrecondition);

  // A read that can never be satisfied resolves with the terminal status.
  EXPECT_EQ(store->Get(3, Soon()).Await().status().code(),
            absl::StatusCode::kDataLoss);
}

TEST(SQLiteChunkStoreTest, CloseWakesBlockedRead) {
  TempDir root("a11-sqlite");
  std::shared_ptr<SQLiteChunkStore> store = OpenStore(root, "wake");
  ASSERT_NE(store, nullptr);

  a11::Future<data::NodeFragment> pending = store->Get(7, Soon());
  ASSERT_TRUE(
      store->CloseWritesWithStatus(absl::DataLossError("failed")).Await().ok());
  EXPECT_EQ(pending.Await().status().code(), absl::StatusCode::kDataLoss);
}

TEST(SQLiteChunkStoreTest, WriteWakesBlockedReadWithoutPolling) {
  TempDir root("a11-sqlite");
  std::shared_ptr<SQLiteChunkStore> store = OpenStore(root, "race");
  ASSERT_NE(store, nullptr);

  // Repeated so the writer sometimes commits before the reader parks and
  // sometimes after: the lost-wakeup window is exactly that interleaving.
  for (int attempt = 0; attempt < 64; ++attempt) {
    std::shared_ptr<SQLiteChunkStore> node =
        OpenStore(root, absl::StrCat("race-", attempt));
    ASSERT_NE(node, nullptr);
    a11::Future<data::NodeFragment> pending = node->Get(0, Soon());
    if (attempt % 2 == 0) {
      thread::SleepFor(absl::Microseconds(50));
    }
    ASSERT_TRUE(node->Put(Fragment(0, true)).Await(Soon()).ok());
    const auto received = pending.Await(Soon());
    ASSERT_TRUE(received.ok())
        << "attempt " << attempt << ": " << received.status();
    EXPECT_EQ(ChunkData(*received), "0");
  }
}

TEST(SQLiteChunkStoreTest, CancellingAWaitDoesNotConsumeFutureData) {
  TempDir root("a11-sqlite");
  std::shared_ptr<SQLiteChunkStore> store = OpenStore(root, "cancel");
  ASSERT_NE(store, nullptr);

  a11::Future<data::NodeFragment> pending = store->Get(0, Soon());
  thread::SleepFor(absl::Milliseconds(5));
  ASSERT_TRUE(pending.Cancel().ok());
  EXPECT_EQ(pending.Await().status().code(), absl::StatusCode::kCancelled);

  // The cancelled read must not have eaten the value the writer then produces.
  ASSERT_TRUE(store->Put(Fragment(0, true)).Await(Soon()).ok());
  const auto received = store->Get(0, Soon()).Await();
  ASSERT_TRUE(received.ok()) << received.status();
  EXPECT_EQ(ChunkData(*received), "0");
}

TEST(SQLiteChunkStoreTest, HonorsAnElapsedDeadline) {
  TempDir root("a11-sqlite");
  std::shared_ptr<SQLiteChunkStore> store = OpenStore(root, "deadline");
  ASSERT_NE(store, nullptr);

  EXPECT_EQ(
      store->Get(0, absl::Now() - absl::Seconds(1)).Await().status().code(),
      absl::StatusCode::kDeadlineExceeded);
  EXPECT_EQ(store->Next(absl::Now() + absl::Milliseconds(50), 1)
                .Await()
                .status()
                .code(),
            absl::StatusCode::kDeadlineExceeded);
  EXPECT_EQ(store->Next(Soon(), 0).Await().status().code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(SQLiteChunkStoreTest, NextReturnsPartialBatchesRatherThanLosingThem) {
  TempDir root("a11-sqlite");
  std::shared_ptr<SQLiteChunkStore> store = OpenStore(root, "partial");
  ASSERT_NE(store, nullptr);

  ASSERT_TRUE(store->PutMany({Fragment(0), Fragment(1)}).Await(Soon()).ok());
  // Asks for more than exists; the deadline elapses at the gap, and the two
  // fragments already collected outrank the timeout.
  const auto batch =
      store->Next(absl::Now() + absl::Milliseconds(100), 5).Await();
  ASSERT_TRUE(batch.ok()) << batch.status();
  EXPECT_EQ(batch->size(), 2);
}

TEST(SQLiteChunkStoreTest, RollsBackWholeBatchesAndLeavesNoOrphanBlobs) {
  TempDir root("a11-sqlite");
  SQLiteChunkStoreOptions options;
  options.inline_data_threshold = 8;
  std::shared_ptr<SQLiteChunkStore> store = OpenStore(root, "atomic", options);
  ASSERT_NE(store, nullptr);

  ASSERT_TRUE(store->Put(Fragment(0)).Await(Soon()).ok());

  // The second fragment collides, so the whole batch must fail and the blob
  // written for the first must be unlinked rather than left for the sweeper.
  const std::string payload(4096, 'z');
  const auto rejected =
      store
          ->PutMany({data::NodeFragment{.data = data::Chunk{.data = payload},
                                        .seq = 5,
                                        .continued = true},
                     Fragment(0)})
          .Await(Soon());
  EXPECT_EQ(rejected.status().code(), absl::StatusCode::kAlreadyExists);

  EXPECT_EQ(*store->Size().Await(), 1);
  EXPECT_EQ(store->Get(5, absl::Now() + absl::Milliseconds(50))
                .Await()
                .status()
                .code(),
            absl::StatusCode::kDeadlineExceeded);
  EXPECT_EQ(
      std::distance(std::filesystem::directory_iterator(root.path() / "blobs"),
                    std::filesystem::directory_iterator()),
      0);
}

TEST(SQLiteChunkStoreTest, ConcurrentBatchesAndCloseCommitWholeBatches) {
  TempDir root("a11-sqlite");
  std::shared_ptr<SQLiteChunkStore> store = OpenStore(root, "concurrent");
  ASSERT_NE(store, nullptr);

  constexpr int kBatches = 24;
  constexpr int kPerBatch = 4;
  std::vector<a11::Future<std::vector<std::uint32_t>>> writes;
  writes.reserve(kBatches);
  for (int batch = 0; batch < kBatches; ++batch) {
    std::vector<data::NodeFragment> fragments;
    fragments.reserve(kPerBatch);
    for (int index = 0; index < kPerBatch; ++index) {
      fragments.push_back(data::NodeFragment{
          .data = data::Chunk{.data = absl::StrCat(batch, ":", index)},
          .continued = true});
    }
    writes.push_back(store->PutMany(std::move(fragments)));
  }
  a11::Future<absl::Status> closing =
      store->CloseWritesWithStatus(absl::OkStatus());

  // Every batch either committed in full or was rejected because the close won
  // the race. Nothing in between, and no sequence handed out twice.
  std::vector<std::uint32_t> all;
  int committed_batches = 0;
  for (auto& write : writes) {
    const auto result = write.Await(Soon());
    if (!result.ok()) {
      EXPECT_EQ(result.status().code(), absl::StatusCode::kFailedPrecondition)
          << result.status();
      continue;
    }
    EXPECT_EQ(result->size(), kPerBatch);
    ++committed_batches;
    all.insert(all.end(), result->begin(), result->end());
  }
  ASSERT_TRUE(closing.Await(Soon()).ok());

  std::sort(all.begin(), all.end());
  EXPECT_EQ(std::adjacent_find(all.begin(), all.end()), all.end())
      << "a sequence number was handed out twice";
  EXPECT_EQ(all.size(), static_cast<size_t>(committed_batches) * kPerBatch);
  EXPECT_EQ(*store->Size().Await(), all.size());
}

TEST(SQLiteChunkStoreTest, ReportsMetadataAndSharesOneDatabasePerRoot) {
  TempDir root("a11-sqlite");
  SQLiteChunkStoreOptions options;
  options.owner_id = "owner";
  absl::StatusOr<std::shared_ptr<SQLiteChunkStoreFactory>> factory =
      SQLiteChunkStoreFactory::Create(root.string(), options);
  ASSERT_TRUE(factory.ok()) << factory.status();

  absl::StatusOr<std::shared_ptr<SQLiteChunkStore>> store =
      (*factory)->Open("described");
  ASSERT_TRUE(store.ok()) << store.status();
  ASSERT_TRUE(
      (*store)->PutMany({Fragment(0), Fragment(1, true)}).Await(Soon()).ok());

  const auto metadata = (*store)->GetMetadata().Await();
  ASSERT_TRUE(metadata.ok()) << metadata.status();
  EXPECT_EQ(metadata->id, "described");
  EXPECT_EQ(metadata->owner_id, "owner");
  EXPECT_FALSE(metadata->closed);
  EXPECT_EQ(metadata->size, 2);
  EXPECT_EQ(metadata->total_chunks_put, 2);
  EXPECT_EQ(metadata->final_seq, 1);
  EXPECT_EQ(metadata->max_seq, 1);
  EXPECT_GT(metadata->revision, 0);

  // A second factory over the same directory reuses one database, so a store
  // it creates observes the first factory's committed writes immediately.
  absl::StatusOr<std::shared_ptr<SQLiteChunkStoreFactory>> second =
      SQLiteChunkStoreFactory::Create(root.string(), options);
  ASSERT_TRUE(second.ok()) << second.status();
  EXPECT_EQ((*second)->root(), (*factory)->root());
  absl::StatusOr<std::shared_ptr<SQLiteChunkStore>> alias =
      (*second)->Open("described");
  ASSERT_TRUE(alias.ok());
  EXPECT_EQ(*(*alias)->Size().Await(), 2);
}

TEST(SQLiteChunkStoreTest, ValidatesOptionsAndIds) {
  TempDir root("a11-sqlite");
  SQLiteChunkStoreOptions options;
  options.cross_process_poll_interval = -absl::Seconds(1);
  EXPECT_EQ(options.Validate().code(), absl::StatusCode::kInvalidArgument);

  EXPECT_EQ(SQLiteChunkStore::Create("", root.string()).status().code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(SQLiteChunkStoreFactory::Create("").status().code(),
            absl::StatusCode::kInvalidArgument);
}

TEST(SQLiteChunkStoreTest, SweepsOrphanBlobsButSparesFreshOnes) {
  TempDir root("a11-sqlite");
  SQLiteChunkStoreOptions options;
  options.inline_data_threshold = 8;
  // Zero grace so the sweep considers everything already old enough.
  options.blob_grace_period = absl::ZeroDuration();
  absl::StatusOr<std::shared_ptr<SQLiteChunkStoreFactory>> factory =
      SQLiteChunkStoreFactory::Create(root.string(), options);
  ASSERT_TRUE(factory.ok()) << factory.status();
  absl::StatusOr<std::shared_ptr<SQLiteChunkStore>> store =
      (*factory)->Open("sweep");
  ASSERT_TRUE(store.ok());

  const std::string payload(4096, 'q');
  ASSERT_TRUE((*store)
                  ->Put(data::NodeFragment{.data = data::Chunk{.data = payload},
                                           .seq = 0,
                                           .continued = true})
                  .Await(Soon())
                  .ok());

  // A file nobody references, as a crash between rename and commit would leave.
  const std::filesystem::path orphan =
      root.path() / "blobs" / "00000000-0000-4000-8000-000000000000";
  { std::ofstream(orphan) << "garbage"; }

  const auto removed = (*factory)->SweepOrphanBlobs().Await();
  ASSERT_TRUE(removed.ok()) << removed.status();
  EXPECT_EQ(*removed, 1);
  EXPECT_FALSE(std::filesystem::exists(orphan));
  // The referenced payload is untouched and still readable.
  EXPECT_EQ(ChunkData(*(*store)->Get(0, Soon()).Await()), payload);
}

TEST(SQLiteChunkStoreTest, ReportsMissingBlobsAsDataLoss) {
  TempDir root("a11-sqlite");
  SQLiteChunkStoreOptions options;
  options.inline_data_threshold = 8;
  std::shared_ptr<SQLiteChunkStore> store = OpenStore(root, "corrupt", options);
  ASSERT_NE(store, nullptr);

  ASSERT_TRUE(store
                  ->Put(data::NodeFragment{
                      .data = data::Chunk{.data = std::string(4096, 'c')},
                      .seq = 0,
                      .continued = true})
                  .Await(Soon())
                  .ok());

  // Delete the payload behind the store's back, as a botched manual cleanup
  // would. The row still exists, so this must be reported, not papered over.
  for (const auto& entry :
       std::filesystem::directory_iterator(root.path() / "blobs")) {
    std::filesystem::remove(entry.path());
  }
  EXPECT_EQ(store->Get(0, Soon()).Await().status().code(),
            absl::StatusCode::kDataLoss);
}

}  // namespace
}  // namespace a11::stores
