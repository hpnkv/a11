// Copyright 2026 The A11 Authors.

#include <climits>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <cmath>
#include <nlohmann/json.hpp>
#include <pybind11/operators.h>
#include <pybind11/pybind11.h>

#include "a11/status.h"
#include "a11/time.h"
#include "python/bindings.h"
#include "python/interop.h"
#include "python/native_types.h"

namespace a11::python {
namespace {

absl::StatusCode CanonicalStatusCode(int value) {
  if (value < static_cast<int>(absl::StatusCode::kOk) ||
      value > static_cast<int>(absl::StatusCode::kUnauthenticated)) {
    ThrowStatus(absl::InvalidArgumentError("status code is not canonical"));
  }
  return static_cast<absl::StatusCode>(value);
}

nlohmann::json JsonFromPython(const py::handle& value) {
  try {
    const std::string encoded =
        py::module_::import("json").attr("dumps")(value).cast<std::string>();
    return nlohmann::json::parse(encoded);
  } catch (py::error_already_set& error) {
    ThrowStatus(StatusFromPythonException(error));
  } catch (const std::exception& error) {
    ThrowStatus(absl::InvalidArgumentError(error.what()));
  }
}

py::object JsonToPython(const nlohmann::json& value) {
  return py::module_::import("json").attr("loads")(value.dump());
}

absl::Status MakeNativeStatus(int code, std::string message,
                              const py::handle& details) {
  nlohmann::json converted = JsonFromPython(details);
  if (!converted.is_array()) {
    ThrowStatus(absl::InvalidArgumentError("status details must be a list"));
  }
  return MakeStatus(CanonicalStatusCode(code), std::move(message),
                    std::move(converted));
}

void SetStatusCode(NativeStatus& status, int code) {
  status.value() = MakeStatus(CanonicalStatusCode(code),
                              std::string(status.value().message()),
                              StatusDetails(status.value()));
}

void SetStatusMessage(NativeStatus& status, std::string message) {
  status.value() = MakeStatus(status.value().code(), std::move(message),
                              StatusDetails(status.value()));
}

void SetStatusDetails(NativeStatus& status, const py::handle& details) {
  status.value() =
      MakeNativeStatus(static_cast<int>(status.value().code()),
                       std::string(status.value().message()), details);
}

NativeDuration ScaledDuration(const std::optional<double>& value,
                              double nanoseconds_per_unit) {
  if (!value.has_value() || *value <= -1e-8)
    return NativeDuration(absl::InfiniteDuration());
  const double nanoseconds = *value * nanoseconds_per_unit;
  if (!std::isfinite(nanoseconds) ||
      nanoseconds > static_cast<double>(INT64_MAX) ||
      nanoseconds < static_cast<double>(INT64_MIN)) {
    ThrowStatus(absl::OutOfRangeError("duration does not fit in nanoseconds"));
  }
  return NativeDuration(
      absl::Nanoseconds(static_cast<std::int64_t>(nanoseconds)));
}

NativeDuration AddDurations(const NativeDuration& left,
                            const NativeDuration& right) {
  const absl::Duration lhs = left.value();
  const absl::Duration rhs = right.value();
  if ((lhs == absl::InfiniteDuration() && rhs == -absl::InfiniteDuration()) ||
      (lhs == -absl::InfiniteDuration() && rhs == absl::InfiniteDuration())) {
    throw std::overflow_error("opposite infinite durations cannot be added");
  }
  return NativeDuration(lhs + rhs);
}

std::int64_t DurationNanosecondsOrThrow(absl::Duration value) {
  return ValueOrThrow(a11::DurationNanoseconds(value));
}

std::int64_t TimeNanosecondsOrThrow(absl::Time value) {
  return ValueOrThrow(a11::TimeNanosecondsSinceEpoch(value));
}

}  // namespace

void BindCore(py::module_& module) {
  py::class_<NativeStatus>(module, "Status", py::dynamic_attr())
      .def(py::init([](int code, std::string message, py::object details) {
             return NativeStatus(
                 MakeNativeStatus(code, std::move(message), details));
           }),
           py::arg("code") = 0, py::arg("message") = "OK",
           py::arg("details") = py::list())
      .def_property(
          "code",
          [](const NativeStatus& status) -> py::object {
            return py::module_::import("a11.status")
                .attr("StatusCode")(static_cast<int>(status.value().code()));
          },
          &SetStatusCode)
      .def_property(
          "message",
          [](const NativeStatus& status) {
            return std::string(status.value().message());
          },
          &SetStatusMessage)
      .def_property(
          "details",
          [](const NativeStatus& status) {
            return JsonToPython(StatusDetails(status.value()));
          },
          &SetStatusDetails)
      .def("is_ok",
           [](const NativeStatus& status) { return status.value().ok(); })
      .def("_as_dict",
           [](const NativeStatus& status) {
             return JsonToPython(ValueOrThrow(StatusToJson(status.value())));
           })
      .def("_copy", [](const NativeStatus& status) { return status; })
      .def(
          "__eq__",
          [](const NativeStatus& left, const NativeStatus& right) {
            return left.value() == right.value() &&
                   StatusDetails(left.value()) == StatusDetails(right.value());
          },
          py::is_operator())
      .def("__str__",
           [](const NativeStatus& status) {
             py::object code = py::module_::import("a11.status")
                                   .attr("StatusCode")(
                                       static_cast<int>(status.value().code()));
             return py::str("{}: {}").attr("format")(
                 code, std::string(status.value().message()));
           })
      .def("__repr__", [](const NativeStatus& status) {
        return "Status(code=" +
               std::to_string(static_cast<int>(status.value().code())) +
               ", message='" + std::string(status.value().message()) + "')";
      });

  py::class_<NativeDuration>(module, "Duration", py::dynamic_attr())
      .def(py::init([](std::int64_t nanoseconds) {
             return NativeDuration(absl::Nanoseconds(nanoseconds));
           }),
           py::arg("nanoseconds"))
      .def_static(
          "nanoseconds",
          [](const std::optional<std::int64_t>& value) {
            if (!value.has_value() || *value < 0)
              return NativeDuration(absl::InfiniteDuration());
            return NativeDuration(absl::Nanoseconds(*value));
          },
          py::arg("value"))
      .def_static(
          "microseconds",
          [](const std::optional<double>& value) {
            return ScaledDuration(value, 1'000.0);
          },
          py::arg("value"))
      .def_static(
          "milliseconds",
          [](const std::optional<double>& value) {
            return ScaledDuration(value, 1e6);
          },
          py::arg("value"))
      .def_static(
          "seconds",
          [](const std::optional<double>& value) {
            return ScaledDuration(value, 1'000'000'000.0);
          },
          py::arg("value"))
      .def_static("_positive_infinity",
                  [] { return NativeDuration(absl::InfiniteDuration()); })
      .def_static("_negative_infinity",
                  [] { return NativeDuration(-absl::InfiniteDuration()); })
      .def("is_infinite",
           [](const NativeDuration& duration) {
             return duration.value() == absl::InfiniteDuration() ||
                    duration.value() == -absl::InfiniteDuration();
           })
      .def_property_readonly(
          "nanoseconds_value",
          [](const NativeDuration& duration) {
            return DurationNanosecondsOrThrow(duration.value());
          })
      .def(
          "float_seconds",
          [](const NativeDuration& duration,
             const py::object& infinity_value) -> py::object {
            if (duration.value() == absl::InfiniteDuration())
              return infinity_value;
            if (duration.value() == -absl::InfiniteDuration())
              return py::float_(0.0);
            if (duration.value() < absl::ZeroDuration()) {
              ThrowStatus(absl::InvalidArgumentError(
                  "float_seconds() is only available for non-negative "
                  "durations"));
            }
            return py::float_(static_cast<double>(
                                  DurationNanosecondsOrThrow(duration.value())) /
                              1'000'000'000.0);
          },
          py::arg("infinity_value") = py::none())
      .def("__neg__",
           [](const NativeDuration& duration) {
             return NativeDuration(-duration.value());
           })
      .def("__add__", &AddDurations, py::is_operator())
      .def(
          "__sub__",
          [](const NativeDuration& left, const NativeDuration& right) {
            return AddDurations(left, NativeDuration(-right.value()));
          },
          py::is_operator())
      .def(
          "__eq__",
          [](const NativeDuration& left, const NativeDuration& right) {
            return left.value() == right.value();
          },
          py::is_operator())
      .def(
          "__lt__",
          [](const NativeDuration& left, const NativeDuration& right) {
            return left.value() < right.value();
          },
          py::is_operator())
      .def(
          "__le__",
          [](const NativeDuration& left, const NativeDuration& right) {
            return left.value() <= right.value();
          },
          py::is_operator())
      .def("__hash__",
           [](const NativeDuration& duration) {
             if (duration.value() == absl::InfiniteDuration())
               return py::hash(py::str("a11.Duration(+inf)"));
             if (duration.value() == -absl::InfiniteDuration())
               return py::hash(py::str("a11.Duration(-inf)"));
             return py::hash(
                 py::int_(DurationNanosecondsOrThrow(duration.value())));
           })
      .def("__repr__", [](const NativeDuration& duration) {
        if (duration.value() == absl::InfiniteDuration())
          return std::string("Duration(+inf)");
        if (duration.value() == -absl::InfiniteDuration())
          return std::string("Duration(-inf)");
        return "Duration(" +
               std::to_string(DurationNanosecondsOrThrow(duration.value())) +
               "ns)";
      });

  py::class_<NativeTime>(module, "Time", py::dynamic_attr())
      .def(py::init([](std::int64_t nanoseconds) {
             return NativeTime(absl::FromUnixNanos(nanoseconds));
           }),
           py::arg("nanoseconds_since_epoch"))
      .def_static("from_nanoseconds_since_epoch",
                  [](std::int64_t nanoseconds) {
                    return NativeTime(absl::FromUnixNanos(nanoseconds));
                  })
      .def_static("_infinite_future",
                  [] { return NativeTime(absl::InfiniteFuture()); })
      .def_static("_infinite_past",
                  [] { return NativeTime(absl::InfinitePast()); })
      .def_static("_now", [] { return NativeTime(a11::Now()); })
      .def_property_readonly("nanoseconds_since_epoch",
                             [](const NativeTime& time) {
                               return TimeNanosecondsOrThrow(time.value());
                             })
      .def(
          "__add__",
          [](const NativeTime& time, const NativeDuration& duration) {
            return NativeTime(time.value() + duration.value());
          },
          py::is_operator())
      .def(
          "__sub__",
          [](const NativeTime& time, const py::object& other) -> py::object {
            if (py::isinstance<NativeDuration>(other)) {
              const auto& duration = other.cast<const NativeDuration&>();
              return py::cast(NativeTime(time.value() - duration.value()));
            }
            if (!py::isinstance<NativeTime>(other))
              return py::reinterpret_borrow<py::object>(Py_NotImplemented);
            const auto& rhs = other.cast<const NativeTime&>();
            if ((time.value() == absl::InfiniteFuture() &&
                 rhs.value() == absl::InfiniteFuture()) ||
                (time.value() == absl::InfinitePast() &&
                 rhs.value() == absl::InfinitePast())) {
              ThrowStatus(absl::InvalidArgumentError(
                  "subtracting equal infinite times is undefined"));
            }
            return py::cast(NativeDuration(time.value() - rhs.value()));
          },
          py::is_operator())
      .def(
          "__eq__",
          [](const NativeTime& left, const NativeTime& right) {
            return left.value() == right.value();
          },
          py::is_operator())
      .def(
          "__lt__",
          [](const NativeTime& left, const NativeTime& right) {
            return left.value() < right.value();
          },
          py::is_operator())
      .def(
          "__le__",
          [](const NativeTime& left, const NativeTime& right) {
            return left.value() <= right.value();
          },
          py::is_operator())
      .def("__hash__",
           [](const NativeTime& time) {
             if (time.value() == absl::InfiniteFuture())
               return py::hash(py::str("a11.Time(+inf)"));
             if (time.value() == absl::InfinitePast())
               return py::hash(py::str("a11.Time(-inf)"));
             return py::hash(py::int_(TimeNanosecondsOrThrow(time.value())));
           })
      .def("__repr__", [](const NativeTime& time) {
        if (time.value() == absl::InfiniteFuture())
          return std::string("Time(+inf)");
        if (time.value() == absl::InfinitePast())
          return std::string("Time(-inf)");
        return "Time(" + std::to_string(TimeNanosecondsOrThrow(time.value())) +
               "ns)";
      });
}

}  // namespace a11::python
