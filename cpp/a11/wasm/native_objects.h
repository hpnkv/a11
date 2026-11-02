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

#ifndef A11_WASM_NATIVE_OBJECTS_H_
#define A11_WASM_NATIVE_OBJECTS_H_

#include <memory>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/status/status.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/nodes/node_map.h"
#include "a11/service/session.h"
#include "a11/net/wire_stream_with_recv.h"
#include "graph_snapshot.h"
#include "host_settings.h"
#include "host_stream.h"
#include "host_binary_channel.h"
#include "thread/boost_primitives.h"

namespace a11::wasm {

struct NativeGraph {
  std::shared_ptr<nodes::NodeMap> nodes;
  std::shared_ptr<StoreRegistry> stores;
  std::shared_ptr<actions::ActionRegistry> registry;
  thread::Mutex mu;
  bool initialized ABSL_GUARDED_BY(mu) = false;
  bool importing_snapshot ABSL_GUARDED_BY(mu) = false;
  absl::Status startup ABSL_GUARDED_BY(mu);
  HostSettings host_settings ABSL_GUARDED_BY(mu);
  std::string storage_id ABSL_GUARDED_BY(mu) = "local";
  absl::flat_hash_map<std::string, std::shared_ptr<actions::Action>>
      actions ABSL_GUARDED_BY(mu);
  absl::flat_hash_map<std::string, std::shared_ptr<service::Session>>
      sessions ABSL_GUARDED_BY(mu);
  std::vector<std::shared_ptr<nodes::NodeMap>> flow_nodes ABSL_GUARDED_BY(mu);
  absl::flat_hash_map<std::string, std::shared_ptr<nodes::NodeMap>>
      retained_flows ABSL_GUARDED_BY(mu);
  absl::flat_hash_map<std::string, std::shared_ptr<HostStreamPair>>
      host_streams ABSL_GUARDED_BY(mu);
  absl::flat_hash_set<std::string> pending_host_streams ABSL_GUARDED_BY(mu);
  absl::flat_hash_map<std::string, std::shared_ptr<HostBinaryChannel>>
      binary_channels ABSL_GUARDED_BY(mu);
  absl::flat_hash_map<std::string, std::shared_ptr<HostChannelWireStream>>
      channel_streams ABSL_GUARDED_BY(mu);
  absl::flat_hash_map<std::string,
                      std::shared_ptr<net::WireStreamWithRecv>>
      channel_receivers ABSL_GUARDED_BY(mu);
};

struct NativeObjects {
  std::shared_ptr<NativeGraph> graph;
};

absl::Status RegisterHostAction(actions::ActionRegistry& registry,
                                actions::ActionSchema schema,
                                const std::shared_ptr<NativeGraph>& graph);

absl::Status HostInvokeSessionCallback(const std::string& session_id,
                                       const std::string& payload);

}  // namespace a11::wasm

#endif  // A11_WASM_NATIVE_OBJECTS_H_
