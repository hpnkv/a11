// Copyright 2026 The A11 Authors.

#ifndef A11_PYTHON_NATIVE_TYPES_H_
#define A11_PYTHON_NATIVE_TYPES_H_

#include <utility>

#include <absl/status/status.h>
#include <absl/time/time.h>
#include <pybind11/pybind11.h>

#include "a11/actions/action.h"

namespace a11::python {

// These small wrappers give Python stable, natively-owned value types while
// the C++ API continues to use Abseil directly.
class NativeStatus {
 public:
  NativeStatus() = default;

  explicit NativeStatus(absl::Status value) : value_(std::move(value)) {}

  [[nodiscard]] const absl::Status& value() const { return value_; }

  [[nodiscard]] absl::Status& value() { return value_; }

 private:
  absl::Status value_;
};

class NativeDuration {
 public:
  NativeDuration() = default;

  explicit NativeDuration(absl::Duration value) : value_(value) {}

  [[nodiscard]] absl::Duration value() const { return value_; }

 private:
  absl::Duration value_ = absl::ZeroDuration();
};

class NativeTime {
 public:
  NativeTime() = default;

  explicit NativeTime(absl::Time value) : value_(value) {}

  [[nodiscard]] absl::Time value() const { return value_; }

 private:
  absl::Time value_ = absl::UnixEpoch();
};

// An Action handler implemented in C++, held so Python can pass it back into
// the API. A distinct type rather than a bound `actions::ActionHandler`:
// pybind11's std::function caster would otherwise claim the binding and turn
// the handler into an ordinary Python callable, losing the native target.
class NativeActionHandler {
 public:
  NativeActionHandler() = default;

  explicit NativeActionHandler(actions::ActionHandler value)
      : value_(std::move(value)) {}

  [[nodiscard]] const actions::ActionHandler& value() const { return value_; }

  [[nodiscard]] explicit operator bool() const {
    return static_cast<bool>(value_);
  }

 private:
  actions::ActionHandler value_;
};

// Whatever an action handler may be, on the Python side: a coroutine function, a
// native handler handed back opaquely, or nothing. A `py::object` subclass rather
// than a caster so the generated stub spells the union out (see the
// `handle_type_name` below) instead of saying `typing.Any`.
class PyActionHandler : public pybind11::object {
  PYBIND11_OBJECT_DEFAULT(PyActionHandler, object, PyObject_Type)
  using object::object;
};

// The `(name, schema, handler)` triples a module of C++ Actions hands over, for
// a caller that registers them itself. Written out by name for the same reason
// as PyActionHandler: pybind resolves a bound class in a signature against what
// is registered *when the function is defined*, and these modules are bound
// before `ActionSchema` is, so the real caster would print its C++ name and the
// stub generator would reduce the whole nested generic to `...`.
class PyActionTriples : public pybind11::object {
  PYBIND11_OBJECT_DEFAULT(PyActionTriples, object, PyObject_Type)
  using object::object;
};

}  // namespace a11::python

PYBIND11_NAMESPACE_BEGIN(PYBIND11_NAMESPACE)
PYBIND11_NAMESPACE_BEGIN(detail)

// ActionHandler is spelled out by the stub generator; NativeActionHandler is the
// bound class in this module.
template <>
struct handle_type_name<a11::python::PyActionHandler> {
  static constexpr auto name =
      const_name("ActionHandler | NativeActionHandler | None");
};

template <>
struct handle_type_name<a11::python::PyActionTriples> {
  static constexpr auto name = const_name(
      "list[tuple[str, ActionSchema, ActionHandler | NativeActionHandler |"
      " None]]");
};

PYBIND11_NAMESPACE_END(detail)
PYBIND11_NAMESPACE_END(PYBIND11_NAMESPACE)

#endif  // A11_PYTHON_NATIVE_TYPES_H_
