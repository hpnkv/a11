// Copyright 2026 The A11 Authors

#include "a11/actions/authorization.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/log/check.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/escaping.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/data/msgpack.h"
#include "a11/data/serialization.h"
#include "a11/json_codec.h"
#include "a11/net/wire_stream.h"
#include "a11/nodes/async_node.h"
#include "a11/service/session.h"
#include "thread/boost_primitives.h"

namespace a11::actions {
namespace {

absl::Status Validate(const AuthorizationEnvelope& envelope) {
  if (envelope.version != kAuthorizationVersion) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Authorization version ", envelope.version, " is not supported"));
  }
  if (envelope.chain.empty() || envelope.chain.size() > kMaxAuthorizationHops) {
    return absl::InvalidArgumentError(
        "An authorization chain must contain between one and eight statements");
  }
  for (const std::string& statement : envelope.chain) {
    if (statement.empty() ||
        std::count(statement.begin(), statement.end(), '.') != 2 ||
        std::any_of(statement.begin(), statement.end(),
                    [](unsigned char c) { return c > 0x7f; })) {
      return absl::InvalidArgumentError(
          "Every authorization statement must be an ASCII compact JWS");
    }
  }
  return absl::OkStatus();
}

std::string UnpaddedWebSafeBase64(std::string_view value) {
  std::string encoded = absl::WebSafeBase64Escape(value);
  while (encoded.ends_with('=')) {
    encoded.pop_back();
  }
  return encoded;
}

bool GlobMatches(std::string_view pattern, std::string_view value) {
  size_t pattern_at = 0;
  size_t value_at = 0;
  size_t star = std::string_view::npos;
  size_t retry = 0;
  while (value_at < value.size()) {
    if (pattern_at < pattern.size() &&
        (pattern[pattern_at] == '?' ||
         pattern[pattern_at] == value[value_at])) {
      ++pattern_at;
      ++value_at;
    } else if (pattern_at < pattern.size() && pattern[pattern_at] == '*') {
      star = pattern_at++;
      retry = value_at;
    } else if (star != std::string_view::npos) {
      pattern_at = star + 1;
      value_at = ++retry;
    } else {
      return false;
    }
  }
  while (pattern_at < pattern.size() && pattern[pattern_at] == '*') {
    ++pattern_at;
  }
  return pattern_at == pattern.size();
}

absl::Status EnforceRequestRestrictions(
    const VerifiedAuthorization& authorization, const Action& action) {
  const nlohmann::json& restrictions = authorization.restrictions;
  if (!restrictions.is_object()) {
    return absl::PermissionDeniedError(
        "Authorization restrictions are not an object");
  }
  if (const auto actions = restrictions.find("actions");
      actions != restrictions.end()) {
    if (!actions->is_array()) {
      return absl::PermissionDeniedError(
          "The authorization action restriction is invalid");
    }
    const std::string& name = action.GetSchema().name;
    bool allowed = false;
    for (const nlohmann::json& pattern : *actions) {
      if (!pattern.is_string()) {
        return absl::PermissionDeniedError(
            "The authorization action restriction is invalid");
      }
      allowed =
          allowed || GlobMatches(pattern.get_ref<const std::string&>(), name);
    }
    if (!allowed) {
      return absl::PermissionDeniedError(
          "The authorization does not allow this action");
    }
  }
  if (const auto identities = restrictions.find("identities");
      identities != restrictions.end()) {
    if (!identities->is_array() ||
        std::find(identities->begin(), identities->end(),
                  authorization.audience) == identities->end()) {
      return absl::PermissionDeniedError(
          "The authorization does not allow this identity");
    }
  }
  if (const auto request_id = restrictions.find("request_id");
      request_id != restrictions.end() &&
      (!request_id->is_string() || *request_id != action.GetId())) {
    return absl::PermissionDeniedError(
        "The authorization is bound to a different request");
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<std::string> EncodeAuthorization(
    const AuthorizationEnvelope& envelope) {
  ABSL_RETURN_IF_ERROR(Validate(envelope));
  nlohmann::json value = nlohmann::json::array();
  value.push_back(envelope.version);
  value.push_back(envelope.chain);
  data::MsgpackWriter writer;
  ABSL_RETURN_IF_ERROR(writer.Pack(value));
  std::string encoded = writer.TakeBytes();
  if (encoded.size() > kMaxAuthorizationBytes) {
    return absl::InvalidArgumentError(
        "An authorization value may contain at most 16384 bytes");
  }
  return encoded;
}

absl::StatusOr<AuthorizationEnvelope> DecodeAuthorization(
    std::string_view value) {
  if (value.size() > kMaxAuthorizationBytes) {
    return absl::InvalidArgumentError(
        "An authorization value may contain at most 16384 bytes");
  }
  data::MsgpackReader reader(value);
  ABSL_ASSIGN_OR_RETURN(nlohmann::json decoded, reader.Read());
  ABSL_RETURN_IF_ERROR(reader.EnsureFullyConsumed());
  if (!decoded.is_array() || decoded.size() != 2 ||
      !decoded[0].is_number_integer() || !decoded[1].is_array()) {
    return absl::InvalidArgumentError(
        "The authorization envelope must be [version, chain]");
  }
  AuthorizationEnvelope envelope;
  const std::int64_t version = decoded[0].get<std::int64_t>();
  if (version != kAuthorizationVersion) {
    return absl::InvalidArgumentError(
        "The authorization version is not supported");
  }
  envelope.version = kAuthorizationVersion;
  for (const nlohmann::json& statement : decoded[1]) {
    if (!statement.is_string()) {
      return absl::InvalidArgumentError(
          "The authorization chain must contain JWS strings");
    }
    envelope.chain.push_back(statement.get<std::string>());
  }
  ABSL_ASSIGN_OR_RETURN(const std::string canonical,
                        EncodeAuthorization(envelope));
  if (canonical != value) {
    return absl::InvalidArgumentError(
        "The authorization envelope is not canonically encoded");
  }
  return envelope;
}

absl::StatusOr<std::string> AuthorizationToText(
    const AuthorizationEnvelope& envelope) {
  ABSL_ASSIGN_OR_RETURN(const std::string encoded,
                        EncodeAuthorization(envelope));
  return absl::StrCat("a11-auth/1.", UnpaddedWebSafeBase64(encoded));
}

absl::StatusOr<std::string> AuthorizationFingerprint(
    const AuthorizationEnvelope& envelope) {
  ABSL_ASSIGN_OR_RETURN(const std::string encoded,
                        EncodeAuthorization(envelope));
  std::string digest(SHA256_DIGEST_LENGTH, '\0');
  SHA256(reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size(),
         reinterpret_cast<unsigned char*>(digest.data()));
  return UnpaddedWebSafeBase64(digest);
}

absl::StatusOr<AuthorizationEnvelope> AuthorizationFromText(
    std::string_view value) {
  constexpr std::string_view prefix = "a11-auth/1.";
  if (!value.starts_with(prefix)) {
    return absl::InvalidArgumentError(
        "The authorization value must start with 'a11-auth/1.'");
  }
  const std::string_view body = value.substr(prefix.size());
  if (std::any_of(body.begin(), body.end(), [](unsigned char c) {
        return !(std::isalnum(c) != 0 || c == '-' || c == '_');
      })) {
    return absl::InvalidArgumentError(
        "The textual authorization value is not base64url");
  }
  std::string decoded;
  if (!absl::WebSafeBase64Unescape(body, &decoded)) {
    return absl::InvalidArgumentError(
        "The textual authorization value is not base64url");
  }
  return DecodeAuthorization(decoded);
}

absl::StatusOr<std::optional<AuthorizationEnvelope>> GetAuthorization(
    const Action& action) {
  ABSL_ASSIGN_OR_RETURN(std::optional<data::Bytes> full,
                        action.GetHeader(kAuthorizationHeader));
  ABSL_ASSIGN_OR_RETURN(std::optional<data::Bytes> reference,
                        action.GetHeader(kAuthorizationReferenceHeader));
  if (full.has_value() && reference.has_value()) {
    return absl::InvalidArgumentError(
        "An action cannot carry both authorization forms");
  }
  if (!full.has_value()) {
    return std::nullopt;
  }
  ABSL_ASSIGN_OR_RETURN(AuthorizationEnvelope envelope,
                        DecodeAuthorization(*full));
  return std::optional<AuthorizationEnvelope>(std::move(envelope));
}

absl::Status SetAuthorization(
    const std::shared_ptr<Action>& action,
    const std::optional<AuthorizationEnvelope>& envelope) {
  if (action == nullptr) {
    return absl::InvalidArgumentError("action must not be null");
  }
  if (!envelope.has_value()) {
    return action->RemoveHeader(kAuthorizationHeader);
  }
  ABSL_ASSIGN_OR_RETURN(std::string encoded, EncodeAuthorization(*envelope));
  ABSL_RETURN_IF_ERROR(
      action->SetHeader(std::string(kAuthorizationHeader), std::move(encoded)));
  return action->RemoveHeader(kAuthorizationReferenceHeader);
}

absl::StatusOr<std::optional<std::string>> GetAuthorizationReference(
    const Action& action) {
  ABSL_ASSIGN_OR_RETURN(std::optional<data::Bytes> reference,
                        action.GetHeader(kAuthorizationReferenceHeader));
  if (reference.has_value() && reference->size() != 16) {
    return absl::InvalidArgumentError(
        "An authorization context ID must contain 128 bits");
  }
  return reference;
}

absl::Status SetAuthorizationReference(
    const std::shared_ptr<Action>& action,
    const std::optional<std::string>& context_id) {
  if (action == nullptr) {
    return absl::InvalidArgumentError("action must not be null");
  }
  if (!context_id.has_value()) {
    return action->RemoveHeader(kAuthorizationReferenceHeader);
  }
  if (context_id->size() != 16) {
    return absl::InvalidArgumentError(
        "An authorization context ID must contain 128 bits");
  }
  ABSL_RETURN_IF_ERROR(action->SetHeader(
      std::string(kAuthorizationReferenceHeader), *context_id));
  return action->RemoveHeader(kAuthorizationHeader);
}

struct AuthorizationContextStore::State {
  struct Held {
    std::shared_ptr<const VerifiedAuthorization> authorization;
    absl::Time last_used = absl::Now();
  };

  struct Stream {
    absl::flat_hash_map<std::string, Held> contexts;
    std::optional<std::string> default_context;
  };

  struct Session {
    std::weak_ptr<service::Session> owner;
    absl::flat_hash_map<std::string, Stream> streams;
  };

  explicit State(size_t maximum) : maximum(maximum) {}

  thread::Mutex mu;
  const size_t maximum;
  absl::flat_hash_map<std::string, Session> sessions ABSL_GUARDED_BY(mu);
};

namespace {

struct AuthorizationScope {
  std::shared_ptr<service::Session> session;
  std::string session_id;
  std::string stream_id;
};

absl::StatusOr<AuthorizationScope> ScopeOf(const Action& action) {
  const std::shared_ptr<service::Session> session = action.GetSession();
  const std::shared_ptr<net::WireStream> stream = action.GetStream();
  if (session == nullptr || stream == nullptr) {
    return absl::UnauthenticatedError(
        "Authorization contexts need an action received on a Session stream");
  }
  return AuthorizationScope{.session = session,
                            .session_id = session->GetId(),
                            .stream_id = stream->GetId()};
}

void Prune(AuthorizationContextStore::State::Stream* stream, absl::Time now) {
  for (auto it = stream->contexts.begin(); it != stream->contexts.end();) {
    if (it->second.authorization->expires_at <= now) {
      if (stream->default_context == it->first) {
        stream->default_context.reset();
      }
      auto expired = it++;
      stream->contexts.erase(expired);
    } else {
      ++it;
    }
  }
}

absl::StatusOr<std::string> RandomContextId() {
  std::string result(16, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char*>(result.data()),
                 static_cast<int>(result.size())) != 1) {
    return absl::InternalError(
        "Could not generate an authorization context ID");
  }
  return result;
}

}  // namespace

AuthorizationContextStore::AuthorizationContextStore(
    size_t max_contexts_per_stream)
    : state_(std::make_unique<State>(max_contexts_per_stream)) {
  CHECK_GT(max_contexts_per_stream, 0);
}

AuthorizationContextStore::~AuthorizationContextStore() = default;

absl::StatusOr<AuthorizationContext> AuthorizationContextStore::Install(
    const std::shared_ptr<Action>& action,
    std::shared_ptr<const VerifiedAuthorization> authorization,
    bool make_default, std::optional<std::string> replace) {
  if (action == nullptr || authorization == nullptr) {
    return absl::InvalidArgumentError(
        "action and authorization must not be null");
  }
  if (authorization->expires_at <= absl::Now()) {
    return absl::UnauthenticatedError("The authorization chain has expired");
  }
  if (replace.has_value() && replace->size() != 16) {
    return absl::InvalidArgumentError(
        "The replacement context ID must contain 128 bits");
  }
  ABSL_ASSIGN_OR_RETURN(const AuthorizationScope scope, ScopeOf(*action));
  thread::MutexLock lock(&state_->mu);
  State::Session& held_session = state_->sessions[scope.session_id];
  if (held_session.owner.lock().get() != scope.session.get()) {
    held_session = State::Session{.owner = scope.session, .streams = {}};
  }
  State::Stream& stream = held_session.streams[scope.stream_id];
  Prune(&stream, absl::Now());
  if (replace.has_value() &&
      stream.contexts.find(*replace) == stream.contexts.end()) {
    return absl::UnauthenticatedError(
        "The authorization context to replace is unknown");
  }
  if (!replace.has_value() && stream.contexts.size() >= state_->maximum) {
    auto victim = stream.contexts.end();
    for (auto it = stream.contexts.begin(); it != stream.contexts.end(); ++it) {
      if (stream.default_context == it->first) {
        continue;
      }
      if (victim == stream.contexts.end() ||
          it->second.last_used < victim->second.last_used) {
        victim = it;
      }
    }
    if (victim == stream.contexts.end()) {
      return absl::ResourceExhaustedError(
          "This stream has too many authorization contexts");
    }
    stream.contexts.erase(victim);
  }
  std::string context_id;
  do {
    ABSL_ASSIGN_OR_RETURN(context_id, RandomContextId());
  } while (stream.contexts.find(context_id) != stream.contexts.end());
  stream.contexts.emplace(
      context_id,
      State::Held{.authorization = authorization, .last_used = absl::Now()});
  if (make_default) {
    stream.default_context = context_id;
  }
  if (replace.has_value()) {
    if (!make_default && stream.default_context == *replace) {
      stream.default_context.reset();
    }
    stream.contexts.erase(*replace);
  }
  return AuthorizationContext{.context_id = std::move(context_id),
                              .authorization = std::move(authorization),
                              .is_default = make_default};
}

absl::StatusOr<std::shared_ptr<const VerifiedAuthorization>>
AuthorizationContextStore::Resolve(const std::shared_ptr<Action>& action) {
  if (action == nullptr) {
    return absl::InvalidArgumentError("action must not be null");
  }
  if (std::shared_ptr<const VerifiedAuthorization> bound =
          action->GetVerifiedAuthorization();
      bound != nullptr) {
    return bound;
  }
  ABSL_ASSIGN_OR_RETURN(std::optional<data::Bytes> full,
                        action->GetHeader(kAuthorizationHeader));
  if (full.has_value()) {
    return absl::UnauthenticatedError(
        "A complete authorization must be verified before it is used");
  }
  ABSL_ASSIGN_OR_RETURN(const AuthorizationScope scope, ScopeOf(*action));
  ABSL_ASSIGN_OR_RETURN(std::optional<std::string> reference,
                        GetAuthorizationReference(*action));
  std::shared_ptr<const VerifiedAuthorization> authorization;
  {
    thread::MutexLock lock(&state_->mu);
    auto session = state_->sessions.find(scope.session_id);
    if (session == state_->sessions.end()) {
      return absl::UnauthenticatedError(
          "This stream has no authorization context");
    }
    if (session->second.owner.lock().get() != scope.session.get()) {
      return absl::UnauthenticatedError(
          "This stream has no authorization context");
    }
    auto stream = session->second.streams.find(scope.stream_id);
    if (stream == session->second.streams.end()) {
      return absl::UnauthenticatedError(
          "This stream has no authorization context");
    }
    Prune(&stream->second, absl::Now());
    const std::optional<std::string>& selected =
        reference.has_value() ? reference : stream->second.default_context;
    if (!selected.has_value()) {
      return absl::UnauthenticatedError(
          "This stream has no authorization context");
    }
    auto found = stream->second.contexts.find(*selected);
    if (found == stream->second.contexts.end()) {
      return absl::UnauthenticatedError(
          "The authorization context is unknown or expired");
    }
    found->second.last_used = absl::Now();
    authorization = found->second.authorization;
  }
  ABSL_RETURN_IF_ERROR(EnforceRequestRestrictions(*authorization, *action));
  ABSL_RETURN_IF_ERROR(action->BindVerifiedAuthorization(authorization));
  return authorization;
}

absl::Status AuthorizationContextStore::ClearStream(const Action& action) {
  ABSL_ASSIGN_OR_RETURN(const AuthorizationScope scope, ScopeOf(action));
  thread::MutexLock lock(&state_->mu);
  auto session = state_->sessions.find(scope.session_id);
  if (session != state_->sessions.end() &&
      session->second.owner.lock().get() == scope.session.get()) {
    session->second.streams.erase(scope.stream_id);
    if (session->second.streams.empty()) {
      state_->sessions.erase(session);
    }
  }
  return absl::OkStatus();
}

void AuthorizationContextStore::ClearSession(std::string_view session_id) {
  thread::MutexLock lock(&state_->mu);
  state_->sessions.erase(session_id);
}

ActionSchema AuthorizationActionSchema() {
  ActionSchema schema;
  schema.name = std::string(kAuthorizeAction);
  schema.description = "Verify authorization and bind it to this connection.";
  schema.outputs.emplace(
      "output",
      ActionPortSchema{.name = "output",
                       .type = std::string(data::kJsonMimetype),
                       .description = "The receiver-issued connection context.",
                       .required = true,
                       .unary = true});
  schema.headers.emplace(
      kAuthorizationHeader,
      ActionHeaderSchema{.name = std::string(kAuthorizationHeader),
                         .description = "A signed A11 authorization chain."});
  schema.headers.emplace(
      kAuthorizationDefaultHeader,
      ActionHeaderSchema{
          .name = std::string(kAuthorizationDefaultHeader),
          .description = "Whether this becomes the stream default."});
  schema.headers.emplace(
      kAuthorizationReplaceHeader,
      ActionHeaderSchema{
          .name = std::string(kAuthorizationReplaceHeader),
          .description = "Context ID replaced after verification."});
  return schema;
}

absl::StatusOr<std::shared_ptr<AuthorizationContextStore>> InstallAuthorizer(
    const std::shared_ptr<ActionRegistry>& registry,
    AuthorizationVerifier verifier,
    std::shared_ptr<AuthorizationContextStore> contexts) {
  if (registry == nullptr || !verifier) {
    return absl::InvalidArgumentError(
        "registry and authorization verifier are required");
  }
  if (contexts == nullptr) {
    contexts = std::make_shared<AuthorizationContextStore>();
  }
  const auto handler =
      [verifier = std::move(verifier),
       contexts](const std::shared_ptr<Action>& action) -> absl::Status {
    ABSL_ASSIGN_OR_RETURN(std::optional<data::Bytes> raw,
                          action->GetHeader(kAuthorizationHeader));
    if (!raw.has_value()) {
      return absl::UnauthenticatedError(
          "The __authorize__ action needs x-a11-auth");
    }
    ABSL_ASSIGN_OR_RETURN(std::optional<data::Bytes> reference,
                          action->GetHeader(kAuthorizationReferenceHeader));
    if (reference.has_value()) {
      return absl::InvalidArgumentError(
          "__authorize__ cannot use x-a11-auth-ref");
    }
    ABSL_ASSIGN_OR_RETURN(std::optional<data::Bytes> default_value,
                          action->GetHeader(kAuthorizationDefaultHeader));
    ABSL_ASSIGN_OR_RETURN(std::optional<data::Bytes> replace,
                          action->GetHeader(kAuthorizationReplaceHeader));
    ABSL_ASSIGN_OR_RETURN(VerifiedAuthorization verified, verifier(*raw));
    auto held = std::make_shared<VerifiedAuthorization>(std::move(verified));
    ABSL_ASSIGN_OR_RETURN(
        AuthorizationContext context,
        contexts->Install(action, held,
                          !default_value.has_value() || *default_value != "0",
                          replace));
    ABSL_ASSIGN_OR_RETURN(const std::string fingerprint,
                          AuthorizationFingerprint(held->envelope));
    nlohmann::json response = {
        {"context_id", UnpaddedWebSafeBase64(context.context_id)},
        {"expires_at",
         absl::ToDoubleSeconds(held->expires_at - absl::UnixEpoch())},
        {"fingerprint", fingerprint},
        {"is_default", context.is_default},
    };
    ABSL_ASSIGN_OR_RETURN(std::string encoded,
                          DumpJson(response, "authorization context"));
    data::Chunk chunk;
    chunk.data = std::move(encoded);
    chunk.metadata =
        data::ChunkMetadata{.mimetype = std::string(data::kJsonMimetype)};
    ABSL_ASSIGN_OR_RETURN(std::shared_ptr<nodes::AsyncNode> output,
                          action->GetOutput("output"));
    return output->Finalize(std::move(chunk)).Await().status();
  };
  ABSL_RETURN_IF_ERROR(registry->RegisterSync(
      std::string(kAuthorizeAction), AuthorizationActionSchema(), handler));
  registry->SetAuthorizationContexts(contexts);
  return contexts;
}

}  // namespace a11::actions
