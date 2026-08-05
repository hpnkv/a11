// Copyright 2026 The A11 Authors.

#ifndef A11_PYTHON_INTEROP_H_
#define A11_PYTHON_INTEROP_H_

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include <Python.h>
#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/status.h"

namespace a11::python {

namespace py = pybind11;

// Owns the event loop used by a Python override. Python methods may be invoked
// by ordinary libuv/libdatachannel threads or by A11 fibers, so they must be
// marshalled back to their asyncio loop.
class PythonLoop {
 public:
  static absl::StatusOr<std::shared_ptr<PythonLoop>> Capture();

  PythonLoop(const PythonLoop&) = delete;
  PythonLoop& operator=(const PythonLoop&) = delete;
  ~PythonLoop();

  class Cancellation;
  absl::StatusOr<std::shared_ptr<Cancellation>> Schedule(
      const py::object& awaitable, const py::object& completion) const;

 private:
  explicit PythonLoop(PyObject* absl_nonnull loop) : loop_(loop) {}

  PyObject* absl_nullable loop_ = nullptr;
};

class PythonLoop::Cancellation {
 public:
  explicit Cancellation(py::handle callback);
  Cancellation(const Cancellation&) = delete;
  Cancellation& operator=(const Cancellation&) = delete;
  ~Cancellation();

  void Cancel() const;

 private:
  PyObject* absl_nullable callback_ = nullptr;
};

absl::Status StatusFromPython(const py::handle& value);
py::object StatusToPython(const absl::Status& status);
absl::Status StatusFromPythonException(py::error_already_set& error);

absl::StatusOr<absl::Time> TimeFromPython(const py::handle& value,
                                          bool none_is_infinite = true);
py::object TimeToPython(absl::Time value);
absl::StatusOr<absl::Duration> DurationFromPython(const py::handle& value,
                                                  bool none_is_infinite = true);
py::object DurationToPython(absl::Duration value);

absl::StatusOr<data::ByteMap> ByteMapFromPython(const py::handle& value,
                                                bool none_is_empty = true);
py::dict ByteMapToPython(const data::ByteMap& value);

py::object StatusException(const absl::Status& status);
[[noreturn]] void ThrowStatus(const absl::Status& status);

template <typename T>
T ValueOrThrow(absl::StatusOr<T> value) {
  if (!value.ok())
    ThrowStatus(value.status());
  T result = std::move(value).value();
  return result;
}

// Keeps Python references safe even when the final C++ owner is released by
// an external transport thread.
class PythonReferences {
 public:
  PythonReferences(py::handle loop, py::handle future, py::handle completion);
  PythonReferences(const PythonReferences&) = delete;
  PythonReferences& operator=(const PythonReferences&) = delete;
  ~PythonReferences();

  py::object loop() const;
  py::object future() const;
  py::object completion() const;
  void ClearWithGilHeld();

 private:
  PyObject* absl_nullable loop_ = nullptr;
  PyObject* absl_nullable future_ = nullptr;
  PyObject* absl_nullable completion_ = nullptr;
};

template <typename T, typename Converter>
py::object FutureToPythonConverted(a11::Future<T> future, Converter converter) {
  py::module_ asyncio = py::module_::import("asyncio");
  py::object loop = asyncio.attr("get_running_loop")();
  py::module_ coordination = py::module_::import("a11._asyncio");
  py::object py_future = coordination.attr("_create_native_future")(
      loop, py::cpp_function([future]() mutable { (void)future.Cancel(); }));
  py::object completion = coordination.attr("_complete_future");
  auto references =
      std::make_shared<PythonReferences>(loop, py_future, completion);

  future.OnReady([references, converter = std::move(converter)](
                     const absl::StatusOr<T>& result) mutable {
    py::gil_scoped_acquire acquire;
    try {
      py::object value = py::none();
      py::object exception = py::none();
      if (result.ok()) {
        value = converter(*result);
      } else {
        exception = StatusException(result.status());
      }
      references->loop().attr("call_soon_threadsafe")(
          references->completion(), references->future(), std::move(value),
          std::move(exception));
    } catch (const py::error_already_set&) {
      // The loop may have closed concurrently. There is no live Python waiter
      // to receive another error in that case.
      PyErr_Clear();
    }
    references->ClearWithGilHeld();
  });
  return py_future;
}

template <typename T>
py::object FutureToPython(a11::Future<T> future) {
  return FutureToPythonConverted(
      std::move(future), [](const T& value) -> py::object {
        if constexpr (std::is_same_v<T, a11::Unit>) {
          return py::none();
        } else if constexpr (std::is_same_v<T, absl::Status>) {
          return StatusToPython(value);
        } else {
          return py::cast(value);
        }
      });
}

template <typename T>
a11::Future<T> PythonAwaitableToFuture(const std::shared_ptr<PythonLoop>& loop,
                                       const py::object& awaitable) {
  auto promise = std::make_shared<a11::Promise<T>>();
  a11::Future<T> future = promise->future();
  try {
    py::object completion =
        py::cpp_function([promise](const py::object& completed) {
          try {
            py::object value = completed.attr("result")();
            if constexpr (std::is_same_v<T, a11::Unit>) {
              if (!value.is_none()) {
                (void)promise->SetStatus(absl::InvalidArgumentError(
                    "Python async callback must return None"));
              } else {
                (void)promise->SetValue(a11::Unit{});
              }
            } else if constexpr (std::is_same_v<T, absl::Status>) {
              (void)promise->SetValue(StatusFromPython(value));
            } else {
              (void)promise->SetValue(value.cast<T>());
            }
          } catch (py::error_already_set& error) {
            (void)promise->SetStatus(StatusFromPythonException(error));
          } catch (const std::exception& error) {
            (void)promise->SetStatus(absl::InvalidArgumentError(error.what()));
          } catch (...) {
            (void)promise->SetStatus(absl::UnknownError(
                "Converting a Python async result raised an exception"));
          }
        });
    absl::StatusOr<std::shared_ptr<PythonLoop::Cancellation>> scheduled =
        loop->Schedule(awaitable, completion);
    if (!scheduled.ok()) {
      (void)promise->SetStatus(scheduled.status());
    } else {
      const absl::Status installed = promise->SetCancellationCallback(
          [cancellation = std::move(*scheduled)] { cancellation->Cancel(); });
      (void)installed;
    }
  } catch (py::error_already_set& error) {
    (void)promise->SetStatus(StatusFromPythonException(error));
  } catch (const std::exception& error) {
    (void)promise->SetStatus(absl::UnknownError(error.what()));
  } catch (...) {
    (void)promise->SetStatus(absl::UnknownError(
        "Scheduling a Python awaitable raised an exception"));
  }
  return future;
}

template <typename T, typename... Args>
a11::Future<T> CallPythonAsync(const std::shared_ptr<PythonLoop>& loop,
                               const py::function& function, Args&&... args) {
  py::gil_scoped_acquire acquire;
  try {
    // Creating a coroutine by calling the override on an arbitrary native
    // worker can execute Python before its asyncio loop is current. Defer the
    // invocation itself into the captured loop as well as awaiting its result.
    py::object awaitable =
        py::module_::import("a11._asyncio")
            .attr("_invoke_async")(function, std::forward<Args>(args)...);
    return PythonAwaitableToFuture<T>(loop, awaitable);
  } catch (py::error_already_set& error) {
    return a11::FailedFuture<T>(StatusFromPythonException(error));
  } catch (const std::exception& error) {
    return a11::FailedFuture<T>(absl::UnknownError(error.what()));
  } catch (...) {
    return a11::FailedFuture<T>(
        absl::UnknownError("Calling a Python override raised an exception"));
  }
}

/**
 * Owns an asynchronous Python callback and the asyncio loop it was created on.
 * Native fibers and transport threads may call it safely; invocation and
 * awaiting are marshalled to the captured loop and surfaced as an A11 Task.
 */
class AsyncPythonCallback {
 public:
  static absl::StatusOr<std::shared_ptr<AsyncPythonCallback>> Create(
      const py::object& callable);

  AsyncPythonCallback(const AsyncPythonCallback&) = delete;
  AsyncPythonCallback& operator=(const AsyncPythonCallback&) = delete;
  ~AsyncPythonCallback();

  template <typename... Args>
  a11::Task Call(Args&&... args) const {
    py::gil_scoped_acquire acquire;
    py::function function = py::reinterpret_borrow<py::function>(callable_);
    return CallPythonAsync<a11::Unit>(loop_, function,
                                      std::forward<Args>(args)...);
  }

 private:
  AsyncPythonCallback(PyObject* absl_nonnull callable,
                      std::shared_ptr<PythonLoop> loop)
      : callable_(callable), loop_(std::move(loop)) {}

  PyObject* absl_nullable callable_ = nullptr;
  std::shared_ptr<PythonLoop> loop_;
};

template <typename T>
absl::StatusOr<T> DataFromPython(const py::handle& value) {
  try {
    py::bytes packed = value.attr("to_msgpack")().cast<py::bytes>();
    std::string bytes = packed;
    return T::FromMsgpack(bytes);
  } catch (py::error_already_set& error) {
    return StatusFromPythonException(error);
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  } catch (...) {
    return absl::InvalidArgumentError(
        "Converting an A11 Python data object raised an exception");
  }
}

template <typename T>
py::handle DataToPython(const T& value, const char* absl_nonnull class_name) {
  py::gil_scoped_acquire acquire;
  absl::StatusOr<std::string> packed = value.ToMsgpack();
  if (!packed.ok())
    ThrowStatus(packed.status());
  py::object cls = py::module_::import("a11.data.types").attr(class_name);
  return cls.attr("from_msgpack")(py::bytes(*packed)).release();
}

}  // namespace a11::python

#endif  // A11_PYTHON_INTEROP_H_
