/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file
 * @brief Typed access to an action's optional `options` object.
 *
 * Readers apply defaults and report invalid values with the key, expected
 * type and received type. Shape checks avoid nlohmann's throwing accessors.
 */

#ifndef A11_SDK_FLOW_ACTIONS_OPTIONS_H_
#define A11_SDK_FLOW_ACTIONS_OPTIONS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

namespace a11::sdk::flow {

/**
 * @brief An action's `options` object, read key by key.
 *
 * Absent, null and `{}` are the same thing -- every setting takes its
 * fallback -- so a handler needs no test for whether options were supplied.
 */
class Options {
 public:
  Options() = default;

  /**
   * @brief Wraps a value read off an `options` port.
   * @param value The parsed port value; null or absent yields empty options.
   * @return InvalidArgument when it is neither null nor an object, because a
   *         list or a number there means the caller meant something else.
   */
  static absl::StatusOr<Options> Parse(const nlohmann::json* value);

  /** @brief Whether key @p key was supplied at all. */
  [[nodiscard]] bool Has(std::string_view key) const;

  /** @brief Reads a boolean, or @p fallback. */
  absl::StatusOr<bool> Bool(std::string_view key, bool fallback) const;
  /** @brief Reads an integer, or @p fallback. */
  absl::StatusOr<std::int64_t> Int(std::string_view key,
                                   std::int64_t fallback) const;
  /**
   * @brief Reads an integer within @p minimum and @p maximum, or @p fallback.
   *
   * Values outside the bounds return an error naming @p key. For example,
   * `chunk_bytes` must be positive and small enough to allocate.
   */
  absl::StatusOr<std::int64_t> IntInRange(std::string_view key,
                                          std::int64_t fallback,
                                          std::int64_t minimum,
                                          std::int64_t maximum) const;
  /** @brief Reads a byte count, accepting a plain number. */
  absl::StatusOr<std::uint64_t> Bytes(std::string_view key,
                                      std::uint64_t fallback) const;
  /** @brief Reads a string, or @p fallback. */
  absl::StatusOr<std::string> String(std::string_view key,
                                     std::string_view fallback) const;
  /**
   * @brief Reads a string that has to be one of @p allowed.
   *
   * Errors list the allowed values.
   */
  absl::StatusOr<std::string> Enum(
      std::string_view key, std::string_view fallback,
      const std::vector<std::string_view>& allowed) const;
  /** @brief Reads a list of strings; a bare string counts as a list of one. */
  absl::StatusOr<std::vector<std::string>> StringList(
      std::string_view key) const;
  /**
   * @brief Reads a duration, as Flow writes one.
   *
   * Accepts `"30s"`, `"1m30s"`, `"250ms"` and numeric seconds. Rejects
   * negative durations; omit the setting for an unbounded duration.
   */
  absl::StatusOr<absl::Duration> Duration(std::string_view key,
                                          absl::Duration fallback) const;
  /** @brief Reads a nested object, or an empty one. */
  absl::StatusOr<Options> Object(std::string_view key) const;

  /** @brief The `omit` list every multi-port action in this library accepts. */
  absl::StatusOr<std::vector<std::string>> Omit() const;

  /** @brief The underlying object, for an action with a bespoke setting. */
  [[nodiscard]] const nlohmann::json& json() const { return value_; }

 private:
  explicit Options(nlohmann::json value) : value_(std::move(value)) {}

  /// The message every accessor here produces. `options.chunk_bytes must be an
  /// integer, got a string` -- key, expectation, and what arrived.
  absl::Status Wrong(std::string_view key, std::string_view expected) const;

  nlohmann::json value_ = nlohmann::json::object();
  /// Prefix for messages: `options` at the top level, `options.tls` nested.
  std::string path_ = "options";
};

/**
 * @brief Parses a duration written the way Flow writes one.
 *
 * Exposed because a duration also arrives on ports and in headers, not only in
 * `options`. Accepts `500ns`, `250ms`, `30s`, `2m`, `1h`, compounds like
 * `1m30s500ms`, and a bare number of seconds.
 */
absl::StatusOr<absl::Duration> ParseDuration(std::string_view text);

/**
 * @brief Parses the `x-a11-deadline` header: bare is milliseconds, `ns` is
 *        nanoseconds, both since the Unix epoch.
 */
absl::StatusOr<absl::Time> ParseDeadlineHeader(std::string_view value);

}  // namespace a11::sdk::flow

#endif  // A11_SDK_FLOW_ACTIONS_OPTIONS_H_
