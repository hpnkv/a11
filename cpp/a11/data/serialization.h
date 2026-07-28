// Copyright 2026 The A11 Authors.

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
#include <absl/status/statusor.h>

#include "a11/data/types.h"

namespace a11::data {

inline constexpr std::string_view kJsonMimetype = "application/json";
inline constexpr std::string_view kMsgpackMimetype = "application/x-msgpack";

class SerializationRegistry {
 public:
  explicit SerializationRegistry(bool register_defaults = false);
  ~SerializationRegistry();

  SerializationRegistry(const SerializationRegistry&) = delete;
  SerializationRegistry& operator=(const SerializationRegistry&) = delete;

  template <typename T>
  using Serializer = std::function<absl::StatusOr<Chunk>(const T&)>;

  template <typename T>
  using Deserializer = std::function<absl::StatusOr<T>(const Chunk&)>;

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
            absl::StatusOr<T> result = deserializer(chunk);
            if (!result.ok())
              return result.status();
            return std::any(std::move(*result));
          } catch (const std::exception& error) {
            return absl::UnknownError(error.what());
          } catch (...) {
            return absl::UnknownError(
                "deserializer raised a non-standard exception");
          }
        });
  }

  template <typename T>
  absl::Status Register(std::string type_name, std::string mimetype,
                        Serializer<T> serializer,
                        Deserializer<T> deserializer) {
    absl::Status status =
        RegisterSerializer<T>(type_name, mimetype, serializer);
    if (!status.ok())
      return status;
    status = RegisterDeserializer<T>(std::move(type_name), std::move(mimetype),
                                     std::move(deserializer));
    if (!status.ok()) {
      RemoveSerializer(typeid(T), type_name, mimetype);
      return status;
    }
    return absl::OkStatus();
  }

  template <typename T>
  absl::StatusOr<Chunk> ToChunk(const T& value,
                                std::string_view mimetype = {}) const {
    return ToChunkErased(typeid(T), &value, mimetype);
  }

  template <typename T>
  absl::StatusOr<T> FromChunk(
      const Chunk& chunk,
      const std::vector<std::string>& mimetype_patterns = {}) const {
    absl::StatusOr<std::any> result =
        FromChunkErased(chunk, typeid(T), mimetype_patterns);
    if (!result.ok())
      return result.status();
    try {
      return std::any_cast<T>(std::move(*result));
    } catch (const std::bad_any_cast& error) {
      return absl::InternalError(error.what());
    }
  }

  absl::Status RegisterDefaults();
  [[nodiscard]] size_t serializer_count() const;
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

SerializationRegistry& GlobalSerializationRegistry();

}  // namespace a11::data

#endif  // A11_DATA_SERIALIZATION_H_
