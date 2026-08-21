// Copyright 2026 The A11 Authors.

#include "a11/nodes/async_node.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <absl/status/statusor.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a11/data/serializable.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/nodes/node_map.h"
#include "a11/stores/local_chunk_store.h"

namespace a11::nodes {
namespace {

// --- the local fast path -----------------------------------------------------

/**
 * A value that counts how often it is encoded and decoded.
 *
 * The point of the fast path is that these counters stay at zero for a value
 * written and read back in one process, and a test that only checked the value
 * came back would pass just as well with the encoding still happening. So the
 * counters are the assertion.
 */
struct Counted {
  int number = 0;

  static int encodes;
  static int decodes;

  static void Reset() {
    encodes = 0;
    decodes = 0;
  }
};

int Counted::encodes = 0;
int Counted::decodes = 0;

inline std::string_view A11SerialTag(data::TypeTag<Counted>) {
  return "a11.test.Counted";
}

inline absl::StatusOr<nlohmann::json> A11ToJson(const Counted& value) {
  ++Counted::encodes;
  return nlohmann::json{{"number", value.number}};
}

inline absl::StatusOr<Counted> A11FromJson(data::TypeTag<Counted>,
                                           const nlohmann::json& json) {
  ++Counted::decodes;
  return Counted{.number = json.value("number", 0)};
}

std::shared_ptr<data::SerializationRegistry> CountedRegistry() {
  auto registry = std::make_shared<data::SerializationRegistry>(
      /*register_defaults=*/true);
  (void)data::RegisterJsonSerializable<Counted>(*registry);
  return registry;
}

std::shared_ptr<AsyncNode> CountedNode() {
  auto store = *stores::LocalChunkStore::Create("counted");
  return *AsyncNode::Create(store, CountedRegistry());
}

TEST(ChunkObjectTest, AValueWrittenAndReadLocallyIsNeverEncoded) {
  Counted::Reset();
  const std::shared_ptr<AsyncNode> node = CountedNode();
  ASSERT_TRUE(node->PutObject<Counted>(Counted{.number = 7}, "application/json",
                                       std::nullopt, true)
                  .Await()
                  .ok());
  const absl::StatusOr<std::optional<Counted>> back =
      node->NextObject<Counted>().Await();
  ASSERT_TRUE(back.ok()) << back.status();
  ASSERT_TRUE(back->has_value());
  EXPECT_EQ((*back)->number, 7);
  // The whole claim, in two lines.
  EXPECT_EQ(Counted::encodes, 0);
  EXPECT_EQ(Counted::decodes, 0);
}

TEST(ChunkObjectTest, EveryReaderGetsItsOwnCopy) {
  Counted::Reset();
  const std::shared_ptr<AsyncNode> node = CountedNode();
  ASSERT_TRUE(node->PutObject<Counted>(Counted{.number = 1}, "application/json",
                                       std::nullopt, true)
                  .Await()
                  .ok());
  const absl::StatusOr<std::optional<Counted>> first =
      node->NextObject<Counted>().Await();
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(node->ResetReader().ok());
  const absl::StatusOr<std::optional<Counted>> second =
      node->NextObject<Counted>().Await();
  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(second->has_value());
  EXPECT_EQ((*second)->number, 1);
  // Copies, not aliases: a store replays to every reader, so two readers must
  // not be handed one object between them.
  EXPECT_NE(&**first, &**second);
  EXPECT_EQ(Counted::encodes, 0);
}

TEST(ChunkObjectTest, AskingForTheChunkProducesTheBytes) {
  Counted::Reset();
  const std::shared_ptr<AsyncNode> node = CountedNode();
  ASSERT_TRUE(node->PutObject<Counted>(Counted{.number = 3}, "application/json",
                                       std::nullopt, true)
                  .Await()
                  .ok());
  // A reader that wants bytes gets bytes, exactly as it did before chunks could
  // carry values. This is what keeps the mechanism invisible.
  const absl::StatusOr<std::optional<data::Chunk>> chunk =
      node->NextChunk().Await();
  ASSERT_TRUE(chunk.ok()) << chunk.status();
  ASSERT_TRUE(chunk->has_value());
  EXPECT_FALSE((*chunk)->HasObject());
  EXPECT_FALSE((*chunk)->data.empty());
  EXPECT_EQ(Counted::encodes, 1);
  EXPECT_EQ((*chunk)->GetMimetype(), "application/json");
}

TEST(ChunkObjectTest, AReaderWantingAnotherTypeFallsBackToTheBytes) {
  Counted::Reset();
  const std::shared_ptr<AsyncNode> node = CountedNode();
  ASSERT_TRUE(node->PutObject<Counted>(Counted{.number = 5}, "application/json",
                                       std::nullopt, true)
                  .Await()
                  .ok());
  const absl::StatusOr<std::optional<nlohmann::json>> as_json =
      node->NextObject<nlohmann::json>().Await();
  ASSERT_TRUE(as_json.ok()) << as_json.status();
  ASSERT_TRUE(as_json->has_value());
  EXPECT_EQ((*as_json)->value("number", 0), 5);
  // Encoded exactly once, because this reader asked for something the producer
  // did not write -- which is when the wire format earns its keep.
  EXPECT_EQ(Counted::encodes, 1);
}

TEST(ChunkObjectTest, AnObjectCarryingChunkIsNotEmptyAndNotNull) {
  // A chunk with no bytes that read as empty would read as a null stream
  // terminator, ending a stream on its first value.
  data::Chunk chunk = data::MakeChunkObject<Counted>(
      Counted{.number = 9}, "a11.test.Counted", "application/octet-stream",
      CountedRegistry());
  EXPECT_TRUE(chunk.HasObject());
  EXPECT_FALSE(chunk.IsEmpty());
  EXPECT_FALSE(chunk.IsNull());
  EXPECT_TRUE(chunk.Validate().ok());
}

TEST(ChunkObjectTest, MaterializeIsIdempotentAndReleasesTheValue) {
  Counted::Reset();
  data::Chunk chunk =
      data::MakeChunkObject<Counted>(Counted{.number = 2}, "a11.test.Counted",
                                     "application/json", CountedRegistry());
  ASSERT_TRUE(chunk.Materialize().ok());
  const std::string once = chunk.data;
  ASSERT_TRUE(chunk.Materialize().ok());
  EXPECT_EQ(chunk.data, once);
  EXPECT_FALSE(chunk.HasObject());
  EXPECT_EQ(Counted::encodes, 1) << "the second call must not encode again";
}

TEST(ChunkObjectTest, ATagMismatchDoesNotCast) {
  data::Chunk chunk =
      data::MakeChunkObject<Counted>(Counted{.number = 4}, "a11.test.Counted",
                                     "application/json", CountedRegistry());
  // The guard that stands in for RTTI. A wrong tag must yield nothing rather
  // than a plausible-looking reinterpretation of the bytes.
  EXPECT_FALSE(data::TryTakeObject<Counted>(chunk, "a11.test.SomethingElse")
                   .has_value());
  EXPECT_TRUE(
      data::TryTakeObject<Counted>(chunk, "a11.test.Counted").has_value());
}

TEST(AsyncNodeTest, SerializesAndStreamsObjects) {
  auto map = *NodeMap::Create();
  auto node = *map->Get("values");
  nlohmann::json expected{{"answer", 42}};
  auto confirmation = node->Put(expected, std::nullopt, true);
  EXPECT_EQ(*confirmation.Await(), 0);
  auto restored = node->NextObject<nlohmann::json>().Await();
  ASSERT_TRUE(restored.ok()) << restored.status();
  ASSERT_TRUE(restored->has_value());
  EXPECT_EQ(**restored, expected);
  EXPECT_FALSE(node->NextObject<nlohmann::json>().Await()->has_value());
}

TEST(AsyncNodeTest, FinalizeWritesTheLastValueAndCloses) {
  auto map = *NodeMap::Create();
  auto node = *map->Get("unary");
  nlohmann::json expected{{"answer", 42}};
  ASSERT_TRUE(node->Finalize(expected, {.wait = true}).Await().ok());
  EXPECT_FALSE(*node->IsWritable().Await());
  auto restored = node->NextObject<nlohmann::json>().Await();
  ASSERT_TRUE(restored.ok()) << restored.status();
  ASSERT_TRUE(restored->has_value());
  EXPECT_EQ(**restored, expected);
  EXPECT_FALSE(node->NextObject<nlohmann::json>().Await()->has_value());
}

TEST(AsyncNodeTest, FinalizeWithoutWaitingStillEndsTheStream) {
  auto map = *NodeMap::Create();
  auto node = *map->Get("deferred");
  // Nothing is awaited here: the write and the close are the writer's pump's
  // work, which is what lets a producer finalise and walk away.
  ASSERT_TRUE(node->Put(nlohmann::json{{"token", "hi"}}).Await().ok());
  ASSERT_TRUE(node->Finalize().Await().ok());
  auto first = node->NextObject<nlohmann::json>().Await();
  ASSERT_TRUE(first.ok()) << first.status();
  ASSERT_TRUE(first->has_value());
  EXPECT_FALSE(node->NextObject<nlohmann::json>().Await()->has_value());
  EXPECT_FALSE(*node->IsWritable().Await());
}

TEST(AsyncNodeTest, FinalizeCanLeaveTheWriterOpen) {
  auto map = *NodeMap::Create();
  auto node = *map->Get("open");
  ASSERT_TRUE(node->Finalize({.wait = true, .close = false}).Await().ok());
  // The node reports unwritable because a final sequence exists -- that is
  // finality, not closure. The writer itself is still open until Close().
  EXPECT_FALSE(*node->IsWritable().Await());
  EXPECT_TRUE((*node->writer())->IsWritable());
  EXPECT_FALSE(node->NextObject<nlohmann::json>().Await()->has_value());
  ASSERT_TRUE(node->Close().Await().ok());
  EXPECT_FALSE((*node->writer())->IsWritable());
}

TEST(AsyncNodeTest, CloseEndsAStreamWithNoFinality) {
  auto map = *NodeMap::Create();
  auto node = *map->Get("logs");
  ASSERT_TRUE(node->Put(nlohmann::json{{"line", 1}}).Await().ok());
  ASSERT_TRUE(node->Close().Await().ok());
  EXPECT_TRUE(node->NextObject<nlohmann::json>().Await()->has_value());
  EXPECT_FALSE(node->NextObject<nlohmann::json>().Await()->has_value());
}

TEST(AsyncNodeTest, NodeMapCachesAndDiscardsIdentity) {
  auto map = *NodeMap::Create();
  auto first = *map->Get("node");
  auto second = *map->Get("node");
  EXPECT_EQ(first, second);
  EXPECT_EQ(map->Size(), 1);
  EXPECT_EQ(*map->Discard("node", first), first);
  EXPECT_EQ(map->Size(), 0);
}

}  // namespace
}  // namespace a11::nodes
