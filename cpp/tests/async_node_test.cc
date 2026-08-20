// Copyright 2026 The A11 Authors.

#include "a11/nodes/async_node.h"

#include <memory>

#include <absl/status/statusor.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a11/data/types.h"
#include "a11/nodes/node_map.h"

namespace a11::nodes {
namespace {

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
