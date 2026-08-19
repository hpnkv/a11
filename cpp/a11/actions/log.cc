// Copyright 2026 The A11 Authors.

#include "a11/actions/log.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/base/log_severity.h>
#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/ascii.h>
#include <absl/strings/match.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>

#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "thread/boost_primitives.h"

namespace a11::actions {
namespace {

thread::Mutex& SinkMutex() {
  static auto* mutex = new thread::Mutex();
  return *mutex;
}

ActionLogSink& SinkSlot() {
  static auto* sink = new ActionLogSink();
  return *sink;
}

// The default sink. Abseil's log is where A11's C++ diagnostics already go, and
// a host language picks them up from there without a second consumer of the log
// port: cpp/python/logging_bindings.cc turns every entry into a LogRecord.
//
// AtLocation is what carries the log's own file and line through; without it a
// Python consumer would be told every action log came from this function.
void ReportThroughAbseil(const LogRecord& record) {
  const absl::LogSeverity severity = LogLevelToSeverity(record.level);
  const std::string prefix =
      record.channel.empty()
          ? absl::StrCat("[", record.action_name, "] ")
          : absl::StrCat("[", record.action_name, "/", record.channel, "] ");
  const std::string text = LogText(record);
  if (record.file.empty()) {
    LOG(LEVEL(severity)) << prefix << text;
    return;
  }
  LOG(LEVEL(severity)).AtLocation(record.file, record.lineno.value_or(0))
      << prefix << text;
}

std::string_view Attribute(const data::Chunk& chunk, std::string_view key) {
  if (!chunk.metadata.has_value()) {
    return {};
  }
  const auto found = chunk.metadata->attributes.find(key);
  if (found == chunk.metadata->attributes.end()) {
    return {};
  }
  return found->second;
}

}  // namespace

std::string_view LogLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return "debug";
    case LogLevel::kInfo:
      return "info";
    case LogLevel::kWarning:
      return "warning";
    case LogLevel::kError:
      return "error";
    case LogLevel::kCritical:
      return "critical";
  }
  return "info";
}

absl::StatusOr<LogLevel> ParseLogLevel(std::string_view name) {
  if (name.empty()) {
    return kDefaultLogLevel;
  }
  const std::string lowered = absl::AsciiStrToLower(name);
  if (lowered == "debug") return LogLevel::kDebug;
  if (lowered == "info") return LogLevel::kInfo;
  if (lowered == "warning" || lowered == "warn") return LogLevel::kWarning;
  if (lowered == "error") return LogLevel::kError;
  if (lowered == "critical" || lowered == "fatal") return LogLevel::kCritical;
  return absl::InvalidArgumentError(absl::StrCat(
      "Unknown log level '", name,
      "'; expected debug, info, warning, error or critical"));
}

absl::LogSeverity LogLevelToSeverity(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
    case LogLevel::kInfo:
      return absl::LogSeverity::kInfo;
    case LogLevel::kWarning:
      return absl::LogSeverity::kWarning;
    case LogLevel::kError:
    case LogLevel::kCritical:
      // Never kFatal: reporting a log must not end the process.
      return absl::LogSeverity::kError;
  }
  return absl::LogSeverity::kInfo;
}

void SetActionLogSink(ActionLogSink sink) {
  thread::MutexLock lock(&SinkMutex());
  SinkSlot() = std::move(sink);
}

ActionLogSink GetActionLogSink() {
  {
    thread::MutexLock lock(&SinkMutex());
    if (SinkSlot() != nullptr) {
      return SinkSlot();
    }
  }
  return ReportThroughAbseil;
}

void ReportLog(const LogRecord& record) {
  ActionLogSink sink;
  {
    thread::MutexLock lock(&SinkMutex());
    sink = SinkSlot();
  }
  if (sink == nullptr) {
    ReportThroughAbseil(record);
    return;
  }
  sink(record);
}

bool IsTextualLogMimetype(std::string_view mimetype) {
  if (absl::StartsWith(mimetype, "text/")) return true;
  // `application/json` and anything ending `+json`, with or without the
  // `;type=` parameter A11's JSON codec adds.
  const std::string_view media = mimetype.substr(0, mimetype.find(';'));
  return media == data::kJsonMimetype || absl::EndsWith(media, "+json");
}

std::string LogText(const LogRecord& record) {
  if (IsTextualLogMimetype(record.mimetype)) {
    return std::string(record.data);
  }
  return absl::StrCat("<", record.data.size(), " bytes of ", record.mimetype,
                      ">");
}

LogRecord LogRecordFromChunk(const data::Chunk& chunk,
                            std::string_view action_name,
                            std::string_view action_id) {
  LogRecord record;
  record.action_name = action_name;
  record.action_id = action_id;
  record.data = chunk.data;
  if (chunk.metadata.has_value()) {
    record.mimetype = chunk.metadata->mimetype;
    record.timestamp =
        chunk.metadata->timestamp.value_or(absl::InfinitePast());
  }
  const absl::StatusOr<LogLevel> level =
      ParseLogLevel(Attribute(chunk, kLogLevelAttribute));
  record.level = level.ok() ? *level : kDefaultLogLevel;
  record.channel = Attribute(chunk, kLogChannelAttribute);
  record.file = Attribute(chunk, kLogFileAttribute);
  record.internal = Attribute(chunk, kLogInternalAttribute) == kLogInternalTrue;
  int lineno = 0;
  if (absl::SimpleAtoi(Attribute(chunk, kLogLinenoAttribute), &lineno)) {
    record.lineno = lineno;
  }
  return record;
}

}  // namespace a11::actions
