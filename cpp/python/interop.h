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
#include <pybind11/typing.h>

#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/status.h"
#include "python/native_types.h"

namespace a11::python {

namespace py = pybind11;

/**
 * An `asyncio.Future` resolving to @c T.
 *
 * Bindings that hand a future back to Python return this rather than a bare
 * @c py::object, so the generated stub says @c asyncio.Future[T] instead of
 * @c typing.Any. It carries no behaviour: the object is the one
 * FutureToPythonConverted() built, and @c T only names its result.
 */
template <typename T>
class PyFuture : public py::object {
  PYBIND11_OBJECT_DEFAULT(PyFuture, object, PyObject_Type)
  using object::object;
};

/**
 * A @c py::object a signature reports as @c T.
 *
 * Naming @c T directly would make pybind convert at the boundary; this only
 * annotates, so the binding still receives whatever the caller passed and
 * applies its own coercion. Use it where a parameter accepts several
 * spellings of one logical type.
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
 * A *synchronous* binding that can block must let Python run while it waits.
 * A11's mutexes and events are fibre-aware (@c thread::Mutex, @c
 * thread::Select), and whoever holds one may itself need the GIL -- a Python
 * action handler, a chunk built through the serialisation registry, a future
 * completed onto an asyncio loop. Waiting with the GIL held closes that cycle:
 * nothing Python can run, so the holder never finishes, so the wait never ends.
 * The GIL is taken again before the result is turned into a Python object, so a
 * converting call (@c ValueOrThrow, @c FutureToPython) belongs outside.
 */
template <typename Callable>
auto WithoutGil(Callable&& call) -> decltype(call()) {
  const py::gil_scoped_release release;
  return std::forward<Callable>(call)();
}

/**
 * A JSON object as Python holds it: @c dict[str,Any] in a signature.
 *
 * The payloads A11 hands over as plain data -- a `flow.diagnostics/v1`
 * envelope, an action's headers, a recorded span -- have known keys and values
 * of whatever JSON allows. Returning @c py::dict says only @c dict, which tells
 * a reader of the stub nothing about the keys; this at least says what the keys
 * are and admits that a value is anything.
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
  /**
   * Whether the calling thread is the one the loop runs on.
   *
   * Native work often finishes *on* the loop thread -- a store read or write
   * driven inline from a binding -- and a completion that lands there can
   * resolve its future directly rather than posting to the thread it is already
   * on, which would cost the awaiting coroutine an event-loop turn.
   */
  bool OnLoopThread() const;
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
  //
  // Much native work finishes without ever waiting -- a store read whose
  // fragment is buffered, a writer admitting into free space -- and since
  // `ChunkStoreReader::Next` and `LocalChunkStore::Next` gained their inline
  // fast paths that is the normal case, not the exception. Wiring such a future
  // through OnReady costs two event-loop turns for nothing: one to run the
  // completion callback, one to resume whoever was awaiting. Awaiting a future
  // that is already done costs neither -- `Future.__await__` returns the result
  // without yielding -- so a read that the store could satisfy immediately now
  // costs about 0.2us instead of ~45us.
  //
  // The consequence to be aware of: such an await is no longer a yield point.
  // A loop draining thousands of buffered values in a row will not give the
  // event loop a turn, exactly as `StreamReader.read` does not when its buffer
  // is full. Code that must stay fair should drain in batches
  // (`next_fragments`) and yield between them rather than rely on a read
  // suspending.
  if (future.IsReady()) {
    absl::StatusOr<T> result = future.Await();
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
      // can park in the fibre scheduler, and this runs on the event-loop
      // thread -- a `wait_for` timeout calls it as an ordinary loop callback.
      // Holding the GIL across it is the same deadlock as any other blocking
      // binding: a worker resolving a Python future cannot get the GIL from a
      // loop thread that is parked waiting for that worker.
      py::cpp_function([future]() mutable {
        WithoutGil([&] { return future.Cancel(); });
      }));
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
      if (references->OnLoopThread()) {
        // Resolve directly: the work finished on the loop's own thread, so an
        // await that follows in the same coroutine step sees a finished future
        // and never yields. Posting to `call_soon_threadsafe` here would cost
        // that awaiter a turn to learn what this frame already knows. Setting
        // a result only schedules the future's callbacks, so this cannot
        // reenter native code.
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
  });
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
