// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Log levels, log-chunk metadata and the process-wide action log sink.
 *
 * An a11::actions::Action narrates what it is doing through
 * a11::actions::Action::Log, which turns the object it is handed into a
 * ::a11::data::Chunk on the reserved ::a11::actions::kActionLogOutput port.
 * This header holds the vocabulary shared by everything that writes or reads a
 * chunk: the levels, the metadata keys, the a11::actions::LogOptions a caller
 * passes, and the a11::actions::ActionLogSink that decides where a log the
 * process itself consumes ends up. The default sink writes to Abseil's log,
 * which is what carries an action's logs into the host language's own logging
 * (see @c cpp/python/logging_bindings.cc).
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
 * The five levels every host language A11 binds to agrees on. Abseil has no
 * debug level and its fatal level aborts the process, so LogLevelToSeverity
 * folds ::kDebug onto @c INFO and ::kCritical onto @c ERROR; the level written
 * to the chunk stays exact either way, so a sink that can tell them apart
 * still can.
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
 * The log-chunk attributes are unprefixed, unlike ::a11::data::kCloseAttribute,
 * because they only ever appear on the reserved log port: there is no user
 * metadata on that port to collide with, and a consumer in another language
 * reads them as the plain words they are.
 */
inline constexpr std::string_view kLogLevelAttribute = "level";
/**
 * @brief Metadata attribute marking a log not meant for an end user.
 *
 * Written as @c "true" or @c "false"; absent means false. Lets a consumer show
 * a user what an action is doing without also showing it A11's own bookkeeping.
 */
inline constexpr std::string_view kLogInternalAttribute = "internal";
/**
 * @brief Metadata attribute naming the log's logical channel.
 *
 * A free-form label a consumer can filter on, so one action can narrate several
 * unrelated things without a node per thing.
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
 * Every field is optional. @c metadata is merged onto the chunk first and the
 * named fields after it, so an explicit @c level wins over a @c "level" the
 * caller also put in the map.
 *
 * The string fields are views: a LogOptions is built at the call and consumed
 * before Log returns, and copying a channel name per log line is a cost a
 * narrating action pays on every message.
 */
struct LogOptions {
  /// Level name; empty is ::kDefaultLogLevel. See ParseLogLevel.
  std::string_view level{};
  /**
   * @brief Media type hint for the serializer.
   *
   * Empty asks Action::Log for the default, which -- unlike
   * ::a11::nodes::AsyncNode::Put -- is ::a11::data::kTextMimetype for anything
   * string-like. A log is text far more often than it is bytes, and a caller
   * who wanted the bytes reading of a @c std::string says so here.
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
 * A view over a chunk that has already been written, plus the action it came
 * from. Nothing here is owned; a sink that keeps a log must copy it.
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
 * One slot per process rather than one per language: a host language replaces
 * the sink instead of adding a second consumer, because two consumers of one
 * log stream is one log line reported twice.
 *
 * A sink must not block and must not fail: it runs on whichever thread called
 * Action::Log, inside the handler's own frame.
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
 * Text and JSON do; a sink that writes a log as a line of characters can print
 * those as themselves. Anything else is bytes, and a log line is not the place
 * to render a blob -- see LogText, which describes it instead. Shared so the
 * sinks in every language make the same call.
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
 * The inverse of what Action::Log writes: the shape a consumer on the far end
 * of a wire needs, so every language reads the metadata the same way. Unknown
 * attributes and a missing level are not errors; the level falls back to
 * ::kDefaultLogLevel.
 */
[[nodiscard]] LogRecord LogRecordFromChunk(const data::Chunk& chunk,
                                           std::string_view action_name = {},
                                           std::string_view action_id = {});

}  // namespace a11::actions

#endif  // A11_ACTIONS_LOG_H_
