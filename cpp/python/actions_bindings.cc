// Copyright 2026 The A11 Authors.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <Python.h>
#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/typing.h>
#include <pybind11_abseil/absl_casters.h>
#include <pybind11_abseil/no_throw_status.h>
#include <pybind11_abseil/status_casters.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/future.h"
#include "a11/data/types.h"
#include "a11/net/wire_stream.h"
#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/service/session.h"
#include "python/bindings.h"
#include "python/casters.h"
#include "python/interop.h"
#include "python/native_types.h"

namespace a11::python {

/**
 * What a handler looks like coming back out of an Action or a registry: the
 * Python coroutine function it was registered with, an opaque handle to a C++
 * implementation, or nothing at all.
 */
namespace {

template <typename T>
class SchemaMapView {
 public:
  using Map = absl::flat_hash_map<std::string, T>;

  SchemaMapView(Map* values, py::object owner, std::function<void()> validate)
      : values_(values),
        owner_(std::move(owner)),
        validate_(std::move(validate)) {}

  [[nodiscard]] Map& values() const { return *values_; }

  template <typename F>
  void Mutate(F&& mutation) {
    Map previous = *values_;
    try {
      std::forward<F>(mutation)();
      validate_();
    } catch (...) {
      *values_ = std::move(previous);
      throw;
    }
  }

 private:
  Map* values_;
  py::object owner_;
  std::function<void()> validate_;
};

template <typename T>
py::dict SchemaMapToPython(SchemaMapView<T>& view) {
  py::dict result;
  for (auto& [key, value] : view.values()) {
    result[py::str(key)] = py::cast(value);
  }
  return result;
}

template <typename T>
T SchemaValueFromPython(const py::handle& value) {
  if (py::isinstance<T>(value)) {
    return value.cast<T>();
  }
  if constexpr (std::is_same_v<T, actions::ActionPortSchema>) {
    return py::module_::import("a11.actions.action")
        .attr("ActionPortSchema")
        .attr("model_validate")(value)
        .cast<T>();
  } else if constexpr (std::is_same_v<T, actions::ActionHeaderSchema>) {
    return py::module_::import("a11.actions.action")
        .attr("ActionHeaderSchema")
        .attr("model_validate")(value)
        .cast<T>();
  } else {
    return value.cast<T>();
  }
}

template <typename T>
void BindSchemaMapView(py::class_<SchemaMapView<T>>& cls) {
  cls.def(
         "__len__",
         [](const SchemaMapView<T>& view) { return view.values().size(); },
         "Return the number of entries in the map.")
      .def(
          "__bool__",
          [](const SchemaMapView<T>& view) { return !view.values().empty(); },
          "Return True when the map has at least one entry.")
      .def(
          "__iter__",
          [](SchemaMapView<T>& view) -> py::typing::Iterator<py::str> {
            return SchemaMapToPython(view).attr("__iter__")();
          },
          "Return an iterator over the map's keys.")
      .def(
          "__contains__",
          [](const SchemaMapView<T>& view, const std::string& key) {
            return view.values().find(key) != view.values().end();
          },
          "Return True when the map contains the given key.", py::arg("key"))
      .def(
          "__getitem__",
          [](SchemaMapView<T>& view, const std::string& key) -> T& {
            const auto found = view.values().find(key);
            if (found == view.values().end()) {
              throw py::key_error(key);
            }
            return found->second;
          },
          "Return the value stored under the given key.", py::arg("key"),
          py::return_value_policy::reference_internal)
      .def(
          "__setitem__",
          [](SchemaMapView<T>& view, std::string key, const py::handle& value) {
            T converted = SchemaValueFromPython<T>(value);
            view.Mutate([&] {
              view.values().insert_or_assign(std::move(key),
                                             std::move(converted));
            });
          },
          "Store a value under the given key, re-validating the schema.",
          py::arg("key"), py::arg("value"))
      .def(
          "__delitem__",
          [](SchemaMapView<T>& view, const std::string& key) {
            if (view.values().find(key) == view.values().end()) {
              throw py::key_error(key);
            }
            view.Mutate([&] { view.values().erase(key); });
          },
          "Remove the entry stored under the given key.", py::arg("key"))
      .def(
          "get",
          [](SchemaMapView<T>& view, const std::string& key,
             const py::typing::Optional<T>& default_value)
              -> py::typing::Optional<T> {
            const auto found = view.values().find(key);
            if (found == view.values().end()) {
              return default_value;
            }
            return py::typing::Optional<T>(py::cast(found->second));
          },
          "Return the value for the key, or the default if it is absent.",
          py::arg("key"), py::arg("default") = py::none())
      .def(
          "keys",
          [](SchemaMapView<T>& view) -> py::typing::Iterable<py::str> {
            return SchemaMapToPython(view).attr("keys")();
          },
          "Return a view of the map's keys.")
      .def(
          "values",
          [](SchemaMapView<T>& view) -> py::typing::Iterable<T> {
            return SchemaMapToPython(view).attr("values")();
          },
          "Return a view of the map's values.")
      .def(
          "items",
          [](SchemaMapView<T>& view)
              -> py::typing::Iterable<py::typing::Tuple<py::str, T>> {
            return SchemaMapToPython(view).attr("items")();
          },
          "Return a view of the map's (key, value) pairs.")
      .def(
          "update",
          [](SchemaMapView<T>& view, const py::object& updates) {
            py::dict converted =
                py::module_::import("builtins").attr("dict")(updates);
            typename SchemaMapView<T>::Map values;
            for (const auto& [key, value] : converted) {
              values.insert_or_assign(key.cast<std::string>(),
                                      SchemaValueFromPython<T>(value));
            }
            view.Mutate([&] {
              for (auto& [key, value] : values) {
                view.values().insert_or_assign(std::move(key),
                                               std::move(value));
              }
            });
          },
          "Merge the entries of another mapping into this map.",
          py::arg("updates"))
      .def(
          "clear",
          [](SchemaMapView<T>& view) {
            view.Mutate([&] { view.values().clear(); });
          },
          "Remove all entries from the map.")
      .def(
          "copy",
          [](SchemaMapView<T>& view) -> py::typing::Dict<py::str, T> {
            return SchemaMapToPython(view);
          },
          "Return a plain dict copy of the map's entries.")
      .def(
          "__eq__",
          [](SchemaMapView<T>& view, const py::object& other) {
            return SchemaMapToPython(view).equal(other);
          },
          "Return True when the map equals another mapping.", py::arg("other"))
      .def(
          "__repr__",
          [](SchemaMapView<T>& view) {
            return py::repr(SchemaMapToPython(view));
          },
          "Return the repr of the map's entries.");
}

template <typename T>
T ValidateSchema(T value) {
  const absl::Status status = value.Validate();
  if (!status.ok()) {
    ThrowStatus(status);
  }
  return value;
}

std::vector<std::optional<data::NodeFragment>> ActionAutofillsFromPython(
    const py::object& value) {
  if (value.is_none()) {
    return {};
  }

  if (!py::isinstance<py::iterable>(value) || py::isinstance<py::str>(value) ||
      py::isinstance<py::bytes>(value)) {
    ThrowStatus(
        absl::InvalidArgumentError("autofills must be an iterable or None"));
  }

  std::vector<std::optional<data::NodeFragment>> result;
  for (const py::handle item : py::reinterpret_borrow<py::iterable>(value)) {
    if (item.is_none()) {
      result.push_back(std::nullopt);
      continue;
    }
    if (py::isinstance<data::NodeFragment>(item)) {
      result.push_back(item.cast<data::NodeFragment>());
    } else {
      result.push_back(py::module_::import("a11.data.types")
                           .attr("NodeFragment")
                           .attr("model_validate")(item)
                           .cast<data::NodeFragment>());
    }
  }
  return result;
}

template <typename T>
absl::flat_hash_map<std::string, T> ActionSchemaMapFromPython(
    const py::object& value, const char* class_name) {
  if (!py::hasattr(value, "items")) {
    ThrowStatus(absl::InvalidArgumentError(std::string(class_name) +
                                           " collection must be a mapping"));
  }
  absl::flat_hash_map<std::string, T> result;
  for (const py::handle pair : value.attr("items")()) {
    const py::tuple item = py::reinterpret_borrow<py::tuple>(pair);
    const std::string key = item[0].cast<std::string>();
    const py::handle element = item[1];
    if (py::isinstance<T>(element)) {
      result.insert_or_assign(key, element.cast<T>());
    } else {
      result.insert_or_assign(key, py::module_::import("a11.actions.action")
                                       .attr(class_name)
                                       .attr("model_validate")(element)
                                       .cast<T>());
    }
  }
  return result;
}

template <typename T>
SchemaMapView<T> MakeSchemaMapView(absl::flat_hash_map<std::string, T>* values,
                                   actions::ActionSchema* schema) {
  py::object owner = py::cast(schema, py::return_value_policy::reference);
  return SchemaMapView<T>(values, std::move(owner), [schema] {
    const absl::Status status = schema->Validate();
    if (!status.ok()) {
      ThrowStatus(status);
    }
  });
}

class PythonActionCallback {
 public:
  static absl::StatusOr<std::shared_ptr<PythonActionCallback>> Create(
      const py::object& callable, bool needs_loop) {
    if (callable.is_none()) {
      return std::shared_ptr<PythonActionCallback>();
    }
    if (PyCallable_Check(callable.ptr()) == 0) {
      return absl::InvalidArgumentError("action callback must be callable");
    }
    std::shared_ptr<PythonLoop> loop;
    if (needs_loop) {
      absl::StatusOr<std::shared_ptr<PythonLoop>> captured =
          PythonLoop::Capture();
      if (!captured.ok()) {
        return captured.status();
      }
      loop = std::move(*captured);
    }

    struct MakeSharedEnabler final : PythonActionCallback {
      MakeSharedEnabler(PyObject* callable, std::shared_ptr<PythonLoop> loop)
          : PythonActionCallback(callable, std::move(loop)) {}
    };

    return std::make_shared<MakeSharedEnabler>(callable.inc_ref().ptr(),
                                               std::move(loop));
  }

  ~PythonActionCallback() {
    if (Py_IsInitialized() == 0) {
      return;
    }
    PyGILState_STATE state = PyGILState_Ensure();
    Py_CLEAR(callable_);
    PyGILState_Release(state);
  }

  a11::Task CallAsync(std::shared_ptr<actions::Action> action) const {
    py::gil_scoped_acquire acquire;
    py::function callable = py::reinterpret_borrow<py::function>(callable_);
    return CallPythonAsync<a11::Unit>(loop_, callable, std::move(action));
  }

  absl::Status CallSync(std::shared_ptr<actions::Action> action) const {
    py::gil_scoped_acquire acquire;
    try {
      py::function callable = py::reinterpret_borrow<py::function>(callable_);
      py::object result = callable(std::move(action));
      if (!result.is_none()) {
        return absl::InvalidArgumentError(
            "synchronous action callback must return None");
      }
      return absl::OkStatus();
    } catch (py::error_already_set& error) {
      return StatusFromPythonException(error);
    } catch (const std::exception& error) {
      return absl::UnknownError(error.what());
    } catch (...) {
      return absl::UnknownError("Python action callback raised an exception");
    }
  }

  py::object callable() const {
    py::gil_scoped_acquire acquire;
    return py::reinterpret_borrow<py::object>(callable_);
  }

 private:
  PythonActionCallback(PyObject* callable, std::shared_ptr<PythonLoop> loop)
      : callable_(callable), loop_(std::move(loop)) {}

  PyObject* callable_ = nullptr;
  std::shared_ptr<PythonLoop> loop_;
};

struct AsyncPythonActionHandler {
  std::shared_ptr<PythonActionCallback> owner;

  a11::Task operator()(std::shared_ptr<actions::Action> action) const {
    return owner->CallAsync(std::move(action));
  }
};

struct SyncPythonActionHandler {
  std::shared_ptr<PythonActionCallback> owner;

  absl::Status operator()(std::shared_ptr<actions::Action> action) const {
    return owner->CallSync(std::move(action));
  }
};

absl::StatusOr<actions::ActionHandler> MakeActionHandler(
    const py::object& callable) {
  if (callable.is_none()) {
    return actions::ActionHandler{};
  }
  // A handler that is already native (an SDK Action implemented in C++, handed
  // back by ActionHandlerToPython) passes straight through: wrapping it in a
  // PythonActionCallback would bounce every invocation through the interpreter
  // for no reason, and would need a running loop the native handler does not.
  if (py::isinstance<NativeActionHandler>(callable)) {
    return callable.cast<NativeActionHandler>().value();
  }
  absl::StatusOr<std::shared_ptr<PythonActionCallback>> owner =
      PythonActionCallback::Create(callable, true);
  if (!owner.ok()) {
    return owner.status();
  }
  return actions::ActionHandler(AsyncPythonActionHandler{
      .owner = std::move(*owner),
  });
}

absl::StatusOr<actions::SyncActionHandler> MakeSyncActionHandler(
    const py::object& callable) {
  absl::StatusOr<std::shared_ptr<PythonActionCallback>> owner =
      PythonActionCallback::Create(callable, false);
  if (!owner.ok()) {
    return owner.status();
  }
  if (!*owner) {
    return absl::InvalidArgumentError("handler must be callable");
  }
  return actions::SyncActionHandler(SyncPythonActionHandler{
      .owner = std::move(*owner),
  });
}

PyActionHandler ActionHandlerToPython(const actions::ActionHandler& handler) {
  if (!handler) {
    return py::none();
  }
  const auto* python = handler.target<AsyncPythonActionHandler>();
  if (python == nullptr) {
    // A handler implemented in C++ has no Python callable behind it, so hand
    // back an opaque handle. It is accepted anywhere a handler is taken (see
    // MakeActionHandler), which is what lets a native Action be re-registered
    // or bound from Python.
    return py::cast(NativeActionHandler(handler));
  }
  return python->owner->callable();
}

void ThrowIfNotOk(const absl::Status& status) {
  if (!status.ok()) {
    ThrowStatus(status);
  }
}

std::shared_ptr<actions::Action> ReturnAction(
    const std::shared_ptr<actions::Action>& self, const absl::Status& status) {
  ThrowIfNotOk(status);
  return self;
}

void ReleasePortSchemaTypeInfo(void* type_object) {
  if (type_object == nullptr) {
    return;
  }
  if (Py_IsInitialized() == 0) {
    return;
  }
  const PyGILState_STATE gil = PyGILState_Ensure();
  Py_DECREF(static_cast<PyObject*>(type_object));
  PyGILState_Release(gil);
}

// Takes an *owning* reference to the type object so the schema keeps it alive
// for its whole lifetime; the shared_ptr deleter returns that reference.
std::shared_ptr<void> PortSchemaTypeInfoFromPython(const py::handle& value) {
  if (value.is_none()) {
    return nullptr;
  }
  if (PyType_Check(value.ptr()) == 0) {
    ThrowStatus(absl::InvalidArgumentError(
        "ActionPortSchema typeinfo must be a Python type or None"));
  }
  Py_INCREF(value.ptr());
  return std::shared_ptr<void>(value.ptr(), &ReleasePortSchemaTypeInfo);
}

py::object PortSchemaTypeInfoToPython(const std::shared_ptr<void>& typeinfo) {
  if (typeinfo == nullptr) {
    return py::none();
  }
  return py::reinterpret_borrow<py::object>(
      static_cast<PyObject*>(typeinfo.get()));
}

NativeStatus StatusObject(const absl::Status& status) {
  return NativeStatus(status);
}

}  // namespace

void BindActions(py::module_& module) {
  py::class_<NativeActionHandler>(
      module, "NativeActionHandler",
      "An Action handler implemented in C++, such as one of the audio SDK's. "
      "It is an opaque handle rather than something Python calls: pass it "
      "wherever a handler is accepted -- ActionRegistry.register(), "
      "Action.bind_handler() -- and the native implementation runs directly, "
      "without a round trip through the interpreter.")
      .def("__bool__",
           [](const NativeActionHandler& self) {
             return static_cast<bool>(self);
           })
      .def("__repr__", [](const NativeActionHandler& self) {
        return self ? "NativeActionHandler()" : "NativeActionHandler(<empty>)";
      });

  py::class_<SchemaMapView<actions::ActionPortSchema>> port_schema_map(
      module, "_ActionPortSchemaMapView",
      "Mutable dict-like view over an action schema's port map.");
  py::class_<SchemaMapView<actions::ActionHeaderSchema>> header_schema_map(
      module, "_ActionHeaderSchemaMapView",
      "Mutable dict-like view over an action schema's header map.");
  py::class_<SchemaMapView<std::string>> string_schema_map(
      module, "_StringSchemaMapView",
      "Mutable dict-like view over an action schema's string map.");

  py::class_<actions::ActionPortSchema>(
      module, "ActionPortSchema",
      "Schema describing a single input or output port of an action.")
      .def(py::init([](std::string name, std::string type,
                       std::string description, bool required, bool unary,
                       const py::object& autofills,
                       const py::typing::Optional<py::type>& typeinfo) {
             return ValidateSchema(actions::ActionPortSchema{
                 .name = std::move(name),
                 .type = std::move(type),
                 .description = std::move(description),
                 .required = required,
                 .unary = unary,
                 .autofills = ActionAutofillsFromPython(autofills),
                 .typeinfo = PortSchemaTypeInfoFromPython(typeinfo)});
           }),
           "Create a validated port schema.", py::arg("name"), py::arg("type"),
           py::arg("description") = "", py::arg("required") = false,
           py::arg("unary") = false, py::arg("autofills") = std::nullopt,
           py::arg("typeinfo") = py::none())
      .def_readwrite("name", &actions::ActionPortSchema::name,
                     "The port's name.")
      .def_readwrite("type", &actions::ActionPortSchema::type,
                     "The port's data type name.")
      .def_readwrite("description", &actions::ActionPortSchema::description,
                     "Human-readable description of the port.")
      .def_readwrite("required", &actions::ActionPortSchema::required,
                     "Whether the port must be provided.")
      .def_readwrite("unary", &actions::ActionPortSchema::unary,
                     "Whether the port carries a single value.")
      .def_property(
          "typeinfo",
          [](const actions::ActionPortSchema& schema)
              -> py::typing::Optional<py::type> {
            return PortSchemaTypeInfoToPython(schema.typeinfo);
          },
          [](actions::ActionPortSchema& schema, const py::object& value) {
            schema.typeinfo = PortSchemaTypeInfoFromPython(value);
          },
          "Optional Python type associated with the port, or None.")
      .def_property(
          "autofills",
          [](const actions::ActionPortSchema& schema)
              -> py::typing::Optional<
                  py::typing::List<py::typing::Optional<data::NodeFragment>>> {
            return py::cast(schema.autofills);
          },
          [](actions::ActionPortSchema& schema, const py::object& value) {
            schema.autofills = ActionAutofillsFromPython(value);
            (void)ValidateSchema(schema);
          },
          "Node fragments used to autofill the port, or None entries.")
      .def(
          "validate",
          [](const actions::ActionPortSchema& schema) {
            (void)ValidateSchema(schema);
          },
          "Validate the port schema, raising on error.")
      .def(py::self == py::self,
           "Return True when two port schemas are equal.");

  py::class_<actions::ActionHeaderSchema>(
      module, "ActionHeaderSchema",
      "Schema describing a single header of an action.")
      .def(py::init([](std::string name, std::string description,
                       const py::object& default_value) {
             actions::ActionHeaderSchema result{
                 .name = std::move(name),
                 .description = std::move(description)};
             if (!default_value.is_none()) {
               if (!py::isinstance<py::bytes>(default_value)) {
                 ThrowStatus(absl::InvalidArgumentError(
                     "Action header default must be bytes or None"));
               }
               result.default_value = default_value.cast<std::string>();
             }
             return ValidateSchema(std::move(result));
           }),
           "Create a validated header schema.", py::arg("name"),
           py::arg("description") = "", py::arg("default") = py::none())
      .def_readwrite("name", &actions::ActionHeaderSchema::name,
                     "The header's name.")
      .def_readwrite("description", &actions::ActionHeaderSchema::description,
                     "Human-readable description of the header.")
      .def_property(
          "default",
          [](const actions::ActionHeaderSchema& schema)
              -> py::typing::Optional<py::bytes> {
            if (!schema.default_value.has_value()) {
              return py::none();
            }
            return py::bytes(*schema.default_value);
          },
          [](actions::ActionHeaderSchema& schema, const py::object& value) {
            if (value.is_none()) {
              schema.default_value.reset();
            } else if (py::isinstance<py::bytes>(value)) {
              schema.default_value = value.cast<std::string>();
            } else {
              ThrowStatus(absl::InvalidArgumentError(
                  "Action header default must be bytes or None"));
            }
          },
          "Default header value as bytes, or None when unset.")
      .def(
          "validate",
          [](const actions::ActionHeaderSchema& schema) {
            (void)ValidateSchema(schema);
          },
          "Validate the header schema, raising on error.")
      .def(py::self == py::self,
           "Return True when two header schemas are equal.");

  py::class_<actions::ActionSchema>(
      module, "ActionSchema",
      "Schema describing an action's ports, headers and output mappings.")
      .def(
          py::init([](std::string name, std::string description,
                      const py::object& inputs, const py::object& outputs,
                      const py::typing::Optional<PyMapping<py::str, py::bytes>>&
                          headers,
                      absl::flat_hash_map<std::string, std::string>
                          output_to_json_field) {
            return ValidateSchema(actions::ActionSchema{
                .name = std::move(name),
                .description = std::move(description),
                .inputs = ActionSchemaMapFromPython<actions::ActionPortSchema>(
                    inputs, "ActionPortSchema"),
                .outputs = ActionSchemaMapFromPython<actions::ActionPortSchema>(
                    outputs, "ActionPortSchema"),
                .headers =
                    ActionSchemaMapFromPython<actions::ActionHeaderSchema>(
                        headers, "ActionHeaderSchema"),
                .output_to_json_field = std::move(output_to_json_field)});
          }),
          "Create a validated action schema.", py::arg("name"),
          py::arg("description") = "",
          py::arg("inputs") =
              absl::flat_hash_map<std::string, actions::ActionPortSchema>{},
          py::arg("outputs") =
              absl::flat_hash_map<std::string, actions::ActionPortSchema>{},
          py::arg("headers") =
              absl::flat_hash_map<std::string, actions::ActionHeaderSchema>{},
          py::arg("output_to_json_field") =
              absl::flat_hash_map<std::string, std::string>{})
      .def_readwrite("name", &actions::ActionSchema::name, "The action's name.")
      .def_readwrite("description", &actions::ActionSchema::description,
                     "Human-readable description of the action.")
      .def_property(
          "inputs",
          [](actions::ActionSchema& schema) {
            return MakeSchemaMapView(&schema.inputs, &schema);
          },
          [](actions::ActionSchema& schema, const py::object& value) {
            actions::ActionSchema previous = schema;
            schema.inputs =
                ActionSchemaMapFromPython<actions::ActionPortSchema>(
                    value, "ActionPortSchema");
            const absl::Status status = schema.Validate();
            if (!status.ok()) {
              schema = std::move(previous);
              ThrowStatus(status);
            }
          },
          "Mapping of input port names to their port schemas.")
      .def_property(
          "outputs",
          [](actions::ActionSchema& schema) {
            return MakeSchemaMapView(&schema.outputs, &schema);
          },
          [](actions::ActionSchema& schema, const py::object& value) {
            actions::ActionSchema previous = schema;
            schema.outputs =
                ActionSchemaMapFromPython<actions::ActionPortSchema>(
                    value, "ActionPortSchema");
            const absl::Status status = schema.Validate();
            if (!status.ok()) {
              schema = std::move(previous);
              ThrowStatus(status);
            }
          },
          "Mapping of output port names to their port schemas.")
      .def_property(
          "headers",
          [](actions::ActionSchema& schema) {
            return MakeSchemaMapView(&schema.headers, &schema);
          },
          [](actions::ActionSchema& schema, const py::object& value) {
            actions::ActionSchema previous = schema;
            schema.headers =
                ActionSchemaMapFromPython<actions::ActionHeaderSchema>(
                    value, "ActionHeaderSchema");
            const absl::Status status = schema.Validate();
            if (!status.ok()) {
              schema = std::move(previous);
              ThrowStatus(status);
            }
          },
          "Mapping of header names to their header schemas.")
      .def_property(
          "output_to_json_field",
          [](actions::ActionSchema& schema) {
            return MakeSchemaMapView(&schema.output_to_json_field, &schema);
          },
          [](actions::ActionSchema& schema,
             absl::flat_hash_map<std::string, std::string> value) {
            actions::ActionSchema previous = schema;
            schema.output_to_json_field = std::move(value);
            const absl::Status status = schema.Validate();
            if (!status.ok()) {
              schema = std::move(previous);
              ThrowStatus(status);
            }
          },
          "Mapping of output port names to JSON field names.")
      .def(
          "validate",
          [](const actions::ActionSchema& schema) {
            (void)ValidateSchema(schema);
          },
          "Validate the action schema, raising on error.")
      .def(
          "map_output_to_json",
          [](actions::ActionSchema& schema, std::string output_name,
             std::string field_name) {
            const absl::Status status = schema.MapOutputToJson(
                std::move(output_name), std::move(field_name));
            if (!status.ok()) {
              ThrowStatus(status);
            }
          },
          "Map an output port to a JSON field in the action's response.",
          py::arg("output_name"), py::arg("field_name") = "")
      .def(py::self == py::self,
           "Return True when two action schemas are equal.");
  module.attr("WHOLE_JSON") = std::string(actions::ActionSchema::kWholeJson);

  BindSchemaMapView(port_schema_map);
  BindSchemaMapView(header_schema_map);
  BindSchemaMapView(string_schema_map);

  py::class_<actions::ActionSettings>(
      module, "ActionSettings",
      "Runtime settings controlling an action's stream binding and cleanup.",
      py::dynamic_attr())
      .def(py::init([](std::optional<bool> bind_inputs,
                       std::optional<bool> bind_outputs, bool clear_inputs,
                       bool clear_outputs) {
             return actions::ActionSettings{
                 .bind_streams_on_inputs_by_default = bind_inputs,
                 .bind_streams_on_outputs_by_default = bind_outputs,
                 .clear_inputs_after_run = clear_inputs,
                 .clear_outputs_after_run = clear_outputs};
           }),
           "Create action settings.",
           py::arg("bind_streams_on_inputs_by_default") = std::nullopt,
           py::arg("bind_streams_on_outputs_by_default") = std::nullopt,
           py::arg("clear_inputs_after_run") = false,
           py::arg("clear_outputs_after_run") = false)
      .def_readwrite(
          "bind_streams_on_inputs_by_default",
          &actions::ActionSettings::bind_streams_on_inputs_by_default,
          "Default stream-binding behaviour for input ports, or None.")
      .def_readwrite(
          "bind_streams_on_outputs_by_default",
          &actions::ActionSettings::bind_streams_on_outputs_by_default,
          "Default stream-binding behaviour for output ports, or None.")
      .def_readwrite("clear_inputs_after_run",
                     &actions::ActionSettings::clear_inputs_after_run,
                     "Whether input ports are cleared after the action runs.")
      .def_readwrite("clear_outputs_after_run",
                     &actions::ActionSettings::clear_outputs_after_run,
                     "Whether output ports are cleared after the action runs.")
      .def(py::self == py::self,
           "Return True when two settings objects are equal.");

  py::classh<actions::Action> action(
      module, "Action",
      "A runnable unit of work with typed input/output ports and headers.",
      py::dynamic_attr());
  action
      .def(py::init([](actions::ActionSchema schema, std::string action_id,
                       const PyLike<PyActionHandler>& handler,
                       std::shared_ptr<nodes::NodeMap> node_map,
                       std::shared_ptr<net::WireStream> stream,
                       std::shared_ptr<service::Session> session,
                       std::shared_ptr<actions::ActionRegistry> registry,
                       size_t max_concurrent_nested_actions) {
             actions::ActionHandler native_handler =
                 ValueOrThrow(MakeActionHandler(handler));
             return ValueOrThrow(actions::Action::Create(
                 std::move(schema), std::move(action_id),
                 std::move(native_handler), std::move(node_map),
                 std::move(stream), std::move(session), std::move(registry),
                 max_concurrent_nested_actions));
           }),
           "Create an action from a schema and optional bindings.",
           py::arg("schema"), py::arg("action_id") = "",
           py::arg("handler") = py::none(), py::kw_only(),
           py::arg("node_map") = nullptr, py::arg("stream") = nullptr,
           py::arg("session") = nullptr, py::arg("registry") = nullptr,
           py::arg("max_concurrent_nested_actions") =
               actions::kDefaultMaxConcurrentNestedActions,
           py::keep_alive<1, 6>(), py::keep_alive<1, 7>())
      .def_static(
          "make_node_id",
          [](const std::string& action_id, const std::string& node_name) {
            return ValueOrThrow(
                actions::Action::MakeNodeId(action_id, node_name));
          },
          "Build the node id for a named port of the given action.",
          py::arg("action_id"), py::arg("node_name"))
      .def_property(
          "id", &actions::Action::GetId,
          [](actions::Action& self, std::string action_id) {
            ThrowIfNotOk(self.SetId(std::move(action_id)));
          },
          "The action's id.")
      .def("get_id", &actions::Action::GetId, "Return the action's id.")
      .def(
          "set_id",
          [](const std::shared_ptr<actions::Action>& self,
             std::string action_id) {
            return ReturnAction(self, self->SetId(std::move(action_id)));
          },
          "Set the action's id and return the action.", py::arg("action_id"))
      .def_property(
          "schema", &actions::Action::GetSchema,
          [](actions::Action& self, actions::ActionSchema schema) {
            ThrowIfNotOk(self.SetSchema(std::move(schema)));
          },
          "The action's schema.")
      .def("get_schema", &actions::Action::GetSchema,
           "Return the action's schema.")
      .def(
          "set_schema",
          [](const std::shared_ptr<actions::Action>& self,
             actions::ActionSchema schema) {
            return ReturnAction(self, self->SetSchema(std::move(schema)));
          },
          "Set the action's schema and return the action.", py::arg("schema"))
      .def(
          "bind_handler",
          [](const std::shared_ptr<actions::Action>& self,
             const PyLike<PyActionHandler>& handler) {
            return ReturnAction(
                self,
                self->BindHandler(ValueOrThrow(MakeActionHandler(handler))));
          },
          "Bind the action's handler and return the action.",
          py::arg("handler"))
      .def(
          "get_handler",
          [](const actions::Action& self) {
            return ActionHandlerToPython(self.GetHandler());
          },
          "Return the action's Python handler, or None.")
      .def("has_handler", &actions::Action::HasHandler,
           "Return True when the action has a handler bound.")
      .def_property(
          "settings", &actions::Action::GetSettings,
          [](actions::Action& self, actions::ActionSettings settings) {
            ThrowIfNotOk(self.SetSettings(std::move(settings)));
          },
          "The action's runtime settings.")
      .def(
          "bind_streams_on_inputs_by_default",
          [](const std::shared_ptr<actions::Action>& self, bool bind) {
            return ReturnAction(self, self->BindStreamsOnInputsByDefault(bind));
          },
          "Set default stream binding for inputs and return the action.",
          py::arg("bind"))
      .def(
          "bind_streams_on_outputs_by_default",
          [](const std::shared_ptr<actions::Action>& self, bool bind) {
            return ReturnAction(self,
                                self->BindStreamsOnOutputsByDefault(bind));
          },
          "Set default stream binding for outputs and return the action.",
          py::arg("bind"))
      .def(
          "clear_inputs_after_run",
          [](const std::shared_ptr<actions::Action>& self, bool clear) {
            return ReturnAction(self, self->ClearInputsAfterRun(clear));
          },
          "Set whether inputs are cleared after run and return the action.",
          py::arg("clear") = true)
      .def(
          "clear_outputs_after_run",
          [](const std::shared_ptr<actions::Action>& self, bool clear) {
            return ReturnAction(self, self->ClearOutputsAfterRun(clear));
          },
          "Set whether outputs are cleared after run and return the action.",
          py::arg("clear") = true)
      .def(
          "bind_node_map",
          [](const std::shared_ptr<actions::Action>& self,
             std::shared_ptr<nodes::NodeMap> node_map) {
            return ReturnAction(self, self->BindNodeMap(std::move(node_map)));
          },
          "Bind the action's node map and return the action.",
          py::arg("node_map"))
      .def("get_node_map", &actions::Action::GetNodeMap,
           "Return the action's bound node map.")
      .def(
          "bind_stream",
          [](const std::shared_ptr<actions::Action>& self,
             std::shared_ptr<net::WireStream> stream) {
            return ReturnAction(self, self->BindStream(std::move(stream)));
          },
          "Bind the action's wire stream and return the action.",
          py::arg("stream"), py::keep_alive<1, 2>())
      .def("get_stream", &actions::Action::GetStream,
           "Return the action's bound wire stream.")
      .def(
          "bind_registry",
          [](const std::shared_ptr<actions::Action>& self,
             std::shared_ptr<actions::ActionRegistry> registry) {
            return ReturnAction(self, self->BindRegistry(std::move(registry)));
          },
          "Bind the action's registry and return the action.",
          py::arg("registry"))
      .def("get_registry", &actions::Action::GetRegistry,
           "Return the action's bound registry.")
      .def(
          "bind_session",
          [](const std::shared_ptr<actions::Action>& self,
             std::shared_ptr<service::Session> session) {
            return ReturnAction(self, self->BindSession(std::move(session)));
          },
          "Bind the action's session and return the action.",
          py::arg("session"), py::keep_alive<1, 2>())
      .def("get_session", &actions::Action::GetSession,
           "Return the action's bound session.")
      .def(
          "get_node",
          [](actions::Action& self, std::string node_id) {
            return ValueOrThrow(self.GetNode(std::move(node_id)));
          },
          "Return the async node with the given id.", py::arg("node_id"))
      .def(
          "get_input",
          [](actions::Action& self, std::string name,
             std::optional<bool> bind_stream) {
            return ValueOrThrow(self.GetInput(std::move(name), bind_stream));
          },
          "Return the input port node with the given name.", py::arg("name"),
          py::arg("bind_stream") = std::nullopt)
      .def(
          "get_output",
          [](actions::Action& self, std::string name,
             std::optional<bool> bind_stream) {
            return ValueOrThrow(self.GetOutput(std::move(name), bind_stream));
          },
          "Return the output port node with the given name.", py::arg("name"),
          py::arg("bind_stream") = std::nullopt)
      .def(
          "get_port",
          [](actions::Action& self, std::string name) {
            return ValueOrThrow(self.GetPort(std::move(name)));
          },
          "Return the port node with the given name.", py::arg("name"))
      .def(
          "__getitem__",
          [](actions::Action& self, std::string name) {
            return ValueOrThrow(self.GetPort(std::move(name)));
          },
          "Return the port node with the given name.", py::arg("name"))
      .def("contains_port", &actions::Action::ContainsPort,
           "Return True when the action has a port with the given name.",
           py::arg("name"))
      .def("__contains__", &actions::Action::ContainsPort,
           "Return True when the action has a port with the given name.",
           py::arg("name"))
      .def("get_action_message", &actions::Action::GetActionMessage,
           "Return the action's wire message representation.")
      .def(
          "map_ports_from_message",
          [](const std::shared_ptr<actions::Action>& self,
             const data::ActionMessage& message) {
            return ReturnAction(self, self->MapPortsFromMessage(message));
          },
          "Map the action's ports from a wire message and return the action.",
          py::arg("message"))
      .def_property_readonly(
          "headers",
          [](const actions::Action& self) {
            return ByteMapToPython(self.Headers());
          },
          "The action's headers as a mapping of name to bytes.")
      .def(
          "get_header",
          [](const actions::Action& self,
             const std::string& name) -> py::object {
            absl::StatusOr<std::optional<data::Bytes>> value =
                self.GetHeader(name);
            if (!value.ok()) {
              ThrowStatus(value.status());
            }
            if (!value->has_value()) {
              return py::none();
            }
            return py::bytes(**value);
          },
          "Return the header value as bytes, or None when absent.",
          py::arg("name"))
      .def("has_header", &actions::Action::HasHeader,
           "Return True when the action has a header with the given name.",
           py::arg("name"))
      .def(
          "set_header",
          [](const std::shared_ptr<actions::Action>& self, std::string name,
             const py::object& value) {
            std::string bytes;
            if (py::isinstance<py::bytes>(value)) {
              bytes = value.cast<std::string>();
            } else if (py::isinstance<py::str>(value)) {
              bytes = py::str(value).attr("encode")().cast<std::string>();
            } else {
              ThrowStatus(absl::InvalidArgumentError(
                  "header value must be str or bytes"));
            }
            return ReturnAction(
                self, self->SetHeader(std::move(name), std::move(bytes)));
          },
          "Set a header from a str or bytes value and return the action.",
          py::arg("name"), py::arg("value"))
      .def(
          "remove_header",
          [](actions::Action& self, const std::string& name) {
            ThrowIfNotOk(self.RemoveHeader(name));
          },
          "Remove the header with the given name.", py::arg("name"))
      .def(
          "forward_header",
          [](const actions::Action& self,
             const std::shared_ptr<actions::Action>& target,
             const std::string& name) {
            ThrowIfNotOk(self.ForwardHeader(target, name));
          },
          "Copy a single header from this action to a target action.",
          py::arg("target"), py::arg("name"))
      .def(
          "forward_headers_with_prefix",
          [](const actions::Action& self,
             const std::shared_ptr<actions::Action>& target,
             const std::string& prefix) {
            ThrowIfNotOk(self.ForwardHeadersWithPrefix(target, prefix));
          },
          "Copy all headers with the given prefix to a target action.",
          py::arg("target"),
          py::arg("prefix") = std::string(actions::kActionHeaderPrefix))
      .def(
          "make_nested",
          [](actions::Action& self, const actions::ActionSchema& schema,
             bool propagate_io, bool forward_headers) {
            return ValueOrThrow(
                self.MakeNested(schema, propagate_io, forward_headers));
          },
          "Create a nested child action from a schema.", py::arg("schema"),
          py::arg("propagate_io") = true, py::arg("forward_headers") = true)
      .def(
          "make_nested",
          [](actions::Action& self, const std::string& action_name,
             bool propagate_io, bool forward_headers) {
            return ValueOrThrow(
                self.MakeNested(action_name, propagate_io, forward_headers));
          },
          R"doc(Create a nested child action from a registered action name.

Examples:
    Prepare a registered lookup while preserving the parent context:

    ```python
    lookup = action.make_nested("find_customer")
    ```
)doc",
          py::arg("action_name"), py::arg("propagate_io") = true,
          py::arg("forward_headers") = true)
      .def(
          "run", [](actions::Action& self) { return ValueOrThrow(self.Run()); },
          R"doc(Run the action's handler and return the running action. The Python layer also exposes the native entry point as `run_in_background`.

Examples:
    Start local work after binding its handler:

    ```python
    job = Action(SUMMARISE).bind_handler(summarise).run()
    ```
)doc")
      .def(
          "call",
          [](const std::shared_ptr<actions::Action>& self,
             const py::typing::Optional<PyMapping<py::str, py::bytes>>&
                 headers) {
            absl::StatusOr<data::ByteMap> converted =
                ByteMapFromPython(headers);
            if (!converted.ok()) {
              return FutureToPython(
                  a11::FailedFuture<std::shared_ptr<actions::Action>>(
                      converted.status()));
            }
            return FutureToPython(self->Call(std::move(*converted)));
          },
          R"doc(Dispatch the action remotely and return a future of the action.

Examples:
    Call a child action and consume its result:

    ```python
    lookup = action.make_nested("find_customer")
    await lookup["email"].put_final(request.email)
    await lookup["email"].drain_and_close()
    await lookup.call()
    customer = await lookup["customer"].consume(obj_type=Customer)
    ```
)doc",
          py::arg("wire_headers") = py::none())
      .def(
          "wait_for_dispatch",
          [](const std::shared_ptr<actions::Action>& self,
             const py::typing::Optional<NativeDuration>& timeout) {
            absl::StatusOr<absl::Duration> converted =
                DurationFromPython(timeout);
            if (!converted.ok()) {
              return FutureToPython(
                  a11::FailedFuture<absl::Status>(converted.status()));
            }
            return FutureToPython(self->WaitForDispatch(*converted));
          },
          "Return a future that resolves when the action has been dispatched.",
          py::arg("timeout") = py::none())
      .def(
          "wait",
          [](const std::shared_ptr<actions::Action>& self,
             const py::typing::Optional<NativeDuration>& timeout) {
            absl::StatusOr<absl::Duration> converted =
                DurationFromPython(timeout);
            if (!converted.ok()) {
              return FutureToPython(
                  a11::FailedFuture<std::shared_ptr<actions::Action>>(
                      converted.status()));
            }
            return FutureToPython(self->Wait(*converted));
          },
          "Return a future that resolves when the action completes.",
          py::arg("timeout") = py::none())
      .def(
          "cancel", [](actions::Action& self) { ThrowIfNotOk(self.Cancel()); },
          "Cancel the action.")
      .def(
          "set_on_cancelled",
          [](actions::Action& self, const py::object& callback) {
            actions::SyncActionHandler native =
                ValueOrThrow(MakeSyncActionHandler(callback));
            ThrowIfNotOk(self.SetOnCancelled(std::move(native)));
          },
          "Register a synchronous callback invoked when the action is "
          "cancelled.",
          py::arg("callback"))
      .def_property_readonly(
          "trace_id", &actions::Action::TraceId,
          "The action's trace id as lowercase hex, or empty when untraced.")
      .def_property_readonly(
          "span_id", &actions::Action::SpanId,
          "The action's span id as lowercase hex, or empty when untraced.")
      .def(
          "set_span_attribute",
          [](actions::Action& self, const std::string& key,
             const py::object& value) {
            // bool is a subclass of int in Python, so test it first.
            if (py::isinstance<py::bool_>(value)) {
              self.SetSpanAttribute(key, value.cast<bool>());
            } else if (py::isinstance<py::int_>(value)) {
              self.SetSpanAttribute(key, value.cast<std::int64_t>());
            } else if (py::isinstance<py::float_>(value)) {
              self.SetSpanAttribute(key, value.cast<double>());
            } else {
              self.SetSpanAttribute(key, py::str(value).cast<std::string>());
            }
          },
          "Set an attribute on the action's span; no-op when untraced.",
          py::arg("key"), py::arg("value"))
      .def(
          "set_span_name",
          [](actions::Action& self, const std::string& name) {
            self.SetSpanName(name);
          },
          "Set the name of the action's span.", py::arg("name"))
      .def(
          "set_span_status",
          [](actions::Action& self, std::string_view code,
             const std::string& description) {
            obs::SpanStatus status = obs::SpanStatus::kUnset;
            if (code == "ok") {
              status = obs::SpanStatus::kOk;
            } else if (code == "error") {
              status = obs::SpanStatus::kError;
            } else if (code != "unset") {
              ThrowStatus(absl::InvalidArgumentError(
                  "span status must be 'ok', 'error' or 'unset'"));
            }
            self.SetSpanStatus(status, description);
          },
          "Set the span status explicitly ('ok', 'error' or 'unset').",
          py::arg("code"), py::arg("description") = "")
      .def("is_done", &actions::Action::IsDone,
           "Return True when the action has finished.")
      .def("has_been_run", &actions::Action::HasBeenRun,
           "Return True when the action has been run locally.")
      .def("has_been_called", &actions::Action::HasBeenCalled,
           "Return True when the action has been dispatched remotely.")
      .def("cancelled", &actions::Action::Cancelled,
           "Return True when the action has been cancelled.")
      .def(
          "get_status",
          [](const actions::Action& self) {
            return StatusObject(self.GetStatus());
          },
          "Return the action's completion status.")
      .def(
          "get_dispatch_status",
          [](const actions::Action& self)
              -> py::typing::Optional<NativeStatus> {
            std::optional<absl::Status> status = self.GetDispatchStatus();
            return status.has_value() ? StatusToPython(*status) : py::none();
          },
          "Return the action's dispatch status, or None when not dispatched.");

  py::classh<actions::ActionRegistry>(
      module, "ActionRegistry",
      "Registry mapping action names to their schemas and handlers.",
      py::dynamic_attr())
      .def(py::init<>(), "Create an empty action registry.")
      .def(
          "register",
          [](actions::ActionRegistry& self, std::string name,
             actions::ActionSchema schema,
             const PyLike<PyActionHandler>& handler) {
            ThrowIfNotOk(
                self.Register(std::move(name), std::move(schema),
                              ValueOrThrow(MakeActionHandler(handler))));
          },
          R"doc(Register an action with a schema and optional async handler.

Examples:
    Publish an application handler under its schema name:

    ```python
    registry.register("summarise", SUMMARISE, summarise)
    ```
)doc",
          py::arg("action_name"), py::arg("schema"),
          py::arg("handler") = py::none())
      .def(
          "register_sync",
          [](actions::ActionRegistry& self, std::string name,
             actions::ActionSchema schema,
             const PyLike<PyActionHandler>& handler) {
            ThrowIfNotOk(self.RegisterSync(
                std::move(name), std::move(schema),
                ValueOrThrow(MakeSyncActionHandler(handler))));
          },
          "Register an action with a schema and a synchronous handler.",
          py::arg("action_name"), py::arg("schema"), py::arg("handler"))
      .def(
          "unregister",
          [](actions::ActionRegistry& self, const std::string& name) {
            ThrowIfNotOk(self.Unregister(name));
          },
          "Remove the action with the given name from the registry.",
          py::arg("action_name"))
      .def("is_registered", &actions::ActionRegistry::IsRegistered,
           "Return True when an action with the given name is registered.",
           py::arg("action_name"))
      .def(
          "get_schema",
          [](const actions::ActionRegistry& self, const std::string& name) {
            return ValueOrThrow(self.GetSchema(name));
          },
          "Return the schema registered under the given action name.",
          py::arg("action_name"))
      .def(
          "get_handler",
          [](const actions::ActionRegistry& self, const std::string& name) {
            return ActionHandlerToPython(ValueOrThrow(self.GetHandler(name)));
          },
          "Return the handler registered under the given action name: the "
          "Python callable it was registered with, a NativeActionHandler when "
          "the action is implemented in C++, or None when it has no handler.",
          py::arg("action_name"))
      .def(
          "make_action",
          [](actions::ActionRegistry& self, const std::string& action_name,
             std::string action_id, std::shared_ptr<nodes::NodeMap> node_map,
             std::shared_ptr<net::WireStream> stream,
             std::shared_ptr<service::Session> session) {
            return ValueOrThrow(self.MakeAction(
                action_name, std::move(action_id), std::move(node_map),
                std::move(stream), std::move(session)));
          },
          R"doc(Create an action instance from a registered action name.

Examples:
    Construct and start work without repeating the schema:

    ```python
    job = registry.make_action("summarise")
    job.run()
    ```
)doc",
          py::arg("action_name"), py::arg("action_id") = "",
          py::arg("node_map") = nullptr, py::arg("stream") = nullptr,
          py::arg("session") = nullptr, py::keep_alive<0, 5>(),
          py::keep_alive<0, 6>())
      .def(
          "make_action_message",
          [](actions::ActionRegistry& self, const std::string& action_name,
             std::string action_id) {
            return ValueOrThrow(
                self.MakeActionMessage(action_name, std::move(action_id)));
          },
          "Create a wire action message for a registered action name.",
          py::arg("action_name"), py::arg("action_id") = "")
      .def("list_registered_actions",
           &actions::ActionRegistry::ListRegisteredActions,
           "Return the names of all registered actions.")
      .def("copy", &actions::ActionRegistry::Copy,
           "Return a copy of the registry, optionally clearing autofills.",
           py::arg("clear_autofills") = true);

  module.def(
      "status_to_chunk",
      [](const PyLike<NativeStatus>& status, bool closing) {
        return ValueOrThrow(
            data::MakeStatusChunk(StatusFromPython(status), closing));
      },
      "Encode an absl Status as a data chunk. With closing=True the chunk is a "
      "node closure marker rather than a value: it reports that the producer "
      "drained the node and closed its write half with that status.",
      py::arg("status"), py::arg("closing") = false);
  module.def(
      "status_from_chunk",
      [](const data::Chunk& chunk) -> NativeStatus {
        return NativeStatus(ValueOrThrow(actions::StatusFromChunk(chunk)));
      },
      "Decode an absl Status from a data chunk.", py::arg("chunk"));
  module.def("is_status_chunk", &actions::IsStatusChunk,
             "Return True when the chunk carries an action status.",
             py::arg("chunk"));
  module.def("is_close_status_chunk", &actions::IsCloseStatusChunk,
             "Return True when the chunk is a status chunk marking that a "
             "node's write half was closed, rather than a status value.",
             py::arg("chunk"));
  module.attr("ACTION_STATUS_MIMETYPE") =
      std::string(actions::kActionStatusMimetype);
  module.attr("CLOSE_STATUS_ATTRIBUTE") = std::string(data::kCloseAttribute);
  module.attr("ACTION_STATUS_OUTPUT") =
      std::string(actions::kActionStatusOutput);
  module.attr("ACTION_DISPATCH_STATUS_OUTPUT") =
      std::string(actions::kActionDispatchStatusOutput);
  module.attr("CANCEL_ACTION_NAME") = std::string(actions::kCancelActionName);
  module.attr("CANCEL_ACTION_HEADER") =
      std::string(actions::kCancelActionHeader);
  module.attr("ACTION_HEADER_PREFIX") =
      std::string(actions::kActionHeaderPrefix);
  module.attr("DEFAULT_MAX_CONCURRENT_NESTED_ACTIONS") =
      actions::kDefaultMaxConcurrentNestedActions;
}

}  // namespace a11::python

PYBIND11_NAMESPACE_BEGIN(PYBIND11_NAMESPACE)
PYBIND11_NAMESPACE_BEGIN(detail)

PYBIND11_NAMESPACE_END(detail)
PYBIND11_NAMESPACE_END(PYBIND11_NAMESPACE)
