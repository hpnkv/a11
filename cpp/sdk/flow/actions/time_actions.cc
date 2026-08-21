// Copyright 2026 The A11 Authors.

#include "sdk/flow/actions/time_actions.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "sdk/flow/actions/options.h"
#include "sdk/flow/actions/ports.h"
#include "sdk/flow/actions/stop.h"

namespace a11::sdk::flow {
namespace {

using ::a11::actions::ActionHandler;
using ::a11::actions::ActionSchema;

/// An interval below this would spend more time waking a fibre than waiting,
/// and a flow that wants one wants a stream rather than a clock.
constexpr absl::Duration kMinimumInterval = absl::Milliseconds(1);
constexpr absl::Duration kDefaultInterval = absl::Seconds(1);

/// What one tick looks like on the wire. An object rather than a bare instant
/// because a tick's *number* is what a flow usually branches on, and deriving
/// it from a count of values it has seen is work a source can just do.
nlohmann::json TickValue(std::int64_t number, absl::Time fired,
                         absl::Time scheduled, absl::Duration late) {
  return nlohmann::json{
      {"number", number},
      {"at", absl::FormatTime(absl::RFC3339_full, fired, absl::UTCTimeZone())},
      {"scheduled",
       absl::FormatTime(absl::RFC3339_full, scheduled, absl::UTCTimeZone())},
      {"late_ms", absl::ToInt64Milliseconds(late)},
  };
}

struct TickerOptions {
  absl::Duration interval = kDefaultInterval;
  /// How many ticks to deliver, or 0 for as many as it is allowed to.
  std::int64_t count = 0;
  /// How long to keep ticking, or InfiniteDuration.
  absl::Duration duration = absl::InfiniteDuration();
  /// Whether to deliver ticks a slow reader missed, rather than skip them.
  bool catch_up = false;
  /// Whether to deliver one immediately rather than after the first interval.
  bool immediate = false;
};

absl::StatusOr<TickerOptions> ReadTickerOptions(const Options& options) {
  TickerOptions parsed;
  ABSL_ASSIGN_OR_RETURN(parsed.interval,
                        options.Duration("every", kDefaultInterval));
  if (parsed.interval < kMinimumInterval) {
    return absl::InvalidArgumentError(absl::StrCat(
        "options.every must be at least 1ms; a faster source than that is a "
        "stream rather than a clock"));
  }
  ABSL_ASSIGN_OR_RETURN(
      parsed.count,
      options.IntInRange("count", 0, 0,
                         std::numeric_limits<std::int64_t>::max()));
  ABSL_ASSIGN_OR_RETURN(parsed.duration,
                        options.Duration("for", absl::InfiniteDuration()));
  ABSL_ASSIGN_OR_RETURN(parsed.catch_up, options.Bool("catch_up", false));
  ABSL_ASSIGN_OR_RETURN(parsed.immediate, options.Bool("immediate", false));
  return parsed;
}

absl::Status RunTicker(const std::shared_ptr<actions::Action>& action) {
  ABSL_ASSIGN_OR_RETURN(const std::optional<nlohmann::json> raw_options,
                        ReadJsonInput(action, "options"));
  ABSL_ASSIGN_OR_RETURN(
      const Options options,
      Options::Parse(raw_options.has_value() ? &*raw_options : nullptr));
  ABSL_ASSIGN_OR_RETURN(const TickerOptions settings,
                        ReadTickerOptions(options));
  ABSL_ASSIGN_OR_RETURN(const std::vector<std::string> omitted, options.Omit());

  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OpenOutputs(action, options));
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<StopSignal> stop,
                        StopSignal::Create(action, kControlPort));

  const Sink ticks = outputs["ticks"];
  const Sink skipped_out = outputs["skipped"];
  const Sink count_out = outputs["count"];

  const absl::Time started = absl::Now();
  // Both bounds are absolute from here on: a deadline header and a `for` are
  // the same kind of limit, and taking the tighter of them once is simpler than
  // testing two things per tick.
  absl::Time until = settings.duration >= absl::InfiniteDuration()
                         ? absl::InfiniteFuture()
                         : started + settings.duration;
  if (stop->deadline() < until) {
    until = stop->deadline();
  }

  std::int64_t delivered = 0;
  std::int64_t skipped = 0;
  absl::Status status = absl::OkStatus();

  // Scheduled against `started` rather than against the last tick, so a slow
  // reader cannot make the interval creep.
  for (std::int64_t index = settings.immediate ? 0 : 1;; ++index) {
    const absl::Time scheduled = started + settings.interval * index;
    if (scheduled > until) {
      break;
    }
    if (stop->WaitUntil(scheduled)) {
      break;
    }
    const absl::Time fired = absl::Now();
    const absl::Duration late = fired - scheduled;
    if (!settings.catch_up && late >= settings.interval) {
      // A tick delivered this late would be a claim about a moment that has
      // passed. Counted instead, so a flow can see it happened.
      ++skipped;
      continue;
    }
    ++delivered;
    const bool last = settings.count > 0 && delivered >= settings.count;
    status = ticks.PutValue(TickValue(delivered, fired, scheduled, late), last);
    if (!status.ok() || last) {
      break;
    }
  }

  stop->Join();
  if (status.ok()) {
    status = stop->ExitStatus();
  }
  if (!status.ok()) {
    // Whatever went wrong, a reader of `ticks` has to learn that the stream
    // stopped early rather than that the clock simply ran out.
    outputs.Abort(status).IgnoreError();
    return status;
  }
  ABSL_RETURN_IF_ERROR(skipped_out.PutOnly(nlohmann::json(skipped)));
  ABSL_RETURN_IF_ERROR(count_out.PutOnly(nlohmann::json(delivered)));
  return outputs.Finish();
}

absl::Status RunSleep(const std::shared_ptr<actions::Action>& action) {
  ABSL_ASSIGN_OR_RETURN(const std::optional<nlohmann::json> raw_duration,
                        ReadJsonInput(action, "duration"));
  // No options port: the only output is a record of having waited, which has
  // nothing in it that an encoding could change.
  ABSL_ASSIGN_OR_RETURN(OutputPorts outputs, OutputPorts::Open(action));
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<StopSignal> stop,
                        StopSignal::Create(action, kControlPort));

  absl::Duration requested = absl::ZeroDuration();
  if (raw_duration.has_value() && !raw_duration->is_null()) {
    if (raw_duration->is_number()) {
      requested = absl::Seconds(raw_duration->get<double>());
    } else if (raw_duration->is_string()) {
      ABSL_ASSIGN_OR_RETURN(requested,
                            ParseDuration(raw_duration->get<std::string>()));
    } else {
      stop->Join();
      return absl::InvalidArgumentError(
          "sleep_for's duration must be a duration or a number of seconds");
    }
  }
  if (requested < absl::ZeroDuration()) {
    stop->Join();
    return absl::InvalidArgumentError(
        "sleep_for will not wait a negative length of time");
  }

  const bool interrupted = stop->WaitFor(requested);
  absl::Status exit = stop->ExitStatus();
  stop->Join();
  if (!exit.ok()) {
    outputs.Abort(exit).IgnoreError();
    return exit;
  }
  // `woke` says whether the wait ran its course, so a flow can tell "the delay
  // elapsed" from "something asked us to get on with it" -- which is exactly
  // what a flow racing a delay against a call wants to know.
  ABSL_RETURN_IF_ERROR(outputs["woke"].PutOnly(nlohmann::json{
      {"at",
       absl::FormatTime(absl::RFC3339_full, absl::Now(), absl::UTCTimeZone())},
      {"elapsed_ms", absl::ToInt64Milliseconds(requested)},
      {"interrupted", interrupted}}));
  return outputs.Finish();
}

}  // namespace

ActionSchema TickerSchema() {
  ActionSchema schema;
  schema.name = std::string(kTickerAction);
  schema.description =
      "Produce a value every interval, so that a composition can act because "
      "time passed rather than because something arrived. Bounded by count, by "
      "a total duration, by the deadline header, or by a stop command; "
      "unbounded otherwise, and the stream simply ends when it is asked to. "
      "Ticks are scheduled from the start rather than from the last one, so a "
      "slow reader does not make the interval drift -- it makes ticks skip, "
      "counted on `skipped`.";
  schema.inputs.emplace(
      "options",
      Port("options", JsonType(),
           "All optional: every (interval, default 1s, minimum 1ms), count "
           "(how many ticks, default unbounded), for (how long to keep going), "
           "immediate (tick once at the start rather than after the first "
           "interval), catch_up (deliver ticks a slow reader missed instead of "
           "skipping them), and omit -- output port names to close immediately "
           "rather than write.",
           /*required=*/false, /*unary=*/true));
  schema.inputs.emplace(
      std::string(kControlPort),
      Port(kControlPort, JsonType(),
           "Control commands; a {\"command\": \"stop\"} ends the stream "
           "gracefully, which is how a flow stops a clock it started.",
           /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "ticks",
      Port("ticks", JsonType(),
           "One {number, at, scheduled, late_ms} per tick, as it fires.",
           /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "skipped",
      Port("skipped", "integer",
           "How many ticks were not delivered because a reader was more than "
           "an interval behind. Zero unless there was a reason.",
           /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "count", Port("count", "integer", "How many ticks were delivered.",
                    /*required=*/false, /*unary=*/true));
  AddDeadlineHeader(schema, "The stream ends gracefully once it is reached.");
  return schema;
}

ActionSchema SleepSchema() {
  ActionSchema schema;
  schema.name = std::string(kSleepAction);
  schema.description =
      "Wait, then report having waited. What `after` needs to order one "
      "statement behind a delay: a retry's backoff, a settling period before a "
      "check, a beat between two calls. A stop command or a cancellation cuts "
      "the wait short and says so on `woke`.";
  schema.inputs.emplace(
      "duration",
      Port("duration", "string",
           "How long to wait, written as Flow writes a duration (30s, 250ms, "
           "1m30s) or as a number of seconds.",
           /*required=*/true, /*unary=*/true));
  schema.inputs.emplace(
      std::string(kControlPort),
      Port(kControlPort, JsonType(),
           R"(Control commands; a {"command": "stop"} ends the wait early.)",
           /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "woke",
      Port("woke", JsonType(),
           "{at, elapsed_ms, interrupted} -- where `interrupted` says the wait "
           "was cut short rather than ran its course.",
           /*required=*/false, /*unary=*/true));
  AddDeadlineHeader(schema, "The wait ends gracefully once it is reached.");
  return schema;
}

ActionHandler TickerHandler() {
  return [](std::shared_ptr<actions::Action> action) {
    return a11::SubmitTask(
        [action = std::move(action)]() { return RunTicker(action); });
  };
}

ActionHandler SleepHandler() {
  return [](std::shared_ptr<actions::Action> action) {
    return a11::SubmitTask(
        [action = std::move(action)]() { return RunSleep(action); });
  };
}

absl::Status RegisterTimeActions(actions::ActionRegistry& registry) {
  ABSL_RETURN_IF_ERROR(registry.Register(std::string(kTickerAction),
                                         TickerSchema(), TickerHandler()));
  return registry.Register(std::string(kSleepAction), SleepSchema(),
                           SleepHandler());
}

}  // namespace a11::sdk::flow
