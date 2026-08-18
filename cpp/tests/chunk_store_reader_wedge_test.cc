// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief
 *   A read that waits for ever on a fragment its store already holds.
 *
 * `ServerSuite` in cpp/bench/bench_main.cc wedges above a handful of concurrent
 * calls: the server writes its reply, the fragment reaches the client's node, and
 * a `NextChunk()` left on its default `InfiniteDuration` timeout never returns.
 * Every thread is idle at that moment, and a *fresh* read on the same node
 * returns the fragment immediately -- so the data is there and the wake is what
 * went missing.
 *
 * These tests try to reproduce that at the reader, away from the transport, the
 * session and the action layers. The shape they copy is the one the bench
 * produces rather than the one a store test usually uses: a reader per node
 * rather than a shared one, the write racing the read rather than preceding it,
 * and many of those pairs in flight at once. Each read is awaited with a
 * deadline, so a wedge fails the test instead of hanging the suite.
 */

#include "a11/stores/chunk_store_reader.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/executor.h"
#include "a11/data/types.h"
#include "a11/net/in_process_wire_stream.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/stores/local_chunk_store.h"
#include "thread/fiber.h"

namespace a11::stores {
namespace {

using nodes::AsyncNode;
using nodes::NodeMap;

/// Long enough that a scheduler under load is not mistaken for a wedge, short
/// enough that a wedged read is reported rather than waited on.
constexpr absl::Duration kReadDeadline = absl::Seconds(10);

data::NodeFragment FinalFragment(std::string payload) {
  return data::NodeFragment{
      .data = data::Chunk{.metadata = data::ChunkMetadata{.mimetype =
                                                              "application/"
                                                              "octet-stream"},
                          .data = std::move(payload)},
      .seq = 0,
      .continued = false,
  };
}

/**
 * @brief One node, one reader, a read started before the write lands.
 *
 * The reader is asked for a fragment that does not exist yet, so the read has to
 * park; the write then arrives from another fibre. This is the ordering the bench
 * produces when a client asks for its reply before the server has sent it, which
 * the delay experiment showed is the ordering that matters -- reading *after* the
 * fragment lands never wedged.
 */
TEST(ChunkStoreReaderWedgeTest, ReadParkedBeforeTheWriteIsWokenByIt) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    absl::StatusOr<std::shared_ptr<LocalChunkStore>> store =
        LocalChunkStore::Create("wedge");
    ASSERT_TRUE(store.ok()) << store.status();
    absl::StatusOr<std::shared_ptr<ChunkStoreReader>> reader =
        ChunkStoreReader::Create(*store);
    ASSERT_TRUE(reader.ok()) << reader.status();

    // Started first and left on the default infinite timeout: no timer is armed
    // for such a read, so it depends entirely on being woken.
    a11::Future<std::optional<data::NodeFragment>> pending =
        (*reader)->Next(absl::InfiniteDuration());
    a11::Task writing = a11::SubmitTask([store = *store]() -> absl::Status {
      return store->PutMany({FinalFragment("reply")}).Await().status();
    });

    const absl::StatusOr<std::optional<data::NodeFragment>> read =
        pending.Await(absl::Now() + kReadDeadline);
    ASSERT_TRUE(read.ok()) << "attempt " << attempt << ": " << read.status();
    ASSERT_TRUE(read->has_value()) << "attempt " << attempt;
    EXPECT_TRUE(writing.Await(absl::Now() + kReadDeadline).ok());
  }
}

/**
 * @brief Many node/reader pairs racing at once.
 *
 * A reader per node is what the bench has -- every call owns its ports -- so the
 * pumps do not contend with each other. What they do share is the process-wide
 * callback scheduler every reader wakes through, and the worker pool underneath
 * it, which is the only place concurrency here can bite.
 */
TEST(ChunkStoreReaderWedgeTest, ConcurrentNodesEachWakeTheirOwnReader) {
  constexpr size_t kNodes = 256;
  std::vector<std::shared_ptr<ChunkStoreReader>> readers;
  std::vector<a11::Future<std::optional<data::NodeFragment>>> reads;
  std::vector<a11::Task> writes;
  readers.reserve(kNodes);
  reads.reserve(kNodes);
  writes.reserve(kNodes);

  // Every read is parked before any write is issued, so each one must be woken
  // by its own store rather than finding its fragment already there.
  for (size_t index = 0; index < kNodes; ++index) {
    absl::StatusOr<std::shared_ptr<LocalChunkStore>> store =
        LocalChunkStore::Create(absl::StrCat("wedge-", index));
    ASSERT_TRUE(store.ok()) << store.status();
    absl::StatusOr<std::shared_ptr<ChunkStoreReader>> reader =
        ChunkStoreReader::Create(*store);
    ASSERT_TRUE(reader.ok()) << reader.status();
    readers.push_back(*reader);
    reads.push_back((*reader)->Next(absl::InfiniteDuration()));
    writes.push_back(a11::SubmitTask([owned = *store]() -> absl::Status {
      return owned->PutMany({FinalFragment("reply")}).Await().status();
    }));
  }

  size_t wedged = 0;
  const absl::Time deadline = absl::Now() + kReadDeadline;
  for (size_t index = 0; index < kNodes; ++index) {
    const absl::StatusOr<std::optional<data::NodeFragment>> read =
        reads[index].Await(deadline);
    if (!read.ok() || !read->has_value()) {
      ++wedged;
    }
  }
  for (a11::Task& write : writes) {
    (void)write.Await(deadline);
  }
  EXPECT_EQ(wedged, 0u) << wedged << " of " << kNodes
                        << " parked reads were never woken";
}

/**
 * @brief The same race one layer up: whole nodes, written through their writer.
 *
 * The two tests above drive the store directly, which is not what a port does. A
 * node writes through a `ChunkStoreWriter` -- a second pump, with its own inline
 * drive -- and the bench's ports additionally have a wire stream attached, so a
 * write tees to that stream inside the drive and `WireStream::Send` can park.
 * This test adds the writer without the stream, to separate "the writer pump" from
 * "the tee" as candidates.
 */
TEST(ChunkStoreReaderWedgeTest, ConcurrentNodesWrittenThroughTheirWriter) {
  constexpr size_t kNodes = 256;
  absl::StatusOr<std::shared_ptr<NodeMap>> map = NodeMap::Create();
  ASSERT_TRUE(map.ok()) << map.status();

  std::vector<a11::Future<std::optional<data::Chunk>>> reads;
  std::vector<a11::Task> writes;
  reads.reserve(kNodes);
  writes.reserve(kNodes);
  for (size_t index = 0; index < kNodes; ++index) {
    absl::StatusOr<std::shared_ptr<AsyncNode>> node =
        (*map)->Get(absl::StrCat("port-", index));
    ASSERT_TRUE(node.ok()) << node.status();
    // Parked first, on the default infinite timeout -- the bench's ordering.
    reads.push_back((*node)->NextChunk());
    writes.push_back(a11::SubmitTask([owned = *node]() -> absl::Status {
      return owned
          ->PutChunk(data::Chunk{.metadata = data::ChunkMetadata{
                                     .mimetype = "application/octet-stream"},
                                 .data = "reply"},
                     std::nullopt, true)
          .Await()
          .status();
    }));
  }

  size_t wedged = 0;
  const absl::Time deadline = absl::Now() + kReadDeadline;
  for (a11::Future<std::optional<data::Chunk>>& read : reads) {
    const absl::StatusOr<std::optional<data::Chunk>> value = read.Await(deadline);
    if (!value.ok() || !value->has_value()) {
      ++wedged;
    }
  }
  for (a11::Task& write : writes) {
    (void)write.Await(deadline);
  }
  EXPECT_EQ(wedged, 0u) << wedged << " of " << kNodes
                        << " parked node reads were never woken";
}

/**
 * @brief The race with a wire stream attached, which is what a bench port has.
 *
 * This is the layer the previous three do not cover, and the one named as the
 * suspension source: a write on a stream-bound node tees to the stream inside the
 * writer's inline drive, and `WireStream::Send` parks on the peer's fibre-aware
 * mutex. If the wedge needs a parked tee, it should appear here and not above.
 *
 * The peer end is deliberately left undrained. That is the bench's shape too --
 * the client's reply-carrying node has a stream attached and nobody is reading the
 * other side of it -- and it is where a tee has something to park on.
 */
TEST(ChunkStoreReaderWedgeTest, ConcurrentStreamBoundNodesWakeTheirReaders) {
  constexpr size_t kNodes = 128;
  absl::StatusOr<std::shared_ptr<NodeMap>> map = NodeMap::Create();
  ASSERT_TRUE(map.ok()) << map.status();

  std::vector<net::InProcessWireStream::Pair> pairs;
  std::vector<a11::Future<std::optional<data::Chunk>>> reads;
  std::vector<a11::Task> writes;
  pairs.reserve(kNodes);
  reads.reserve(kNodes);
  writes.reserve(kNodes);
  for (size_t index = 0; index < kNodes; ++index) {
    absl::StatusOr<net::InProcessWireStream::Pair> pair =
        net::InProcessWireStream::CreatePair();
    ASSERT_TRUE(pair.ok()) << pair.status();
    pairs.push_back(*pair);
    absl::StatusOr<std::shared_ptr<AsyncNode>> node =
        (*map)->Get(absl::StrCat("streamed-", index));
    ASSERT_TRUE(node.ok()) << node.status();
    ASSERT_TRUE((*node)->AttachStream(pair->first).ok());
    reads.push_back((*node)->NextChunk());
    writes.push_back(a11::SubmitTask([owned = *node]() -> absl::Status {
      return owned
          ->PutChunk(data::Chunk{.metadata = data::ChunkMetadata{
                                     .mimetype = "application/octet-stream"},
                                 .data = "reply"},
                     std::nullopt, true)
          .Await()
          .status();
    }));
  }

  size_t wedged = 0;
  const absl::Time deadline = absl::Now() + kReadDeadline;
  for (a11::Future<std::optional<data::Chunk>>& read : reads) {
    const absl::StatusOr<std::optional<data::Chunk>> value = read.Await(deadline);
    if (!value.ok() || !value->has_value()) {
      ++wedged;
    }
  }
  for (a11::Task& write : writes) {
    (void)write.Await(deadline);
  }
  for (net::InProcessWireStream::Pair& pair : pairs) {
    (void)pair.first->Abort(absl::CancelledError("test over"));
    (void)pair.second->Abort(absl::CancelledError("test over"));
  }
  EXPECT_EQ(wedged, 0u) << wedged << " of " << kNodes
                        << " stream-bound reads were never woken";
}

}  // namespace
}  // namespace a11::stores
