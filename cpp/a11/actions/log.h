// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Log levels, log-chunk metadata and the process-wide action log sink.
 *
 * Action::Log writes a ::a11::data::Chunk to the reserved
 * ::a11::actions::kActionLogOutput port. This header defines its levels,
 * metadata, options, and process-wide sink. The default sink forwards records
 * to Abseil logging.
 */

#ifndef A11_ACTIONS_LOG_H_
#define A11_ACTIONS_LOG_H_

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <absl/base/log_severity.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/data/types.h"

namespace a11::actions {

/**
 * @brief Severity of a log chunk.
 *
 * Log chunks preserve all five levels. LogLevelToSeverity maps ::kDebug to
 * @c INFO and ::kCritical to @c ERROR when forwarding them to Abseil.
 */
enum class LogLevel {
  kDebug,
  kInfo,
  kWarning,
  kError,
  kCritical,
};

/** @brief The default level of a log written without one. */
inline constexpr LogLevel kDefaultLogLevel = LogLevel::kInfo;

/** @brief Canonical lowercase name of @p level, as written to a chunk. */
[[nodiscard]] std::string_view LogLevelName(LogLevel level);
/**
 * @brief Parses a level name.
 *
 * Case-insensitive, and accepts @c warn for ::kWarning and @c fatal for
 * ::kCritical -- the two spellings host languages differ on. An empty name is
 * ::kDefaultLogLevel.
 */
absl::StatusOr<LogLevel> ParseLogLevel(std::string_view name);
/** @brief The Abseil severity @p level is reported at. Never @c FATAL. */
[[nodiscard]] absl::LogSeverity LogLevelToSeverity(LogLevel level);

/**
 * @brief Metadata attribute naming the log's level.
 *
 * Log attributes use unprefixed names on the reserved log port.
 */
inline constexpr std::string_view kLogLevelAttribute = "level";
/**
 * @brief Metadata attribute marking a log not meant for an end user.
 *
 * Written as @c "true" or @c "false"; absent means false. Consumers can use
 * it to filter framework bookkeeping.
 */
inline constexpr std::string_view kLogInternalAttribute = "internal";
/**
 * @brief Metadata attribute naming the log's logical channel.
 *
 * A free-form label consumers can use to filter related messages.
 */
inline constexpr std::string_view kLogChannelAttribute = "channel";
/** @brief Metadata attribute naming the source file the log came from. */
inline constexpr std::string_view kLogFileAttribute = "file";
/** @brief Metadata attribute naming the source line the log came from. */
inline constexpr std::string_view kLogLinenoAttribute = "lineno";

/** @brief The value ::kLogInternalAttribute takes when the log is internal. */
inline constexpr std::string_view kLogInternalTrue = "true";
/** @brief The value ::kLogInternalAttribute takes when it is not. */
inline constexpr std::string_view kLogInternalFalse = "false";

/**
 * @brief Everything about a log other than the object being logged.
 *
 * Every field is optional. Named fields override matching entries in
 * @c metadata. String fields are borrowed for the duration of Action::Log.
 */
struct LogOptions {
  /// Level name; empty is ::kDefaultLogLevel. See ParseLogLevel.
  std::string_view level{};
  /**
   * @brief Media type hint for the serializer.
   *
   * Empty uses ::a11::data::kTextMimetype for string-like values. Set an
   * explicit media type to serialize a @c std::string as bytes.
   */
  std::string_view mimetype{};
  /// Logical channel; see ::kLogChannelAttribute.
  std::string_view channel{};
  /// Source file the log came from; see ::kLogFileAttribute.
  std::string_view file{};
  /// Source line the log came from; see ::kLogLinenoAttribute.
  std::optional<int> lineno{};
  /// Whether this log is A11's own bookkeeping; see ::kLogInternalAttribute.
  bool internal = false;
  /// Extra attributes, merged before the fields above. Not owned.
  const data::ByteMap* absl_nullable metadata = nullptr;
};

/**
 * @brief One log as a sink sees it.
 *
 * A borrowed view of a written chunk and its source action. A sink must copy
 * fields it retains beyond the call.
 */
struct LogRecord {
  std::string_view action_name{};
  std::string_view action_id{};
  LogLevel level = kDefaultLogLevel;
  std::string_view channel{};
  std::string_view file{};
  std::optional<int> lineno{};
  bool internal = false;
  absl::Time timestamp = absl::InfinitePast();
  std::string_view mimetype{};
  /// The chunk's payload as written. Text for a string-like log; see
  /// LogOptions::mimetype.
  std::string_view data{};
};

/**
 * @brief What the process does with a log it consumes itself.
 *
 * One sink is installed per process. It runs on the thread that called
 * Action::Log and must not block or fail.
 */
using ActionLogSink = std::function<void(const LogRecord&)>;

/**
 * @brief Installs @p sink as the process's action log sink.
 *
 * A null @p sink restores the default, which reports the log through Abseil at
 * the record's severity and source location.
 */
void SetActionLogSink(ActionLogSink sink);
/** @brief Returns the installed sink; never null. */
[[nodiscard]] ActionLogSink GetActionLogSink();
/** @brief Reports @p record to the installed sink. Never fails. */
void ReportLog(const LogRecord& record);

/**
 * @brief Whether a log payload of @p mimetype reads as text.
 *
 * Text and JSON payloads are textual. LogText describes other payloads instead
 * of rendering their bytes.
 */
[[nodiscard]] bool IsTextualLogMimetype(std::string_view mimetype);

/**
 * @brief @p record as one line: its payload where that is text, a description
 *        of it where it is not.
 */
[[nodiscard]] std::string LogText(const LogRecord& record);

/**
 * @brief Reads a LogRecord back out of a log chunk.
 *
 * Unknown attributes are ignored. A missing level uses ::kDefaultLogLevel.
 */
[[nodiscard]] LogRecord LogRecordFromChunk(const data::Chunk& chunk,
                                           std::string_view action_name = {},
                                           std::string_view action_id = {});

}  // namespace a11::actions

#endif  // A11_ACTIONS_LOG_H_
