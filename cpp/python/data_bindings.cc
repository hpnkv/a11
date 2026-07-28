// Copyright 2026 The A11 Authors.

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <cmath>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "a11/data/json.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "python/bindings.h"
#include "python/interop.h"
#include "python/native_types.h"

namespace a11::python {
namespace {

class ByteMapView {
 public:
  ByteMapView(data::ByteMap* values, py::object owner)
      : values_(values), owner_(std::move(owner)) {}

  [[nodiscard]] data::ByteMap& values() const { return *values_; }

 private:
  data::ByteMap* values_;
  py::object owner_;
};

template <typename T>
class VectorView {
 public:
  VectorView(std::vector<T>* values, py::object owner,
             std::function<void()> validate)
      : values_(values),
        owner_(std::move(owner)),
        validate_(std::move(validate)) {}

  [[nodiscard]] std::vector<T>& values() const { return *values_; }

  template <typename F>
  void Mutate(F&& mutation) {
    std::vector<T> previous = *values_;
    try {
      std::forward<F>(mutation)();
      validate_();
    } catch (...) {
      *values_ = std::move(previous);
      throw;
    }
  }

 private:
  std::vector<T>* values_;
  py::object owner_;
  std::function<void()> validate_;
};

size_t VectorIndex(py::ssize_t index, size_t size, bool allow_end = false) {
  const py::ssize_t signed_size = static_cast<py::ssize_t>(size);
  if (index < 0)
    index += signed_size;
  const py::ssize_t maximum = allow_end ? signed_size : signed_size - 1;
  if (index < 0 || index > maximum)
    throw py::index_error("vector index out of range");
  return static_cast<size_t>(index);
}

struct SliceIndices {
  py::ssize_t start = 0;
  py::ssize_t stop = 0;
  py::ssize_t step = 1;
  py::ssize_t length = 0;
};

SliceIndices ComputeSlice(const py::slice& slice, size_t size) {
  SliceIndices result;
  if (PySlice_GetIndicesEx(slice.ptr(), static_cast<Py_ssize_t>(size),
                           &result.start, &result.stop, &result.step,
                           &result.length) < 0) {
    throw py::error_already_set();
  }
  return result;
}

std::string BytesFromPython(const py::handle& value, std::string_view field) {
  try {
    if (py::isinstance<py::bytes>(value))
      return value.cast<std::string>();
    if (py::isinstance<py::bytearray>(value) ||
        py::isinstance<py::memoryview>(value)) {
      return py::module_::import("builtins")
          .attr("bytes")(value)
          .cast<std::string>();
    }
  } catch (py::error_already_set& error) {
    ThrowStatus(StatusFromPythonException(error));
  } catch (const std::exception& error) {
    ThrowStatus(absl::InvalidArgumentError(error.what()));
  }
  ThrowStatus(
      absl::InvalidArgumentError(std::string(field) + " must be bytes-like"));
}

std::string ChunkDataFromPython(const py::handle& value,
                                std::string_view mimetype) {
  if (py::isinstance<py::str>(value)) {
    if (!mimetype.empty() && !mimetype.starts_with("text/")) {
      ThrowStatus(absl::InvalidArgumentError(
          "data must be bytes when the MIME type is not text"));
    }
    return value.cast<std::string>();
  }
  return BytesFromPython(value, "data");
}

std::string BufferBytes(const py::handle& value) {
  return BytesFromPython(value, "serialized data");
}

data::ByteMap ByteMapValue(const py::handle& value) {
  if (py::isinstance<ByteMapView>(value))
    return value.cast<const ByteMapView&>().values();
  return ValueOrThrow(ByteMapFromPython(value));
}

template <typename T>
std::vector<T> VectorFromPython(const py::handle& value,
                                std::string_view field) {
  if (py::isinstance<py::str>(value) || py::isinstance<py::bytes>(value) ||
      !py::isinstance<py::iterable>(value)) {
    ThrowStatus(absl::InvalidArgumentError(std::string(field) +
                                           " must be an iterable"));
  }
  try {
    std::vector<T> result;
    for (const py::handle item : py::reinterpret_borrow<py::iterable>(value))
      result.push_back(py::cast<T>(item));
    return result;
  } catch (py::error_already_set& error) {
    ThrowStatus(StatusFromPythonException(error));
  } catch (const py::cast_error& error) {
    ThrowStatus(absl::InvalidArgumentError(error.what()));
  } catch (const std::exception& error) {
    ThrowStatus(absl::InvalidArgumentError(error.what()));
  }
}

template <typename T>
py::list VectorToPython(std::vector<T>& values, const py::handle& parent) {
  py::list result;
  for (T& value : values) {
    result.append(
        py::cast(&value, py::return_value_policy::reference_internal, parent));
  }
  return result;
}

std::uint64_t UnsignedValue(const py::handle& value, std::uint64_t maximum,
                            std::string_view field) {
  try {
    if (!py::isinstance<py::int_>(value)) {
      ThrowStatus(absl::InvalidArgumentError(std::string(field) +
                                             " must be an integer"));
    }
    const py::int_ integer = py::reinterpret_borrow<py::int_>(value);
    if (py::cast<bool>(integer.attr("__lt__")(0))) {
      ThrowStatus(
          absl::OutOfRangeError(std::string(field) + " must not be negative"));
    }
    const std::uint64_t result = integer.cast<std::uint64_t>();
    if (result > maximum) {
      ThrowStatus(absl::OutOfRangeError(std::string(field) +
                                        " exceeds its supported range"));
    }
    return result;
  } catch (py::error_already_set& error) {
    ThrowStatus(StatusFromPythonException(error));
  } catch (const std::exception& error) {
    ThrowStatus(absl::OutOfRangeError(error.what()));
  }
}

std::optional<std::uint32_t> OptionalUint32(const py::handle& value,
                                            std::string_view field) {
  if (value.is_none())
    return std::nullopt;
  return static_cast<std::uint32_t>(
      UnsignedValue(value, std::numeric_limits<std::uint32_t>::max(), field));
}

std::optional<std::uint64_t> OptionalLength(const py::handle& value) {
  if (value.is_none())
    return std::nullopt;
  constexpr std::uint64_t kMaximum =
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
  return UnsignedValue(value, kMaximum, "length");
}

std::optional<absl::Time> TimestampFromPython(const py::handle& value) {
  if (value.is_none())
    return std::nullopt;
  if (py::isinstance<NativeTime>(value))
    return value.cast<const NativeTime&>().value();
  try {
    py::object datetime = py::module_::import("datetime").attr("datetime");
    if (!py::isinstance(value, datetime)) {
      ThrowStatus(absl::InvalidArgumentError(
          "timestamp must be a datetime, timing.Time, or None"));
    }
    const double seconds = value.attr("timestamp")().cast<double>();
    if (!std::isfinite(seconds)) {
      ThrowStatus(absl::OutOfRangeError("timestamp must be finite"));
    }
    const double micros = seconds * 1e6;
    if (micros > static_cast<double>(INT64_MAX) ||
        micros < static_cast<double>(INT64_MIN)) {
      ThrowStatus(absl::OutOfRangeError("timestamp is outside int64 range"));
    }
    return absl::FromUnixMicros(static_cast<std::int64_t>(micros));
  } catch (py::error_already_set& error) {
    ThrowStatus(StatusFromPythonException(error));
  } catch (const std::exception& error) {
    ThrowStatus(absl::InvalidArgumentError(error.what()));
  }
}

py::object TimestampToPython(const std::optional<absl::Time>& value) {
  if (!value.has_value())
    return py::none();
  py::module_ datetime = py::module_::import("datetime");
  py::object epoch = datetime.attr("datetime")(
      1970, 1, 1, 0, 0, 0,
      py::arg("tzinfo") = datetime.attr("timezone").attr("utc"));
  return epoch + datetime.attr("timedelta")(py::arg("microseconds") =
                                                absl::ToUnixMicros(*value));
}

template <typename T>
void ValidateOrThrow(const T& value) {
  const absl::Status status = value.Validate();
  if (!status.ok())
    ThrowStatus(status);
}

template <typename T, typename Parent>
VectorView<T> MakeVectorView(std::vector<T>* values, Parent* parent) {
  py::object owner = py::cast(parent, py::return_value_policy::reference);
  return VectorView<T>(values, std::move(owner),
                       [parent] { ValidateOrThrow(*parent); });
}

template <typename T>
void BindVectorView(py::class_<VectorView<T>>& cls) {
  cls.def("__len__",
          [](const VectorView<T>& view) { return view.values().size(); })
      .def("__bool__",
           [](const VectorView<T>& view) { return !view.values().empty(); })
      .def(
          "__iter__",
          [](VectorView<T>& view) {
            return py::make_iterator<
                py::return_value_policy::reference_internal>(
                view.values().begin(), view.values().end());
          },
          py::keep_alive<0, 1>())
      .def(
          "__getitem__",
          [](VectorView<T>& view, py::ssize_t index) -> T& {
            return view.values()[VectorIndex(index, view.values().size())];
          },
          py::return_value_policy::reference_internal)
      .def("__getitem__",
           [](VectorView<T>& view, const py::slice& slice) {
             const SliceIndices indices =
                 ComputeSlice(slice, view.values().size());
             py::list result;
             py::ssize_t index = indices.start;
             for (py::ssize_t count = 0; count < indices.length; ++count) {
               result.append(py::cast(
                   &view.values()[static_cast<size_t>(index)],
                   py::return_value_policy::reference_internal,
                   py::cast(&view, py::return_value_policy::reference)));
               index += indices.step;
             }
             return result;
           })
      .def("__setitem__",
           [](VectorView<T>& view, py::ssize_t index, T value) {
             const size_t converted = VectorIndex(index, view.values().size());
             view.Mutate([&] { view.values()[converted] = std::move(value); });
           })
      .def("__setitem__",
           [](VectorView<T>& view, const py::slice& slice,
              const py::handle& replacements) {
             const SliceIndices indices =
                 ComputeSlice(slice, view.values().size());
             std::vector<T> converted =
                 VectorFromPython<T>(replacements, "slice assignment");
             if (indices.step != 1 &&
                 converted.size() != static_cast<size_t>(indices.length)) {
               throw py::value_error(
                   "attempt to assign sequence of different size to extended "
                   "slice");
             }
             view.Mutate([&] {
               if (indices.step == 1) {
                 auto first = view.values().begin() + indices.start;
                 auto last = first + indices.length;
                 first = view.values().erase(first, last);
                 view.values().insert(
                     first, std::make_move_iterator(converted.begin()),
                     std::make_move_iterator(converted.end()));
                 return;
               }
               py::ssize_t index = indices.start;
               for (T& value : converted) {
                 view.values()[static_cast<size_t>(index)] = std::move(value);
                 index += indices.step;
               }
             });
           })
      .def("__delitem__",
           [](VectorView<T>& view, py::ssize_t index) {
             const size_t converted = VectorIndex(index, view.values().size());
             view.Mutate([&] {
               view.values().erase(view.values().begin() +
                                   static_cast<std::ptrdiff_t>(converted));
             });
           })
      .def("__delitem__",
           [](VectorView<T>& view, const py::slice& slice) {
             const SliceIndices indices =
                 ComputeSlice(slice, view.values().size());
             view.Mutate([&] {
               if (indices.step == 1) {
                 auto first = view.values().begin() + indices.start;
                 view.values().erase(first, first + indices.length);
                 return;
               }
               std::vector<size_t> erased;
               erased.reserve(static_cast<size_t>(indices.length));
               py::ssize_t index = indices.start;
               for (py::ssize_t count = 0; count < indices.length; ++count) {
                 erased.push_back(static_cast<size_t>(index));
                 index += indices.step;
               }
               std::sort(erased.rbegin(), erased.rend());
               for (const size_t item : erased)
                 view.values().erase(view.values().begin() +
                                     static_cast<std::ptrdiff_t>(item));
             });
           })
      .def("insert",
           [](VectorView<T>& view, py::ssize_t index, T value) {
             const py::ssize_t size =
                 static_cast<py::ssize_t>(view.values().size());
             if (index < 0)
               index = std::max<py::ssize_t>(0, index + size);
             if (index > size)
               index = size;
             view.Mutate([&] {
               view.values().insert(view.values().begin() + index,
                                    std::move(value));
             });
           })
      .def("append",
           [](VectorView<T>& view, T value) {
             view.Mutate([&] { view.values().push_back(std::move(value)); });
           })
      .def("extend",
           [](VectorView<T>& view, const py::handle& values) {
             std::vector<T> converted = VectorFromPython<T>(values, "values");
             view.Mutate([&] {
               view.values().insert(view.values().end(),
                                    std::make_move_iterator(converted.begin()),
                                    std::make_move_iterator(converted.end()));
             });
           })
      .def("clear",
           [](VectorView<T>& view) {
             view.Mutate([&] { view.values().clear(); });
           })
      .def(
          "pop",
          [](VectorView<T>& view, py::ssize_t index) {
            const size_t converted = VectorIndex(index, view.values().size());
            T result = view.values()[converted];
            view.Mutate([&] {
              view.values().erase(view.values().begin() +
                                  static_cast<std::ptrdiff_t>(converted));
            });
            return result;
          },
          py::arg("index") = -1)
      .def("remove",
           [](VectorView<T>& view, const T& value) {
             const auto found =
                 std::find(view.values().begin(), view.values().end(), value);
             if (found == view.values().end())
               throw py::value_error("value is not in vector");
             const size_t index =
                 static_cast<size_t>(found - view.values().begin());
             view.Mutate([&] {
               view.values().erase(view.values().begin() +
                                   static_cast<std::ptrdiff_t>(index));
             });
           })
      .def("count",
           [](const VectorView<T>& view, const T& value) {
             return std::count(view.values().begin(), view.values().end(),
                               value);
           })
      .def("index",
           [](const VectorView<T>& view, const T& value) {
             const auto found =
                 std::find(view.values().begin(), view.values().end(), value);
             if (found == view.values().end())
               throw py::value_error("value is not in vector");
             return static_cast<size_t>(found - view.values().begin());
           })
      .def("reverse",
           [](VectorView<T>& view) {
             view.Mutate([&] {
               std::reverse(view.values().begin(), view.values().end());
             });
           })
      .def("copy",
           [](VectorView<T>& view) {
             return VectorToPython(
                 view.values(),
                 py::cast(&view, py::return_value_policy::reference));
           })
      .def("__contains__",
           [](const VectorView<T>& view, const T& value) {
             return std::find(view.values().begin(), view.values().end(),
                              value) != view.values().end();
           })
      .def("__eq__",
           [](VectorView<T>& view, const py::object& other) {
             return VectorToPython(
                        view.values(),
                        py::cast(&view, py::return_value_policy::reference))
                 .equal(other);
           })
      .def("__repr__", [](VectorView<T>& view) {
        return py::repr(VectorToPython(
            view.values(),
            py::cast(&view, py::return_value_policy::reference)));
      });
}

template <typename T>
py::bytes ToMsgpack(const T& value) {
  return py::bytes(ValueOrThrow(value.ToMsgpack()));
}

template <typename T>
T FromMsgpack(const py::handle& value) {
  return ValueOrThrow(T::FromMsgpack(BufferBytes(value)));
}

template <typename T>
void BindValueProtocol(py::class_<T>& cls) {
  cls.def("validate", [](const T& value) { ValidateOrThrow(value); })
      .def("to_msgpack", &ToMsgpack<T>)
      .def_static("from_msgpack", &FromMsgpack<T>, py::arg("data"))
      .def("__copy__", [](const T& value) { return value; })
      .def(
          "__deepcopy__", [](const T& value, const py::dict&) { return value; },
          py::arg("memo"))
      .def(
          "__eq__", [](const T& left, const T& right) { return left == right; },
          py::is_operator());
}

}  // namespace

void BindData(py::module_& module) {
  module.attr("JSON_MIMETYPE") = std::string(data::kJsonMimetype);
  module.attr("MSGPACK_MIMETYPE") = std::string(data::kMsgpackMimetype);
  module.attr("WIRE_MESSAGE_VERSION") = data::WireMessage::kVersion;
  module.def("validate_name_string", [](const std::string& name) {
    const absl::Status status = data::ValidateName(name);
    if (!status.ok())
      ThrowStatus(status);
    return name;
  });

  py::class_<ByteMapView>(module, "_ByteMapView")
      .def("__len__",
           [](const ByteMapView& value) { return value.values().size(); })
      .def("__iter__",
           [](const ByteMapView& value) {
             return ByteMapToPython(value.values()).attr("__iter__")();
           })
      .def("__contains__",
           [](const ByteMapView& value, const std::string& key) {
             return value.values().find(key) != value.values().end();
           })
      .def("__getitem__",
           [](const ByteMapView& value, const std::string& key) {
             const auto found = value.values().find(key);
             if (found == value.values().end())
               throw py::key_error(key);
             return py::bytes(found->second);
           })
      .def("__setitem__",
           [](ByteMapView& value, std::string key, const py::handle& item) {
             const absl::Status validation = data::ValidateName(key);
             if (!validation.ok())
               ThrowStatus(validation);
             value.values().insert_or_assign(
                 std::move(key), BytesFromPython(item, "mapping value"));
           })
      .def("__delitem__",
           [](ByteMapView& value, const std::string& key) {
             if (value.values().erase(key) == 0)
               throw py::key_error(key);
           })
      .def("keys",
           [](const ByteMapView& value) {
             return ByteMapToPython(value.values()).attr("keys")();
           })
      .def("values",
           [](const ByteMapView& value) {
             return ByteMapToPython(value.values()).attr("values")();
           })
      .def("items",
           [](const ByteMapView& value) {
             return ByteMapToPython(value.values()).attr("items")();
           })
      .def(
          "get",
          [](const ByteMapView& value, const std::string& key,
             const py::object& default_value) -> py::object {
            const auto found = value.values().find(key);
            return found == value.values().end()
                       ? default_value
                       : py::object(py::bytes(found->second));
          },
          py::arg("key"), py::arg("default") = py::none())
      .def("update",
           [](ByteMapView& value, const py::handle& updates) {
             data::ByteMap converted = ByteMapValue(updates);
             for (auto& [key, item] : converted)
               value.values().insert_or_assign(std::move(key), std::move(item));
           })
      .def("clear", [](ByteMapView& value) { value.values().clear(); })
      .def("copy",
           [](const ByteMapView& value) {
             return ByteMapToPython(value.values());
           })
      .def("__eq__",
           [](const ByteMapView& value, const py::object& other) {
             return ByteMapToPython(value.values()).equal(other);
           })
      .def("__repr__", [](const ByteMapView& value) {
        return py::repr(ByteMapToPython(value.values()));
      });

  py::class_<VectorView<data::Port>> port_vector_view(module,
                                                      "_PortVectorView");
  py::class_<VectorView<data::NodeFragment>> fragment_vector_view(
      module, "_NodeFragmentVectorView");
  py::class_<VectorView<data::ActionMessage>> action_vector_view(
      module, "_ActionMessageVectorView");

  py::class_<data::ChunkMetadata> metadata(module, "ChunkMetadata",
                                           py::dynamic_attr());
  metadata
      .def(py::init([](std::string mimetype, const py::object& timestamp,
                       const py::object& attributes) {
             data::ChunkMetadata result{
                 .mimetype = std::move(mimetype),
                 .timestamp = TimestampFromPython(timestamp),
                 .attributes = ByteMapValue(attributes)};
             ValidateOrThrow(result);
             return result;
           }),
           py::arg("mimetype"), py::arg("timestamp") = py::none(),
           py::arg("attributes") = py::dict())
      .def_readwrite("mimetype", &data::ChunkMetadata::mimetype)
      .def_property(
          "timestamp",
          [](const data::ChunkMetadata& value) {
            return TimestampToPython(value.timestamp);
          },
          [](data::ChunkMetadata& value, const py::object& timestamp) {
            value.timestamp = TimestampFromPython(timestamp);
          })
      .def_property(
          "attributes",
          [](data::ChunkMetadata& value) {
            return ByteMapView(
                &value.attributes,
                py::cast(&value, py::return_value_policy::reference));
          },
          [](data::ChunkMetadata& value, const py::object& attributes) {
            value.attributes = ByteMapValue(attributes);
            ValidateOrThrow(value);
          })
      .def_property_readonly("approx_bytes", &data::ChunkMetadata::ApproxBytes)
      .def("get_attribute",
           [](const data::ChunkMetadata& value, const std::string& key) {
             return py::bytes(ValueOrThrow(value.GetAttribute(key)));
           })
      .def("set_attribute",
           [](data::ChunkMetadata& value, std::string key,
              const py::handle& bytes) {
             const absl::Status status = value.SetAttribute(
                 std::move(key), BytesFromPython(bytes, "value"));
             if (!status.ok())
               ThrowStatus(status);
           })
      .def("debug_string", &data::ChunkMetadata::DebugString)
      .def("__repr__", &data::ChunkMetadata::DebugString);
  BindValueProtocol(metadata);

  py::class_<data::Chunk> chunk(module, "Chunk", py::dynamic_attr());
  chunk
      .def(py::init([](const py::object& metadata, std::string ref,
                       const py::handle& raw_data) {
             std::optional<data::ChunkMetadata> converted_metadata;
             if (!metadata.is_none())
               converted_metadata = metadata.cast<data::ChunkMetadata>();
             const std::string mimetype = converted_metadata.has_value()
                                              ? converted_metadata->mimetype
                                              : std::string();
             data::Chunk result{
                 .metadata = std::move(converted_metadata),
                 .ref = std::move(ref),
                 .data = ChunkDataFromPython(raw_data, mimetype)};
             ValidateOrThrow(result);
             return result;
           }),
           py::arg("metadata") = py::none(), py::arg("ref") = "",
           py::arg("data") = py::bytes())
      .def_property(
          "metadata",
          [](data::Chunk& value) -> py::object {
            if (!value.metadata.has_value())
              return py::none();
            return py::cast(
                &*value.metadata, py::return_value_policy::reference_internal,
                py::cast(&value, py::return_value_policy::reference));
          },
          [](data::Chunk& value, const py::object& metadata_value) {
            if (metadata_value.is_none()) {
              value.metadata = std::nullopt;
            } else {
              value.metadata = metadata_value.cast<data::ChunkMetadata>();
            }
            ValidateOrThrow(value);
          })
      .def_readwrite("ref", &data::Chunk::ref)
      .def_property(
          "data",
          [](const data::Chunk& value) { return py::bytes(value.data); },
          [](data::Chunk& value, const py::handle& raw_data) {
            value.data = ChunkDataFromPython(raw_data, value.GetMimetype());
            ValidateOrThrow(value);
          })
      .def_property_readonly("approx_bytes", &data::Chunk::ApproxBytes)
      .def("get_mimetype", &data::Chunk::GetMimetype)
      .def("is_empty", &data::Chunk::IsEmpty)
      .def("is_null", &data::Chunk::IsNull)
      .def("debug_string", &data::Chunk::DebugString)
      .def("__repr__", &data::Chunk::DebugString);
  BindValueProtocol(chunk);

  py::class_<data::NodeRef> node_ref(module, "NodeRef", py::dynamic_attr());
  node_ref
      .def(py::init([](std::string id, const py::handle& offset,
                       const py::handle& length) {
             data::NodeRef result{
                 .id = std::move(id),
                 .offset = static_cast<std::uint32_t>(UnsignedValue(
                     offset, std::numeric_limits<std::uint32_t>::max(),
                     "offset")),
                 .length = OptionalLength(length)};
             ValidateOrThrow(result);
             return result;
           }),
           py::arg("id"), py::arg("offset") = 0, py::arg("length") = py::none())
      .def_readwrite("id", &data::NodeRef::id)
      .def_readwrite("offset", &data::NodeRef::offset)
      .def_readwrite("length", &data::NodeRef::length)
      .def_property_readonly("approx_bytes", &data::NodeRef::ApproxBytes)
      .def("debug_string", &data::NodeRef::DebugString)
      .def("__repr__", &data::NodeRef::DebugString);
  BindValueProtocol(node_ref);

  py::class_<data::NodeFragment> fragment(module, "NodeFragment",
                                          py::dynamic_attr());
  fragment
      .def(py::init([](const py::object& value, std::string id,
                       const py::handle& seq, bool continued) {
             std::variant<data::Chunk, data::NodeRef> converted;
             if (py::isinstance<data::Chunk>(value)) {
               converted = value.cast<data::Chunk>();
             } else if (py::isinstance<data::NodeRef>(value)) {
               converted = value.cast<data::NodeRef>();
             } else {
               ThrowStatus(absl::InvalidArgumentError(
                   "data must be a Chunk or NodeRef"));
             }
             data::NodeFragment result{.id = std::move(id),
                                       .data = std::move(converted),
                                       .seq = OptionalUint32(seq, "seq"),
                                       .continued = continued};
             ValidateOrThrow(result);
             return result;
           }),
           py::arg("data"), py::arg("id") = "", py::arg("seq") = py::none(),
           py::arg("continued") = false)
      .def_readwrite("id", &data::NodeFragment::id)
      .def_property(
          "data",
          [](data::NodeFragment& value) -> py::object {
            if (auto* inner = std::get_if<data::Chunk>(&value.data)) {
              return py::cast(
                  inner, py::return_value_policy::reference_internal,
                  py::cast(&value, py::return_value_policy::reference));
            }
            return py::cast(
                &std::get<data::NodeRef>(value.data),
                py::return_value_policy::reference_internal,
                py::cast(&value, py::return_value_policy::reference));
          },
          [](data::NodeFragment& value, const py::object& inner) {
            if (py::isinstance<data::Chunk>(inner)) {
              value.data = inner.cast<data::Chunk>();
            } else if (py::isinstance<data::NodeRef>(inner)) {
              value.data = inner.cast<data::NodeRef>();
            } else {
              ThrowStatus(absl::InvalidArgumentError(
                  "data must be a Chunk or NodeRef"));
            }
            ValidateOrThrow(value);
          })
      .def_readwrite("seq", &data::NodeFragment::seq)
      .def_readwrite("continued", &data::NodeFragment::continued)
      .def_property_readonly("approx_bytes", &data::NodeFragment::ApproxBytes)
      .def(
          "get_chunk",
          [](data::NodeFragment& value) -> data::Chunk& {
            return *ValueOrThrow(value.GetChunk());
          },
          py::return_value_policy::reference_internal)
      .def(
          "get_node_ref",
          [](data::NodeFragment& value) -> data::NodeRef& {
            return *ValueOrThrow(value.GetNodeRef());
          },
          py::return_value_policy::reference_internal)
      .def("debug_string", &data::NodeFragment::DebugString)
      .def("__repr__", &data::NodeFragment::DebugString);
  BindValueProtocol(fragment);

  py::class_<data::Port> port(module, "Port", py::dynamic_attr());
  port.def(py::init([](std::string name, std::string id) {
             data::Port result{.name = std::move(name), .id = std::move(id)};
             ValidateOrThrow(result);
             return result;
           }),
           py::arg("name") = "", py::arg("id") = "")
      .def_readwrite("name", &data::Port::name)
      .def_readwrite("id", &data::Port::id)
      .def_property_readonly("approx_bytes", &data::Port::ApproxBytes)
      .def("debug_string", &data::Port::DebugString)
      .def("__repr__", &data::Port::DebugString);
  BindValueProtocol(port);

  py::class_<data::ActionMessage> action(module, "ActionMessage",
                                         py::dynamic_attr());
  action
      .def(py::init([](std::string id, std::string name,
                       const py::object& inputs, const py::object& outputs,
                       const py::object& headers) {
             data::ActionMessage result{
                 .id = std::move(id),
                 .name = std::move(name),
                 .inputs = VectorFromPython<data::Port>(inputs, "inputs"),
                 .outputs = VectorFromPython<data::Port>(outputs, "outputs"),
                 .headers = ByteMapValue(headers)};
             ValidateOrThrow(result);
             return result;
           }),
           py::arg("id") = "", py::arg("name") = "",
           py::arg("inputs") = py::list(), py::arg("outputs") = py::list(),
           py::arg("headers") = py::dict())
      .def_readwrite("id", &data::ActionMessage::id)
      .def_readwrite("name", &data::ActionMessage::name)
      .def_property(
          "inputs",
          [](data::ActionMessage& value) {
            return MakeVectorView(&value.inputs, &value);
          },
          [](data::ActionMessage& value, const py::object& inputs) {
            value.inputs = VectorFromPython<data::Port>(inputs, "inputs");
            ValidateOrThrow(value);
          })
      .def_property(
          "outputs",
          [](data::ActionMessage& value) {
            return MakeVectorView(&value.outputs, &value);
          },
          [](data::ActionMessage& value, const py::object& outputs) {
            value.outputs = VectorFromPython<data::Port>(outputs, "outputs");
            ValidateOrThrow(value);
          })
      .def_property(
          "headers",
          [](data::ActionMessage& value) {
            return ByteMapView(
                &value.headers,
                py::cast(&value, py::return_value_policy::reference));
          },
          [](data::ActionMessage& value, const py::object& headers) {
            value.headers = ByteMapValue(headers);
            ValidateOrThrow(value);
          })
      .def_property_readonly("approx_bytes", &data::ActionMessage::ApproxBytes)
      .def("debug_string", &data::ActionMessage::DebugString)
      .def("__repr__", &data::ActionMessage::DebugString);
  BindValueProtocol(action);

  py::class_<data::WireMessage> wire(module, "WireMessage", py::dynamic_attr());
  wire.def(py::init([](const py::object& node_fragments,
                       const py::object& actions, const py::object& headers) {
             data::WireMessage result{
                 .node_fragments = VectorFromPython<data::NodeFragment>(
                     node_fragments, "node_fragments"),
                 .actions =
                     VectorFromPython<data::ActionMessage>(actions, "actions"),
                 .headers = ByteMapValue(headers)};
             ValidateOrThrow(result);
             return result;
           }),
           py::arg("node_fragments") = py::list(),
           py::arg("actions") = py::list(), py::arg("headers") = py::dict())
      .def_property(
          "node_fragments",
          [](data::WireMessage& value) {
            return MakeVectorView(&value.node_fragments, &value);
          },
          [](data::WireMessage& value, const py::object& fragments) {
            value.node_fragments = VectorFromPython<data::NodeFragment>(
                fragments, "node_fragments");
            ValidateOrThrow(value);
          })
      .def_property(
          "actions",
          [](data::WireMessage& value) {
            return MakeVectorView(&value.actions, &value);
          },
          [](data::WireMessage& value, const py::object& actions) {
            value.actions =
                VectorFromPython<data::ActionMessage>(actions, "actions");
            ValidateOrThrow(value);
          })
      .def_property(
          "headers",
          [](data::WireMessage& value) {
            return ByteMapView(
                &value.headers,
                py::cast(&value, py::return_value_policy::reference));
          },
          [](data::WireMessage& value, const py::object& headers) {
            value.headers = ByteMapValue(headers);
            ValidateOrThrow(value);
          })
      .def_property_readonly("approx_bytes", &data::WireMessage::ApproxBytes)
      .def("debug_string", &data::WireMessage::DebugString)
      .def("to_json",
           [](const data::WireMessage& value) {
             return ValueOrThrow(data::WireMessageToJson(value));
           })
      .def_static("from_json",
                  [](const std::string& value) {
                    return ValueOrThrow(data::WireMessageFromJson(value));
                  })
      .def("__repr__", &data::WireMessage::DebugString);
  BindValueProtocol(wire);
  wire.attr("VERSION") = data::WireMessage::kVersion;

  BindVectorView(port_vector_view);
  BindVectorView(fragment_vector_view);
  BindVectorView(action_vector_view);

  module.def("is_half_close_message", &data::IsHalfCloseMessage);
  module.def(
      "make_half_close_message",
      [](const py::object& trailers) {
        return data::MakeHalfCloseMessage(ByteMapValue(trailers));
      },
      py::arg("trailers") = py::dict());
}

}  // namespace a11::python
