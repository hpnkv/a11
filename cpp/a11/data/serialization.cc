// Copyright 2026 The A11 Authors.

#include "a11/data/serialization.h"

#include <algorithm>
#include <any>
#include <cctype>
#include <cstddef>
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
#include "a11/data/serial_tags.h"
#include "a11/data/serializable.h"
#include "a11/data/types.h"
#include "a11/json_codec.h"
#include "absl/strings/match.h"
#include "thread/boost_primitives.h"

namespace a11::data {
namespace {

struct Mimetype {
  std::string media_type;
  absl::flat_hash_map<std::string, std::string> parameters = {};
};

std::string Trim(std::string_view input) {
  return std::string(absl::StripAsciiWhitespace(input));
}

bool IsTokenChar(char value) {
  if (absl::ascii_isalnum(static_cast<unsigned char>(value))) {
    return true;
  }
  constexpr std::string_view extra = "!#$%&'*+-.^_`|~";
  return absl::StrContains(extra, value);
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
  if (!allow_patterns && absl::StrContains(media_type, '*')) {
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
    if (!allow_patterns && absl::StrContains(value, '*')) {
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

// Tags a JSON or MessagePack payload already spells out for itself. A chunk
// holding one of these carries no type parameter: writing ";type=object" on an
// object says nothing a parser did not already know, and it stops a peer that
// only has "application/json" from being understood.
bool IsGenericTag(std::string_view tag) {
  static constexpr std::string_view kGeneric[] = {
      "json",   "object",  "array",   "string",
      "number", "integer", "boolean", "null"};
  return std::find(std::begin(kGeneric), std::end(kGeneric), tag) !=
         std::end(kGeneric);
}

// Media types that describe their content completely on their own, so a chunk
// using one carries no type parameter and no framing inside the payload. See
// kTextMimetype and kBytesMimetype.
bool IsSelfDescribingMediaType(std::string_view media_type) {
  return media_type == kTextMimetype || media_type == kBytesMimetype;
}

std::string FormatExactMimetype(const Mimetype& mimetype,
                                std::string_view type_name) {
  std::string result = mimetype.media_type;
  for (const auto& [key, value] : mimetype.parameters) {
    absl::StrAppend(&result, ";", key, "=", value);
  }
  if (!IsGenericTag(type_name) &&
      !IsSelfDescribingMediaType(mimetype.media_type)) {
    absl::StrAppend(&result, ";type=", type_name);
  }
  return result;
}

Mimetype WithoutType(Mimetype value) {
  value.parameters.erase("type");
  return value;
}

absl::StatusOr<Chunk> SerializeJson(const nlohmann::json& value) {
  ABSL_ASSIGN_OR_RETURN(std::string encoded, DumpJson(value, "JSON"));
  return Chunk{.data = std::move(encoded)};
}

absl::StatusOr<nlohmann::json> DeserializeJson(const Chunk& chunk) {
  return ParseJson(chunk.data, "JSON data");
}

absl::StatusOr<Chunk> SerializeJsonMsgpack(const nlohmann::json& value) {
  ABSL_ASSIGN_OR_RETURN(std::string encoded, PackMsgpack(value, "JSON"));
  return Chunk{.data = std::move(encoded)};
}

absl::StatusOr<nlohmann::json> DeserializeJsonMsgpack(const Chunk& chunk) {
  return UnpackMsgpack(chunk.data, "JSON");
}

// Registers a runtime type's MessagePack representation under the canonical
// cross-language tag it publishes through A11SerialTag, so the chunk a C++ peer
// writes names the same type Python, TypeScript and Kotlin look for.
template <typename T>
absl::Status RegisterNative(SerializationRegistry* registry) {
  return registry->Register<T>(
      SerialTypeTag<T>(), std::string(kMsgpackMimetype),
      [](const T& value) -> absl::StatusOr<Chunk> {
        ABSL_ASSIGN_OR_RETURN(Bytes encoded, value.ToMsgpack());
        return Chunk{.data = std::move(encoded)};
      },
      [](const Chunk& chunk) -> absl::StatusOr<T> {
        return T::FromMsgpack(chunk.data);
      });
}

using ::a11::IsValidUtf8;

// Registers std::string under both self-describing media types.
//
// C++ has no type that means "text" as opposed to "bytes" -- a std::string is a
// sequence of bytes, and whether those bytes are UTF-8 is a fact about the
// value, not about its type. So the default is application/octet-stream, and
// text/plain is available by asking for it, which is the only way the
// distinction can be expressed here. Registration order decides the default:
// bytes is registered first.
//
// Neither codec transforms anything. That is the entire point: the JSON
// representation of a std::string is a quoted, escaped copy, and of bytes a
// base64 copy a third larger again.
absl::Status RegisterStringCodecs(
    SerializationRegistry* absl_nonnull registry) {
  ABSL_RETURN_IF_ERROR(registry->Register<std::string>(
      "bytes", std::string(kBytesMimetype),
      [](const std::string& value) -> absl::StatusOr<Chunk> {
        return Chunk{.data = value};
      },
      [](const Chunk& chunk) -> absl::StatusOr<std::string> {
        return chunk.data;
      }));
  return registry->Register<std::string>(
      "string", std::string(kTextMimetype),
      [](const std::string& value) -> absl::StatusOr<Chunk> {
        // Checked on the way out, not on the way in. A peer in a language whose
        // string type *is* text -- which is all three of the others -- will
        // reject this chunk on arrival, and the useful place to report that is
        // here, where the offending value came from, rather than in a session
        // teardown one hop away.
        if (!IsValidUtf8(value)) {
          return absl::InvalidArgumentError(absl::StrCat(
              "A ", kTextMimetype, " chunk must be valid UTF-8; use ",
              kBytesMimetype, " for bytes that are not text"));
        }
        return Chunk{.data = value};
      },
      [](const Chunk& chunk) -> absl::StatusOr<std::string> {
        if (!IsValidUtf8(chunk.data)) {
          return absl::InvalidArgumentError(
              absl::StrCat("A ", kTextMimetype, " chunk is not valid UTF-8"));
        }
        return chunk.data;
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
    std::type_index type, std::string type_name, const std::string& mimetype,
    ErasedSerializer serializer) {
  if (type_name.empty()) {
    return absl::InvalidArgumentError("type_name must not be empty");
  }
  // A registration names a representation. Which type it produces is the
  // template argument, so any type parameter here is redundant.
  ABSL_ASSIGN_OR_RETURN(Mimetype parsed, ParseMimetype(mimetype, false));
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
    std::type_index type, std::string type_name, const std::string& mimetype,
    ErasedDeserializer deserializer) {
  if (type_name.empty()) {
    return absl::InvalidArgumentError("type_name must not be empty");
  }
  // A registration names a representation. Which type it produces is the
  // template argument, so any type parameter here is redundant.
  ABSL_ASSIGN_OR_RETURN(Mimetype parsed, ParseMimetype(mimetype, false));
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
      // A selector picks a representation; the value's own type decides the
      // tag, so a type parameter in it is ignored.
      if (selection.has_value() &&
          !Matches(registration.mimetype, *selection)) {
        continue;
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
  // No guard here: a codec is wrapped where it is registered, in the
  // translation unit that owns it -- see RegisterSerializer in serialization.h.
  // The erased signatures are private, so registration through those templates
  // is the only way one can arrive.
  absl::StatusOr<Chunk> chunk = serializer(value);
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
      // An explicit selector is authoritative, so it stands in for the chunk's
      // own media type when the two disagree -- which is how stale metadata
      // gets repaired.
      selectors.push_back(actual.has_value() && Matches(*actual, pattern)
                              ? *actual
                              : std::move(pattern));
    }
  }

  // The caller named the type it wants through the template argument, so the
  // chunk's own tag has nothing left to decide: a payload written as
  // "application/json;type=a11.sdk.Interaction" is still valid JSON, and
  // FromChunk<nlohmann::json> is entitled to read it as such.
  ErasedDeserializer deserializer;
  {
    const Impl* impl = GetImpl();
    thread::MutexLock lock(&impl->mu);
    for (const Mimetype& selector : selectors) {
      const DeserializerRegistration* selected = nullptr;
      for (const DeserializerRegistration& registration : impl->deserializers) {
        if (registration.type != requested_type ||
            !Matches(registration.mimetype, selector)) {
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
  // Guarded at registration; see ToChunkErased above.
  return deserializer(chunk);
}

absl::Status SerializationRegistry::RegisterDefaults() {
  // Before the JSON codecs, so a std::string with no mimetype asked for lands on
  // application/octet-stream rather than a quoted JSON copy of itself.
  ABSL_RETURN_IF_ERROR(RegisterStringCodecs(this));
  ABSL_RETURN_IF_ERROR(Register<nlohmann::json>(
      "json", std::string(kJsonMimetype), SerializeJson, DeserializeJson));
  ABSL_RETURN_IF_ERROR(
      Register<nlohmann::json>("json", std::string(kMsgpackMimetype),
                               SerializeJsonMsgpack, DeserializeJsonMsgpack));
  ABSL_RETURN_IF_ERROR(RegisterNative<ChunkMetadata>(this));
  ABSL_RETURN_IF_ERROR(RegisterNative<Chunk>(this));
  ABSL_RETURN_IF_ERROR(RegisterNative<NodeRef>(this));
  ABSL_RETURN_IF_ERROR(RegisterNative<NodeFragment>(this));
  ABSL_RETURN_IF_ERROR(RegisterNative<Port>(this));
  ABSL_RETURN_IF_ERROR(RegisterNative<ActionMessage>(this));
  return RegisterNative<WireMessage>(this);
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
