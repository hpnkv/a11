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

#ifndef A11_JVM_BRIDGE_H_
#define A11_JVM_BRIDGE_H_

#include <any>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/concurrency/future.h"
#include "a11/data/json.h"
#include "a11/status.h"
#include "jvm/value_reference.h"

namespace a11::actions {
struct ActionSchema;
}

namespace a11::jvm {
using Operation = std::function<absl::StatusOr<nlohmann::json>(
    std::int64_t, const nlohmann::json&)>;

std::int64_t Retain(std::any value);
absl::StatusOr<std::any> Lookup(std::int64_t id);
void Release(std::int64_t id);
size_t HandleCount();
size_t HandleCount(const std::type_info& type);
nlohmann::json Envelope(const absl::StatusOr<nlohmann::json>& result);
absl::StatusOr<nlohmann::json> Dispatch(const nlohmann::json& request);
void EnqueueCompletion(std::int64_t token,
                       std::function<nlohmann::json()> encode);
absl::Status Subscribe(std::int64_t token);
void Unsubscribe(std::int64_t token);
nlohmann::json TakeEvents();
void Shutdown();
absl::Status CompleteCallback(std::int64_t id, absl::Status status);

absl::StatusOr<actions::ActionSchema> ReadSchema(const nlohmann::json& fields);
absl::StatusOr<nlohmann::json> SchemaFields(
    const actions::ActionSchema& schema);
void BindSchemas(
    absl::flat_hash_map<std::string, Operation>* absl_nonnull operations);
void BindCore(
    absl::flat_hash_map<std::string, Operation>* absl_nonnull operations);
void BindStores(
    absl::flat_hash_map<std::string, Operation>* absl_nonnull operations);
void BindNet(
    absl::flat_hash_map<std::string, Operation>* absl_nonnull operations);
void BindFlow(
    absl::flat_hash_map<std::string, Operation>* absl_nonnull operations);

inline nlohmann::json Null() {
  return nullptr;
}

inline nlohmann::json Handle(std::int64_t id) {
  return nlohmann::json{{"handle", id}};
}

template <typename T>
nlohmann::json Save(std::shared_ptr<T> value) {
  if (value == nullptr) {
    return nullptr;
  }
  return Handle(Retain(std::move(value)));
}

template <typename T>
absl::StatusOr<std::shared_ptr<T>> Object(std::int64_t id) {
  ABSL_ASSIGN_OR_RETURN(std::any value, Lookup(id));
  const auto* pointer = std::any_cast<std::shared_ptr<T>>(&value);
  if (pointer == nullptr) {
    return absl::InvalidArgumentError("Native handle has the wrong type");
  }
  return *pointer;
}

template <typename T>
absl::StatusOr<T> Arg(const nlohmann::json& args, std::string_view name) {
  const auto found = args.find(name);
  if (found == args.end()) {
    return absl::InvalidArgumentError("Missing native argument: " +
                                      std::string(name));
  }
  if constexpr (std::is_same_v<T, nlohmann::json>) {
    return *found;
  } else if constexpr (std::is_same_v<T, std::string>) {
    if (found->is_binary()) {
      const auto& bytes = found->get_binary();
      return std::string(bytes.begin(), bytes.end());
    }
    if (found->is_string()) {
      return found->get<std::string>();
    }
  } else if constexpr (std::is_same_v<T, bool>) {
    if (found->is_boolean()) {
      return found->get<bool>();
    }
  } else if constexpr (std::is_integral_v<T>) {
    if (found->is_number_integer()) {
      if constexpr (std::is_unsigned_v<T>) {
        if (!found->is_number_unsigned() && found->get<std::int64_t>() < 0) {
          return absl::OutOfRangeError("Negative unsigned native argument");
        }
        const auto number = found->get<std::uint64_t>();
        if (number <= std::numeric_limits<T>::max()) {
          return static_cast<T>(number);
        }
      } else {
        if (found->is_number_unsigned() &&
            found->get<std::uint64_t>() >
                static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
          return absl::OutOfRangeError(
              "Native integer argument exceeds its range");
        }
        const auto number = found->get<std::int64_t>();
        if (number >= std::numeric_limits<T>::min() &&
            number <= std::numeric_limits<T>::max()) {
          return static_cast<T>(number);
        }
      }
      return absl::OutOfRangeError("Native integer argument exceeds its range");
    }
  } else if constexpr (std::is_floating_point_v<T>) {
    if (found->is_number()) {
      return found->get<T>();
    }
  }
  return absl::InvalidArgumentError("Invalid native argument: " +
                                    std::string(name));
}

template <typename T>
absl::StatusOr<T> DefaultArg(const nlohmann::json& args, std::string_view name,
                             T fallback) {
  if (args.find(name) == args.end() || args.at(name).is_null()) {
    return fallback;
  }
  return Arg<T>(args, name);
}

template <typename T>
absl::StatusOr<std::shared_ptr<T>> ObjectArg(const nlohmann::json& args,
                                             std::string_view name,
                                             bool optional = false) {
  ABSL_ASSIGN_OR_RETURN(nlohmann::json raw,
                        DefaultArg<nlohmann::json>(args, name, nullptr));
  if (optional && raw.is_null()) {
    return nullptr;
  }
  if (!raw.is_object()) {
    return absl::InvalidArgumentError("Expected a native handle");
  }
  ABSL_ASSIGN_OR_RETURN(auto id, Arg<std::int64_t>(raw, "handle"));
  return Object<T>(id);
}

inline nlohmann::json Binary(std::string_view value) {
  return nlohmann::json::binary(
      std::vector<std::uint8_t>(value.begin(), value.end()));
}

inline absl::StatusOr<nlohmann::json> Result(absl::Status status) {
  if (!status.ok()) {
    return status;
  }
  return Null();
}

inline absl::StatusOr<nlohmann::json> Encode(const nlohmann::json& value) {
  return value;
}

inline absl::StatusOr<nlohmann::json> Encode(const std::string& value) {
  return nlohmann::json(value);
}

inline absl::StatusOr<nlohmann::json> Encode(const absl::Status& value) {
  return StatusToJson(value);
}

inline absl::StatusOr<nlohmann::json> Encode(const Unit&) {
  return Null();
}

inline absl::StatusOr<nlohmann::json> Encode(const data::WireMessage& value) {
  return Save(std::make_shared<ValueReference<data::WireMessage>>(value));
}

inline absl::StatusOr<nlohmann::json> Encode(const data::Chunk& value) {
  return Save(std::make_shared<ValueReference<data::Chunk>>(value));
}

inline absl::StatusOr<nlohmann::json> Encode(const data::NodeFragment& value) {
  return Save(std::make_shared<ValueReference<data::NodeFragment>>(value));
}

template <typename T>
requires std::is_arithmetic_v<T> absl::StatusOr<nlohmann::json> Encode(
    T value) {
  return nlohmann::json(value);
}

template <typename T>
absl::StatusOr<nlohmann::json> Encode(const std::shared_ptr<T>& value) {
  return Save(value);
}

template <typename T>
absl::StatusOr<nlohmann::json> Encode(const std::optional<T>& value) {
  if (!value.has_value()) {
    return Null();
  }
  return Encode(*value);
}

template <typename T>
absl::StatusOr<nlohmann::json> Encode(const std::vector<T>& values) {
  nlohmann::json result = nlohmann::json::array();
  for (const auto& value : values) {
    ABSL_ASSIGN_OR_RETURN(nlohmann::json encoded, Encode(value));
    result.push_back(std::move(encoded));
  }
  return result;
}

class Callback final : public std::enable_shared_from_this<Callback> {
 public:
  explicit Callback(std::int64_t token) : token_(token) {}

  ~Callback();
  Task Invoke(nlohmann::json arguments, std::vector<std::int64_t> owned = {});

 private:
  std::int64_t token_;
};

class PendingOperation {
 public:
  virtual ~PendingOperation() = default;
  virtual void SubscribeResult(std::int64_t token) const = 0;
  virtual absl::Status Cancel() const = 0;
  [[nodiscard]] virtual bool IsReady() const = 0;
};

template <typename T>
struct OwnedHandles {
  static void Collect(const nlohmann::json&,
                      std::vector<std::int64_t>* absl_nonnull) {}
};

template <typename T>
struct OwnedHandles<std::shared_ptr<T>> {
  static void Collect(const nlohmann::json& value,
                      std::vector<std::int64_t>* absl_nonnull handles) {
    if (!value.is_null()) {
      handles->push_back(value.at("handle").get<std::int64_t>());
    }
  }
};

template <>
struct OwnedHandles<data::WireMessage>
    : OwnedHandles<std::shared_ptr<ValueReference<data::WireMessage>>> {};

template <>
struct OwnedHandles<data::Chunk>
    : OwnedHandles<std::shared_ptr<ValueReference<data::Chunk>>> {};

template <>
struct OwnedHandles<data::NodeFragment>
    : OwnedHandles<std::shared_ptr<ValueReference<data::NodeFragment>>> {};

template <typename T>
struct OwnedHandles<std::optional<T>> {
  static void Collect(const nlohmann::json& value,
                      std::vector<std::int64_t>* absl_nonnull handles) {
    if (!value.is_null()) {
      OwnedHandles<T>::Collect(value, handles);
    }
  }
};

template <typename T>
struct OwnedHandles<std::vector<T>> {
  static void Collect(const nlohmann::json& value,
                      std::vector<std::int64_t>* absl_nonnull handles) {
    for (const auto& item : value) {
      OwnedHandles<T>::Collect(item, handles);
    }
  }
};

template <typename T>
class PendingFuture final
    : public PendingOperation,
      public std::enable_shared_from_this<PendingFuture<T>> {
 public:
  explicit PendingFuture(Future<T> future) : future_(std::move(future)) {}

  void SubscribeResult(std::int64_t token) const override {
    future_.OnReady(
        [token, weak = this->weak_from_this()](const absl::StatusOr<T>&) {
          auto retained = weak.lock();
          if (retained == nullptr) {
            return;
          }
          EnqueueCompletion(token, [token, retained = std::move(retained)] {
            nlohmann::json event;
            // OnReady reads shared completion state without copying its payload.
            retained->future_.OnReady([&event,
                                       token](const absl::StatusOr<T>& result) {
              absl::StatusOr<nlohmann::json> encoded =
                  result.ok() ? Encode(*result)
                              : absl::StatusOr<nlohmann::json>(result.status());
              std::vector<std::int64_t> owned;
              if (encoded.ok()) {
                OwnedHandles<T>::Collect(*encoded, &owned);
              }
              event = nlohmann::json{{"token", token},
                                     {"result", Envelope(encoded)},
                                     {"owned", std::move(owned)}};
            });
            return event;
          });
        });
  }

  absl::Status Cancel() const override { return future_.Cancel(); }

  [[nodiscard]] bool IsReady() const override { return future_.IsReady(); }

 private:
  Future<T> future_;
};

template <typename T>
nlohmann::json Async(const Future<T>& source) {
  return Save(std::shared_ptr<PendingOperation>(
      std::make_shared<PendingFuture<T>>(source)));
}

absl::StatusOr<absl::Duration> ReadDuration(const nlohmann::json& value);
absl::StatusOr<absl::Time> ReadTime(const nlohmann::json& value);
absl::StatusOr<absl::Duration> Timeout(const nlohmann::json& args);
absl::StatusOr<absl::Time> Deadline(const nlohmann::json& args);
absl::StatusOr<std::optional<std::uint32_t>> Sequence(
    const nlohmann::json& args);
absl::StatusOr<absl::Status> StatusArg(const nlohmann::json& args,
                                       std::string_view name);

}  // namespace a11::jvm
#endif  // A11_JVM_BRIDGE_H_
