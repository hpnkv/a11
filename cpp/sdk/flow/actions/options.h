// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Reading an `options` port, and saying what was wrong with it.
 *
 * Every action here takes its settings as one optional JSON object, the way
 * @c make_http_request does, because a port per setting would be forty ports
 * and because a flow writes `options: {"chunk_bytes": 4096}` more readably than
 * it writes forty assignments.
 *
 * What this file is really for is the error message. A caller who wrote
 * `{"timeout": "quickly"}` gets told which key, what was expected, and what
 * arrived -- from one place, so no action has to remember to say it. That
 * matters more than usual here: a flow may have been written by a model, and
 * "options.timeout must be a duration, got a string" is repairable where
 * "invalid argument" is not.
 *
 * Everything reads with a fallback and nothing throws, which is the same
 * statement twice: nlohmann's `at()` and `get<T>()` raise on a missing key or a
 * wrong type, so this library checks the shape and never relies on being told.
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
   * The bounds are the point rather than a safety net: a `chunk_bytes` of zero
   * is an infinite loop and one of a gigabyte is an allocation nobody meant, so
   * both are reported where they were written.
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
   * The message lists what was allowed, which is the difference between a
   * caller fixing a typo and a caller guessing.
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
   * `"30s"`, `"1m30s"`, `"250ms"` and a bare number of seconds all read, so a
   * timeout that arrived from a flow literal, a header or a model's JSON is the
   * same value. A negative one is refused rather than taken as infinite: this
   * is a bound on work, and "forever" is spelled by leaving it out.
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
