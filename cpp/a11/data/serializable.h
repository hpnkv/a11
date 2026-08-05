// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief ADL-based customization points that make a custom C++ type
 *   serializable through a11::data::SerializationRegistry.
 *
 * A type opts in by defining a handful of free functions in its own namespace,
 * found by argument-dependent lookup via the a11::data::TypeTag<T> tag:
 *
 * @code
 * namespace my::ns {
 * struct Widget { ... };
 *
 * // Required: the language-agnostic type tag written into the chunk mimetype
 * // (e.g. "application/json;type=my.ns.Widget").
 * std::string_view A11SerialTag(a11::data::TypeTag<Widget>);
 *
 * // JSON representation (optional; enables the application/json codec).
 * absl::StatusOr<nlohmann::json> A11ToJson(const Widget&);
 * absl::StatusOr<Widget> A11FromJson(a11::data::TypeTag<Widget>,
 *                                    const nlohmann::json&);
 *
 * // MessagePack representation (optional). When omitted but JSON is present,
 * // MessagePack is derived from the JSON form automatically.
 * absl::StatusOr<std::string> A11ToMsgpackBytes(const Widget&);
 * absl::StatusOr<Widget> A11FromMsgpackBytes(a11::data::TypeTag<Widget>,
 *                                            std::string_view);
 * }  // namespace my::ns
 * @endcode
 *
 * Then a single call registers every representation the type supports:
 *
 * @code
 * RegisterSerializable<my::ns::Widget>(registry);
 * @endcode
 *
 * The SerializationRegistry itself is untouched: RegisterSerializable simply
 * wraps the ADL functions into the registry's Serializer/Deserializer pairs,
 * using the tag returned by A11SerialTag as the wire type name. The registry
 * appends the @c ;type=<tag> parameter and matches it on decode.
 */

#ifndef A11_DATA_SERIALIZABLE_H_
#define A11_DATA_SERIALIZABLE_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <nlohmann/json.hpp>

#include "a11/data/serialization.h"
#include "a11/data/types.h"

namespace a11::data {

/**
 * @brief Empty tag carrying a type @c T so customization points are found by
 *   argument-dependent lookup even when @c T does not appear elsewhere in the
 *   call (e.g. deserialization, whose only runtime argument is a Chunk).
 */
template <typename T>
struct TypeTag {};

namespace serializable_internal {

// Unqualified calls so ADL locates the customization points in T's namespace.
// No same-named functions are declared in a11::data, so ordinary lookup finds
// nothing here and ADL takes over.

template <typename T>
concept HasSerialTag = requires {
  { A11SerialTag(TypeTag<T>{}) } -> std::convertible_to<std::string_view>;
};

template <typename T>
concept HasJson = requires(const T& value, const nlohmann::json& json) {
  { A11ToJson(value) } -> std::same_as<absl::StatusOr<nlohmann::json>>;
  { A11FromJson(TypeTag<T>{}, json) } -> std::same_as<absl::StatusOr<T>>;
};

template <typename T>
concept HasMsgpackBytes =
    requires(const T& value, std::string_view bytes) {
      { A11ToMsgpackBytes(value) } -> std::same_as<absl::StatusOr<std::string>>;
      {
        A11FromMsgpackBytes(TypeTag<T>{}, bytes)
      } -> std::same_as<absl::StatusOr<T>>;
    };

template <typename T>
std::string SerialTag() {
  return std::string(A11SerialTag(TypeTag<T>{}));
}

}  // namespace serializable_internal

/** @brief Whether @c T provides an ADL JSON representation. */
template <typename T>
concept JsonSerializable = serializable_internal::HasSerialTag<T> &&
                           serializable_internal::HasJson<T>;

/**
 * @brief Whether @c T can be serialized to MessagePack, either directly via
 *   A11ToMsgpackBytes / A11FromMsgpackBytes or derived from its JSON form.
 */
template <typename T>
concept MsgpackSerializable =
    serializable_internal::HasSerialTag<T> &&
    (serializable_internal::HasMsgpackBytes<T> ||
     serializable_internal::HasJson<T>);

/** @brief Whether @c T supports at least one representation. */
template <typename T>
concept Serializable = JsonSerializable<T> || MsgpackSerializable<T>;

/** @brief Returns the language-agnostic type tag registered for @c T. */
template <typename T>
  requires serializable_internal::HasSerialTag<T>
std::string SerialTypeTag() {
  return serializable_internal::SerialTag<T>();
}

/**
 * @brief Registers a JSON codec (media type @c application/json) for @c T.
 * @return OK, or AlreadyExists when a JSON codec is already registered.
 */
template <typename T>
  requires JsonSerializable<T>
absl::Status RegisterJsonSerializable(SerializationRegistry& registry) {
  return registry.Register<T>(
      serializable_internal::SerialTag<T>(), std::string(kJsonMimetype),
      [](const T& value) -> absl::StatusOr<Chunk> {
        absl::StatusOr<nlohmann::json> json = A11ToJson(value);
        if (!json.ok())
          return json.status();
        try {
          return Chunk{.data = json->dump()};
        } catch (const std::exception& error) {
          return absl::InvalidArgumentError(
              absl::StrCat("Failed to serialize JSON: ", error.what()));
        }
      },
      [](const Chunk& chunk) -> absl::StatusOr<T> {
        nlohmann::json json;
        try {
          json = nlohmann::json::parse(chunk.data);
        } catch (const std::exception& error) {
          return absl::InvalidArgumentError(
              absl::StrCat("Invalid JSON data: ", error.what()));
        }
        return A11FromJson(TypeTag<T>{}, json);
      });
}

/**
 * @brief Registers a MessagePack codec (media type @c application/x-msgpack)
 *   for @c T, using direct bytes when available or deriving from JSON.
 * @return OK, or AlreadyExists when a MessagePack codec is already registered.
 */
template <typename T>
  requires MsgpackSerializable<T>
absl::Status RegisterMsgpackSerializable(SerializationRegistry& registry) {
  if constexpr (serializable_internal::HasMsgpackBytes<T>) {
    return registry.Register<T>(
        serializable_internal::SerialTag<T>(), std::string(kMsgpackMimetype),
        [](const T& value) -> absl::StatusOr<Chunk> {
          absl::StatusOr<std::string> bytes = A11ToMsgpackBytes(value);
          if (!bytes.ok())
            return bytes.status();
          return Chunk{.data = std::move(*bytes)};
        },
        [](const Chunk& chunk) -> absl::StatusOr<T> {
          return A11FromMsgpackBytes(TypeTag<T>{}, chunk.data);
        });
  } else {
    // Derive MessagePack from the JSON representation.
    return registry.Register<T>(
        serializable_internal::SerialTag<T>(), std::string(kMsgpackMimetype),
        [](const T& value) -> absl::StatusOr<Chunk> {
          absl::StatusOr<nlohmann::json> json = A11ToJson(value);
          if (!json.ok())
            return json.status();
          try {
            const std::vector<std::uint8_t> encoded =
                nlohmann::json::to_msgpack(*json);
            return Chunk{.data = std::string(
                             reinterpret_cast<const char*>(encoded.data()),
                             encoded.size())};
          } catch (const std::exception& error) {
            return absl::InvalidArgumentError(
                absl::StrCat("Failed to serialize MessagePack: ", error.what()));
          }
        },
        [](const Chunk& chunk) -> absl::StatusOr<T> {
          nlohmann::json json;
          try {
            const auto* first =
                reinterpret_cast<const std::uint8_t*>(chunk.data.data());
            json = nlohmann::json::from_msgpack(first, first + chunk.data.size(),
                                                true, true);
          } catch (const std::exception& error) {
            return absl::InvalidArgumentError(
                absl::StrCat("Invalid MessagePack data: ", error.what()));
          }
          return A11FromJson(TypeTag<T>{}, json);
        });
  }
}

/**
 * @brief Registers every representation @c T supports into @p registry.
 *
 * JSON is registered first (so it is the default when no mimetype is given)
 * when available, followed by MessagePack. Passing @p json / @p msgpack as
 * false skips that representation even when the type supports it.
 *
 * @return OK on success, or the first registration error.
 */
template <typename T>
  requires Serializable<T>
absl::Status RegisterSerializable(SerializationRegistry& registry,
                                  bool json = true, bool msgpack = true) {
  if (json && JsonSerializable<T>) {
    if constexpr (JsonSerializable<T>) {
      absl::Status status = RegisterJsonSerializable<T>(registry);
      if (!status.ok())
        return status;
    }
  }
  if (msgpack && MsgpackSerializable<T>) {
    if constexpr (MsgpackSerializable<T>) {
      absl::Status status = RegisterMsgpackSerializable<T>(registry);
      if (!status.ok())
        return status;
    }
  }
  return absl::OkStatus();
}

}  // namespace a11::data

#endif  // A11_DATA_SERIALIZABLE_H_
