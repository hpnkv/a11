// Copyright 2026 The A11 Authors.

#ifndef A11_NET_SIGNALLING_H_
#define A11_NET_SIGNALLING_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include "a11/concurrency/future.h"

namespace a11::net {

enum class SignallingMessageType {
  kDescription,
  kCandidate,
  kError,
};

struct SignallingMessage {
  SignallingMessageType type = SignallingMessageType::kDescription;
  std::string sender;
  std::string recipient;
  std::string description;
  std::string description_type;
  std::string candidate;
  std::string mid;
  absl::Status error;

  absl::Status Validate() const;
  absl::StatusOr<std::string> ToJson() const;
  static absl::StatusOr<SignallingMessage> FromJson(std::string_view json);
};

class SignallingService;

using OnSignallingMessage = std::function<a11::Task(SignallingMessage message)>;

class SignallingTransport {
 public:
  virtual ~SignallingTransport() = default;
  virtual absl::Status Send(SignallingMessage message) = 0;
  virtual absl::Status SetOnMessage(OnSignallingMessage on_message) = 0;
  virtual absl::Status Close() = 0;
  [[nodiscard]] virtual std::string identity() const = 0;
  [[nodiscard]] virtual bool connected() const = 0;
  [[nodiscard]] virtual absl::Status GetStatus() const = 0;
};

// An identity-bound endpoint. Send() is non-blocking but ordered: the service
// invokes each recipient callback serially on A11 fibers. This makes it safe
// to use directly from libdatachannel's ordinary callback threads.
class SignallingEndpoint
    : public SignallingTransport,
      public std::enable_shared_from_this<SignallingEndpoint> {
 public:
  ~SignallingEndpoint();

  absl::Status Send(SignallingMessage message) override;
  absl::Status SetOnMessage(OnSignallingMessage on_message) override;
  absl::Status Close() override;
  [[nodiscard]] std::string identity() const override;
  [[nodiscard]] bool connected() const override;
  [[nodiscard]] absl::Status GetStatus() const override;

 private:
  struct State;

  explicit SignallingEndpoint(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<State> state_;

  friend class SignallingService;
};

class SignallingService
    : public std::enable_shared_from_this<SignallingService> {
 public:
  static std::shared_ptr<SignallingService> Create();
  ~SignallingService();

  absl::StatusOr<std::shared_ptr<SignallingEndpoint>> Connect(
      std::string identity, OnSignallingMessage on_message);
  [[nodiscard]] bool Contains(std::string_view identity) const;
  [[nodiscard]] std::vector<std::string> Identities() const;
  absl::Status Stop();

 private:
  struct State;

  explicit SignallingService(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  absl::Status Route(const std::shared_ptr<SignallingEndpoint::State>& sender,
                     SignallingMessage message);
  void Disconnect(const std::shared_ptr<SignallingEndpoint::State>& endpoint,
                  absl::Status status);
  static void Pump(const std::shared_ptr<SignallingEndpoint::State>& endpoint);

  std::shared_ptr<State> state_;

  friend class SignallingEndpoint;
};

}  // namespace a11::net

#endif  // A11_NET_SIGNALLING_H_
