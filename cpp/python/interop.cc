// Copyright 2026 The A11 Authors.

#include "python/interop.h"

#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <Python.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11_abseil/absl_casters.h>
#include <pybind11_abseil/compat/status_from_py_exc.h>
#include <pybind11_abseil/status_casters.h>

#include "a11/status.h"
#include "python/native_types.h"

namespace a11::python {
namespace {

class GilForDestructor {
 public:
  GilForDestructor() {
    if (Py_IsInitialized() != 0)
      state_ = PyGILState_Ensure();
  }

  ~GilForDestructor() {
    if (state_.has_value())
      PyGILState_Release(*state_);
  }

  [[nodiscard]] bool acquired() const { return state_.has_value(); }

 private:
  std::optional<PyGILState_STATE> state_;
};

absl::StatusCode CanonicalStatusCode(int value) {
  if (value < static_cast<int>(absl::StatusCode::kOk) ||
      value > static_cast<int>(absl::StatusCode::kUnauthenticated)) {
    return absl::StatusCode::kUnknown;
  }
  return static_cast<absl::StatusCode>(value);
}

}  // namespace

absl::StatusOr<std::shared_ptr<PythonLoop>> PythonLoop::Capture() {
  py::gil_scoped_acquire acquire;
  try {
    py::module_ asyncio = py::module_::import("asyncio");
    py::object loop;
    try {
      loop = asyncio.attr("get_running_loop")();
    } catch (const py::error_already_set&) {
      PyErr_Clear();
      loop = asyncio.attr("get_event_loop_policy")().attr("get_event_loop")();
    }
    PyObject* owned = loop.release().ptr();

    struct MakeSharedEnabler final : PythonLoop {
      explicit MakeSharedEnabler(PyObject* loop) : PythonLoop(loop) {}
    };

    return std::make_shared<MakeSharedEnabler>(owned);
  } catch (py::error_already_set& error) {
    return StatusFromPythonException(error);
  } catch (const std::exception& error) {
    return absl::FailedPreconditionError(error.what());
  } catch (...) {
    return absl::FailedPreconditionError(
        "Unable to capture a Python asyncio event loop");
  }
}

PythonLoop::~PythonLoop() {
  GilForDestructor gil;
  if (gil.acquired())
    Py_XDECREF(loop_);
  loop_ = nullptr;
}

absl::StatusOr<std::shared_ptr<PythonLoop::Cancellation>> PythonLoop::Schedule(
    const py::object& awaitable, const py::object& completion) const {
  try {
    py::object loop = py::reinterpret_borrow<py::object>(loop_);
    py::object cancellation = py::module_::import("a11._asyncio")
                                  .attr("_schedule_awaitable_threadsafe")(
                                      loop, awaitable, completion);
    return std::make_shared<Cancellation>(cancellation);
  } catch (py::error_already_set& error) {
    return StatusFromPythonException(error);
  } catch (const std::exception& error) {
    return absl::UnknownError(error.what());
  } catch (...) {
    return absl::UnknownError(
        "Scheduling work on the Python event loop raised an exception");
  }
}

PythonLoop::Cancellation::Cancellation(py::handle callback)
    : callback_(callback.inc_ref().ptr()) {}

PythonLoop::Cancellation::~Cancellation() {
  GilForDestructor gil;
  if (gil.acquired())
    Py_CLEAR(callback_);
}

void PythonLoop::Cancellation::Cancel() const {
  py::gil_scoped_acquire acquire;
  try {
    py::reinterpret_borrow<py::function>(callback_)();
  } catch (const py::error_already_set&) {
    PyErr_Clear();
  }
}

absl::StatusOr<std::shared_ptr<AsyncPythonCallback>>
AsyncPythonCallback::Create(const py::object& callable) {
  if (!PyCallable_Check(callable.ptr())) {
    return absl::InvalidArgumentError("callback must be callable");
  }
  absl::StatusOr<std::shared_ptr<PythonLoop>> loop = PythonLoop::Capture();
  if (!loop.ok()) {
    return loop.status();
  }

  struct MakeSharedEnabler final : AsyncPythonCallback {
    MakeSharedEnabler(PyObject* callable, std::shared_ptr<PythonLoop> loop)
        : AsyncPythonCallback(callable, std::move(loop)) {}
  };

  return std::make_shared<MakeSharedEnabler>(callable.inc_ref().ptr(),
                                             std::move(*loop));
}

AsyncPythonCallback::~AsyncPythonCallback() {
  GilForDestructor gil;
  if (gil.acquired()) {
    Py_CLEAR(callable_);
  }
}

absl::Status StatusFromPython(const py::handle& value) {
  try {
    if (py::isinstance<NativeStatus>(value))
      return value.cast<const NativeStatus&>().value();
    py::object ground_status = py::module_::import("a11.status").attr("Status");
    if (py::isinstance(value, ground_status)) {
      const int code = value.attr("code").cast<int>();
      const std::string message = value.attr("message").cast<std::string>();
      py::object details_object = value.attr("details");
      std::string details_json = py::module_::import("json")
                                     .attr("dumps")(details_object)
                                     .cast<std::string>();
      nlohmann::json details = nlohmann::json::parse(details_json);
      return MakeStatus(CanonicalStatusCode(code), message, std::move(details));
    }
    return value.cast<absl::Status>();
  } catch (py::error_already_set& error) {
    return StatusFromPythonException(error);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  } catch (...) {
    return absl::InvalidArgumentError(
        "Converting a Python status raised an exception");
  }
}

py::object StatusToPython(const absl::Status& status) {
  py::gil_scoped_acquire acquire;
  return py::cast(NativeStatus(status));
}

absl::Status StatusFromPythonException(py::error_already_set& error) {
  try {
    py::object exception = error.value();
    py::object cancelled_error =
        py::module_::import("asyncio").attr("CancelledError");
    if (py::isinstance(exception, cancelled_error)) {
      error.restore();
      PyErr_Clear();
      return absl::CancelledError("Python awaitable was cancelled");
    }
    py::object status_exception =
        py::module_::import("a11.status").attr("StatusException");
    if (py::isinstance(exception, status_exception)) {
      absl::Status status = StatusFromPython(exception.attr("status"));
      // Consume the fetched exception without reporting it as unraisable: it
      // is an expected, recoverable status crossing the language boundary.
      error.restore();
      PyErr_Clear();
      return status;
    }
    const std::string message = py::str(exception).cast<std::string>();
    error.restore();
    PyErr_Clear();
    return absl::UnknownError(message);
  } catch (const py::error_already_set&) {
    PyErr_Clear();
  } catch (...) {
    PyErr_Clear();
  }
  error.restore();
  return pybind11_abseil::compat::StatusFromPyExcGivenErrOccurred();
}

absl::StatusOr<absl::Time> TimeFromPython(const py::handle& value,
                                          bool none_is_infinite) {
  if (value.is_none()) {
    if (none_is_infinite)
      return absl::InfiniteFuture();
    return absl::InvalidArgumentError("time must not be None");
  }
  try {
    if (py::isinstance<NativeTime>(value))
      return value.cast<const NativeTime&>().value();
    py::module_ timing = py::module_::import("a11.timing");
    if (py::isinstance(value, timing.attr("Time"))) {
      if (value.equal(timing.attr("infinite_future")())) {
        return absl::InfiniteFuture();
      }
      if (value.equal(timing.attr("infinite_past")())) {
        return absl::InfinitePast();
      }
      return absl::FromUnixNanos(
          value.attr("nanoseconds_since_epoch").cast<std::int64_t>());
    }
    return value.cast<absl::Time>();
  } catch (py::error_already_set& error) {
    return StatusFromPythonException(error);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  } catch (...) {
    return absl::InvalidArgumentError("Invalid Python time value");
  }
}

py::object TimeToPython(absl::Time value) {
  py::gil_scoped_acquire acquire;
  return py::cast(NativeTime(value));
}

absl::StatusOr<absl::Duration> DurationFromPython(const py::handle& value,
                                                  bool none_is_infinite) {
  if (value.is_none()) {
    if (none_is_infinite)
      return absl::InfiniteDuration();
    return absl::InvalidArgumentError("duration must not be None");
  }
  try {
    if (py::isinstance<NativeDuration>(value))
      return value.cast<const NativeDuration&>().value();
    py::module_ timing = py::module_::import("a11.timing");
    if (py::isinstance(value, timing.attr("Duration"))) {
      if (value.equal(timing.attr("infinite_duration")()))
        return absl::InfiniteDuration();
      if (value.equal(-timing.attr("infinite_duration")()))
        return -absl::InfiniteDuration();
      return absl::Nanoseconds(
          value.attr("nanoseconds_value").cast<std::int64_t>());
    }
    return value.cast<absl::Duration>();
  } catch (py::error_already_set& error) {
    return StatusFromPythonException(error);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  } catch (...) {
    return absl::InvalidArgumentError("Invalid Python duration value");
  }
}

py::object DurationToPython(absl::Duration value) {
  py::gil_scoped_acquire acquire;
  return py::cast(NativeDuration(value));
}

absl::StatusOr<data::ByteMap> ByteMapFromPython(const py::handle& value,
                                                bool none_is_empty) {
  if (value.is_none()) {
    if (none_is_empty)
      return data::ByteMap{};
    return absl::InvalidArgumentError("mapping must not be None");
  }
  try {
    py::object items;
    if (py::isinstance<py::dict>(value)) {
      items = py::reinterpret_borrow<py::dict>(value).attr("items")();
    } else if (py::hasattr(value, "items")) {
      items = value.attr("items")();
    } else {
      return absl::InvalidArgumentError("expected a mapping of str to bytes");
    }
    data::ByteMap result;
    for (const py::handle raw_item : items) {
      const py::tuple item = py::cast<py::tuple>(raw_item);
      if (item.size() != 2 || !py::isinstance<py::str>(item[0]) ||
          !py::isinstance<py::bytes>(item[1])) {
        return absl::InvalidArgumentError(
            "mapping keys must be str and values must be bytes");
      }
      result.emplace(item[0].cast<std::string>(), item[1].cast<std::string>());
    }
    return result;
  } catch (py::error_already_set& error) {
    return StatusFromPythonException(error);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
}

py::dict ByteMapToPython(const data::ByteMap& value) {
  py::gil_scoped_acquire acquire;
  py::dict result;
  for (const auto& [key, bytes] : value)
    result[py::str(key)] = py::bytes(bytes);
  return result;
}

py::object StatusException(const absl::Status& status) {
  py::gil_scoped_acquire acquire;
  return py::module_::import("a11.status")
      .attr("StatusException")(StatusToPython(status));
}

[[noreturn]] void ThrowStatus(const absl::Status& status) {
  py::object exception = StatusException(status);
  PyErr_SetObject(reinterpret_cast<PyObject*>(Py_TYPE(exception.ptr())),
                  exception.ptr());
  throw py::error_already_set();
}

PythonReferences::PythonReferences(py::handle loop, py::handle future,
                                   py::handle completion)
    : loop_(loop.inc_ref().ptr()),
      future_(future.inc_ref().ptr()),
      completion_(completion.inc_ref().ptr()) {}

PythonReferences::~PythonReferences() {
  GilForDestructor gil;
  if (gil.acquired())
    ClearWithGilHeld();
}

py::object PythonReferences::loop() const {
  return py::reinterpret_borrow<py::object>(loop_);
}

py::object PythonReferences::future() const {
  return py::reinterpret_borrow<py::object>(future_);
}

py::object PythonReferences::completion() const {
  return py::reinterpret_borrow<py::object>(completion_);
}

void PythonReferences::ClearWithGilHeld() {
  Py_CLEAR(loop_);
  Py_CLEAR(future_);
  Py_CLEAR(completion_);
}

}  // namespace a11::python
