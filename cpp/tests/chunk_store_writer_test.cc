// Copyright 2026 The A11 Authors.

#include "a11/stores/chunk_store_writer.h"

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/data/types.h"
#include "a11/net/in_process_wire_stream.h"
#include "a11/net/wire_stream.h"
#include "a11/net/wire_stream_with_recv.h"
#include "a11/stores/chunk_store.h"
#include "a11/stores/local_chunk_store.h"
#include "thread/boost_primitives.h"
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

  [[nodiscard]] const std::vector<absl::Status>& close_statuses() const {
    return close_statuses_;
  }

 private:
  const std::shared_ptr<ChunkStore> store_;
  const absl::Status failure_;
  std::vector<absl::Status> close_statuses_;
};

/**
 * A stream whose Send blocks until the test releases it.
 *
 * Stands in for a transport applying backpressure: ChannelWireStream writes a
 * claimed message inline, and HttpTransport::PostWrite waits rather than posts
 * once its queued bytes reach the bound.
 */
class GatedSendStream final : public net::WireStream {
 public:
  absl::Status Send(data::WireMessage message) override {
    {
      thread::MutexLock lock(&mu_);
      ++entered_;
      messages_.push_back(std::move(message));
    }
    return gate_.future().Await(absl::Now() + absl::Seconds(30)).status();
  }

  void Release() { gate_.SetValue(a11::Unit{}).IgnoreError(); }

  [[nodiscard]] size_t entered() const {
    thread::MutexLock lock(&mu_);
    return entered_;
  }

  [[nodiscard]] std::vector<data::WireMessage> messages() const {
    thread::MutexLock lock(&mu_);
    return messages_;
  }

  a11::Task Start(net::OnMessage, net::OnDone) override {
    return a11::ReadyTask();
  }

  a11::Task Accept(net::OnMessage, net::OnDone) override {
    return a11::ReadyTask();
  }

  absl::Status HalfClose(data::ByteMap) override { return absl::OkStatus(); }

  a11::Task DrainOutgoingMessages() override { return a11::ReadyTask(); }

  absl::Status Abort(absl::Status) override { return absl::OkStatus(); }

  absl::Status SetDeadline(absl::Time) override { return absl::OkStatus(); }

  [[nodiscard]] absl::Time deadline() const override {
    return absl::InfiniteFuture();
  }

  [[nodiscard]] absl::Status GetStatus() const override {
    return absl::OkStatus();
  }

  [[nodiscard]] std::optional<data::ByteMap> GetTrailers() const override {
    return std::nullopt;
  }

  [[nodiscard]] std::string GetId() const override { return "gated-stream"; }

  [[nodiscard]] void* absl_nullable GetImpl() const override { return nullptr; }

 private:
  mutable thread::Mutex mu_;
  size_t entered_ ABSL_GUARDED_BY(mu_) = 0;
  std::vector<data::WireMessage> messages_ ABSL_GUARDED_BY(mu_);
  a11::Promise<a11::Unit> gate_;
};

/// A stream whose every Send fails, standing in for an unreachable peer.
class FailingSendStream final : public net::WireStream {
 public:
  explicit FailingSendStream(absl::Status failure)
      : failure_(std::move(failure)) {}

  absl::Status Send(data::WireMessage) override { return failure_; }

  a11::Task Start(net::OnMessage, net::OnDone) override {
    return a11::ReadyTask();
  }

  a11::Task Accept(net::OnMessage, net::OnDone) override {
    return a11::ReadyTask();
  }

  absl::Status HalfClose(data::ByteMap) override { return absl::OkStatus(); }

  a11::Task DrainOutgoingMessages() override { return a11::ReadyTask(); }

  absl::Status Abort(absl::Status) override { return absl::OkStatus(); }

  absl::Status SetDeadline(absl::Time) override { return absl::OkStatus(); }

  [[nodiscard]] absl::Time deadline() const override {
    return absl::InfiniteFuture();
  }

  [[nodiscard]] absl::Status GetStatus() const override { return failure_; }

  [[nodiscard]] std::optional<data::ByteMap> GetTrailers() const override {
    return std::nullopt;
  }

  [[nodiscard]] std::string GetId() const override {
    return "failing-send-stream";
  }

  [[nodiscard]] void* absl_nullable GetImpl() const override { return nullptr; }

 private:
  const absl::Status failure_;
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

TEST(ChunkStoreWriterTest, StickyMimetypeCompressesOnlyContiguousWrites) {
  auto store = *LocalChunkStore::Create("writer-sticky-mimetype");
  auto writer = *ChunkStoreWriter::Create(
      store, ChunkStoreWriterOptions{.sticky_mimetype = true});
  const auto chunk = [](std::string value, bool with_details = false) {
    return data::Chunk{
        .metadata =
            data::ChunkMetadata{
                .mimetype = "text/plain",
                .timestamp = with_details ? std::optional(absl::UnixEpoch())
                                          : std::nullopt,
                .attributes = with_details
                                  ? data::ByteMap{{"role", "assistant"}}
                                  : data::ByteMap{}},
        .data = std::move(value),
    };
  };

  auto first = writer->PutChunk(chunk("first"));
  auto gap_anchor = writer->PutChunk(chunk("gap-anchor"), 3);
  auto detailed = writer->PutChunk(chunk("detailed", true), 4);
  auto stripped = writer->PutChunk(chunk("stripped"), 5);
  auto second_gap_anchor =
      writer->PutChunk(chunk("second-gap-anchor"), 7, true);
  EXPECT_EQ(*first.Await(), 0);
  EXPECT_EQ(*gap_anchor.Await(), 3);
  EXPECT_EQ(*detailed.Await(), 4);
  EXPECT_EQ(*stripped.Await(), 5);
  EXPECT_EQ(*second_gap_anchor.Await(), 7);
  ASSERT_TRUE(writer->DrainAndClose().Await().ok());

  auto stored_first = store->Get(0).Await();
  auto stored_gap_anchor = store->Get(3).Await();
  auto stored_detailed = store->Get(4).Await();
  auto stored_stripped = store->Get(5).Await();
  auto stored_second_gap_anchor = store->Get(7).Await();
  ASSERT_TRUE(stored_first.ok());
  ASSERT_TRUE(stored_gap_anchor.ok());
  ASSERT_TRUE(stored_detailed.ok());
  ASSERT_TRUE(stored_stripped.ok());
  ASSERT_TRUE(stored_second_gap_anchor.ok());
  const auto& first_chunk = std::get<data::Chunk>(stored_first->data);
  const auto& gap_chunk = std::get<data::Chunk>(stored_gap_anchor->data);
  const auto& detailed_chunk = std::get<data::Chunk>(stored_detailed->data);
  const auto& stripped_chunk = std::get<data::Chunk>(stored_stripped->data);
  const auto& second_gap_chunk =
      std::get<data::Chunk>(stored_second_gap_anchor->data);
  EXPECT_EQ(first_chunk.GetMimetype(), "text/plain");
  EXPECT_EQ(gap_chunk.GetMimetype(), "text/plain");
  ASSERT_TRUE(detailed_chunk.metadata.has_value());
  EXPECT_TRUE(detailed_chunk.metadata->mimetype.empty());
  EXPECT_TRUE(detailed_chunk.metadata->timestamp.has_value());
  EXPECT_EQ(*detailed_chunk.metadata->GetAttribute("role"), "assistant");
  EXPECT_FALSE(stripped_chunk.metadata.has_value());
  EXPECT_EQ(second_gap_chunk.GetMimetype(), "text/plain");
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
  ASSERT_TRUE(pair.first
                  ->Start(
                      [](const std::optional<data::WireMessage>&) {
                        return a11::ReadyTask();
                      },
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

TEST(ChunkStoreWriterTest, DrainTeesAClosureMarkerAfterTheData) {
  auto pair = *net::InProcessWireStream::CreatePair();
  auto receiver = *net::WireStreamWithRecv::Create(pair.second);
  ASSERT_TRUE(pair.first
                  ->Start(
                      [](const std::optional<data::WireMessage>&) {
                        return a11::ReadyTask();
                      },
                      [] { return a11::ReadyTask(); })
                  .Await()
                  .ok());
  ASSERT_TRUE(receiver->Accept().Await().ok());

  auto store = *LocalChunkStore::Create("writer-close-tee");
  auto writer = *ChunkStoreWriter::Create(store);
  ASSERT_TRUE(writer->AttachStream(pair.first).ok());
  // Omit a final fragment so the peer receives only the closure marker.
  ASSERT_EQ(*writer->PutChunk(data::Chunk{.data = "sent"}).Await(), 0);
  EXPECT_TRUE(writer->DrainAndClose().Await().ok());

  const absl::Time deadline = absl::Now() + absl::Seconds(5);
  auto data_message = receiver->Receive().Await(deadline);
  ASSERT_TRUE(data_message.ok()) << data_message.status();
  ASSERT_TRUE(data_message->has_value());
  ASSERT_EQ((**data_message).node_fragments.size(), 1);
  const data::Chunk* data_chunk =
      std::get_if<data::Chunk>(&(**data_message).node_fragments.front().data);
  ASSERT_NE(data_chunk, nullptr);
  EXPECT_EQ(data_chunk->data, "sent");
  EXPECT_FALSE(data::IsCloseStatusChunk(*data_chunk));

  auto close_message = receiver->Receive().Await(deadline);
  ASSERT_TRUE(close_message.ok()) << close_message.status();
  ASSERT_TRUE(close_message->has_value());
  ASSERT_EQ((**close_message).node_fragments.size(), 1);
  const data::NodeFragment& marker = (**close_message).node_fragments.front();
  EXPECT_EQ(marker.id, "writer-close-tee");
  EXPECT_FALSE(marker.continued);
  const data::Chunk* marker_chunk = std::get_if<data::Chunk>(&marker.data);
  ASSERT_NE(marker_chunk, nullptr);
  ASSERT_TRUE(data::IsCloseStatusChunk(*marker_chunk));
  const absl::StatusOr<absl::Status> closed =
      data::StatusFromStatusChunk(*marker_chunk);
  ASSERT_TRUE(closed.ok()) << closed.status();
  EXPECT_TRUE(closed->ok());

  ASSERT_TRUE(pair.first->HalfClose().ok());
  ASSERT_TRUE(receiver->HalfClose().ok());
  const absl::Time shutdown_deadline = absl::Now() + absl::Seconds(5);
  ASSERT_TRUE(pair.first->Done().Await(shutdown_deadline).ok());
  ASSERT_TRUE(pair.second->Done().Await(shutdown_deadline).ok());
}

TEST(ChunkStoreWriterTest, ABlockedTeeDoesNotHoldUpTheNextBatch) {
  // Sixteen chunks against the default eight-per-batch limit: two batches, so
  // the second has to be written while the first batch's tee is still out.
  auto store = *LocalChunkStore::Create("writer-tee-head-of-line");
  auto writer = *ChunkStoreWriter::Create(store);
  auto gated = std::make_shared<GatedSendStream>();
  ASSERT_TRUE(writer->AttachStream(gated).ok());

  std::vector<a11::Future<std::uint32_t>> confirmations;
  confirmations.reserve(16);
  for (int index = 0; index < 16; ++index) {
    ChunkStoreWrite write = writer->EnqueueChunk(
        data::Chunk{.data = absl::StrCat("chunk-", index)}, std::nullopt,
        /*final=*/index == 15, /*ensure_started=*/false);
    ASSERT_TRUE(write.admitted.Await().ok());
    confirmations.push_back(std::move(write.confirmation));
  }
  writer->Flush();

  const absl::Time deadline = absl::Now() + absl::Seconds(10);
  // The last chunk of the second batch has its sequence while the first
  // batch's tee is blocked. This is the stall FINDINGS.md measured: the writer
  // used to hold its operation across the tee, so nothing after the first
  // eight could be written at all.
  const absl::StatusOr<std::uint32_t> last = confirmations.back().Await(deadline);
  ASSERT_TRUE(last.ok()) << last.status();
  EXPECT_EQ(*last, 15);
  EXPECT_GE(gated->entered(), 1U);
  for (size_t index = 0; index < confirmations.size(); ++index) {
    const absl::StatusOr<std::uint32_t> confirmed =
        confirmations[index].Await(deadline);
    ASSERT_TRUE(confirmed.ok()) << index << ": " << confirmed.status();
    EXPECT_EQ(*confirmed, index);
  }
  EXPECT_EQ(*store->Size().Await(deadline), 16);

  gated->Release();
  // Both batches reach the stream, and every fragment with it.
  const absl::Status closed = writer->DrainAndClose().Await(deadline).status();
  ASSERT_TRUE(closed.ok()) << closed;
  size_t fragments = 0;
  bool marker_last = false;
  const std::vector<data::WireMessage> sent = gated->messages();
  for (size_t index = 0; index < sent.size(); ++index) {
    for (const data::NodeFragment& fragment : sent[index].node_fragments) {
      const data::Chunk* chunk = std::get_if<data::Chunk>(&fragment.data);
      ASSERT_NE(chunk, nullptr);
      const bool is_marker = data::IsCloseStatusChunk(*chunk);
      marker_last = is_marker;
      if (!is_marker) {
        ++fragments;
      }
    }
  }
  EXPECT_EQ(fragments, 16);
  // The closure marker still follows the data it closes.
  EXPECT_TRUE(marker_last);
}

TEST(ChunkStoreWriterTest, ClosureMarkerFailureStillClosesTheStore) {
  auto store = *LocalChunkStore::Create("writer-close-tee-failure");
  auto writer = *ChunkStoreWriter::Create(store);
  const absl::Status send_failure = absl::UnavailableError("peer is gone");
  ASSERT_TRUE(
      writer->AttachStream(std::make_shared<FailingSendStream>(send_failure))
          .ok());

  EXPECT_EQ(writer->DrainAndClose().Await().status(), send_failure);
  ASSERT_TRUE(writer->GetStatus().has_value());
  EXPECT_EQ(*writer->GetStatus(), send_failure);
  EXPECT_FALSE(writer->IsWritable());
  // The store closed regardless, so a second close reports the recorded status.
  const absl::StatusOr<absl::Status> second =
      store->CloseWritesWithStatus(absl::OkStatus(), true).Await();
  ASSERT_TRUE(second.ok()) << second.status();
  EXPECT_TRUE(second->ok());
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
