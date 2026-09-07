// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef A11_ACTIONS_AUTHORIZATION_H_
#define A11_ACTIONS_AUTHORIZATION_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

namespace a11::actions {

class Action;
class ActionRegistry;
struct ActionSchema;

inline constexpr std::string_view kAuthorizationHeader = "x-a11-auth";
inline constexpr std::string_view kAuthorizationReferenceHeader =
    "x-a11-auth-ref";
inline constexpr std::string_view kAuthorizationDefaultHeader =
    "x-a11-auth-default";
inline constexpr std::string_view kAuthorizationReplaceHeader =
    "x-a11-auth-replace";
inline constexpr std::string_view kAuthorizeAction = "__authorize__";
inline constexpr int kAuthorizationVersion = 1;
inline constexpr size_t kMaxAuthorizationBytes = 16 * 1024;
inline constexpr size_t kMaxAuthorizationHops = 8;

/** A versioned ordered sequence of compact signed delegation statements. */
struct AuthorizationEnvelope {
  int version = kAuthorizationVersion;
  std::vector<std::string> chain;

  friend bool operator==(const AuthorizationEnvelope&,
                         const AuthorizationEnvelope&) = default;
};

/** Identity and effective authority returned by an application verifier. */
struct VerifiedAuthorization {
  AuthorizationEnvelope envelope;
  std::string subject;
  std::string subject_kind;
  std::vector<std::string> actors;
  std::vector<std::string> provenance;
  std::string assurance;
  std::string audience;
  absl::Time expires_at = absl::UnixEpoch();
  nlohmann::json grants = nlohmann::json::array();
  nlohmann::json restrictions = nlohmann::json::object();
  std::int64_t authorization_epoch = 0;
};

/** Result of installing a verified connection context. */
struct AuthorizationContext {
  std::string context_id;
  std::shared_ptr<const VerifiedAuthorization> authorization;
  bool is_default = false;
};

/** Bounded verified contexts scoped to the receiving Session and stream. */
class AuthorizationContextStore {
 public:
  struct State;

  explicit AuthorizationContextStore(size_t max_contexts_per_stream = 128);
  ~AuthorizationContextStore();

  absl::StatusOr<AuthorizationContext> Install(
      const std::shared_ptr<Action>& action,
      std::shared_ptr<const VerifiedAuthorization> authorization,
      bool make_default = true,
      std::optional<std::string> replace = std::nullopt);
  absl::StatusOr<std::shared_ptr<const VerifiedAuthorization>> Resolve(
      const std::shared_ptr<Action>& action);
  absl::Status ClearStream(const Action& action);
  void ClearSession(std::string_view session_id);

 private:
  std::unique_ptr<State> state_;
};

using AuthorizationVerifier =
    std::function<absl::StatusOr<VerifiedAuthorization>(std::string_view)>;

/** Schema of the transport-independent connection authorizing action. */
ActionSchema AuthorizationActionSchema();

/** Register `__authorize__` for a native verifier and context store. */
absl::StatusOr<std::shared_ptr<AuthorizationContextStore>> InstallAuthorizer(
    const std::shared_ptr<ActionRegistry>& registry,
    AuthorizationVerifier verifier,
    std::shared_ptr<AuthorizationContextStore> contexts = nullptr);

/** Validate and canonically encode an authorization envelope as MessagePack. */
absl::StatusOr<std::string> EncodeAuthorization(
    const AuthorizationEnvelope& envelope);

/** Decode a canonical, bounded native authorization value. */
absl::StatusOr<AuthorizationEnvelope> DecodeAuthorization(
    std::string_view value);

/** Convert a native value to an ASCII physical HTTP header. */
absl::StatusOr<std::string> AuthorizationToText(
    const AuthorizationEnvelope& envelope);

/** Return an unpadded base64url SHA-256 identifier for the exact envelope. */
absl::StatusOr<std::string> AuthorizationFingerprint(
    const AuthorizationEnvelope& envelope);

/** Convert an ASCII physical HTTP header to the native envelope. */
absl::StatusOr<AuthorizationEnvelope> AuthorizationFromText(
    std::string_view value);

/** Decode the complete authorization on an action, when present. */
absl::StatusOr<std::optional<AuthorizationEnvelope>> GetAuthorization(
    const Action& action);

/** Set or remove a complete authorization and clear any context reference. */
absl::Status SetAuthorization(
    const std::shared_ptr<Action>& action,
    const std::optional<AuthorizationEnvelope>& envelope);

/** Read the receiver-issued raw 128-bit context reference, when present. */
absl::StatusOr<std::optional<std::string>> GetAuthorizationReference(
    const Action& action);

/** Set or remove a raw 128-bit context reference and clear any full proof. */
absl::Status SetAuthorizationReference(
    const std::shared_ptr<Action>& action,
    const std::optional<std::string>& context_id);

}  // namespace a11::actions

#endif  // A11_ACTIONS_AUTHORIZATION_H_
