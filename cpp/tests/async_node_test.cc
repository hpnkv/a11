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
