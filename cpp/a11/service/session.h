// Copyright 2026 The A11 Authors.

#ifndef A11_SERVICE_SESSION_H_
#define A11_SERVICE_SESSION_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/wire_stream.h"
#include "a11/nodes/node_map.h"

namespace a11::service {

inline constexpr std::string_view kSessionStatusHeader = "x-a11-session-status";
inline constexpr size_t kMaxSingleMessageSize = 32 * 1024 * 1024;

struct SessionOptions {
  size_t max_buffered_messages_total = 256;
  size_t max_buffered_messages_per_stream = 32;
  size_t max_concurrent_root_actions = 32;
  size_t max_concurrent_nested_actions = 128;
  size_t max_single_message_size = kMaxSingleMessageSize;
  size_t max_buffered_bytes_total = 32 * 1024 * 1024;
  size_t max_buffered_bytes_per_stream = 4 * 1024 * 1024;
  absl::Duration no_stream_timeout = absl::Seconds(30);
  absl::Time deadline = absl::InfiniteFuture();

  absl::Status Validate() const;
};

class Session;

using OnSessionStreamMessage = std::function<a11::Task(
    std::optional<data::WireMessage>, std::shared_ptr<net::WireStream>,
    std::shared_ptr<Session>)>;
using OnSessionStreamDone = std::function<a11::Task(
    std::shared_ptr<net::WireStream>, std::shared_ptr<Session>)>;

enum class StreamMode { kStart, kAccept };

class Session : public std::enable_shared_from_this<Session> {
 public:
  static absl::StatusOr<std::shared_ptr<Session>> Create(
      std::string session_id = {},
      OnSessionStreamMessage on_stream_message = {},
      OnSessionStreamDone on_stream_done = {}, data::ByteMap headers = {},
      SessionOptions options = {},
      std::shared_ptr<nodes::NodeMap> node_map = nullptr,
      std::shared_ptr<actions::ActionRegistry> action_registry = nullptr);

  virtual ~Session() = default;

  absl::StatusOr<
      std::vector<std::pair<std::string, std::shared_ptr<net::WireStream>>>>
  Streams() const;
  absl::StatusOr<std::shared_ptr<net::WireStream>> GetStream(
      std::string_view stream_id) const;
  [[nodiscard]] std::string GetId() const;
  [[nodiscard]] std::shared_ptr<nodes::NodeMap> GetNodeMap() const;
  absl::Status SetNodeMap(std::shared_ptr<nodes::NodeMap> node_map);
  [[nodiscard]] std::shared_ptr<actions::ActionRegistry> GetActionRegistry()
      const;
  absl::Status SetActionRegistry(
      std::shared_ptr<actions::ActionRegistry> registry);

  [[nodiscard]] std::vector<
      std::pair<std::string, std::shared_ptr<actions::Action>>>
  Actions() const;
  absl::StatusOr<std::shared_ptr<actions::Action>> GetAction(
      std::string_view action_id) const;
  absl::Status CancelAction(std::string_view action_id);
  absl::Status CancelAllActions();
  a11::Task AwaitAllActions(absl::Duration timeout = absl::InfiniteDuration());

  a11::Future<std::uint32_t> DispatchNodeFragment(data::NodeFragment fragment);
  a11::Task DispatchActionMessage(
      data::ActionMessage message,
      std::shared_ptr<net::WireStream> origin_stream = nullptr);
  a11::Task DispatchAction(std::shared_ptr<actions::Action> action);
  a11::Task DispatchWireMessage(
      data::WireMessage message,
      std::shared_ptr<net::WireStream> origin_stream = nullptr);

  [[nodiscard]] bool IsClosed() const;
  [[nodiscard]] bool IsDone() const;
  [[nodiscard]] a11::Task Done() const;
  [[nodiscard]] absl::Status GetStatus() const;
  absl::StatusOr<a11::Task> AddStream(std::shared_ptr<net::WireStream> stream,
                                      StreamMode mode = StreamMode::kStart);
  absl::Status HalfClose();
  virtual absl::Status Abort(absl::Status status);
  absl::Status Send(data::WireMessage message, std::string_view stream_id = {});
  [[nodiscard]] absl::Time deadline() const;
  absl::Status SetDeadline(absl::Time deadline = absl::InfiniteFuture());

 protected:
  Session() = default;
  absl::Status Initialize(
      const std::shared_ptr<Session>& self, std::string session_id,
      OnSessionStreamMessage on_stream_message,
      OnSessionStreamDone on_stream_done, data::ByteMap headers,
      SessionOptions options, std::shared_ptr<nodes::NodeMap> node_map,
      std::shared_ptr<actions::ActionRegistry> action_registry);

 private:
  struct State;
  struct StreamState;
  std::shared_ptr<State> state_;

  a11::Task HandleStreamMessage(
      const std::shared_ptr<StreamState>& stream_state,
      std::optional<data::WireMessage> message);
  void ProcessStreamMessages(const std::shared_ptr<StreamState>& stream_state);
  a11::Task HandleStreamDone(const std::shared_ptr<StreamState>& stream_state);
  void RemoveStream(const std::shared_ptr<StreamState>& stream_state);
  void FinishIfPossible();
  void NotifyStateChanged();

  absl::Status TrackAction(const std::shared_ptr<actions::Action>& action);
  void UntrackAction(const std::shared_ptr<actions::Action>& action);
  std::shared_ptr<actions::ActionLimiter> GetActionLimiter(bool nested) const;

  friend class actions::Action;
};

struct ReceivedSessionMessage {
  data::WireMessage message;
  std::string stream_id;
};

class SessionWithRecv final : public Session {
 public:
  static absl::StatusOr<std::shared_ptr<SessionWithRecv>> Create(
      std::string session_id = {}, data::ByteMap headers = {},
      SessionOptions options = {},
      std::shared_ptr<nodes::NodeMap> node_map = nullptr,
      std::shared_ptr<actions::ActionRegistry> action_registry = nullptr);

  a11::Future<std::optional<ReceivedSessionMessage>> ReceiveWithStreamId(
      absl::Time deadline = absl::InfiniteFuture());
  a11::Future<std::optional<data::WireMessage>> Receive(
      absl::Time deadline = absl::InfiniteFuture());
  absl::Status Abort(absl::Status status) override;

 private:
  struct ReceiveState;
  std::shared_ptr<ReceiveState> receive_state_;

  a11::Task OnMessage(std::optional<data::WireMessage> message,
                      std::shared_ptr<net::WireStream> stream);
  a11::Task OnDone(std::shared_ptr<net::WireStream> stream);
  void SignalReceiveError(absl::Status status);
};

absl::StatusOr<data::ByteMap> NormalizeSessionHeaders(data::ByteMap headers);

}  // namespace a11::service

#endif  // A11_SERVICE_SESSION_H_
