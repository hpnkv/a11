#include <memory>

#include "a11/nodes/node_map.h"

int main() {
  const auto node_map = a11::nodes::NodeMap::Create();
  return node_map.ok() && *node_map != nullptr ? 0 : 1;
}
