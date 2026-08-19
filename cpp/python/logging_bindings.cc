// Copyright 2026 The A11 Authors.

#include <memory>
#include <string>
#include <utility>

#include <Python.h>
#include <absl/base/log_severity.h>
#include <absl/log/globals.h>
#include <absl/log/log.h>
#include <absl/log/log_entry.h>
#include <absl/log/log_sink.h>
#include <absl/log/log_sink_registry.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <pybind11/pybind11.h>

#include "a11/actions/log.h"
#include "python/bindings.h"
#include "python/interop.h"

namespace a11::python {
namespace {

namespace py = pybind11;

absl::LogSeverityAtLeast SeverityAtLeast(int value) {
  if (value <= static_cast<int>(absl::LogSeverity::kInfo)) {
    return absl::LogSeverityAtLeast::kInfo;
  }
  if (value == static_cast<int>(absl::LogSeverity::kWarning)) {
    return absl::LogSeverityAtLeast::kWarning;
  }
  if (value == static_cast<int>(absl::LogSeverity::kError)) {
    return absl::LogSeverityAtLeast::kError;
  }
  return absl::LogSeverityAtLeast::kFatal;
}

// Forwards each Abseil log entry to a Python callable, which turns it into a
// LogRecord on an ordinary `logging.Logger` (see a11/logging.py). Only the
// fields a LogRecord needs are handed over; Abseil's own prefix is dropped so
// the Python formatter owns the textual shape.
//
// FATAL is left alone. Abseil writes it to stderr with a backtrace and then
// ends the process, and taking the GIL on a dying thread buys nothing.
class PythonLogSink final : public absl::LogSink {
 public:
  explicit PythonLogSink(py::object callback)
      : callback_(std::move(callback)) {}

  // Called from whichever thread wrote the entry: libuv, a fiber worker, or
  // the interpreter itself. Abseil disables sink dispatch per-thread for the
  // duration of Send(), so a LOG raised by the Python handler cannot recurse
  // back into here.
  void Send(const absl::LogEntry& entry) override {
    if (entry.log_severity() == absl::LogSeverity::kFatal) {
      return;
    }
    if (InterpreterIsGoingAway()) {
      return;
    }
    const PyGILState_STATE gil = PyGILState_Ensure();
    try {
      callback_(static_cast<int>(entry.log_severity()), entry.verbosity(),
                std::string(entry.source_filename()), entry.source_line(),
                std::string(entry.text_message()),
                absl::ToDoubleSeconds(entry.timestamp() - absl::UnixEpoch()));
    } catch (const py::error_already_set&) {
      // A failure to log must never reach the code that was logging.
      PyErr_Clear();
    } catch (...) {
      PyErr_Clear();
    }
    PyGILState_Release(gil);
  }

 private:
  py::object callback_;
};

// The one installed sink, owned here so Python can swap or drop it. Guarded by
// the GIL: every mutation runs from a binding call.
std::unique_ptr<PythonLogSink>& InstalledSink() {
  static auto* sink = new std::unique_ptr<PythonLogSink>();
  return *sink;
}

// Forwards each action log to a Python callable. The same shape as the Abseil
// bridge above and for the same reason: one sink slot, so a log is reported once.
// Replacing this slot is what keeps Python from adding a *second* consumer of the
// log port alongside the default, which would report every line twice.
void SetActionLogSink(const py::object& callback) {
  if (callback.is_none()) {
    actions::SetActionLogSink(nullptr);
    return;
  }
  py::object held = callback;
  actions::SetActionLogSink([held = std::move(held)](
                                const actions::LogRecord& record) {
    if (InterpreterIsGoingAway()) {
      return;
    }
    const PyGILState_STATE gil = PyGILState_Ensure();
    try {
      held(std::string(record.action_name), std::string(record.action_id),
           std::string(actions::LogLevelName(record.level)),
           std::string(record.channel), std::string(record.file),
           record.lineno.has_value() ? py::cast(*record.lineno) : py::none(),
           record.internal, std::string(record.mimetype),
           py::bytes(record.data.data(), record.data.size()),
           absl::ToDoubleSeconds(record.timestamp - absl::UnixEpoch()));
    } catch (const py::error_already_set&) {
      // A failure to log must never reach the code that was logging.
      PyErr_Clear();
    } catch (...) {
      PyErr_Clear();
    }
    PyGILState_Release(gil);
  });
}

void SetLogSink(const py::object& callback) {
  std::unique_ptr<PythonLogSink>& installed = InstalledSink();
  if (installed != nullptr) {
    absl::RemoveLogSink(installed.get());
    installed.reset();
  }
  if (callback.is_none()) {
    return;
  }
  installed = std::make_unique<PythonLogSink>(callback);
  absl::AddLogSink(installed.get());
}

}  // namespace

void BindLogging(py::module_& module) {
  module.def(
      "release_deferred_python_refs", [] { DeferredPythonRefs::Drain(); },
      "Release Python references that native destructors queued instead of "
      "dropping.\n\n"
      "A native destructor may run on a worker thread, where acquiring the GIL "
      "races interpreter finalization and gets the thread killed with "
      "pthread_exit -- which then unwinds through frames compiled "
      "-fno-exceptions and aborts the process. Such references are queued "
      "instead. This drains the queue, and is safe only because the caller "
      "holds the GIL by virtue of being called from Python. Registered with "
      "atexit by a11/__init__.py; calling it by hand is harmless.");

  module.def(
      "deferred_python_refs_pending",
      [] { return DeferredPythonRefs::PendingCount(); },
      "How many Python references native destructors have queued and not yet "
      "released.");

  module.def(
      "deferred_python_refs_high_water",
      [] { return DeferredPythonRefs::HighWaterCount(); },
      "The most references ever queued at once.\n\n"
      "This is the number that says whether the deferred queue is a small "
      "buffer or a leak: every Python invocation drains it, and a destructor "
      "running on a thread that already holds the GIL never queues at all, so "
      "it should stay small however many sessions churn through the process.");

  module.def(
      "set_min_log_level",
      [](int severity) { absl::SetMinLogLevel(SeverityAtLeast(severity)); },
      py::arg("severity"),
      "Drop native log entries below this absl severity (0=INFO, 1=WARNING, "
      "2=ERROR, 3=FATAL) before they are formatted.");

  module.def(
      "set_stderr_threshold",
      [](int severity) { absl::SetStderrThreshold(SeverityAtLeast(severity)); },
      py::arg("severity"),
      "Write native log entries at or above this absl severity straight to "
      "stderr, bypassing any installed sink.");

  module.def(
      "set_vlog_level", [](int level) { absl::SetGlobalVLogLevel(level); },
      py::arg("level"),
      "Emit VLOG(n) entries for n at or below this level. 0 disables them.");

  module.def(
      "emit_log",
      [](int severity, const std::string& message, int verbosity) {
        absl::LogSeverity level = absl::LogSeverity::kInfo;
        if (severity == static_cast<int>(absl::LogSeverity::kWarning)) {
          level = absl::LogSeverity::kWarning;
        } else if (severity >= static_cast<int>(absl::LogSeverity::kError)) {
          level = absl::LogSeverity::kError;
        }
        LOG(LEVEL(level)).WithVerbosity(verbosity) << message;
      },
      py::arg("severity"), py::arg("message"), py::arg("verbosity") = -1,
      "Write one entry to the native log, so an application can check that "
      "its logging configuration reaches the C++ runtime. FATAL is not "
      "available here.");

  module.def(
      "set_log_sink", &SetLogSink, py::arg("callback"),
      "Route native log entries to `callback(severity, verbosity, filename, "
      "line, message, unix_seconds)`, replacing any previously installed "
      "sink. None removes the sink. FATAL entries are never routed; Abseil "
      "writes those to stderr with a backtrace.");

  module.def(
      "set_action_log_sink", &SetActionLogSink, py::arg("callback"),
      "Route what actions log to `callback(action_name, action_id, level, "
      "channel, file, lineno, internal, mimetype, data, unix_seconds)`, "
      "replacing the default, which reports each one through the native log. "
      "None restores that default.\n\n"
      "One slot rather than one consumer per language: the native default "
      "already reaches Python through set_log_sink, so a Python sink installed "
      "*beside* it would report every line twice.");
}

}  // namespace a11::python
