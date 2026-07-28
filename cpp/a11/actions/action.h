// Copyright 2026 The A11 Authors.

#ifndef A11_ACTIONS_ACTION_H_
#define A11_ACTIONS_ACTION_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/base/thread_annotations.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/actions/schema.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "thread/boost_primitives.h"

namespace thread {
class PermanentEvent;
}  // namespace thread

namespace a11::net {
class WireStream;
}  // namespace a11::net

namespace a11::nodes {
class AsyncNode;
class NodeMap;
}  // namespace a11::nodes

namespace a11::service {
class Session;
}  // namespace a11::service

namespace a11::actions {

class Action;
class ActionRegistry;

inline constexpr size_t kDefaultMaxConcurrentNestedActions = 64;

using ActionHandler = std::function<a11::Task(std::shared_ptr<Action>)>;
using SyncActionHandler = std::function<absl::Status(std::shared_ptr<Action>)>;
using OnActionCancelled = std::function<absl::Status(std::shared_ptr<Action>)>;

ActionHandler MakeAsyncActionHandler(SyncActionHandler handler);

// Cancellation-aware semaphore shared by root or nested actions.
class ActionLimiter {
 public:
  static absl::StatusOr<std::shared_ptr<ActionLimiter>> Create(size_t maximum);
  absl::Status Acquire();
  void Release();

 private:
  explicit ActionLimiter(size_t maximum);

  thread::Mutex mu_;
  const size_t maximum_;
  size_t active_ ABSL_GUARDED_BY(mu_) = 0;
  std::shared_ptr<thread::PermanentEvent> changed_ ABSL_GUARDED_BY(mu_);
};

class Action : public std::enable_shared_from_this<Action> {
 public:
  static absl::StatusOr<std::shared_ptr<Action>> Create(
      ActionSchema schema, std::string action_id = {},
      ActionHandler handler = {},
      std::shared_ptr<nodes::NodeMap> node_map = nullptr,
      std::shared_ptr<net::WireStream> stream = nullptr,
      std::shared_ptr<service::Session> session = nullptr,
      std::shared_ptr<ActionRegistry> registry = nullptr,
      size_t max_concurrent_nested_actions =
          kDefaultMaxConcurrentNestedActions);

  static absl::StatusOr<std::string> MakeNodeId(std::string_view action_id,
                                                std::string_view node_name);

  [[nodiscard]] std::string GetId() const;
  absl::Status SetId(std::string action_id);
  [[nodiscard]] ActionSchema GetSchema() const;
  absl::Status SetSchema(ActionSchema schema);
  absl::Status BindHandler(ActionHandler handler);
  [[nodiscard]] ActionHandler GetHandler() const;
  [[nodiscard]] bool HasHandler() const;

  [[nodiscard]] ActionSettings GetSettings() const;
  absl::Status SetSettings(ActionSettings settings);
  absl::Status BindStreamsOnInputsByDefault(bool bind);
  absl::Status BindStreamsOnOutputsByDefault(bool bind);
  absl::Status ClearInputsAfterRun(bool clear = true);
  absl::Status ClearOutputsAfterRun(bool clear = true);

  absl::Status BindNodeMap(std::shared_ptr<nodes::NodeMap> node_map);
  [[nodiscard]] std::shared_ptr<nodes::NodeMap> GetNodeMap() const;
  absl::Status BindStream(std::shared_ptr<net::WireStream> stream);
  [[nodiscard]] std::shared_ptr<net::WireStream> GetStream() const;
  absl::Status BindRegistry(std::shared_ptr<ActionRegistry> registry);
  [[nodiscard]] std::shared_ptr<ActionRegistry> GetRegistry() const;
  absl::Status BindSession(std::shared_ptr<service::Session> session);
  [[nodiscard]] std::shared_ptr<service::Session> GetSession() const;

  absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> GetNode(
      std::string node_id);
  absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> GetInput(
      std::string name, std::optional<bool> bind_stream = std::nullopt);
  absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> GetOutput(
      std::string name, std::optional<bool> bind_stream = std::nullopt);
  absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> GetPort(std::string name);
  [[nodiscard]] bool ContainsPort(std::string_view name) const;

  [[nodiscard]] data::ActionMessage GetActionMessage() const;
  absl::Status MapPortsFromMessage(const data::ActionMessage& message);

  [[nodiscard]] data::ByteMap Headers() const;
  absl::StatusOr<std::optional<data::Bytes>> GetHeader(
      std::string_view name) const;
  [[nodiscard]] bool HasHeader(std::string_view name) const;
  absl::Status SetHeader(std::string name, data::Bytes value);
  absl::Status RemoveHeader(std::string_view name);
  absl::Status ForwardHeader(const std::shared_ptr<Action>& target,
                             std::string_view name) const;
  absl::Status ForwardHeadersWithPrefix(
      const std::shared_ptr<Action>& target,
      std::string_view prefix = kActionHeaderPrefix) const;

  absl::StatusOr<std::shared_ptr<Action>> MakeNested(
      const ActionSchema& schema, bool propagate_io = true,
      bool forward_headers = true);
  absl::StatusOr<std::shared_ptr<Action>> MakeNested(
      std::string_view action_name, bool propagate_io = true,
      bool forward_headers = true);

  absl::StatusOr<std::shared_ptr<Action>> Run();
  a11::Future<std::shared_ptr<Action>> Call(data::ByteMap wire_headers = {});
  a11::Future<absl::Status> WaitForDispatch(
      absl::Duration timeout = absl::InfiniteDuration());
  a11::Future<std::shared_ptr<Action>> Wait(
      absl::Duration timeout = absl::InfiniteDuration());
  absl::Status Cancel();
  absl::Status SetOnCancelled(OnActionCancelled callback);

  [[nodiscard]] bool IsDone() const;
  [[nodiscard]] bool HasBeenRun() const;
  [[nodiscard]] bool HasBeenCalled() const;
  [[nodiscard]] bool Cancelled() const;
  [[nodiscard]] absl::Status GetStatus() const;
  [[nodiscard]] std::optional<absl::Status> GetDispatchStatus() const;

 private:
  enum class Mode { kNone, kRun, kCall, kCancelled };

  Action(ActionSchema schema, std::string id, ActionHandler handler,
         std::shared_ptr<nodes::NodeMap> node_map,
         std::shared_ptr<net::WireStream> stream,
         std::shared_ptr<service::Session> session,
         std::shared_ptr<ActionRegistry> registry,
         std::shared_ptr<ActionLimiter> nested_limiter);

  absl::Status Begin(Mode mode);
  absl::Status RemapDefaultPorts() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  absl::Status AttachStreamIfRequested(
      const std::shared_ptr<nodes::AsyncNode>& node, bool bind);
  absl::Status ValidateMessagePorts(
      const std::vector<data::Port>& ports,
      const absl::flat_hash_map<std::string, ActionPortSchema>& schema_ports,
      std::string_view kind) const;
  void RunHandler(std::shared_ptr<ActionLimiter> limiter);
  void StartFinish(absl::Status status);
  absl::Status FinishRun(absl::Status status);
  absl::Status FinishOutputNodes(const absl::Status& status);
  absl::Status CommunicateStatus(const absl::Status& status);
  absl::Status AbortInputs(const absl::Status& status);
  absl::Status SendNodeAbortStatuses(
      const absl::flat_hash_set<std::string>& node_ids,
      const absl::Status& status);
  absl::Status ReleaseNodesAfterRun();
  absl::Status DetachBoundStreamNodes();
  absl::Status SendRemoteCancel();
  void CompleteCall(absl::Status status, bool remove_from_session);
  void AbortLocalCallOutputs(absl::Status status);
  absl::Status TrackInSession(const std::shared_ptr<service::Session>& session);
  void UntrackFromSession();
  void SetDispatchStatus(absl::Status status);
  void SetCompletionStatus(absl::Status status);

  mutable thread::Mutex mu_;
  ActionSchema schema_ ABSL_GUARDED_BY(mu_);
  ActionHandler handler_ ABSL_GUARDED_BY(mu_);
  std::string id_ ABSL_GUARDED_BY(mu_);
  data::ByteMap headers_ ABSL_GUARDED_BY(mu_);
  ActionSettings settings_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<nodes::NodeMap> node_map_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<net::WireStream> stream_ ABSL_GUARDED_BY(mu_);
  std::weak_ptr<service::Session> session_ ABSL_GUARDED_BY(mu_);
  std::weak_ptr<service::Session> tracked_session_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<ActionRegistry> registry_ ABSL_GUARDED_BY(mu_);
  absl::flat_hash_map<std::string, std::string> input_ids_ ABSL_GUARDED_BY(mu_);
  absl::flat_hash_map<std::string, std::string> output_ids_
      ABSL_GUARDED_BY(mu_);
  absl::flat_hash_set<std::shared_ptr<nodes::AsyncNode>> input_nodes_
      ABSL_GUARDED_BY(mu_);
  absl::flat_hash_set<std::shared_ptr<nodes::AsyncNode>> output_nodes_
      ABSL_GUARDED_BY(mu_);
  absl::flat_hash_set<std::shared_ptr<nodes::AsyncNode>> stream_bound_nodes_
      ABSL_GUARDED_BY(mu_);
  Mode mode_ ABSL_GUARDED_BY(mu_) = Mode::kNone;
  a11::Task task_ ABSL_GUARDED_BY(mu_);
  bool cancel_requested_ ABSL_GUARDED_BY(mu_) = false;
  bool finishing_ ABSL_GUARDED_BY(mu_) = false;
  std::optional<absl::Status> completion_status_ ABSL_GUARDED_BY(mu_);
  std::optional<absl::Status> dispatch_status_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<a11::Promise<a11::Unit>> done_promise_ ABSL_GUARDED_BY(mu_);
  a11::Task done_future_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<a11::Promise<a11::Unit>> dispatch_promise_
      ABSL_GUARDED_BY(mu_);
  a11::Task dispatch_future_ ABSL_GUARDED_BY(mu_);
  std::vector<OnActionCancelled> cancel_callbacks_ ABSL_GUARDED_BY(mu_);
  std::weak_ptr<Action> parent_ ABSL_GUARDED_BY(mu_);
  absl::flat_hash_set<std::shared_ptr<Action>> children_ ABSL_GUARDED_BY(mu_);
  std::shared_ptr<ActionLimiter> nested_limiter_ ABSL_GUARDED_BY(mu_);

  friend class ActionRegistry;
  friend class service::Session;
};

}  // namespace a11::actions

#endif  // A11_ACTIONS_ACTION_H_
