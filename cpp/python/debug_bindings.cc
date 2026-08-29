// Copyright 2026 The A11 Authors.

#include <cstdint>
#include <string>
#include <vector>

#include <absl/time/time.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "python/bindings.h"
#include "thread/introspect.h"

namespace a11::python {
namespace {

namespace py = pybind11;

py::dict SnapshotToDict(const thread::FiberSnapshot& fiber) {
  py::list stack;
  for (void* pc : fiber.stack) {
    stack.append(reinterpret_cast<std::uintptr_t>(pc));
  }
  py::list selectables;
  for (const void* selectable : fiber.selectables) {
    selectables.append(reinterpret_cast<std::uintptr_t>(selectable));
  }
  py::dict entry;
  entry["id"] = fiber.id;
  entry["parent_id"] = fiber.parent_id;
  entry["name"] = fiber.name;
  entry["wait"] = thread::WaitKindName(fiber.kind);
  entry["wait_object"] = reinterpret_cast<std::uintptr_t>(fiber.wait_object);
  entry["blocking_fiber_id"] = fiber.blocking_fiber_id;
  entry["waited_seconds"] = absl::ToDoubleSeconds(fiber.waited);
  entry["stack"] = stack;
  entry["selectables"] = selectables;
  entry["trace_raced"] = fiber.trace_raced;
  entry["waits_completed"] = fiber.waits_completed;
  return entry;
}

}  // namespace

void BindDebug(py::module_& module) {
  module.def(
      "fiber_report",
      [](double stall_threshold_seconds, size_t max_frames,
         bool include_running) {
        thread::FiberReportOptions options;
        options.stall_threshold = absl::Seconds(stall_threshold_seconds);
        options.max_frames = max_frames;
        options.include_running = include_running;
        py::gil_scoped_release release;
        return thread::FormatFiberReport(options);
      },
      py::arg("stall_threshold_seconds") = 0.0, py::arg("max_frames") = 24,
      py::arg("include_running") = false,
      "A symbolized report of every live fiber: a census by wait kind, any "
      "deadlock cycles, then the stalled fibers with their parked stacks.");

  module.def(
      "fiber_snapshot",
      [](size_t max_frames) {
        std::vector<thread::FiberSnapshot> snapshot;
        {
          py::gil_scoped_release release;
          snapshot = thread::SnapshotFibers(max_frames);
        }
        py::list entries;
        for (const thread::FiberSnapshot& fiber : snapshot) {
          entries.append(SnapshotToDict(fiber));
        }
        return entries;
      },
      py::arg("max_frames") = 24,
      "Every live fiber as a list of dicts, with parked stacks unwound to "
      "raw program counters. Pass max_frames=0 to skip the unwind.");

  module.def(
      "find_fiber_deadlock",
      [] {
        py::gil_scoped_release release;
        return thread::FindWaitCycles();
      },
      "Wait-for cycles among the fibers, as lists of fiber ids. A cycle here "
      "is a deadlock: only mutex ownership and joins produce an edge whose "
      "other end is known.");

  module.def(
      "install_fiber_watchdog",
      [](double stall_threshold_seconds, bool abort_on_stall) {
        thread::InstallFiberWatchdog(absl::Seconds(stall_threshold_seconds),
                                     abort_on_stall);
      },
      py::arg("stall_threshold_seconds"), py::arg("abort_on_stall") = false,
      "Starts a thread that logs a fiber report once any fiber has waited "
      "longer than the threshold. Idempotent.");

  module.def("request_fiber_dump", &thread::RequestFiberDump,
             "Asks the watchdog thread to log a report. Requires a watchdog.");

  module.def(
      "install_fiber_dump_signal_handler",
      &thread::InstallFiberDumpSignalHandler, py::arg("signal_number") = 0,
      "Installs a handler that logs a fiber report on a signal, default "
      "SIGUSR2 or A11_FIBER_DUMP_SIGNAL. Returns whether it was installed.");

  module.def("current_fiber_id", &thread::CurrentFiberId,
             "The calling fiber's id, or 0 when the caller is not on a fiber.");

  module.def(
      "set_current_fiber_name",
      [](const std::string& name) { thread::SetCurrentFiberName(name); },
      py::arg("name"),
      "Names the calling fiber for reports, truncating at 47 characters. "
      "No-op off a fiber.");

  module.def(
      "total_completed_fiber_waits", &thread::TotalCompletedWaits,
      "Completed waits across all live fibers. Two equal readings a second "
      "apart mean nothing moved.");
}

}  // namespace a11::python
