// Copyright 2026 The A11 Authors.

#include "a11/net/signalling.h"

#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/base/thread_annotations.h>
#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <nlohmann/json.hpp>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/status.h"
#include "thread/boost_primitives.h"

namespace a11::net {
namespace {

std::string_view MessageTypeName(SignallingMessageType type) {
  switch (type) {
    case SignallingMessageType::kDescription:
      return "description";
    case SignallingMessageType::kCandidate:
      return "candidate";
    case SignallingMessageType::kError:
      return "error";
  }
  return "unknown";
}

absl::StatusOr<SignallingMessageType> ParseMessageType(std::string_view value) {
  if (value == "description" || value == "offer" || value == "answer") {
    return SignallingMessageType::kDescription;
  }
  if (value == "candidate")
    return SignallingMessageType::kCandidate;
  if (value == "error")
    return SignallingMessageType::kError;
  return absl::InvalidArgumentError(
      absl::StrCat("Unknown signalling message type: ", value));
}

absl::Status CallbackException(const std::exception& error) {
  return absl::UnknownError(
      absl::StrCat("Signalling callback raised: ", error.what()));
}

}  // namespace

absl::Status SignallingMessage::Validate() const {
  absl::Status status = data::ValidateName(sender);
  if (!status.ok())
    return status;
  status = data::ValidateName(recipient);
  if (!status.ok())
    return status;
  switch (type) {
    case SignallingMessageType::kDescription:
      if (description.empty()) {
        return absl::InvalidArgumentError(
            "A signalling description must not be empty");
      }
      if (description_type != "offer" && description_type != "answer" &&
          description_type != "pranswer" && description_type != "rollback") {
        return absl::InvalidArgumentError(
            "A signalling description has an invalid SDP type");
      }
      break;
    case SignallingMessageType::kCandidate:
      if (candidate.empty()) {
        return absl::InvalidArgumentError(
            "A signalling ICE candidate must not be empty");
      }
      break;
    case SignallingMessageType::kError:
      if (error.ok()) {
        return absl::InvalidArgumentError(
            "A signalling error message must contain a non-OK status");
      }
      break;
  }
  return absl::OkStatus();
}

absl::StatusOr<std::string> SignallingMessage::ToJson() const {
  absl::Status validation = Validate();
  if (!validation.ok())
    return validation;
  try {
    nlohmann::json value = {
        {"type", MessageTypeName(type)},
        {"from", sender},
        {"to", recipient},
    };
    switch (type) {
      case SignallingMessageType::kDescription:
        value["description"] = description;
        value["description_type"] = description_type;
        // Compatibility with ActionEngine's signalling envelope.
        value["type"] = description_type;
        value["id"] = recipient;
        break;
      case SignallingMessageType::kCandidate:
        value["candidate"] = candidate;
        value["mid"] = mid;
        value["id"] = recipient;
        break;
      case SignallingMessageType::kError: {
        absl::StatusOr<nlohmann::json> encoded = StatusToJson(error);
        if (!encoded.ok())
          return encoded.status();
        value["status"] = std::move(*encoded);
        break;
      }
    }
    return value.dump();
  } catch (const std::exception& error) {
    return absl::InternalError(
        absl::StrCat("Failed to encode signalling JSON: ", error.what()));
  } catch (...) {
    return absl::InternalError(
        "Failed to encode signalling JSON with a non-standard exception");
  }
}

absl::StatusOr<SignallingMessage> SignallingMessage::FromJson(
    std::string_view encoded) {
  try {
    const nlohmann::json value = nlohmann::json::parse(encoded);
    if (!value.is_object()) {
      return absl::InvalidArgumentError(
          "A signalling message must be a JSON object");
    }
    const auto type_iterator = value.find("type");
    if (type_iterator == value.end() || !type_iterator->is_string()) {
      return absl::InvalidArgumentError(
          "A signalling message requires a string type");
    }
    const std::string type_name = type_iterator->get<std::string>();
    absl::StatusOr<SignallingMessageType> type = ParseMessageType(type_name);
    if (!type.ok())
      return type.status();
    SignallingMessage result;
    result.type = *type;
    const auto sender = value.find("from");
    const auto recipient = value.find("to");
    if (sender != value.end() && sender->is_string()) {
      result.sender = sender->get<std::string>();
    }
    if (recipient != value.end() && recipient->is_string()) {
      result.recipient = recipient->get<std::string>();
    } else if (const auto id = value.find("id");
               id != value.end() && id->is_string()) {
      result.recipient = id->get<std::string>();
    }
    if (result.type == SignallingMessageType::kDescription) {
      const auto description = value.find("description");
      if (description != value.end() && description->is_string()) {
        result.description = description->get<std::string>();
      }
      const auto description_type = value.find("description_type");
      result.description_type =
          description_type != value.end() && description_type->is_string()
              ? description_type->get<std::string>()
              : type_name;
    } else if (result.type == SignallingMessageType::kCandidate) {
      const auto candidate = value.find("candidate");
      if (candidate != value.end() && candidate->is_string()) {
        result.candidate = candidate->get<std::string>();
      }
      const auto mid = value.find("mid");
      if (mid != value.end() && mid->is_string()) {
        result.mid = mid->get<std::string>();
      }
    } else {
      const auto status = value.find("status");
      if (status == value.end()) {
        return absl::InvalidArgumentError(
            "A signalling error message requires status");
      }
      absl::StatusOr<absl::Status> decoded = StatusFromJson(*status);
      if (!decoded.ok())
        return decoded.status();
      result.error = std::move(*decoded);
    }
    absl::Status validation = result.Validate();
    if (!validation.ok())
      return validation;
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Failed to parse signalling JSON: ", error.what()));
  } catch (...) {
    return absl::InvalidArgumentError(
        "Failed to parse signalling JSON with a non-standard exception");
  }
}

struct SignallingEndpoint::State {
  State(std::string value_identity, OnSignallingMessage callback,
        std::weak_ptr<SignallingService> value_service)
      : identity(std::move(value_identity)),
        on_message(std::move(callback)),
        service(std::move(value_service)) {}

  mutable thread::Mutex mu;
  const std::string identity;
  OnSignallingMessage on_message ABSL_GUARDED_BY(mu);
  const std::weak_ptr<SignallingService> service;
  std::deque<SignallingMessage> incoming ABSL_GUARDED_BY(mu);
  bool pumping ABSL_GUARDED_BY(mu) = false;
  bool connected ABSL_GUARDED_BY(mu) = true;
  absl::Status status ABSL_GUARDED_BY(mu);
};

struct SignallingService::State {
  mutable thread::Mutex mu;
  bool stopped ABSL_GUARDED_BY(mu) = false;
  absl::flat_hash_map<std::string, std::weak_ptr<SignallingEndpoint::State>>
      endpoints ABSL_GUARDED_BY(mu);
};

std::shared_ptr<SignallingService> SignallingService::Create() {
  struct MakeSharedEnabler final : SignallingService {
    explicit MakeSharedEnabler(std::shared_ptr<State> state)
        : SignallingService(std::move(state)) {}
  };

  return std::make_shared<MakeSharedEnabler>(std::make_shared<State>());
}

SignallingService::~SignallingService() {
  (void)Stop();
}

absl::StatusOr<std::shared_ptr<SignallingEndpoint>> SignallingService::Connect(
    std::string identity, OnSignallingMessage on_message) {
  absl::Status validation = data::ValidateName(identity);
  if (!validation.ok())
    return validation;
  if (!on_message) {
    return absl::InvalidArgumentError(
        "Signalling on_message callback must be callable");
  }
  auto endpoint_state = std::make_shared<SignallingEndpoint::State>(
      std::move(identity), std::move(on_message), shared_from_this());
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->stopped) {
      return absl::FailedPreconditionError("Signalling service is stopped");
    }
    const auto existing = state_->endpoints.find(endpoint_state->identity);
    if (existing != state_->endpoints.end() && !existing->second.expired()) {
      return absl::AlreadyExistsError(
          absl::StrCat("Signalling identity is already connected: ",
                       endpoint_state->identity));
    }
    state_->endpoints.insert_or_assign(endpoint_state->identity,
                                       endpoint_state);
  }

  struct MakeSharedEnabler final : SignallingEndpoint {
    explicit MakeSharedEnabler(std::shared_ptr<SignallingEndpoint::State> state)
        : SignallingEndpoint(std::move(state)) {}
  };

  return std::make_shared<MakeSharedEnabler>(std::move(endpoint_state));
}

absl::Status SignallingService::Route(
    const std::shared_ptr<SignallingEndpoint::State>& sender,
    SignallingMessage message) {
  if (sender == nullptr) {
    return absl::FailedPreconditionError("Signalling sender is disconnected");
  }
  {
    thread::MutexLock lock(&sender->mu);
    if (!sender->connected)
      return sender->status;
    if (!message.sender.empty() && message.sender != sender->identity) {
      return absl::PermissionDeniedError(
          "A signalling endpoint cannot impersonate another identity");
    }
    message.sender = sender->identity;
  }
  absl::Status validation = message.Validate();
  if (!validation.ok())
    return validation;
  std::shared_ptr<SignallingEndpoint::State> recipient;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->stopped) {
      return absl::FailedPreconditionError("Signalling service is stopped");
    }
    const auto iterator = state_->endpoints.find(message.recipient);
    if (iterator == state_->endpoints.end() ||
        !(recipient = iterator->second.lock())) {
      return absl::NotFoundError(absl::StrCat(
          "Signalling recipient is not connected: ", message.recipient));
    }
  }
  bool start_pump = false;
  {
    thread::MutexLock lock(&recipient->mu);
    if (!recipient->connected)
      return recipient->status;
    recipient->incoming.push_back(std::move(message));
    if (!recipient->pumping) {
      recipient->pumping = true;
      start_pump = true;
    }
  }
  if (start_pump) {
    a11::Schedule([recipient]() { Pump(std::move(recipient)); });
  }
  return absl::OkStatus();
}

void SignallingService::Pump(
    const std::shared_ptr<SignallingEndpoint::State>& endpoint) {
  while (true) {
    SignallingMessage message;
    OnSignallingMessage callback;
    {
      thread::MutexLock lock(&endpoint->mu);
      if (!endpoint->connected || endpoint->incoming.empty()) {
        endpoint->pumping = false;
        return;
      }
      message = std::move(endpoint->incoming.front());
      endpoint->incoming.pop_front();
      callback = endpoint->on_message;
    }
    absl::Status status;
    try {
      status = callback(std::move(message)).Await().status();
    } catch (const std::exception& error) {
      status = CallbackException(error);
    } catch (...) {
      status = absl::UnknownError(
          "Signalling callback raised a non-standard exception");
    }
    if (!status.ok()) {
      std::shared_ptr<SignallingService> service = endpoint->service.lock();
      if (service != nullptr)
        service->Disconnect(endpoint, status);
      return;
    }
  }
}

void SignallingService::Disconnect(
    const std::shared_ptr<SignallingEndpoint::State>& endpoint,
    absl::Status status) {
  if (endpoint == nullptr)
    return;
  std::string identity;
  {
    thread::MutexLock lock(&endpoint->mu);
    if (!endpoint->connected)
      return;
    endpoint->connected = false;
    endpoint->status = status.ok()
                           ? absl::CancelledError("Signalling endpoint closed")
                           : std::move(status);
    endpoint->incoming.clear();
    endpoint->on_message = {};
    identity = endpoint->identity;
  }
  thread::MutexLock lock(&state_->mu);
  const auto iterator = state_->endpoints.find(identity);
  if (iterator != state_->endpoints.end()) {
    std::shared_ptr<SignallingEndpoint::State> current =
        iterator->second.lock();
    if (current == nullptr || current == endpoint)
      state_->endpoints.erase(iterator);
  }
}

bool SignallingService::Contains(std::string_view identity) const {
  thread::MutexLock lock(&state_->mu);
  const auto iterator = state_->endpoints.find(identity);
  return iterator != state_->endpoints.end() && !iterator->second.expired();
}

std::vector<std::string> SignallingService::Identities() const {
  std::vector<std::string> result;
  thread::MutexLock lock(&state_->mu);
  result.reserve(state_->endpoints.size());
  for (const auto& [identity, endpoint] : state_->endpoints) {
    if (!endpoint.expired())
      result.push_back(identity);
  }
  return result;
}

absl::Status SignallingService::Stop() {
  std::vector<std::shared_ptr<SignallingEndpoint::State>> endpoints;
  {
    thread::MutexLock lock(&state_->mu);
    if (state_->stopped)
      return absl::OkStatus();
    state_->stopped = true;
    for (const auto& [identity, endpoint] : state_->endpoints) {
      (void)identity;
      if (auto connected = endpoint.lock()) {
        endpoints.push_back(std::move(connected));
      }
    }
    state_->endpoints.clear();
  }
  for (const auto& endpoint : endpoints) {
    thread::MutexLock lock(&endpoint->mu);
    endpoint->connected = false;
    endpoint->status = absl::CancelledError("Signalling service stopped");
    endpoint->incoming.clear();
    endpoint->on_message = {};
  }
  return absl::OkStatus();
}

SignallingEndpoint::~SignallingEndpoint() {
  (void)Close();
}

absl::Status SignallingEndpoint::Send(SignallingMessage message) {
  std::shared_ptr<SignallingService> service = state_->service.lock();
  if (service == nullptr) {
    return absl::FailedPreconditionError("Signalling service is unavailable");
  }
  return service->Route(state_, std::move(message));
}

absl::Status SignallingEndpoint::SetOnMessage(OnSignallingMessage on_message) {
  if (!on_message) {
    return absl::InvalidArgumentError(
        "Signalling on_message callback must be callable");
  }
  thread::MutexLock lock(&state_->mu);
  if (!state_->connected)
    return state_->status;
  state_->on_message = std::move(on_message);
  return absl::OkStatus();
}

absl::Status SignallingEndpoint::Close() {
  std::shared_ptr<SignallingService> service = state_->service.lock();
  if (service == nullptr)
    return absl::OkStatus();
  service->Disconnect(state_,
                      absl::CancelledError("Signalling endpoint closed"));
  return absl::OkStatus();
}

std::string SignallingEndpoint::identity() const {
  thread::MutexLock lock(&state_->mu);
  return state_->identity;
}

bool SignallingEndpoint::connected() const {
  thread::MutexLock lock(&state_->mu);
  return state_->connected;
}

absl::Status SignallingEndpoint::GetStatus() const {
  thread::MutexLock lock(&state_->mu);
  return state_->status;
}

}  // namespace a11::net
