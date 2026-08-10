// Copyright 2026 The A11 Authors.

#ifndef A11_PYTHON_NATIVE_TYPES_H_
#define A11_PYTHON_NATIVE_TYPES_H_

#include <utility>

#include <absl/status/status.h>
#include <absl/time/time.h>

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

}  // namespace a11::python

#endif  // A11_PYTHON_NATIVE_TYPES_H_
