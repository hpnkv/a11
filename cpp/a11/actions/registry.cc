// Copyright 2026 The A11 Authors.

#include "a11/actions/registry.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>

#include "a11/actions/action.h"
#include "a11/actions/schema.h"
#include "a11/data/types.h"
#include "thread/boost_primitives.h"

namespace a11::actions {

absl::Status ActionRegistry::Register(std::string action_name,
                                      ActionSchema schema,
                                      ActionHandler handler) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(action_name));
  if (action_name == kCancelActionName) {
    return absl::InvalidArgumentError("The cancel action name is reserved");
  }
  ABSL_RETURN_IF_ERROR(schema.Validate());
  if (schema.name != action_name) {
    return absl::InvalidArgumentError(
        "Registry action name does not match schema name");
  }
  thread::MutexLock lock(&mu_);
  schemas_.insert_or_assign(action_name, std::move(schema));
  if (handler) {
    handlers_.insert_or_assign(std::move(action_name), std::move(handler));
  } else {
    handlers_.erase(action_name);
  }
  return absl::OkStatus();
}

absl::Status ActionRegistry::RegisterSync(std::string action_name,
                                          ActionSchema schema,
                                          SyncActionHandler handler) {
  if (!handler) {
    return absl::InvalidArgumentError("handler must be callable");
  }
  return Register(std::move(action_name), std::move(schema),
                  MakeAsyncActionHandler(std::move(handler)));
}

absl::Status ActionRegistry::Unregister(std::string_view action_name) {
  ABSL_RETURN_IF_ERROR(data::ValidateName(action_name));
  thread::MutexLock lock(&mu_);
  if (schemas_.find(std::string(action_name)) == schemas_.end()) {
    return absl::NotFoundError(
        absl::StrCat("Action '", action_name, "' is not registered"));
  }
  schemas_.erase(std::string(action_name));
  handlers_.erase(std::string(action_name));
  return absl::OkStatus();
}

bool ActionRegistry::IsRegistered(std::string_view action_name) const {
  if (!data::ValidateName(action_name).ok())
    return false;
  thread::MutexLock lock(&mu_);
  return schemas_.find(std::string(action_name)) != schemas_.end();
}

absl::StatusOr<ActionSchema> ActionRegistry::GetSchema(
    std::string_view action_name) const {
  ABSL_RETURN_IF_ERROR(data::ValidateName(action_name));
  thread::MutexLock lock(&mu_);
  const auto found = schemas_.find(action_name);
  if (found == schemas_.end()) {
    return absl::NotFoundError(
        absl::StrCat("Action '", action_name, "' is not registered"));
  }
  return found->second;
}

absl::StatusOr<ActionHandler> ActionRegistry::GetHandler(
    std::string_view action_name) const {
  ABSL_RETURN_IF_ERROR(data::ValidateName(action_name));
  thread::MutexLock lock(&mu_);
  const auto found = handlers_.find(action_name);
  if (found != handlers_.end())
    return found->second;
  if (schemas_.find(action_name) != schemas_.end()) {
    return absl::NotFoundError(absl::StrCat(
        "Action '", action_name, "' is registered without a handler"));
  }
  return absl::NotFoundError(
      absl::StrCat("Action '", action_name, "' is not registered"));
}

absl::StatusOr<std::shared_ptr<Action>> ActionRegistry::MakeAction(
    std::string_view action_name, std::string action_id,
    std::shared_ptr<nodes::NodeMap> node_map,
    std::shared_ptr<net::WireStream> stream,
    std::shared_ptr<service::Session> session) {
  ABSL_ASSIGN_OR_RETURN(ActionSchema schema, GetSchema(action_name));
  ActionHandler handler;
  {
    thread::MutexLock lock(&mu_);
    const auto found = handlers_.find(action_name);
    if (found != handlers_.end())
      handler = found->second;
  }
  return Action::Create(std::move(schema), std::move(action_id),
                        std::move(handler), std::move(node_map),
                        std::move(stream), std::move(session),
                        shared_from_this());
}

absl::StatusOr<data::ActionMessage> ActionRegistry::MakeActionMessage(
    std::string_view action_name, std::string action_id) {
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<Action> action,
                        MakeAction(action_name, std::move(action_id)));
  return action->GetActionMessage();
}

std::vector<std::string> ActionRegistry::ListRegisteredActions() const {
  thread::MutexLock lock(&mu_);
  std::vector<std::string> result;
  result.reserve(schemas_.size());
  for (const auto& [name, unused] : schemas_) {
    (void)unused;
    result.push_back(name);
  }
  return result;
}

std::shared_ptr<ActionRegistry> ActionRegistry::Copy(
    bool clear_autofills) const {
  auto result = std::make_shared<ActionRegistry>();
  thread::MutexLock lock(&mu_);
  thread::MutexLock result_lock(&result->mu_);
  for (const auto& [name, original] : schemas_) {
    ActionSchema schema = original;
    if (clear_autofills) {
      for (auto& [unused, port] : schema.inputs) {
        (void)unused;
        port.autofills.clear();
      }
      for (auto& [unused, port] : schema.outputs) {
        (void)unused;
        port.autofills.clear();
      }
    }
    result->schemas_.emplace(name, std::move(schema));
    const auto handler = handlers_.find(name);
    if (handler != handlers_.end()) {
      result->handlers_.emplace(name, handler->second);
    }
  }
  return result;
}

}  // namespace a11::actions
