// Copyright 2026 The A11 Authors.

#ifndef A11_ACTIONS_REGISTRY_H_
#define A11_ACTIONS_REGISTRY_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/base/thread_annotations.h>
#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/actions/action.h"
#include "a11/actions/schema.h"
#include "a11/data/types.h"
#include "thread/boost_primitives.h"

namespace a11::actions {

class ActionRegistry : public std::enable_shared_from_this<ActionRegistry> {
 public:
  absl::Status Register(std::string action_name, ActionSchema schema,
                        ActionHandler handler = {});
  absl::Status RegisterSync(std::string action_name, ActionSchema schema,
                            SyncActionHandler handler);
  absl::Status Unregister(std::string_view action_name);
  [[nodiscard]] bool IsRegistered(std::string_view action_name) const;
  absl::StatusOr<ActionSchema> GetSchema(std::string_view action_name) const;
  absl::StatusOr<ActionHandler> GetHandler(std::string_view action_name) const;
  absl::StatusOr<std::shared_ptr<Action>> MakeAction(
      std::string_view action_name, std::string action_id = {},
      std::shared_ptr<nodes::NodeMap> node_map = nullptr,
      std::shared_ptr<net::WireStream> stream = nullptr,
      std::shared_ptr<service::Session> session = nullptr);
  absl::StatusOr<data::ActionMessage> MakeActionMessage(
      std::string_view action_name, std::string action_id = {});
  [[nodiscard]] std::vector<std::string> ListRegisteredActions() const;
  std::shared_ptr<ActionRegistry> Copy(bool clear_autofills = true) const;

 private:
  mutable thread::Mutex mu_;
  absl::flat_hash_map<std::string, ActionSchema> schemas_ ABSL_GUARDED_BY(mu_);
  absl::flat_hash_map<std::string, ActionHandler> handlers_
      ABSL_GUARDED_BY(mu_);
};

}  // namespace a11::actions

#endif  // A11_ACTIONS_REGISTRY_H_
