// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Type-and-mimetype indexed serialization of values to/from Chunks.
 *
 * a11::data::SerializationRegistry maps a C++ type together with a media type
 * (its representation) to a serializer/deserializer pair, so arbitrary values
 * can be converted to and from ::a11::data::Chunk. A serialized chunk records
 * both halves: its mimetype embeds a stable type tag (for example
 * @c "application/json;type=..."), letting the deserializer recover the
 * intended type. JSON and MessagePack codecs are available as defaults.
 */

#ifndef A11_DATA_SERIALIZATION_H_
#define A11_DATA_SERIALIZATION_H_

#include <any>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>

#include "a11/data/types.h"

namespace a11::data {

/** @brief Media type of the built-in JSON codec. */
inline constexpr std::string_view kJsonMimetype = "application/json";
/** @brief Media type of the built-in MessagePack codec. */
inline constexpr std::string_view kMsgpackMimetype = "application/x-msgpack";

/**
 * @brief A registry of serializers and deserializers indexed by type and MIME.
 *
 * Register a codec for a C++ type and an exact media type, then use ToChunk /
 * FromChunk to convert values to and from ::a11::data::Chunk. A new registry
 * is empty; pass @c register_defaults or call RegisterDefaults to install the
 * standard JSON and MessagePack codecs. The process-wide registry from
 * GlobalSerializationRegistry already has them. Not copyable.
 */
class SerializationRegistry {
 public:
  /**
   * @brief Constructs a registry.
   * @param register_defaults When true, install the JSON and MessagePack
   *        codecs; otherwise start empty.
   */
  explicit SerializationRegistry(bool register_defaults = false);
  ~SerializationRegistry();

  SerializationRegistry(const SerializationRegistry&) = delete;
  SerializationRegistry& operator=(const SerializationRegistry&) = delete;

  /** @brief Callable turning a @c T into a Chunk. */
  template <typename T>
  using Serializer = std::function<absl::StatusOr<Chunk>(const T&)>;

  /** @brief Callable reconstructing a @c T from a Chunk. */
  template <typename T>
  using Deserializer = std::function<absl::StatusOr<T>(const Chunk&)>;

  /**
   * @brief Registers a serializer for type @c T and an exact media type.
   * @param type_name Wire tag identifying @c T in serialized chunks.
   * @param mimetype Exact media type produced by @p serializer.
   * @param serializer Callable converting a @c T to a Chunk.
   * @return OK on success, or an error (e.g. when @p serializer is empty).
   */
  template <typename T>
  absl::Status RegisterSerializer(std::string type_name, std::string mimetype,
                                  Serializer<T> serializer) {
    if (!serializer) {
      return absl::InvalidArgumentError("serializer must be callable");
    }
    return RegisterSerializerErased(
        typeid(T), std::move(type_name), std::move(mimetype),
        [serializer = std::move(serializer)](
            const void* absl_nonnull value) -> absl::StatusOr<Chunk> {
          if (value == nullptr) {
            return absl::InvalidArgumentError("serializer value is null");
          }
          try {
            return serializer(*static_cast<const T*>(value));
          } catch (const std::exception& error) {
            return absl::UnknownError(error.what());
          } catch (...) {
            return absl::UnknownError(
                "serializer raised a non-standard exception");
          }
        });
  }

  /**
   * @brief Registers a deserializer for type @c T and an exact media type.
   * @param type_name Wire tag identifying @c T in serialized chunks.
   * @param mimetype Exact media type accepted by @p deserializer.
   * @param deserializer Callable reconstructing a @c T from a Chunk.
   * @return OK on success, or an error (e.g. when @p deserializer is empty).
   */
  template <typename T>
  absl::Status RegisterDeserializer(std::string type_name, std::string mimetype,
                                    Deserializer<T> deserializer) {
    if (!deserializer) {
      return absl::InvalidArgumentError("deserializer must be callable");
    }
    return RegisterDeserializerErased(
        typeid(T), std::move(type_name), std::move(mimetype),
        [deserializer = std::move(deserializer)](
            const Chunk& chunk) -> absl::StatusOr<std::any> {
          try {
            ABSL_ASSIGN_OR_RETURN(T result, deserializer(chunk));
            return std::any(std::move(result));
          } catch (const std::exception& error) {
            return absl::UnknownError(error.what());
          } catch (...) {
            return absl::UnknownError(
                "deserializer raised a non-standard exception");
          }
        });
  }

  /**
   * @brief Atomically registers a serializer/deserializer pair for @c T.
   *
   * On failure to register the deserializer, the serializer added by this
   * call is rolled back.
   *
   * @param type_name Wire tag identifying @c T in serialized chunks.
   * @param mimetype Exact media type for both codecs.
   * @param serializer Callable converting a @c T to a Chunk.
   * @param deserializer Callable reconstructing a @c T from a Chunk.
   * @return OK on success, or the first error encountered.
   */
  template <typename T>
  absl::Status Register(std::string type_name, std::string mimetype,
                        Serializer<T> serializer,
                        Deserializer<T> deserializer) {
    ABSL_RETURN_IF_ERROR(
        RegisterSerializer<T>(type_name, mimetype, serializer));
    absl::Status status = RegisterDeserializer<T>(
        std::move(type_name), std::move(mimetype), std::move(deserializer));
    if (!status.ok()) {
      RemoveSerializer(typeid(T), type_name, mimetype);
      return status;
    }
    return absl::OkStatus();
  }

  /**
   * @brief Serializes @p value into a Chunk.
   * @param value Value to serialize.
   * @param mimetype Optional media type to select a representation; when empty
   *        the type's registered default (by registration order) is used.
   * @return A chunk with an exact mimetype and type tag, or an error when no
   *         matching serializer is registered.
   */
  template <typename T>
  absl::StatusOr<Chunk> ToChunk(const T& value,
                                std::string_view mimetype = {}) const {
    return ToChunkErased(typeid(T), &value, mimetype);
  }

  /**
   * @brief Deserializes @p chunk into a value of type @c T.
   * @param chunk Chunk to decode.
   * @param mimetype_patterns Optional ordered media-type selectors (may use
   *        wildcards); the first that matches the chunk is used. When empty,
   *        the chunk's own mimetype selects the codec.
   * @return The decoded value, or an error when no codec matches or the
   *         result is not a @c T.
   */
  template <typename T>
  absl::StatusOr<T> FromChunk(
      const Chunk& chunk,
      const std::vector<std::string>& mimetype_patterns = {}) const {
    ABSL_ASSIGN_OR_RETURN(std::any result,
                          FromChunkErased(chunk, typeid(T), mimetype_patterns));
    try {
      return std::any_cast<T>(std::move(result));
    } catch (const std::bad_any_cast& error) {
      return absl::InternalError(error.what());
    }
  }

  /** @brief Installs the standard JSON and MessagePack codecs. */
  absl::Status RegisterDefaults();
  /** @brief Number of registered serializers. */
  [[nodiscard]] size_t serializer_count() const;
  /** @brief Number of registered deserializers. */
  [[nodiscard]] size_t deserializer_count() const;

 private:
  using ErasedSerializer =
      std::function<absl::StatusOr<Chunk>(const void* absl_nonnull value)>;
  using ErasedDeserializer =
      std::function<absl::StatusOr<std::any>(const Chunk& chunk)>;

  struct SerializerRegistration;
  struct DeserializerRegistration;
  struct Impl;

  static constexpr size_t kImplSize = 192;
  static constexpr size_t kImplAlignment = alignof(std::max_align_t);

  Impl* absl_nonnull GetImpl();
  const Impl* absl_nonnull GetImpl() const;

  absl::Status RegisterSerializerErased(std::type_index type,
                                        std::string type_name,
                                        std::string mimetype,
                                        ErasedSerializer serializer);
  absl::Status RegisterDeserializerErased(std::type_index type,
                                          std::string type_name,
                                          std::string mimetype,
                                          ErasedDeserializer deserializer);
  void RemoveSerializer(std::type_index type, std::string_view type_name,
                        std::string_view mimetype);

  absl::StatusOr<Chunk> ToChunkErased(std::type_index type,
                                      const void* absl_nonnull value,
                                      std::string_view mimetype) const;
  absl::StatusOr<std::any> FromChunkErased(
      const Chunk& chunk, std::type_index requested_type,
      const std::vector<std::string>& mimetype_patterns) const;

  alignas(kImplAlignment) std::byte impl_[kImplSize];
};

/** @brief Returns the process-wide registry (defaults pre-installed). */
SerializationRegistry& GlobalSerializationRegistry();

}  // namespace a11::data

#endif  // A11_DATA_SERIALIZATION_H_
