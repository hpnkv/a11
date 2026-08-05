// Copyright 2026 The A11 Authors.

#include "a11/data/serialization.h"

#include <algorithm>
#include <any>
#include <cctype>
#include <cstddef>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

#include <absl/base/no_destructor.h>
#include <absl/container/flat_hash_map.h>
#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_split.h>
#include <nlohmann/json.hpp>

#include "a11/data/msgpack.h"
#include "a11/data/types.h"
#include "thread/boost_primitives.h"

namespace a11::data {
namespace {

struct Mimetype {
  std::string media_type;
  absl::flat_hash_map<std::string, std::string> parameters;
};

std::string Trim(std::string_view input) {
  while (!input.empty() &&
         absl::ascii_isspace(static_cast<unsigned char>(input.front()))) {
    input.remove_prefix(1);
  }
  while (!input.empty() &&
         absl::ascii_isspace(static_cast<unsigned char>(input.back()))) {
    input.remove_suffix(1);
  }
  return std::string(input);
}

bool IsTokenChar(char value) {
  if (std::isalnum(static_cast<unsigned char>(value)) != 0) {
    return true;
  }
  constexpr std::string_view extra = "!#$%&'*+-.^_`|~";
  return extra.find(value) != std::string_view::npos;
}

absl::StatusOr<Mimetype> ParseMimetype(std::string_view input,
                                       bool allow_patterns) {
  std::vector<std::string_view> pieces = absl::StrSplit(input, ';');
  if (pieces.empty()) {
    return absl::InvalidArgumentError("Mimetype is empty");
  }
  std::string media_type = absl::AsciiStrToLower(Trim(pieces.front()));
  const size_t slash = media_type.find('/');
  if (slash == std::string::npos || slash == 0 ||
      slash + 1 == media_type.size() ||
      media_type.find('/', slash + 1) != std::string::npos) {
    return absl::InvalidArgumentError("Mimetype must contain type/subtype");
  }
  for (char value : media_type) {
    if (value == '/') {
      continue;
    }
    if (allow_patterns && value == '*') {
      continue;
    }
    if (!IsTokenChar(value)) {
      return absl::InvalidArgumentError("Mimetype contains an invalid token");
    }
  }
  if (!allow_patterns && media_type.find('*') != std::string::npos) {
    return absl::InvalidArgumentError(
        "Registered mimetypes cannot contain wildcards");
  }
  Mimetype result{.media_type = std::move(media_type)};
  for (size_t index = 1; index < pieces.size(); ++index) {
    std::string piece = Trim(pieces[index]);
    const size_t equal = piece.find('=');
    if (equal == std::string::npos || equal == 0) {
      return absl::InvalidArgumentError("Invalid mimetype parameter");
    }
    std::string key = absl::AsciiStrToLower(Trim(piece.substr(0, equal)));
    std::string value = Trim(piece.substr(equal + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    if (key.empty() || value.empty()) {
      return absl::InvalidArgumentError("Empty mimetype parameter");
    }
    if (!allow_patterns && value.find('*') != std::string::npos) {
      return absl::InvalidArgumentError(
          "Registered mimetype parameters cannot contain wildcards");
    }
    if (!result.parameters.emplace(std::move(key), std::move(value)).second) {
      return absl::InvalidArgumentError("Duplicate mimetype parameter");
    }
  }
  return result;
}

bool WildcardMatches(std::string_view value, std::string_view pattern) {
  const size_t wildcard = pattern.find('*');
  if (wildcard == std::string_view::npos) {
    return value == pattern;
  }
  const std::string_view prefix = pattern.substr(0, wildcard);
  const std::string_view suffix = pattern.substr(wildcard + 1);
  return value.size() >= prefix.size() + suffix.size() &&
         value.starts_with(prefix) && value.ends_with(suffix);
}

bool Matches(const Mimetype& registration, const Mimetype& selection) {
  if (!WildcardMatches(registration.media_type, selection.media_type)) {
    return false;
  }
  for (const auto& [key, pattern] : selection.parameters) {
    if (key == "type") {
      continue;
    }
    const auto found = registration.parameters.find(key);
    if (found == registration.parameters.end() ||
        !WildcardMatches(found->second, pattern)) {
      return false;
    }
  }
  return true;
}

std::string FormatExactMimetype(const Mimetype& mimetype,
                                std::string_view type_name) {
  std::string result = mimetype.media_type;
  for (const auto& [key, value] : mimetype.parameters) {
    absl::StrAppend(&result, ";", key, "=", value);
  }
  absl::StrAppend(&result, ";type=", type_name);
  return result;
}

Mimetype WithoutType(Mimetype value) {
  value.parameters.erase("type");
  return value;
}

absl::StatusOr<Chunk> SerializeJson(const nlohmann::json& value) {
  try {
    return Chunk{.data = value.dump()};
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Failed to serialize JSON: ", error.what()));
  }
}

absl::StatusOr<nlohmann::json> DeserializeJson(const Chunk& chunk) {
  try {
    return nlohmann::json::parse(chunk.data);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid JSON data: ", error.what()));
  }
}

absl::StatusOr<Chunk> SerializeJsonMsgpack(const nlohmann::json& value) {
  try {
    const std::vector<std::uint8_t> encoded = nlohmann::json::to_msgpack(value);
    return Chunk{.data =
                     std::string(reinterpret_cast<const char*>(encoded.data()),
                                 encoded.size())};
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Failed to serialize MessagePack: ", error.what()));
  }
}

absl::StatusOr<nlohmann::json> DeserializeJsonMsgpack(const Chunk& chunk) {
  try {
    const auto* first =
        reinterpret_cast<const std::uint8_t*>(chunk.data.data());
    return nlohmann::json::from_msgpack(first, first + chunk.data.size(), true,
                                        true);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid MessagePack data: ", error.what()));
  }
}

template <typename T>
absl::Status RegisterNative(SerializationRegistry* registry,
                            std::string type_name) {
  return registry->Register<T>(
      std::move(type_name), std::string(kMsgpackMimetype),
      [](const T& value) -> absl::StatusOr<Chunk> {
        ABSL_ASSIGN_OR_RETURN(Bytes encoded, value.ToMsgpack());
        return Chunk{.data = std::move(encoded)};
      },
      [](const Chunk& chunk) -> absl::StatusOr<T> {
        return T::FromMsgpack(chunk.data);
      });
}

}  // namespace

struct SerializationRegistry::SerializerRegistration {
  std::type_index type;
  std::string type_name;
  Mimetype mimetype;
  ErasedSerializer serializer;
  size_t order;
};

struct SerializationRegistry::DeserializerRegistration {
  std::type_index type;
  std::string type_name;
  Mimetype mimetype;
  ErasedDeserializer deserializer;
  size_t order;
};

struct SerializationRegistry::Impl {
  mutable thread::Mutex mu;
  std::vector<SerializerRegistration> serializers ABSL_GUARDED_BY(mu);
  std::vector<DeserializerRegistration> deserializers ABSL_GUARDED_BY(mu);
  size_t next_order ABSL_GUARDED_BY(mu) = 0;
};

SerializationRegistry::~SerializationRegistry() {
  std::destroy_at(GetImpl());
}

SerializationRegistry::SerializationRegistry(bool register_defaults) {
  static_assert(sizeof(Impl) <= kImplSize);
  static_assert(alignof(Impl) <= kImplAlignment);
  std::construct_at(reinterpret_cast<Impl*>(impl_));
  if (register_defaults) {
    const absl::Status status = RegisterDefaults();
    if (!status.ok()) {
      // Registration failure here is an invariant violation, rather than a
      // recoverable caller error: the registry was just constructed empty.
      LOG(FATAL) << "Could not register built-in A11 serializers: " << status;
    }
  }
}

SerializationRegistry::Impl* SerializationRegistry::GetImpl() {
  return std::launder(reinterpret_cast<Impl*>(impl_));
}

const SerializationRegistry::Impl* SerializationRegistry::GetImpl() const {
  return std::launder(reinterpret_cast<const Impl*>(impl_));
}

absl::Status SerializationRegistry::RegisterSerializerErased(
    std::type_index type, std::string type_name, std::string mimetype,
    ErasedSerializer serializer) {
  if (type_name.empty()) {
    return absl::InvalidArgumentError("type_name must not be empty");
  }
  ABSL_ASSIGN_OR_RETURN(Mimetype parsed, ParseMimetype(mimetype, false));
  const auto encoded_type = parsed.parameters.find("type");
  if (encoded_type != parsed.parameters.end() &&
      encoded_type->second != type_name) {
    return absl::InvalidArgumentError(
        "A registered type parameter must equal type_name");
  }
  parsed = WithoutType(std::move(parsed));
  Impl* impl = GetImpl();
  thread::MutexLock lock(&impl->mu);
  for (const SerializerRegistration& registration : impl->serializers) {
    if (registration.type == type &&
        registration.mimetype.media_type == parsed.media_type &&
        registration.mimetype.parameters == parsed.parameters) {
      return absl::AlreadyExistsError(
          "A serializer is already registered for this type and mimetype");
    }
  }
  impl->serializers.push_back(SerializerRegistration{
      .type = type,
      .type_name = std::move(type_name),
      .mimetype = std::move(parsed),
      .serializer = std::move(serializer),
      .order = impl->next_order++,
  });
  return absl::OkStatus();
}

absl::Status SerializationRegistry::RegisterDeserializerErased(
    std::type_index type, std::string type_name, std::string mimetype,
    ErasedDeserializer deserializer) {
  if (type_name.empty()) {
    return absl::InvalidArgumentError("type_name must not be empty");
  }
  ABSL_ASSIGN_OR_RETURN(Mimetype parsed, ParseMimetype(mimetype, false));
  const auto encoded_type = parsed.parameters.find("type");
  if (encoded_type != parsed.parameters.end() &&
      encoded_type->second != type_name) {
    return absl::InvalidArgumentError(
        "A registered type parameter must equal type_name");
  }
  parsed = WithoutType(std::move(parsed));
  Impl* impl = GetImpl();
  thread::MutexLock lock(&impl->mu);
  for (const DeserializerRegistration& registration : impl->deserializers) {
    if (registration.type == type &&
        registration.mimetype.media_type == parsed.media_type &&
        registration.mimetype.parameters == parsed.parameters) {
      return absl::AlreadyExistsError(
          "A deserializer is already registered for this type and mimetype");
    }
  }
  impl->deserializers.push_back(DeserializerRegistration{
      .type = type,
      .type_name = std::move(type_name),
      .mimetype = std::move(parsed),
      .deserializer = std::move(deserializer),
      .order = impl->next_order++,
  });
  return absl::OkStatus();
}

void SerializationRegistry::RemoveSerializer(std::type_index type,
                                             std::string_view type_name,
                                             std::string_view mimetype) {
  absl::StatusOr<Mimetype> parsed = ParseMimetype(mimetype, false);
  if (!parsed.ok()) {
    return;
  }
  *parsed = WithoutType(std::move(*parsed));
  Impl* impl = GetImpl();
  thread::MutexLock lock(&impl->mu);
  std::erase_if(
      impl->serializers, [&](const SerializerRegistration& registration) {
        return registration.type == type &&
               registration.type_name == type_name &&
               registration.mimetype.media_type == parsed->media_type &&
               registration.mimetype.parameters == parsed->parameters;
      });
}

absl::StatusOr<Chunk> SerializationRegistry::ToChunkErased(
    std::type_index type, const void* absl_nonnull value,
    std::string_view mimetype) const {
  std::optional<Mimetype> selection;
  if (!mimetype.empty()) {
    ABSL_ASSIGN_OR_RETURN(Mimetype parsed, ParseMimetype(mimetype, true));
    selection = std::move(parsed);
  }
  ErasedSerializer serializer;
  std::string exact_mimetype;
  {
    const Impl* impl = GetImpl();
    thread::MutexLock lock(&impl->mu);
    const SerializerRegistration* selected = nullptr;
    for (const SerializerRegistration& registration : impl->serializers) {
      if (registration.type != type) {
        continue;
      }
      if (selection.has_value() &&
          !Matches(registration.mimetype, *selection)) {
        continue;
      }
      if (selection.has_value()) {
        const auto requested_type = selection->parameters.find("type");
        if (requested_type != selection->parameters.end() &&
            !WildcardMatches(registration.type_name, requested_type->second)) {
          continue;
        }
      }
      if (selected == nullptr || registration.order < selected->order) {
        selected = &registration;
      }
    }
    if (selected == nullptr) {
      return absl::NotFoundError("No serializer matched the type and mimetype");
    }
    serializer = selected->serializer;
    exact_mimetype =
        FormatExactMimetype(selected->mimetype, selected->type_name);
  }
  absl::StatusOr<Chunk> chunk;
  try {
    chunk = serializer(value);
  } catch (const std::exception& error) {
    return absl::UnknownError(error.what());
  } catch (...) {
    return absl::UnknownError("serializer raised a non-standard exception");
  }
  if (!chunk.ok()) {
    return chunk.status();
  }
  if (!chunk->metadata.has_value()) {
    chunk->metadata.emplace();
  }
  chunk->metadata->mimetype = exact_mimetype;
  ABSL_RETURN_IF_ERROR(chunk->Validate());
  return chunk;
}

absl::StatusOr<std::any> SerializationRegistry::FromChunkErased(
    const Chunk& chunk, std::type_index requested_type,
    const std::vector<std::string>& mimetype_patterns) const {
  if (!chunk.ref.empty()) {
    return absl::InvalidArgumentError(
        "A referenced chunk must be resolved before deserialization");
  }
  std::vector<Mimetype> selectors;
  std::optional<Mimetype> actual;
  if (!chunk.GetMimetype().empty()) {
    ABSL_ASSIGN_OR_RETURN(Mimetype parsed,
                          ParseMimetype(chunk.GetMimetype(), false));
    actual = std::move(parsed);
  }
  if (mimetype_patterns.empty()) {
    if (!actual.has_value()) {
      return absl::InvalidArgumentError(
          "The chunk has no mimetype and no selector was supplied");
    }
    selectors.push_back(*actual);
  } else {
    for (const std::string& pattern_text : mimetype_patterns) {
      ABSL_ASSIGN_OR_RETURN(Mimetype pattern,
                            ParseMimetype(pattern_text, true));
      if (actual.has_value() && Matches(*actual, pattern)) {
        selectors.push_back(*actual);
      } else {
        if (actual.has_value() &&
            pattern.parameters.find("type") == pattern.parameters.end()) {
          const auto encoded = actual->parameters.find("type");
          if (encoded != actual->parameters.end()) {
            pattern.parameters["type"] = encoded->second;
          }
        }
        selectors.push_back(std::move(pattern));
      }
    }
  }

  ErasedDeserializer deserializer;
  {
    const Impl* impl = GetImpl();
    thread::MutexLock lock(&impl->mu);
    for (const Mimetype& selector : selectors) {
      const auto encoded_type = selector.parameters.find("type");
      const DeserializerRegistration* selected = nullptr;
      for (const DeserializerRegistration& registration : impl->deserializers) {
        if (registration.type != requested_type ||
            !Matches(registration.mimetype, selector)) {
          continue;
        }
        if (encoded_type != selector.parameters.end() &&
            registration.type_name != encoded_type->second) {
          continue;
        }
        if (selected == nullptr || registration.order < selected->order) {
          selected = &registration;
        }
      }
      if (selected != nullptr) {
        deserializer = selected->deserializer;
        break;
      }
    }
  }
  if (deserializer == nullptr) {
    return absl::NotFoundError("No deserializer matched the chunk");
  }
  try {
    return deserializer(chunk);
  } catch (const std::exception& error) {
    return absl::UnknownError(error.what());
  } catch (...) {
    return absl::UnknownError("deserializer raised a non-standard exception");
  }
}

absl::Status SerializationRegistry::RegisterDefaults() {
  ABSL_RETURN_IF_ERROR(Register<nlohmann::json>(
      "json", std::string(kJsonMimetype), SerializeJson, DeserializeJson));
  ABSL_RETURN_IF_ERROR(
      Register<nlohmann::json>("json", std::string(kMsgpackMimetype),
                               SerializeJsonMsgpack, DeserializeJsonMsgpack));
  ABSL_RETURN_IF_ERROR(RegisterNative<ChunkMetadata>(this, "ChunkMetadata"));
  ABSL_RETURN_IF_ERROR(RegisterNative<Chunk>(this, "Chunk"));
  ABSL_RETURN_IF_ERROR(RegisterNative<NodeRef>(this, "NodeRef"));
  ABSL_RETURN_IF_ERROR(RegisterNative<NodeFragment>(this, "NodeFragment"));
  ABSL_RETURN_IF_ERROR(RegisterNative<Port>(this, "Port"));
  ABSL_RETURN_IF_ERROR(RegisterNative<ActionMessage>(this, "ActionMessage"));
  return RegisterNative<WireMessage>(this, "WireMessage");
}

size_t SerializationRegistry::serializer_count() const {
  const Impl* impl = GetImpl();
  thread::MutexLock lock(&impl->mu);
  return impl->serializers.size();
}

size_t SerializationRegistry::deserializer_count() const {
  const Impl* impl = GetImpl();
  thread::MutexLock lock(&impl->mu);
  return impl->deserializers.size();
}

SerializationRegistry& GlobalSerializationRegistry() {
  static absl::NoDestructor<SerializationRegistry> registry(true);
  return *registry;
}

}  // namespace a11::data
