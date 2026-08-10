// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The action registry: a catalogue of named schema/handler pairs.
 *
 * a11::actions::ActionRegistry maps an action name to its
 * a11::actions::ActionSchema and its a11::actions::ActionHandler. It is the
 * factory a service uses to turn an incoming request into a runnable
 * a11::actions::Action (via MakeAction), and the source of truth an
 * a11::service::Session consults when dispatching calls it receives from a
 * peer.
 */

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

/**
 * @brief A thread-safe catalogue mapping action names to schema and handler.
 *
 * Register an action once, then build ready-to-run a11::actions::Action
 * instances from its name. The same registry can back many actions and is
 * shared with a11::service::Session so incoming calls can be dispatched to the
 * registered handler.
 */
class ActionRegistry : public std::enable_shared_from_this<ActionRegistry> {
 public:
  /**
   * @brief Registers an action under @p action_name.
   *
   * Re-registering a name replaces its schema and handler rather than failing,
   * which is what lets a connection-scoped registry copy be specialised after
   * it is built: a peer announcing a tool this side also serves shadows it (see
   * a11's remote tool bridge). Passing an empty @p handler registers the schema
   * alone and drops any handler the name had.
   *
   * @param action_name Name to register.
   * @param schema Schema describing the action's interface. Its `name` must
   *        equal @p action_name.
   * @param handler Asynchronous handler; may be empty to register schema only.
   * @return OK, or InvalidArgument when the name or schema is not valid.
   */
  absl::Status Register(std::string action_name, ActionSchema schema,
                        ActionHandler handler = {});
  /**
   * @brief Registers an action with a synchronous handler.
   *
   * The @p handler is wrapped so it runs as an asynchronous action handler.
   */
  absl::Status RegisterSync(std::string action_name, ActionSchema schema,
                            SyncActionHandler handler);
  /** @brief Removes the registration for @p action_name. */
  absl::Status Unregister(std::string_view action_name);
  /** @brief Whether @p action_name is registered. */
  [[nodiscard]] bool IsRegistered(std::string_view action_name) const;
  /** @brief Returns the schema for @p action_name, or NotFound. */
  absl::StatusOr<ActionSchema> GetSchema(std::string_view action_name) const;
  /** @brief Returns the handler for @p action_name, or NotFound. */
  absl::StatusOr<ActionHandler> GetHandler(std::string_view action_name) const;
  /**
   * @brief Builds a runnable Action for a registered action.
   * @param action_name Name of the registered action.
   * @param action_id Instance id; a fresh one is generated when empty.
   * @param node_map Node map backing the action's ports.
   * @param stream Optional wire stream; when supplied the action can be
   *        dispatched remotely with Action::Call.
   * @param session Optional owning session for tracking and dispatch.
   * @return The constructed action, or NotFound when the name is unknown.
   */
  absl::StatusOr<std::shared_ptr<Action>> MakeAction(
      std::string_view action_name, std::string action_id = {},
      std::shared_ptr<nodes::NodeMap> node_map = nullptr,
      std::shared_ptr<net::WireStream> stream = nullptr,
      std::shared_ptr<service::Session> session = nullptr);
  /**
   * @brief Builds the wire ::a11::data::ActionMessage for a registered action.
   * @param action_name Name of the registered action.
   * @param action_id Instance id; a fresh one is generated when empty.
   */
  absl::StatusOr<data::ActionMessage> MakeActionMessage(
      std::string_view action_name, std::string action_id = {});
  /** @brief Returns the names of all registered actions. */
  [[nodiscard]] std::vector<std::string> ListRegisteredActions() const;
  /**
   * @brief Returns a copy of this registry.
   * @param clear_autofills When true, drop port autofill defaults in the copy.
   */
  std::shared_ptr<ActionRegistry> Copy(bool clear_autofills = true) const;

 private:
  mutable thread::Mutex mu_;
  absl::flat_hash_map<std::string, ActionSchema> schemas_ ABSL_GUARDED_BY(mu_);
  absl::flat_hash_map<std::string, ActionHandler> handlers_
      ABSL_GUARDED_BY(mu_);
};

}  // namespace a11::actions

#endif  // A11_ACTIONS_REGISTRY_H_
