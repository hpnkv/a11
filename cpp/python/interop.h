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

#ifndef A11_PYTHON_INTEROP_H_
#define A11_PYTHON_INTEROP_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <Python.h>
#include <absl/base/no_destructor.h>
#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/synchronization/mutex.h>
#include <absl/time/time.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/typing.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/status.h"
#include "python/native_types.h"

namespace a11::python {

namespace py = pybind11;

/**
 * @brief Whether Python has begun finalization or is uninitialized.
 *
 * Py_IsInitialized() remains true during finalization. Destructors must leave
 * Python references unreleased once finalization begins; acquiring the GIL
 * then can terminate a worker thread. Python 3.12 uses _Py_IsFinalizing();
 * Python 3.13 and later provide Py_IsFinalizing().
 */
inline bool InterpreterIsGoingAway() {
  if (Py_IsInitialized() == 0) {
    return true;
  }
  // `Py_IsFinalizing` became public in 3.13; before that only the underscored
  // `_Py_IsFinalizing` exists.
#if PY_VERSION_HEX >= 0x030D0000
  return Py_IsFinalizing() != 0;
#else
  return _Py_IsFinalizing() != 0;
#endif
}

/**
 * @brief Defers Python reference release until a thread holds the GIL.
 *
 * Destructors queue references without acquiring the GIL. Binding entry
 * points and the atexit handler drain the queue with the GIL held. References
 * still queued at process exit remain unreleased.
 */
class DeferredPythonRefs {
 public:
  /// Release @p object, now if that is provably safe and otherwise later.
  ///
  /// Safe from any thread and takes no Python lock in the deferring case.
  static void Retire(PyObject* absl_nullable object) {
    if (object == nullptr) {
      return;
    }
    // Release immediately when the caller already holds the GIL.
    if (Py_IsInitialized() != 0 && PyGILState_Check() != 0) {
      Py_DECREF(object);
      return;
    }
    absl::MutexLock lock(Mutex());
    Pending().push_back(object);
    Size().store(Pending().size(), std::memory_order_relaxed);
    std::size_t seen = HighWater().load(std::memory_order_relaxed);
    while (Pending().size() > seen &&
           !HighWater().compare_exchange_weak(seen, Pending().size(),
                                              std::memory_order_relaxed)) {}
  }

  /// Release everything queued. **The caller must hold the GIL.**
  static void Drain() {
    // Keep the empty path lock-free; callbacks check it for every message.
    if (Size().load(std::memory_order_relaxed) == 0) {
      return;
    }
    std::vector<PyObject*> objects;
    {
      absl::MutexLock lock(Mutex());
      objects.swap(Pending());
      Size().store(0, std::memory_order_relaxed);
    }
    for (PyObject* object : objects) {
      Py_DECREF(object);
    }
  }

  /// How many references are queued right now. Diagnostics and tests.
  static std::size_t PendingCount() {
    return Size().load(std::memory_order_relaxed);
  }

  /// The highest number of queued references, for diagnostics.
  static std::size_t HighWaterCount() {
    return HighWater().load(std::memory_order_relaxed);
  }

 private:
  static std::atomic<std::size_t>& Size() {
    static absl::NoDestructor<std::atomic<std::size_t>> size{0};
    return *size;
  }

  static std::atomic<std::size_t>& HighWater() {
    static absl::NoDestructor<std::atomic<std::size_t>> high{0};
    return *high;
  }

  // Uses absl::Mutex, not thread::Mutex, because this is reachable from a
  // fiber destructor, and a fiber-aware mutex would want a scheduler that a
  // teardown path may no longer have.
  static absl::Mutex& Mutex() {
    static absl::NoDestructor<absl::Mutex> mutex;
    return *mutex;
  }

  static std::vector<PyObject*>& Pending() {
    static absl::NoDestructor<std::vector<PyObject*>> pending;
    return *pending;
  }
};

/**
 * An `asyncio.Future` resolving to @c T.
 *
 * Bindings return this annotation so generated stubs say
 * @c asyncio.Future[T]. FutureToPythonConverted() builds the Python object.
 */
template <typename T>
class PyFuture : public py::object {
  PYBIND11_OBJECT_DEFAULT(PyFuture, object, PyObject_Type)
  using object::object;
};

/**
 * A @c py::object a signature reports as @c T.
 *
 * Annotates a parameter without pybind conversion. The binding handles
 * coercion for accepted representations of @c T.
 */
template <typename T>
class PyLike : public py::object {
  PYBIND11_OBJECT_DEFAULT(PyLike, object, PyObject_Type)
  using object::object;
};

/**
 * A read-only mapping parameter, which @c pybind11/typing.h has no wrapper
 * for. Annotation-only, like PyLike.
 */
template <typename K, typename V>
class PyMapping : public py::object {
  PYBIND11_OBJECT_DEFAULT(PyMapping, object, PyObject_Type)
  using object::object;
};

/// The HTTP header pairs a handshake carries, as they read back.
using PyHeaderPairs = py::typing::List<py::typing::Tuple<py::str, py::str>>;

/**
 * And as one may be *given*: pairs, or a mapping of name to value.
 *
 * Annotated rather than converted, because HeaderPairsFromPython() does the
 * coercion and needs to see what the caller actually passed.
 */
using PyHeadersLike =
    PyLike<py::typing::Union<PyMapping<py::str, py::str>, PyHeaderPairs>>;

/// Render header pairs as the list of tuples Python reads them back as.
inline PyHeaderPairs HeaderPairsToPython(
    const std::vector<std::pair<std::string, std::string>>& headers) {
  py::list result;
  for (const auto& [name, value] : headers) {
    result.append(py::make_tuple(name, value));
  }
  return result;
}

/**
 * Read header pairs from a mapping or a sequence of pairs.
 *
 * @return The pairs, or `INVALID_ARGUMENT` when the value is not either shape.
 */
inline absl::StatusOr<std::vector<std::pair<std::string, std::string>>>
HeaderPairsFromPython(const py::handle& value, const char* absl_nonnull what) {
  std::vector<std::pair<std::string, std::string>> headers;
  py::object entries =
      PyMapping_Check(value.ptr()) != 0 && py::hasattr(value, "items")
          ? value.attr("items")()
          : py::reinterpret_borrow<py::object>(value);
  for (const py::handle item : entries) {
    auto pair = py::reinterpret_borrow<py::sequence>(item);
    if (pair.size() != 2 || !py::isinstance<py::str>(pair[0]) ||
        !py::isinstance<py::str>(pair[1])) {
      return absl::InvalidArgumentError(std::string(what) +
                                        " must contain pairs of strings");
    }
    headers.emplace_back(pair[0].cast<std::string>(),
                         pair[1].cast<std::string>());
  }
  return headers;
}

/**
 * The `a11.status.StatusCode` enum member a binding hands back.
 *
 * The enum itself is written in Python, so pybind has no type to name; this
 * carries the name into the signature.
 */
class PyStatusCode : public py::object {
  PYBIND11_OBJECT_DEFAULT(PyStatusCode, object, PyObject_Type)
  using object::object;
};

/**
 * Runs a blocking native call with the GIL released, and hands back its result.
 *
 * Synchronous waits release the GIL so Python callbacks can complete them.
 * The GIL is reacquired before converting the result to a Python object.
 */
template <typename Callable>
auto WithoutGil(Callable&& call) -> decltype(call()) {
  const py::gil_scoped_release release;
  return std::forward<Callable>(call)();
}

/**
 * A JSON object as Python holds it: @c dict[str,Any] in a signature.
 *
 * Used for JSON data with string keys and arbitrary values in generated stubs.
 */
using PyJsonObject = py::typing::Dict<py::str, py::object>;

/** A JSON array whose elements are whatever they are: @c list[Any]. */
using PyJsonArray = py::typing::List<py::object>;

/** A JSON array of objects: @c list[dict[str,Any]]. */
using PyJsonObjects = py::typing::List<PyJsonObject>;

/** A mapping of strings to strings: @c dict[str,str]. */
using PyStringMap = py::typing::Dict<py::str, py::str>;

/** A11's header map as Python holds it: @c dict[str,bytes]. */
using PyByteMap = py::typing::Dict<py::str, py::bytes>;

namespace future_internal {

// The Python type a Future<T> resolves to, for the annotation.
template <typename T>
struct Payload {
  using type = T;
};

template <>
struct Payload<a11::Unit> {
  using type = py::none;
};

template <>
struct Payload<absl::Status> {
  using type = NativeStatus;
};

template <typename T>
using PayloadType = typename Payload<T>::type;

}  // namespace future_internal

// Owns the event loop used by a Python override. Python methods may be invoked
// by ordinary libuv/libdatachannel threads or by A11 fibers, so they must be
// marshalled back to their asyncio loop.
class PythonLoop {
 public:
  /**
   * The loop to marshal onto, resolved now.
   *
   * Uses the running loop or the event loop policy's loop.
   */
  static absl::StatusOr<std::shared_ptr<PythonLoop>> Capture();

  /**
   * The loop running on this thread, or null when there is none.
   *
   * Returns null when no loop runs here. Found loops are remembered
   * process-wide for Resolve().
   */
  static std::shared_ptr<PythonLoop> CaptureRunning();

  /** Remember the running loop, if there is one, and nothing else. */
  static void NoteRunningLoop();

  /**
   * The loop to marshal onto, resolved as late as possible.
   *
   * For owners created before an event loop existed. Tries the current loop,
   * the last open loop A11 used, then the policy's existing loop. Reports an
   * error if none is available.
   */
  static absl::StatusOr<std::shared_ptr<PythonLoop>> Resolve();

  PythonLoop(const PythonLoop&) = delete;
  PythonLoop& operator=(const PythonLoop&) = delete;
  ~PythonLoop();

  /** Whether the loop is closed, so nothing posted to it will ever run. */
  [[nodiscard]] bool IsClosed() const;

  class Cancellation;
  absl::StatusOr<std::shared_ptr<Cancellation>> Schedule(
      const py::object& awaitable, const py::object& completion) const;

 private:
  explicit PythonLoop(PyObject* absl_nonnull loop) : loop_(loop) {}

  static std::shared_ptr<PythonLoop> Adopt(py::object loop);
  static void Remember(const std::shared_ptr<PythonLoop>& loop);
  static std::shared_ptr<PythonLoop> Remembered();

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
PyByteMap ByteMapToPython(const data::ByteMap& value);

py::object StatusException(const absl::Status& status);
[[noreturn]] void ThrowStatus(const absl::Status& status);

template <typename T>
T ValueOrThrow(absl::StatusOr<T> value) {
  if (!value.ok()) {
    ThrowStatus(value.status());
  }
  T result = std::move(value).value();
  return result;
}

/// Raises the Python status exception for @p status unless it is OK.
inline void ThrowIfNotOk(const absl::Status& status) {
  if (!status.ok()) {
    ThrowStatus(status);
  }
}

/**
 * @brief Runs a blocking native operation with the GIL released.
 *
 * Convert Python arguments before this call, while holding the GIL. Native
 * waits release it so libuv completions can access Python objects.
 */
template <typename Operation>
void CallWithoutGil(Operation&& operation) {
  absl::Status status;
  {
    py::gil_scoped_release release;
    status = std::forward<Operation>(operation)();
  }
  ThrowIfNotOk(status);
}

/// As CallWithoutGil, for an operation yielding an `absl::StatusOr<T>`; the
/// value is unwrapped, or thrown, with the GIL re-held.
template <typename Operation>
auto ValueWithoutGil(Operation&& operation) {
  auto result = [&] {
    py::gil_scoped_release release;
    return std::forward<Operation>(operation)();
  }();
  return ValueOrThrow(std::move(result));
}

// Keeps Python references safe even when the final C++ owner is released by
// an external transport thread.
class PythonReferences {
 public:
  PythonReferences(py::handle loop, py::handle future, py::handle completion);
  PythonReferences(const PythonReferences&) = delete;
  PythonReferences& operator=(const PythonReferences&) = delete;
  ~PythonReferences();

  [[nodiscard]] py::object loop() const;
  [[nodiscard]] py::object future() const;
  [[nodiscard]] py::object completion() const;
  /**
   * Whether the calling thread is the one the loop runs on.
   *
   * Native work often finishes *on* the loop thread -- a store read or write
   * driven inline from a binding -- and a completion that lands there can
   * resolve its future directly rather than posting to the thread it is already
   * on, which would cost the awaiting coroutine an event-loop turn.
   */
  [[nodiscard]] bool OnLoopThread() const;
  void ClearWithGilHeld();

 private:
  PyObject* absl_nullable loop_ = nullptr;
  PyObject* absl_nullable future_ = nullptr;
  PyObject* absl_nullable completion_ = nullptr;
  unsigned long loop_thread_ = 0;
};

template <typename T, typename Converter>
py::object FutureToPythonConverted(a11::Future<T> future, Converter converter) {
  py::module_ asyncio = py::module_::import("asyncio");
  py::object loop = asyncio.attr("get_running_loop")();
  py::module_ coordination = py::module_::import("a11._asyncio");

  // Hand back a future that is *already* resolved when the native one is.
  // Both calls release the GIL while taking the future's fiber-aware lock.
  if (WithoutGil([&] { return future.IsReady(); })) {
    absl::StatusOr<T> result = WithoutGil([&] { return future.Await(); });
    py::object resolved = coordination.attr("_create_native_future")(
        loop, py::cpp_function([]() {}));
    try {
      if (result.ok()) {
        coordination.attr("_complete_future")(resolved, converter(*result),
                                              py::none());
      } else {
        coordination.attr("_complete_future")(resolved, py::none(),
                                              StatusException(result.status()));
      }
    } catch (const py::error_already_set&) {
      PyErr_Clear();
    }
    return resolved;
  }

  py::object py_future = coordination.attr("_create_native_future")(
      loop,
      // Without the GIL: cancelling reaches into the producing operation and
      // can park in the fiber scheduler, and this runs on the event-loop thread
      // -- a `wait_for` timeout calls it as an ordinary loop callback.
      py::cpp_function([future]() mutable {
        // Nothing to report a failed cancellation to: asyncio has already
        // marked the future cancelled, and a future that resolved first is
        // the ordinary race rather than an error.
        WithoutGil([&] { return future.Cancel(); }).IgnoreError();
      }));
  py::object completion = coordination.attr("_complete_future");
  auto references =
      std::make_shared<PythonReferences>(loop, py_future, completion);

  // Registration may park on the future's fiber-aware lock. Release the GIL
  // only for that handover; constructing the callable retains Python objects.
  auto on_ready = [references, converter = std::move(converter)](
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
      if (references->OnLoopThread()) {
        // Resolve directly: the work finished on the loop's own thread, so an
        // await that follows in the same coroutine step sees a finished future
        // and never yields.
        references->completion()(references->future(), std::move(value),
                                 std::move(exception));
      } else {
        references->loop().attr("call_soon_threadsafe")(
            references->completion(), references->future(), std::move(value),
            std::move(exception));
      }
    } catch (const py::error_already_set&) {
      // The loop may have closed concurrently. There is no live Python waiter
      // to receive another error in that case.
      PyErr_Clear();
    }
    references->ClearWithGilHeld();
  };
  WithoutGil([&] { future.OnReady(std::move(on_ready)); });
  return py_future;
}

// Wraps a converted future so its Python result type is part of the
// signature. @c PyPayload must match what @p converter actually returns.
template <typename PyPayload, typename T, typename Converter>
PyFuture<PyPayload> FutureToPythonAs(a11::Future<T> future,
                                     Converter converter) {
  return PyFuture<PyPayload>(
      FutureToPythonConverted(std::move(future), std::move(converter)));
}

template <typename T>
PyFuture<future_internal::PayloadType<T>> FutureToPython(
    a11::Future<T> future) {
  return FutureToPythonAs<future_internal::PayloadType<T>>(
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
  // The GIL is held here, so this is a safe and frequent place to release
  // whatever native destructors could not.
  DeferredPythonRefs::Drain();
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
  [[nodiscard]] a11::Task Call(Args&&... args) const {
    py::gil_scoped_acquire acquire;
    auto function = py::reinterpret_borrow<py::function>(callable_);
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
    auto packed = value.attr("to_msgpack")().cast<py::bytes>();
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
  if (!packed.ok()) {
    ThrowStatus(packed.status());
  }
  py::object cls = py::module_::import("a11.data.types").attr(class_name);
  return cls.attr("from_msgpack")(py::bytes(*packed)).release();
}

}  // namespace a11::python

PYBIND11_NAMESPACE_BEGIN(PYBIND11_NAMESPACE)
PYBIND11_NAMESPACE_BEGIN(detail)

// Renders PyFuture<T> as ``asyncio.Future[T]`` in generated signatures.
template <typename T>
struct handle_type_name<a11::python::PyFuture<T>> {
  static constexpr auto name =
      const_name("asyncio.Future[") + make_caster<T>::name + const_name("]");
};

template <typename T>
struct handle_type_name<a11::python::PyLike<T>> {
  static constexpr auto name = make_caster<T>::name;
};

template <typename K, typename V>
struct handle_type_name<a11::python::PyMapping<K, V>> {
  static constexpr auto name = const_name("collections.abc.Mapping[") +
                               make_caster<K>::name + const_name(", ") +
                               make_caster<V>::name + const_name("]");
};

template <>
struct handle_type_name<a11::python::PyStatusCode> {
  static constexpr auto name = const_name("a11.status.StatusCode");
};

PYBIND11_NAMESPACE_END(detail)
PYBIND11_NAMESPACE_END(PYBIND11_NAMESPACE)

#endif  // A11_PYTHON_INTEROP_H_
