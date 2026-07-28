// Copyright 2026 The A11 Authors.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
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

namespace a11::python {
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
  for (auto& [key, value] : view.values())
    result[py::str(key)] = py::cast(value);
  return result;
}

template <typename T>
T SchemaValueFromPython(const py::handle& value) {
  if (py::isinstance<T>(value))
    return value.cast<T>();
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
  cls.def("__len__",
          [](const SchemaMapView<T>& view) { return view.values().size(); })
      .def("__bool__",
           [](const SchemaMapView<T>& view) { return !view.values().empty(); })
      .def("__iter__",
           [](SchemaMapView<T>& view) {
             return SchemaMapToPython(view).attr("__iter__")();
           })
      .def("__contains__",
           [](const SchemaMapView<T>& view, const std::string& key) {
             return view.values().find(key) != view.values().end();
           })
      .def(
          "__getitem__",
          [](SchemaMapView<T>& view, const std::string& key) -> T& {
            const auto found = view.values().find(key);
            if (found == view.values().end())
              throw py::key_error(key);
            return found->second;
          },
          py::return_value_policy::reference_internal)
      .def(
          "__setitem__",
          [](SchemaMapView<T>& view, std::string key, const py::handle& value) {
            T converted = SchemaValueFromPython<T>(value);
            view.Mutate([&] {
              view.values().insert_or_assign(std::move(key),
                                             std::move(converted));
            });
          })
      .def("__delitem__",
           [](SchemaMapView<T>& view, const std::string& key) {
             if (view.values().find(key) == view.values().end())
               throw py::key_error(key);
             view.Mutate([&] { view.values().erase(key); });
           })
      .def(
          "get",
          [](SchemaMapView<T>& view, const std::string& key,
             const py::object& default_value) -> py::object {
            const auto found = view.values().find(key);
            return found == view.values().end() ? default_value
                                                : py::cast(found->second);
          },
          py::arg("key"), py::arg("default") = py::none())
      .def("keys",
           [](SchemaMapView<T>& view) {
             return SchemaMapToPython(view).attr("keys")();
           })
      .def("values",
           [](SchemaMapView<T>& view) {
             return SchemaMapToPython(view).attr("values")();
           })
      .def("items",
           [](SchemaMapView<T>& view) {
             return SchemaMapToPython(view).attr("items")();
           })
      .def("update",
           [](SchemaMapView<T>& view, const py::object& updates) {
             py::dict converted =
                 py::module_::import("builtins").attr("dict")(updates);
             typename SchemaMapView<T>::Map values;
             for (const auto& [key, value] : converted)
               values.insert_or_assign(key.cast<std::string>(),
                                       SchemaValueFromPython<T>(value));
             view.Mutate([&] {
               for (auto& [key, value] : values)
                 view.values().insert_or_assign(std::move(key),
                                                std::move(value));
             });
           })
      .def("clear",
           [](SchemaMapView<T>& view) {
             view.Mutate([&] { view.values().clear(); });
           })
      .def("copy",
           [](SchemaMapView<T>& view) { return SchemaMapToPython(view); })
      .def("__eq__",
           [](SchemaMapView<T>& view, const py::object& other) {
             return SchemaMapToPython(view).equal(other);
           })
      .def("__repr__", [](SchemaMapView<T>& view) {
        return py::repr(SchemaMapToPython(view));
      });
}

template <typename T>
T ValidateSchema(T value) {
  const absl::Status status = value.Validate();
  if (!status.ok())
    ThrowStatus(status);
  return value;
}

std::optional<std::vector<data::Chunk>> ActionAutofillsFromPython(
    const py::object& value) {
  if (value.is_none())
    return std::nullopt;
  if (!py::isinstance<py::iterable>(value) || py::isinstance<py::str>(value) ||
      py::isinstance<py::bytes>(value)) {
    ThrowStatus(
        absl::InvalidArgumentError("autofills must be an iterable or None"));
  }
  std::vector<data::Chunk> result;
  for (const py::handle item : py::reinterpret_borrow<py::iterable>(value)) {
    if (py::isinstance<data::Chunk>(item)) {
      result.push_back(item.cast<data::Chunk>());
    } else {
      result.push_back(py::module_::import("a11.data.types")
                           .attr("Chunk")
                           .attr("model_validate")(item)
                           .cast<data::Chunk>());
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
    if (!status.ok())
      ThrowStatus(status);
  });
}

class PythonActionCallback {
 public:
  static absl::StatusOr<std::shared_ptr<PythonActionCallback>> Create(
      const py::object& callable, bool needs_loop) {
    if (callable.is_none())
      return std::shared_ptr<PythonActionCallback>();
    if (PyCallable_Check(callable.ptr()) == 0) {
      return absl::InvalidArgumentError("action callback must be callable");
    }
    std::shared_ptr<PythonLoop> loop;
    if (needs_loop) {
      absl::StatusOr<std::shared_ptr<PythonLoop>> captured =
          PythonLoop::Capture();
      if (!captured.ok())
        return captured.status();
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
    if (Py_IsInitialized() == 0)
      return;
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
  if (callable.is_none())
    return actions::ActionHandler{};
  absl::StatusOr<std::shared_ptr<PythonActionCallback>> owner =
      PythonActionCallback::Create(callable, true);
  if (!owner.ok())
    return owner.status();
  return actions::ActionHandler(AsyncPythonActionHandler{
      .owner = std::move(*owner),
  });
}

absl::StatusOr<actions::SyncActionHandler> MakeSyncActionHandler(
    const py::object& callable) {
  absl::StatusOr<std::shared_ptr<PythonActionCallback>> owner =
      PythonActionCallback::Create(callable, false);
  if (!owner.ok())
    return owner.status();
  if (!*owner) {
    return absl::InvalidArgumentError("handler must be callable");
  }
  return actions::SyncActionHandler(SyncPythonActionHandler{
      .owner = std::move(*owner),
  });
}

py::object ActionHandlerToPython(const actions::ActionHandler& handler) {
  if (!handler)
    return py::none();
  const auto* python = handler.target<AsyncPythonActionHandler>();
  if (python == nullptr) {
    ThrowStatus(absl::UnimplementedError(
        "The Action handler was not created from a Python callable"));
  }
  return python->owner->callable();
}

void ThrowIfNotOk(const absl::Status& status) {
  if (!status.ok())
    ThrowStatus(status);
}

std::shared_ptr<actions::Action> ReturnAction(
    const std::shared_ptr<actions::Action>& self, const absl::Status& status) {
  ThrowIfNotOk(status);
  return self;
}

py::object StatusObject(const absl::Status& status) {
  return StatusToPython(status);
}

}  // namespace

void BindActions(py::module_& module) {
  py::class_<SchemaMapView<actions::ActionPortSchema>> port_schema_map(
      module, "_ActionPortSchemaMapView");
  py::class_<SchemaMapView<actions::ActionHeaderSchema>> header_schema_map(
      module, "_ActionHeaderSchemaMapView");
  py::class_<SchemaMapView<std::string>> string_schema_map(
      module, "_StringSchemaMapView");

  py::class_<actions::ActionPortSchema>(module, "ActionPortSchema")
      .def(py::init([](std::string name, std::string type,
                       std::string description, bool required, bool unary,
                       const py::object& autofills) {
             return ValidateSchema(actions::ActionPortSchema{
                 .name = std::move(name),
                 .type = std::move(type),
                 .description = std::move(description),
                 .required = required,
                 .unary = unary,
                 .autofills = ActionAutofillsFromPython(autofills)});
           }),
           py::arg("name"), py::arg("type"), py::arg("description") = "",
           py::arg("required") = false, py::arg("unary") = false,
           py::arg("autofills") = std::nullopt)
      .def_readwrite("name", &actions::ActionPortSchema::name)
      .def_readwrite("type", &actions::ActionPortSchema::type)
      .def_readwrite("description", &actions::ActionPortSchema::description)
      .def_readwrite("required", &actions::ActionPortSchema::required)
      .def_readwrite("unary", &actions::ActionPortSchema::unary)
      .def_property(
          "autofills",
          [](const actions::ActionPortSchema& schema) -> py::object {
            return schema.autofills.has_value() ? py::cast(*schema.autofills)
                                                : py::none();
          },
          [](actions::ActionPortSchema& schema, const py::object& value) {
            schema.autofills = ActionAutofillsFromPython(value);
            (void)ValidateSchema(schema);
          })
      .def("validate",
           [](const actions::ActionPortSchema& schema) {
             (void)ValidateSchema(schema);
           })
      .def(py::self == py::self);

  py::class_<actions::ActionHeaderSchema>(module, "ActionHeaderSchema")
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
           py::arg("name"), py::arg("description") = "",
           py::arg("default") = py::none())
      .def_readwrite("name", &actions::ActionHeaderSchema::name)
      .def_readwrite("description", &actions::ActionHeaderSchema::description)
      .def_property(
          "default",
          [](const actions::ActionHeaderSchema& schema) -> py::object {
            if (!schema.default_value.has_value())
              return py::none();
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
          })
      .def("validate",
           [](const actions::ActionHeaderSchema& schema) {
             (void)ValidateSchema(schema);
           })
      .def(py::self == py::self);

  py::class_<actions::ActionSchema>(module, "ActionSchema")
      .def(
          py::init([](std::string name, std::string description,
                      const py::object& inputs, const py::object& outputs,
                      const py::object& headers,
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
          py::arg("name"), py::arg("description") = "",
          py::arg("inputs") =
              absl::flat_hash_map<std::string, actions::ActionPortSchema>{},
          py::arg("outputs") =
              absl::flat_hash_map<std::string, actions::ActionPortSchema>{},
          py::arg("headers") =
              absl::flat_hash_map<std::string, actions::ActionHeaderSchema>{},
          py::arg("output_to_json_field") =
              absl::flat_hash_map<std::string, std::string>{})
      .def_readwrite("name", &actions::ActionSchema::name)
      .def_readwrite("description", &actions::ActionSchema::description)
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
          })
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
          })
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
          })
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
          })
      .def("validate",
           [](const actions::ActionSchema& schema) {
             (void)ValidateSchema(schema);
           })
      .def(
          "map_output_to_json",
          [](actions::ActionSchema& schema, std::string output_name,
             std::string field_name) {
            const absl::Status status = schema.MapOutputToJson(
                std::move(output_name), std::move(field_name));
            if (!status.ok())
              ThrowStatus(status);
          },
          py::arg("output_name"), py::arg("field_name") = "")
      .def(py::self == py::self);
  module.attr("WHOLE_JSON") = std::string(actions::ActionSchema::kWholeJson);

  BindSchemaMapView(port_schema_map);
  BindSchemaMapView(header_schema_map);
  BindSchemaMapView(string_schema_map);

  py::class_<actions::ActionSettings>(module, "ActionSettings",
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
           py::arg("bind_streams_on_inputs_by_default") = std::nullopt,
           py::arg("bind_streams_on_outputs_by_default") = std::nullopt,
           py::arg("clear_inputs_after_run") = false,
           py::arg("clear_outputs_after_run") = false)
      .def_readwrite(
          "bind_streams_on_inputs_by_default",
          &actions::ActionSettings::bind_streams_on_inputs_by_default)
      .def_readwrite(
          "bind_streams_on_outputs_by_default",
          &actions::ActionSettings::bind_streams_on_outputs_by_default)
      .def_readwrite("clear_inputs_after_run",
                     &actions::ActionSettings::clear_inputs_after_run)
      .def_readwrite("clear_outputs_after_run",
                     &actions::ActionSettings::clear_outputs_after_run)
      .def(py::self == py::self);

  py::class_<actions::Action, std::shared_ptr<actions::Action>> action(
      module, "Action", py::dynamic_attr());
  action
      .def(py::init([](actions::ActionSchema schema, std::string action_id,
                       const py::object& handler,
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
          })
      .def_property("id", &actions::Action::GetId,
                    [](actions::Action& self, std::string action_id) {
                      ThrowIfNotOk(self.SetId(std::move(action_id)));
                    })
      .def("get_id", &actions::Action::GetId)
      .def("set_id",
           [](const std::shared_ptr<actions::Action>& self,
              std::string action_id) {
             return ReturnAction(self, self->SetId(std::move(action_id)));
           })
      .def_property("schema", &actions::Action::GetSchema,
                    [](actions::Action& self, actions::ActionSchema schema) {
                      ThrowIfNotOk(self.SetSchema(std::move(schema)));
                    })
      .def("get_schema", &actions::Action::GetSchema)
      .def("set_schema",
           [](const std::shared_ptr<actions::Action>& self,
              actions::ActionSchema schema) {
             return ReturnAction(self, self->SetSchema(std::move(schema)));
           })
      .def("bind_handler",
           [](const std::shared_ptr<actions::Action>& self,
              const py::object& handler) {
             return ReturnAction(
                 self,
                 self->BindHandler(ValueOrThrow(MakeActionHandler(handler))));
           })
      .def("get_handler",
           [](const actions::Action& self) {
             return ActionHandlerToPython(self.GetHandler());
           })
      .def("has_handler", &actions::Action::HasHandler)
      .def_property(
          "settings", &actions::Action::GetSettings,
          [](actions::Action& self, actions::ActionSettings settings) {
            ThrowIfNotOk(self.SetSettings(std::move(settings)));
          })
      .def("bind_streams_on_inputs_by_default",
           [](const std::shared_ptr<actions::Action>& self, bool bind) {
             return ReturnAction(self,
                                 self->BindStreamsOnInputsByDefault(bind));
           })
      .def("bind_streams_on_outputs_by_default",
           [](const std::shared_ptr<actions::Action>& self, bool bind) {
             return ReturnAction(self,
                                 self->BindStreamsOnOutputsByDefault(bind));
           })
      .def(
          "clear_inputs_after_run",
          [](const std::shared_ptr<actions::Action>& self, bool clear) {
            return ReturnAction(self, self->ClearInputsAfterRun(clear));
          },
          py::arg("clear") = true)
      .def(
          "clear_outputs_after_run",
          [](const std::shared_ptr<actions::Action>& self, bool clear) {
            return ReturnAction(self, self->ClearOutputsAfterRun(clear));
          },
          py::arg("clear") = true)
      .def("bind_node_map",
           [](const std::shared_ptr<actions::Action>& self,
              std::shared_ptr<nodes::NodeMap> node_map) {
             return ReturnAction(self, self->BindNodeMap(std::move(node_map)));
           })
      .def("get_node_map", &actions::Action::GetNodeMap)
      .def(
          "bind_stream",
          [](const std::shared_ptr<actions::Action>& self,
             std::shared_ptr<net::WireStream> stream) {
            return ReturnAction(self, self->BindStream(std::move(stream)));
          },
          py::keep_alive<1, 2>())
      .def("get_stream", &actions::Action::GetStream)
      .def("bind_registry",
           [](const std::shared_ptr<actions::Action>& self,
              std::shared_ptr<actions::ActionRegistry> registry) {
             return ReturnAction(self, self->BindRegistry(std::move(registry)));
           })
      .def("get_registry", &actions::Action::GetRegistry)
      .def(
          "bind_session",
          [](const std::shared_ptr<actions::Action>& self,
             std::shared_ptr<service::Session> session) {
            return ReturnAction(self, self->BindSession(std::move(session)));
          },
          py::keep_alive<1, 2>())
      .def("get_session", &actions::Action::GetSession)
      .def("get_node",
           [](actions::Action& self, std::string node_id) {
             return ValueOrThrow(self.GetNode(std::move(node_id)));
           })
      .def(
          "get_input",
          [](actions::Action& self, std::string name,
             std::optional<bool> bind_stream) {
            return ValueOrThrow(self.GetInput(std::move(name), bind_stream));
          },
          py::arg("name"), py::arg("bind_stream") = std::nullopt)
      .def(
          "get_output",
          [](actions::Action& self, std::string name,
             std::optional<bool> bind_stream) {
            return ValueOrThrow(self.GetOutput(std::move(name), bind_stream));
          },
          py::arg("name"), py::arg("bind_stream") = std::nullopt)
      .def("get_port",
           [](actions::Action& self, std::string name) {
             return ValueOrThrow(self.GetPort(std::move(name)));
           })
      .def("__getitem__",
           [](actions::Action& self, std::string name) {
             return ValueOrThrow(self.GetPort(std::move(name)));
           })
      .def("contains_port", &actions::Action::ContainsPort)
      .def("__contains__", &actions::Action::ContainsPort)
      .def("get_action_message", &actions::Action::GetActionMessage)
      .def("map_ports_from_message",
           [](const std::shared_ptr<actions::Action>& self,
              const data::ActionMessage& message) {
             return ReturnAction(self, self->MapPortsFromMessage(message));
           })
      .def_property_readonly("headers",
                             [](const actions::Action& self) {
                               return ByteMapToPython(self.Headers());
                             })
      .def("get_header",
           [](const actions::Action& self,
              const std::string& name) -> py::object {
             absl::StatusOr<std::optional<data::Bytes>> value =
                 self.GetHeader(name);
             if (!value.ok())
               ThrowStatus(value.status());
             if (!value->has_value())
               return py::none();
             return py::bytes(**value);
           })
      .def("has_header", &actions::Action::HasHeader)
      .def("set_header",
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
           })
      .def("remove_header",
           [](actions::Action& self, const std::string& name) {
             ThrowIfNotOk(self.RemoveHeader(name));
           })
      .def("forward_header",
           [](const actions::Action& self,
              const std::shared_ptr<actions::Action>& target,
              const std::string& name) {
             ThrowIfNotOk(self.ForwardHeader(target, name));
           })
      .def(
          "forward_headers_with_prefix",
          [](const actions::Action& self,
             const std::shared_ptr<actions::Action>& target,
             const std::string& prefix) {
            ThrowIfNotOk(self.ForwardHeadersWithPrefix(target, prefix));
          },
          py::arg("target"),
          py::arg("prefix") = std::string(actions::kActionHeaderPrefix))
      .def(
          "make_nested",
          [](actions::Action& self, const actions::ActionSchema& schema,
             bool propagate_io, bool forward_headers) {
            return ValueOrThrow(
                self.MakeNested(schema, propagate_io, forward_headers));
          },
          py::arg("schema"), py::arg("propagate_io") = true,
          py::arg("forward_headers") = true)
      .def(
          "make_nested",
          [](actions::Action& self, const std::string& action_name,
             bool propagate_io, bool forward_headers) {
            return ValueOrThrow(
                self.MakeNested(action_name, propagate_io, forward_headers));
          },
          py::arg("action_name"), py::arg("propagate_io") = true,
          py::arg("forward_headers") = true)
      .def("run",
           [](actions::Action& self) { return ValueOrThrow(self.Run()); })
      .def(
          "call",
          [](const std::shared_ptr<actions::Action>& self,
             const py::object& headers) {
            absl::StatusOr<data::ByteMap> converted =
                ByteMapFromPython(headers);
            if (!converted.ok()) {
              return FutureToPython(
                  a11::FailedFuture<std::shared_ptr<actions::Action>>(
                      converted.status()));
            }
            return FutureToPython(self->Call(std::move(*converted)));
          },
          py::arg("wire_headers") = py::none())
      .def(
          "wait_for_dispatch",
          [](const std::shared_ptr<actions::Action>& self,
             const py::object& timeout) {
            absl::StatusOr<absl::Duration> converted =
                DurationFromPython(timeout);
            if (!converted.ok()) {
              return FutureToPython(
                  a11::FailedFuture<absl::Status>(converted.status()));
            }
            return FutureToPython(self->WaitForDispatch(*converted));
          },
          py::arg("timeout") = py::none())
      .def(
          "wait",
          [](const std::shared_ptr<actions::Action>& self,
             const py::object& timeout) {
            absl::StatusOr<absl::Duration> converted =
                DurationFromPython(timeout);
            if (!converted.ok()) {
              return FutureToPython(
                  a11::FailedFuture<std::shared_ptr<actions::Action>>(
                      converted.status()));
            }
            return FutureToPython(self->Wait(*converted));
          },
          py::arg("timeout") = py::none())
      .def("cancel", [](actions::Action& self) { ThrowIfNotOk(self.Cancel()); })
      .def("set_on_cancelled",
           [](actions::Action& self, const py::object& callback) {
             actions::SyncActionHandler native =
                 ValueOrThrow(MakeSyncActionHandler(callback));
             ThrowIfNotOk(self.SetOnCancelled(std::move(native)));
           })
      .def("is_done", &actions::Action::IsDone)
      .def("has_been_run", &actions::Action::HasBeenRun)
      .def("has_been_called", &actions::Action::HasBeenCalled)
      .def("cancelled", &actions::Action::Cancelled)
      .def("get_status",
           [](const actions::Action& self) {
             return StatusObject(self.GetStatus());
           })
      .def("get_dispatch_status",
           [](const actions::Action& self) -> py::object {
             std::optional<absl::Status> status = self.GetDispatchStatus();
             return status.has_value() ? StatusToPython(*status) : py::none();
           });

  py::class_<actions::ActionRegistry, std::shared_ptr<actions::ActionRegistry>>(
      module, "ActionRegistry", py::dynamic_attr())
      .def(py::init<>())
      .def(
          "register",
          [](actions::ActionRegistry& self, std::string name,
             actions::ActionSchema schema, const py::object& handler) {
            ThrowIfNotOk(
                self.Register(std::move(name), std::move(schema),
                              ValueOrThrow(MakeActionHandler(handler))));
          },
          py::arg("action_name"), py::arg("schema"),
          py::arg("handler") = py::none())
      .def(
          "register_sync",
          [](actions::ActionRegistry& self, std::string name,
             actions::ActionSchema schema, const py::object& handler) {
            ThrowIfNotOk(self.RegisterSync(
                std::move(name), std::move(schema),
                ValueOrThrow(MakeSyncActionHandler(handler))));
          },
          py::arg("action_name"), py::arg("schema"), py::arg("handler"))
      .def("unregister",
           [](actions::ActionRegistry& self, const std::string& name) {
             ThrowIfNotOk(self.Unregister(name));
           })
      .def("is_registered", &actions::ActionRegistry::IsRegistered)
      .def("get_schema",
           [](const actions::ActionRegistry& self, const std::string& name) {
             return ValueOrThrow(self.GetSchema(name));
           })
      .def("get_handler",
           [](const actions::ActionRegistry& self, const std::string& name) {
             return ActionHandlerToPython(ValueOrThrow(self.GetHandler(name)));
           })
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
          py::arg("action_name"), py::arg("action_id") = "")
      .def("list_registered_actions",
           &actions::ActionRegistry::ListRegisteredActions)
      .def("copy", &actions::ActionRegistry::Copy,
           py::arg("clear_autofills") = true);

  module.def("status_to_chunk", [](const py::handle& status) {
    return ValueOrThrow(actions::StatusToChunk(StatusFromPython(status)));
  });
  module.def("status_from_chunk", [](const data::Chunk& chunk) {
    return StatusToPython(ValueOrThrow(actions::StatusFromChunk(chunk)));
  });
  module.def("is_status_chunk", &actions::IsStatusChunk);
  module.attr("ACTION_STATUS_MIMETYPE") =
      std::string(actions::kActionStatusMimetype);
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
