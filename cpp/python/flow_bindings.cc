// Copyright 2026 The A11 Authors.

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <absl/types/span.h>
#include <nlohmann/json.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/typing.h>

#include "a11/actions/action.h"
#include "a11/actions/schema.h"
#include "a11/data/types.h"
#include "a11/flow/complete.h"
#include "a11/flow/diagnostic.h"
#include "a11/flow/discover.h"
#include "a11/flow/emit_json.h"
#include "a11/flow/format.h"
#include "a11/flow/generate.h"
#include "a11/flow/highlight.h"
#include "a11/flow/inspect.h"
#include "a11/flow/interpreter/interpreter.h"
#include "a11/flow/lexer.h"
#include "a11/flow/parser.h"
#include "a11/flow/resolve.h"
#include "a11/flow/runtime.h"
#include "a11/flow/service.h"
#include "a11/flow/token.h"
#include "a11/flow/values.h"
#include "a11/flow/vocabulary.h"
#include "a11/net/wire_stream.h"
#include "a11/service/session.h"
#include "python/bindings.h"
#include "python/interop.h"
#include "python/native_types.h"
#include "sdk/flow/actions/flow_actions.h"
#include "sdk/flow/actions/policy.h"

namespace py = pybind11;

namespace a11::python {
namespace {

/// A token as the dict `a11.flow` reads it back from.
///
/// Dicts rather than bound classes on purpose: these cross the boundary in
/// thousands at a time and are consumed immediately -- by a highlighter, a
/// formatter, a parity test -- so the cheapest faithful shape wins. The parsed
/// tree, which callers hold on to, gets real types.
py::dict TokenToDict(const flow::Token& token) {
  py::dict value;
  value["kind"] = std::string(flow::KindName(token.kind));
  value["text"] = std::string(token.text);
  value["start"] = token.start;
  value["end"] = token.end;
  value["line"] = token.line;
  value["column"] = token.column;
  switch (token.kind) {
    case flow::TokenKind::kString:
      value["value"] = token.string_value;
      break;
    case flow::TokenKind::kNumber:
      // Integral where it was written without a fractional part, which is what
      // the counted forms of the grammar ask for.
      if (token.is_integer) {
        value["value"] = static_cast<long long>(token.number);
      } else {
        value["value"] = token.number;
      }
      break;
    case flow::TokenKind::kDuration:
      // The interop helper, so a duration arrives as the Python type A11
      // hands back everywhere else rather than an unregistered C++ one.
      value["value"] = DurationToPython(token.duration);
      break;
    default:
      // Only where the value differs from the text: a word and a punctuation
      // mark *are* their text, and a second copy of it would be one more thing
      // to keep in step.
      value["value"] = py::none();
      break;
  }
  return value;
}

py::dict PositionToDict(const flow::Position& position) {
  py::dict value;
  value["offset"] = position.offset;
  value["line"] = position.line;
  value["column"] = position.column;
  return value;
}

py::dict DiagnosticToDict(const flow::Diagnostic& diagnostic) {
  py::dict value;
  value["code"] = diagnostic.code;
  value["severity"] = std::string(flow::SeverityName(diagnostic.severity));
  value["family"] = std::string(flow::FamilyName(diagnostic.family));
  value["message"] = diagnostic.message;
  py::dict range;
  range["start"] = PositionToDict(diagnostic.range.start);
  range["end"] = PositionToDict(diagnostic.range.end);
  value["range"] = range;
  if (!diagnostic.flow.empty()) {
    value["flow"] = diagnostic.flow;
  }
  py::list fixes;
  for (const flow::Fix& fix : diagnostic.fixes) {
    py::dict entry;
    entry["label"] = fix.label;
    py::list edits;
    for (const flow::Edit& edit : fix.edits) {
      py::dict written;
      written["start"] = edit.start;
      written["end"] = edit.end;
      written["text"] = edit.text;
      edits.append(written);
    }
    entry["edits"] = edits;
    fixes.append(entry);
  }
  value["fixes"] = fixes;
  return value;
}

PyJsonObject LexToDict(std::string_view source, bool keep_comments) {
  const flow::LexResult result =
      flow::Lex(source, flow::LexOptions{.keep_comments = keep_comments});
  py::list tokens;
  for (const flow::Token& token : result.tokens) {
    tokens.append(TokenToDict(token));
  }
  py::list diagnostics;
  for (const flow::Diagnostic& diagnostic : result.diagnostics) {
    diagnostics.append(DiagnosticToDict(diagnostic));
  }
  PyJsonObject value;
  value["tokens"] = tokens;
  value["diagnostics"] = diagnostics;
  return value;
}

/// A JSON value as the Python objects it stands for.
///
/// Built here rather than handed over as text: the syntax tree is the one
/// payload big enough for the difference to show, and a caller that got a
/// string back would parse it again immediately.
py::object JsonToPython(const nlohmann::json& value) {
  switch (value.type()) {
    case nlohmann::json::value_t::null:
      return py::none();
    case nlohmann::json::value_t::boolean:
      return py::bool_(value.get<bool>());
    case nlohmann::json::value_t::number_integer:
      return py::int_(value.get<long long>());
    case nlohmann::json::value_t::number_unsigned:
      return py::int_(value.get<unsigned long long>());
    case nlohmann::json::value_t::number_float:
      return py::float_(value.get<double>());
    case nlohmann::json::value_t::string:
      return py::str(value.get_ref<const std::string&>());
    case nlohmann::json::value_t::array: {
      py::list list;
      for (const nlohmann::json& item : value) {
        list.append(JsonToPython(item));
      }
      return list;
    }
    case nlohmann::json::value_t::object: {
      py::dict dict;
      for (const auto& [key, item] : value.items()) {
        dict[py::str(key)] = JsonToPython(item);
      }
      return dict;
    }
    default:
      return py::none();
  }
}

/// A Python object as the JSON value it stands for.
///
/// Only what a request is made of -- the containers, the scalars, and nothing
/// else -- because that is the whole of what crosses this way. Anything else is
/// a caller passing something a request cannot hold, and saying so is better
/// than quietly stringifying it.
nlohmann::json PythonToJson(const py::handle& value) {
  if (value.is_none()) {
    return nullptr;
  }
  if (py::isinstance<py::bool_>(value)) {
    return value.cast<bool>();
  }
  if (py::isinstance<py::int_>(value)) {
    return value.cast<long long>();
  }
  if (py::isinstance<py::float_>(value)) {
    return value.cast<double>();
  }
  if (py::isinstance<py::str>(value)) {
    return value.cast<std::string>();
  }
  if (py::isinstance<py::dict>(value)) {
    nlohmann::json object = nlohmann::json::object();
    for (auto [key, item] : value.cast<py::dict>()) {
      object[py::str(key).cast<std::string>()] = PythonToJson(item);
    }
    return object;
  }
  if (py::isinstance<py::list>(value) || py::isinstance<py::tuple>(value)) {
    nlohmann::json array = nlohmann::json::array();
    for (py::handle item : value) {
      array.push_back(PythonToJson(item));
    }
    return array;
  }
  throw py::type_error(
      "A request holds strings, numbers, booleans, lists, dicts and None.");
}

// --- Values across the boundary ---------------------------------------------

py::object ValueToPython(const flow::Value& value);
flow::Value ValueFromPython(const py::handle& value);

/// A Python object the language only knows how to ask questions of.
///
/// What `coerce` produces: a pydantic model, a `Duration`, anything a
/// serialisation registry has been told about. The language never takes one
/// apart -- it renders it, indexes it, compares it, asks whether it is there --
/// and each of those is one call back into the interpreter.
class PythonObject : public flow::HostObject {
 public:
  PythonObject(py::object object, std::string tag)
      : object_(std::move(object)), tag_(std::move(tag)) {}

  ~PythonObject() override {
    if (InterpreterIsGoingAway()) {
      // Interpreter gone: releasing the reference would touch freed state, and
      // nothing is left to leak into.
      object_.release();
      return;
    }
    const py::gil_scoped_acquire acquire;
    object_ = py::object();
  }

  [[nodiscard]] std::string Tag() const override { return tag_; }

  [[nodiscard]] std::string Text() const override {
    const py::gil_scoped_acquire acquire;
    try {
      // What the value is *on the wire*, which for a model is its JSON and for
      // an enum member is the member's value -- the same rule
      // `a11.flow.values.as_text` had.
      if (py::hasattr(object_, "model_dump_json")) {
        return object_.attr("model_dump_json")().cast<std::string>();
      }
      if (py::hasattr(object_, "value") && py::hasattr(object_, "name")) {
        return flow::AsText(ValueFromPython(object_.attr("value")));
      }
      const py::module_ json = py::module_::import("json");
      py::dict options;
      options["default"] = py::module_::import("builtins").attr("str");
      options["sort_keys"] = true;
      return json.attr("dumps")(object_, **options).cast<std::string>();
    } catch (py::error_already_set& error) {
      error.discard_as_unraisable(__func__);
      return py::str(object_).cast<std::string>();
    }
  }

  [[nodiscard]] bool Truthy() const override {
    const py::gil_scoped_acquire acquire;
    try {
      return py::bool_(object_);
    } catch (py::error_already_set& error) {
      error.discard_as_unraisable(__func__);
      return true;
    }
  }

  [[nodiscard]] std::optional<size_t> Size() const override {
    const py::gil_scoped_acquire acquire;
    try {
      return py::len(object_);
    } catch (py::error_already_set& error) {
      error.discard_as_unraisable(__func__);
      return std::nullopt;
    }
  }

  [[nodiscard]] flow::Value Field(std::string_view name) const override {
    const py::gil_scoped_acquire acquire;
    const std::string key(name);
    try {
      if (py::hasattr(object_, key.c_str())) {
        return ValueFromPython(object_.attr(key.c_str()));
      }
      if (py::isinstance<py::dict>(object_)) {
        const auto mapping = object_.cast<py::dict>();
        if (mapping.contains(key)) {
          return ValueFromPython(mapping[key.c_str()]);
        }
      }
    } catch (py::error_already_set& error) {
      error.discard_as_unraisable(__func__);
    }
    // Reading a field a producer did not send answers nothing rather than
    // failing, which is what lets a flow say `if not thing.field`.
    return flow::Value::Null();
  }

  [[nodiscard]] flow::Value Element(const flow::Value& key) const override {
    const py::gil_scoped_acquire acquire;
    try {
      return ValueFromPython(object_[ValueToPython(key)]);
    } catch (py::error_already_set& error) {
      error.discard_as_unraisable(__func__);
      return flow::Value::Null();
    }
  }

  [[nodiscard]] bool Equals(const flow::HostObject& other) const override {
    const auto* twin = dynamic_cast<const PythonObject*>(&other);
    if (twin == nullptr) {
      return false;
    }
    const py::gil_scoped_acquire acquire;
    try {
      return object_.equal(twin->object_);
    } catch (py::error_already_set& error) {
      error.discard_as_unraisable(__func__);
      return false;
    }
  }

  /// The object itself, for handing back to the host.
  [[nodiscard]] py::object object() const {
    const py::gil_scoped_acquire acquire;
    return object_;
  }

 private:
  py::object object_;
  std::string tag_;
};

/// The name a value's own type goes by, for a message.
std::string TypeName(const py::handle& value) {
  try {
    return py::str(py::type::handle_of(value).attr("__name__"))
        .cast<std::string>();
  } catch (py::error_already_set& error) {
    error.discard_as_unraisable(__func__);
    return "object";
  }
}

/// A Python value as the Flow value it stands for.
///
/// Everything the language has a kind for becomes that kind, and everything
/// else -- a pydantic model, an enum member, a dataclass -- becomes a host
/// object, which is the escape hatch that lets `coerce` mean anything at all.
flow::Value ValueFromPython(const py::handle& value) {
  if (value.is_none()) {
    return flow::Value::Null();
  }
  if (py::isinstance<py::bool_>(value)) {
    return flow::Value::Bool(value.cast<bool>());
  }
  if (py::isinstance<py::int_>(value)) {
    return flow::Value::Integer(value.cast<std::int64_t>());
  }
  if (py::isinstance<py::float_>(value)) {
    return flow::Value::Double(value.cast<double>());
  }
  if (py::isinstance<py::str>(value)) {
    return flow::Value::String(value.cast<std::string>());
  }
  if (py::isinstance<py::bytes>(value) ||
      py::isinstance<py::bytearray>(value)) {
    return flow::Value::Bytes(value.cast<std::string>());
  }
  if (py::isinstance<py::list>(value) || py::isinstance<py::tuple>(value)) {
    std::vector<flow::Value> items;
    for (py::handle item : value) {
      items.push_back(ValueFromPython(item));
    }
    return flow::Value::List(std::move(items));
  }
  if (py::isinstance<py::dict>(value)) {
    flow::Value::Pairs pairs;
    for (auto [key, item] : value.cast<py::dict>()) {
      pairs.emplace_back(py::str(key).cast<std::string>(),
                         ValueFromPython(item));
    }
    return flow::Value::Object(std::move(pairs));
  }
  if (py::isinstance<data::Chunk>(value)) {
    return flow::Value::Chunk(value.cast<data::Chunk>());
  }
  if (absl::StatusOr<absl::Duration> duration = DurationFromPython(value, true);
      duration.ok()) {
    return flow::Value::Duration(*duration);
  }
  if (absl::StatusOr<absl::Time> time = TimeFromPython(value, true);
      time.ok()) {
    return flow::Value::Time(*time);
  }
  return flow::Value::Host(std::make_shared<PythonObject>(
      py::reinterpret_borrow<py::object>(value), TypeName(value)));
}

/// A Flow value as the Python object it stands for.
py::object ValueToPython(const flow::Value& value) {
  switch (value.kind()) {
    case flow::Value::Kind::kNull:
      return py::none();
    case flow::Value::Kind::kBool:
      return py::bool_(value.boolean());
    case flow::Value::Kind::kInteger:
      return py::int_(value.integer());
    case flow::Value::Kind::kDouble:
      return py::float_(value.number());
    case flow::Value::Kind::kString:
      return py::str(value.text());
    case flow::Value::Kind::kBytes:
      return py::bytes(value.text());
    case flow::Value::Kind::kList: {
      py::list items;
      for (const flow::Value& item : value.items()) {
        items.append(ValueToPython(item));
      }
      return items;
    }
    case flow::Value::Kind::kObject: {
      py::dict pairs;
      for (const auto& [key, item] : value.pairs()) {
        pairs[py::str(key)] = ValueToPython(item);
      }
      return pairs;
    }
    case flow::Value::Kind::kDuration:
      return DurationToPython(value.duration());
    case flow::Value::Kind::kTime:
      return TimeToPython(value.time());
    case flow::Value::Kind::kChunk:
      return py::cast(value.chunk());
    case flow::Value::Kind::kHost: {
      const auto* object = dynamic_cast<const PythonObject*>(&value.host());
      if (object != nullptr) {
        return object->object();
      }
      return py::str(value.host().Text());
    }
  }
  return py::none();
}

/// The three questions only the host can answer, answered by the interpreter.
///
/// Every one of these reaches into CPython from a flow's fibre, which is why
/// the runtime gives its fibres a stack an interpreter frame chain fits in. The
/// GIL is taken here rather than held across a flow: a flow spends its time
/// waiting on nodes, and holding the interpreter while it did would serialise
/// every other Python thread behind it.
class PythonBridge : public flow::HostBridge {
 public:
  absl::StatusOr<flow::Value> Coerce(std::string_view tag,
                                     const flow::Value& value) override {
    const py::gil_scoped_acquire acquire;
    try {
      const py::object registry = Registry();
      const py::object target = registry.attr("resolve_type")(std::string(tag));
      if (target.is_none()) {
        return absl::InvalidArgumentError(absl::StrCat(
            "Nothing here knows the type '", tag,
            "'. A tag names a type a serialization registry has been told "
            "about, so the module defining it has to be imported where the "
            "flow runs."));
      }
      const py::object given = ValueToPython(value);
      if (py::isinstance(given, target)) {
        return value;
      }
      if (py::hasattr(target, "model_validate")) {
        return ValueFromPython(target.attr("model_validate")(given));
      }
      if (py::isinstance<py::dict>(given)) {
        return ValueFromPython(target(**given.cast<py::dict>()));
      }
      return ValueFromPython(target(given));
    } catch (py::error_already_set& error) {
      return StatusFromPythonException(error);
    }
  }

  absl::StatusOr<flow::Value> FromChunk(const data::Chunk& chunk) override {
    const py::gil_scoped_acquire acquire;
    absl::Status from_registry;
    try {
      return ValueFromPython(Registry().attr("from_chunk")(py::cast(chunk)));
    } catch (py::error_already_set& error) {
      from_registry = StatusFromPythonException(error);
    }
    // Nothing is registered for a media type that describes bytes rather than a
    // structure -- `application/octet-stream` for a response body, `text/plain`
    // for a log line -- and the default C++ bridge reads those as a bytes or
    // string value rather than failing. The two bridges have to agree, or a
    // flow that runs in a C++ host stops working in a Python one.
    if (absl::IsNotFound(from_registry)) {
      const std::string mimetype = chunk.GetMimetype();
      if (absl::StartsWith(mimetype, "text/")) {
        return flow::Value::String(chunk.data);
      }
      return flow::Value::Bytes(chunk.data);
    }
    return from_registry;
  }

  absl::StatusOr<data::Chunk> ToChunk(const flow::Value& value,
                                      std::string_view mimetype) override {
    const py::gil_scoped_acquire acquire;
    try {
      const py::object chunk = Registry().attr("to_chunk")(
          ValueToPython(value), std::string(mimetype));
      return chunk.cast<data::Chunk>();
    } catch (py::error_already_set& error) {
      return StatusFromPythonException(error);
    }
  }

  /// A batch of chunks decoded with one visit to the interpreter.
  ///
  /// The cost of this bridge is not the decoding, it is the crossing: taking
  /// the GIL from a flow's fibre puts it behind the interpreter thread that
  /// dispatched the flow, twice per value through a stage. A pipeline usually
  /// has several values in hand, so the runtime asks for them together and this
  /// pays the crossing once.
  ///
  /// Per-value statuses, so a batch fails exactly where a one-at-a-time decode
  /// would have: a chunk nothing can read is that chunk's failure and not the
  /// batch's.
  std::vector<absl::StatusOr<flow::Value>> FromChunks(
      absl::Span<const data::Chunk* const> chunks) override {
    std::vector<absl::StatusOr<flow::Value>> values;
    values.reserve(chunks.size());
    const py::gil_scoped_acquire acquire;
    py::object from_chunk;
    try {
      from_chunk = Registry().attr("from_chunk");
    } catch (py::error_already_set& error) {
      const absl::Status failed = StatusFromPythonException(error);
      values.assign(chunks.size(), failed);
      return values;
    }
    for (const data::Chunk* chunk : chunks) {
      absl::Status from_registry;
      try {
        values.emplace_back(ValueFromPython(from_chunk(py::cast(*chunk))));
        continue;
      } catch (py::error_already_set& error) {
        from_registry = StatusFromPythonException(error);
      }
      // The same fallback the single-value path takes, for the media types that
      // describe bytes rather than a structure. See [FromChunk].
      if (absl::IsNotFound(from_registry)) {
        const std::string mimetype = chunk->GetMimetype();
        values.emplace_back(absl::StartsWith(mimetype, "text/")
                                ? flow::Value::String(chunk->data)
                                : flow::Value::Bytes(chunk->data));
      } else {
        values.emplace_back(from_registry);
      }
    }
    return values;
  }

  /// The writing counterpart of [FromChunks]: one crossing for the batch.
  std::vector<absl::StatusOr<data::Chunk>> ToChunks(
      absl::Span<const flow::Value* const> values,
      std::string_view mimetype) override {
    std::vector<absl::StatusOr<data::Chunk>> chunks;
    chunks.reserve(values.size());
    const py::gil_scoped_acquire acquire;
    py::object to_chunk;
    try {
      to_chunk = Registry().attr("to_chunk");
    } catch (py::error_already_set& error) {
      const absl::Status failed = StatusFromPythonException(error);
      chunks.assign(values.size(), failed);
      return chunks;
    }
    const std::string requested(mimetype);
    for (const flow::Value* value : values) {
      try {
        chunks.emplace_back(
            to_chunk(ValueToPython(*value), requested).cast<data::Chunk>());
      } catch (py::error_already_set& error) {
        chunks.emplace_back(StatusFromPythonException(error));
      }
    }
    return chunks;
  }

  /// A value of a `struct` becomes an instance of the pydantic model that shape
  /// describes.
  ///
  /// The record has already been validated against the shape on the C++ side --
  /// there is one implementation of what a shape means -- so this is about how
  /// Python would rather *hold* it. Building the model is Python's job because
  /// that is where a pydantic model can exist at all; the model is cached
  /// against the shape, so a stream of ten thousand records builds one class.
  ///
  /// A host that cannot build the model says so by leaving the record as it is:
  /// a flow that ran without pydantic installed should still run, with plain
  /// mappings, rather than fail at the first coercion.
  absl::StatusOr<flow::Value> Adopt(const flow::DtoPlan& shape,
                                    const flow::Program& program,
                                    const flow::Value& value) override {
    const py::gil_scoped_acquire acquire;
    try {
      const py::object model =
          py::module_::import("a11.flow.plan")
              .attr("_model_for_dto")(
                  flow::DtoToJsonValue(shape, &program).dump());
      if (model.is_none()) {
        return value;
      }
      return flow::Value::Host(std::make_shared<PythonObject>(
          model.attr("model_validate")(ValueToPython(value)), shape.name));
    } catch (py::error_already_set& error) {
      return StatusFromPythonException(error);
    }
  }

 private:
  /// The registry every conversion goes through.
  ///
  /// The *getter* is cached, not the registry:
  /// `set_global_serialization_registry` rebinds the module's own global, so
  /// asking it each time is what keeps a swapped registry honest -- while
  /// importing the module and looking the getter up again is not. This runs
  /// twice per value through a stage, which made those two lookups a per-value
  /// cost of the language.
  ///
  /// REQUIRES: the GIL is held.
  py::object Registry() {
    if (!getter_.has_value()) {
      getter_ = py::module_::import("a11.data.serialization")
                    .attr("get_global_serialization_registry");
    }
    return (*getter_)();
  }

  /// Never destroyed: the bridge itself is a deliberately leaked singleton (see
  /// [HostBridgeForPython]), so this reference outlives interpreter teardown
  /// rather than being released without the GIL. See DeferredPythonRefs for the
  /// cases that are not like that.
  std::optional<py::object> getter_;
};

std::shared_ptr<flow::HostBridge> HostBridgeForPython() {
  static const auto* const kBridge =
      new std::shared_ptr<flow::HostBridge>(std::make_shared<PythonBridge>());
  return *kBridge;
}

// --- Compiled programs ------------------------------------------------------

/// One flow of a compiled program, as Python holds it.
///
/// A handle rather than a copy: the plan, the graph and the tree it borrows are
/// the program's, and this keeps the program alive for as long as anything is
/// holding one of its flows -- which is what makes `program["x"].handler`
/// outlive the program variable it came from.
struct BoundFlow {
  std::shared_ptr<const flow::CompiledProgram> program;
  std::string name;

  [[nodiscard]] const flow::FlowPlan& plan() const {
    const flow::ResolvedFlow* found = program->Flow(name);
    if (found == nullptr) {
      throw py::key_error(absl::StrCat("No flow named '", name, "' any more."));
    }
    return found->plan;
  }
};

struct BoundProgram {
  std::shared_ptr<const flow::CompiledProgram> program;
};

/// The plan of one flow, as the `flow.plan/v1` payload describes it.
PyJsonObject DescribeFlow(const BoundFlow& flow) {
  const nlohmann::json envelope = flow::PlanToJsonValue(
      flow.program->source_name(), flow.program->program());
  for (const nlohmann::json& one : envelope.at("flows")) {
    if (one.value("flow", std::string()) == flow.name) {
      return JsonToPython(one).cast<PyJsonObject>();
    }
  }
  // Unreachable: the handle only exists for a flow the program declares.
  return {};
}

}  // namespace

void BindFlow(py::module_& module) {
  py::module_ flow = module.def_submodule(
      "flow",
      "The Flow language: one lexer, one grammar, one set of checks, shared by "
      "the Python API, the `a11 flow` command and every editor.");

  py::class_<BoundFlow>(
      flow, "FlowPlan",
      R"doc(One compiled flow: an action schema, and the graph implementing it.

A handle onto the program it came from, which keeps the program -- and the syntax
tree its graph borrows -- alive for as long as anything holds the flow. So a
handler taken from one still runs after the program variable has gone.
)doc")
      .def_property_readonly(
          "name", [](const BoundFlow& self) { return self.plan().name; })
      .def_property_readonly(
          "description",
          [](const BoundFlow& self) { return self.plan().description; })
      .def_property_readonly(
          "source_name",
          [](const BoundFlow& self) { return self.program->source_name(); })
      .def_property_readonly(
          "node_maps",
          [](const BoundFlow& self) { return self.plan().node_maps; })
      .def("describe", &DescribeFlow,
           R"doc(The whole composition as plain data.

The `flow.plan/v1` entry for this flow: its ports, headers, node maps and steps,
nested bodies and all.
)doc")
      .def(
          "make_handler",
          [](const BoundFlow& self,
             const py::typing::Optional<PyLike<net::WireStream>>&
                 dispatch_stream) -> PyActionHandler {
            flow::RunOptions options;
            options.bridge = HostBridgeForPython();
            if (!dispatch_stream.is_none()) {
              options.dispatch_stream =
                  dispatch_stream.cast<std::shared_ptr<net::WireStream>>();
            }
            // A native handler, handed over as the opaque holder the bindings
            // accept anywhere a handler is taken: wrapping it in a Python
            // callable would bounce every invocation through the interpreter
            // for nothing, and would need a running loop it does not have.
            return py::cast(NativeActionHandler(ValueOrThrow(flow::MakeHandler(
                self.program, self.name, std::move(options)))));
          },
          py::arg("dispatch_stream") = py::none(),
          R"doc(The action handler that runs this flow.

``dispatch_stream`` is only for a flow a *client* runs over a session it already
holds: the calls that belong to the peer are bound to that stream, and the flow's
own action is not. An action that is run locally *and* holds a stream ends that
stream when it finishes, after which the session can dispatch nothing.
)doc")
      .def("__repr__", [](const BoundFlow& self) {
        return absl::StrCat("<FlowPlan ", self.plan().name, ">");
      });

  py::class_<BoundProgram>(flow, "Program",
                           R"doc(The flows compiled from one Flow source file.

A program is self-contained: its flows may call each other by name, and anything
else they call is looked up in the action registry of whatever runtime dispatches
them.
)doc")
      .def_property_readonly(
          "source_name",
          [](const BoundProgram& self) { return self.program->source_name(); })
      .def_property_readonly(
          "names",
          [](const BoundProgram& self) {
            std::vector<std::string> names;
            for (const flow::ResolvedFlow& one : self.program->flows()) {
              // The entry flow is left out: it has no name, and every caller of
              // this iterates it to look a flow up or to register one as an
              // action. An entry point is neither.
              if (one.plan.entry) {
                continue;
              }
              names.push_back(one.plan.name);
            }
            return names;
          },
          "Every named flow, in the order the file declares them. The entry "
          "flow -- `flow { ... }` -- is not here: it has no name, and it is "
          "run "
          "rather than called.")
      .def_property_readonly(
          "has_entry",
          [](const BoundProgram& self) {
            return self.program->Entry() != nullptr;
          },
          R"doc(Whether the file declares a `flow { ... }` -- a program.

A bool rather than the flow itself, because an entry flow is deliberately
unaddressable: it has no name, so there is no `program["..."]` that reaches it and
nothing can `run` or `call` it. What a caller does with this is decide whether to
run the file with `run_program` or to pick one of `names`.
)doc")
      .def(
          "get",
          [](const BoundProgram& self,
             const std::string& name) -> std::optional<BoundFlow> {
            if (self.program->Flow(name) == nullptr) {
              return std::nullopt;
            }
            return BoundFlow{self.program, name};
          },
          py::arg("name"), "The flow of this name, or ``None``.")
      .def("describe",
           [](const BoundProgram& self) {
             return JsonToPython(
                        flow::PlanToJsonValue(self.program->source_name(),
                                              self.program->program()))
                 .cast<PyJsonObject>();
           })
      .def("__repr__", [](const BoundProgram& self) {
        std::vector<std::string> names;
        for (const flow::ResolvedFlow& one : self.program->flows()) {
          names.push_back(one.plan.name);
        }
        std::sort(names.begin(), names.end());
        return absl::StrCat("<Program [", absl::StrJoin(names, ", "), "]>");
      });

  flow.def(
      "compile",
      [](std::string source, std::string source_name) {
        return BoundProgram{ValueOrThrow(flow::CompiledProgram::Compile(
            std::move(source), std::move(source_name)))};
      },
      py::arg("source"), py::arg("source_name") = "",
      R"doc(Compile Flow source into a runnable program.

The strict door onto the engine every other function here reads through: the
parser and the resolver both recover and report everything, and this refuses on
the first error with the line, the column and the message. ``a11.flow.loads``
turns that into ``FlowSyntaxError``.
)doc");

  flow.def(
      "strformat",
      [](const PyLike<py::str>& format,
         const py::typing::Iterable<py::object>& arguments) -> std::string {
        std::vector<flow::Value> values;
        for (py::handle argument : arguments) {
          values.push_back(ValueFromPython(argument));
        }
        return flow::Strformat(ValueFromPython(format), values);
      },
      py::arg("format"), py::arg("arguments"),
      R"doc(``format`` with each ``%`` conversion replaced by one of ``arguments``.

printf's syntax, and *only* a format string: no attribute access, no indexing,
nothing a template can reach through. A flow's templates can come from a model,
so that matters more here than a richer template language would.
)doc");

  flow.def(
      "tokenize",
      [](std::string_view source, bool keep_comments) {
        return LexToDict(source, keep_comments);
      },
      py::arg("source"), py::arg("keep_comments") = true,
      R"doc(Tokenize Flow source.

Returns a dict of ``tokens`` and ``diagnostics``. Lexing never fails: an
unterminated string ends at its line, an unknown character is one ``bad`` token,
and what follows is still read, because an editor is looking at a file somebody is
in the middle of typing.

With ``keep_comments`` a comment is a token, which is what a highlighter and a
formatter need; without it the stream is what the parser reads.
)doc");

  flow.def(
      "highlight",
      [](std::string_view source, std::string_view source_name) {
        return JsonToPython(flow::TokensToJsonValue(source_name, source))
            .cast<PyJsonObject>();
      },
      py::arg("source"), py::arg("source_name") = "-",
      R"doc(Classify Flow source for colouring.

Returns a ``flow.tokens/v1`` payload: one entry per token with the *meaning* of
the word at that position -- a stage after a ``|``, a type past a port's ``:``, a
member after a ``.``, a function only where it is called. This is the one
implementation of that judgement; an editor maps its names to a palette.
)doc");

  flow.def(
      "parse",
      [](std::string_view source,
         std::string_view source_name) -> PyJsonObject {
        const flow::ParseResult result = flow::Parse(source);
        // The envelope is always an object with known keys, and saying so keeps
        // the generated stub from calling it `Any` or a bare `dict`.
        return JsonToPython(flow::SyntaxToJsonValue(source_name, result))
            .cast<PyJsonObject>();
      },
      py::arg("source"), py::arg("source_name") = "-",
      R"doc(Parse Flow source into its syntax tree.

Returns a ``flow.syntax/v1`` payload: the flows the file declares, and every
problem found in it. Both, always -- parsing never fails. The parser recovers
where the Python reference raises: a statement it cannot read costs its own line,
stands in as an ``error`` node, and the rest of the file is parsed and reported on.

``flow.loads`` is the strict door onto the same engine: it raises
``FlowSyntaxError`` built from the first ``error`` diagnostic here, with the line,
column and message the Python compiler has always reported.
)doc");

  flow.def(
      "check",
      [](std::string_view source, std::string_view source_name) {
        const flow::ParseResult parsed = flow::Parse(source);
        flow::ResolveResult resolved = flow::Resolve(source, parsed);
        for (flow::Diagnostic& found :
             flow::Inspect(source, parsed, resolved)) {
          resolved.diagnostics.push_back(std::move(found));
        }
        flow::SortDiagnostics(resolved.diagnostics);
        return JsonToPython(flow::DiagnosticsToJsonValue(source_name,
                                                         resolved.diagnostics))
            .cast<PyJsonObject>();
      },
      py::arg("source"), py::arg("source_name") = "-",
      R"doc(Everything wrong with one flow file.

Returns a ``flow.diagnostics/v1`` payload: the syntax and form problems the parser
found, and the name, sequence and barrier problems the resolver found, in source
order. Every problem in the file, not the first -- both passes recover.

This is the whole of what ``a11 flow check`` and an editor need. ``flow.compile``
is the same engine with a strict door on it, for actually running one.
)doc");

  flow.def(
      "format",
      [](std::string_view source) {
        return JsonToPython(flow::FormatToJsonValue(flow::Format(source)))
            .cast<PyJsonObject>();
      },
      py::arg("source"),
      R"doc(Format Flow source.

Returns a ``flow.format/v1`` payload: the formatted text, whether it differs, one
edit that turns the input into it, and any problems found on the way.

It decides indentation, the spaces between tokens, blank lines and the columns of a
run of declarations. It does *not* decide where the lines break: that is a judgement
about what belongs together, and it stays the author's. A file with an error in it is
returned exactly as it was, with the diagnostics saying why.
)doc");

  flow.def(
      "codes",
      []() -> PyJsonObjects {
        PyJsonObjects codes;
        for (const flow::CodeInfo& info : flow::KnownCodes()) {
          PyJsonObject value;
          value["code"] = std::string(info.code);
          value["family"] = std::string(flow::FamilyName(info.family));
          value["severity"] = std::string(flow::SeverityName(info.severity));
          value["summary"] = std::string(info.summary);
          codes.append(value);
        }
        return codes;
      },
      R"doc(Every diagnostic code the language publishes, with its meaning.

The same table ``testdata/flow/codes.json`` is generated from, so a toolchain may
read either and get the same answer.
)doc");

  flow.def(
      "vocabulary",
      []() {
        return JsonToPython(flow::VocabularyToJsonValue()).cast<PyJsonObject>();
      },
      R"doc(Every word set the language gives meaning to.

The one table, as ``flow.vocabulary/v1``. Anything generating a static grammar file
reads this rather than restating it, and ``a11 flow syntax`` holds an editor
definition that still keeps a copy to it.
)doc");

  flow.def(
      "stages",
      []() -> PyStringMap {
        PyStringMap stages;
        for (const std::string_view stage : flow::vocabulary::Stages()) {
          stages[py::str(std::string(stage))] =
              std::string(flow::vocabulary::StageArgumentName(
                  *flow::vocabulary::StageTakes(stage)));
        }
        return stages;
      },
      R"doc(Every pipeline stage, and what each one takes after its name.

``"none"``, ``"number"``, ``"expr"``, ``"string"``, ``"string?"`` or ``"stream"``
-- the same table the parser reads, so an editor offering completions after a
``|`` needs no list of its own.
)doc");

  flow.def(
      "complete",
      [](std::string_view source, size_t offset) {
        return JsonToPython(flow::CompletionsToJsonValue(
                                flow::CompleteAt(source, offset)))
            .cast<PyJsonObject>();
      },
      py::arg("source"), py::arg("offset"),
      R"doc(What may be written at ``offset``.

Returns a ``flow.completions/v1`` payload: the proposals in the order they should
be offered, the partial word at the caret, and where that word starts. After a
``|`` only a stage; past a port's ``:`` only a type; after ``x.`` only what ``x``
has. Unfiltered on purpose -- every frontend filters by its own rules, and
filtering twice drops what a fuzzy matcher would have kept.
)doc");

  flow.def(
      "plan",
      [](std::string_view source, std::string_view source_name) {
        const flow::ParseResult parsed = flow::Parse(source);
        const flow::ResolveResult resolved = flow::Resolve(source, parsed);
        auto value =
            JsonToPython(flow::PlanToJsonValue(source_name, resolved.program))
                .cast<PyJsonObject>();
        py::list diagnostics;
        for (const flow::Diagnostic& diagnostic : resolved.diagnostics) {
          diagnostics.append(DiagnosticToDict(diagnostic));
        }
        value["diagnostics"] = diagnostics;
        return value;
      },
      py::arg("source"), py::arg("source_name") = "-",
      R"doc(What each flow of a file resolved to.

Returns a ``flow.plan/v1`` payload -- the ports, headers, node maps and steps of
every flow in the file, nested bodies and all -- and the diagnostics, because a
plan of a file with an error in it is a partial plan and a reader shown it as the
whole truth would be misled.
)doc");

  flow.def(
      "syntax",
      [](std::string_view target) {
        flow::SyntaxTarget wanted = flow::SyntaxTarget::kSublime;
        if (!flow::SyntaxTargetFromName(target, wanted)) {
          throw py::value_error(
              absl::StrCat("No editor definition is generated for ", target,
                           ". Ask `a11 flow syntax --help` which there are."));
        }
        PyStringMap value;
        value["target"] = std::string(flow::SyntaxTargetName(wanted));
        value["path"] = std::string(flow::SyntaxTargetPath(wanted));
        value["text"] = flow::GenerateSyntax(wanted);
        return value;
      },
      py::arg("target") = "sublime",
      R"doc(An editor definition, generated from the language's own tables.

Returns where the file belongs and what should be in it. A static grammar file is a
copy of the word lists, and a copy falls behind; generating it means a word added to
the language reaches the editor by running this, and CI notices when nobody has.

``target`` is ``"sublime"`` for the Sublime/TextMate-family grammar or
``"pygments"`` for the lexer that colours a fenced flow in A11's documentation.
)doc");

  flow.def(
      "syntax_targets",
      [] {
        py::typing::List<PyStringMap> targets;
        for (const flow::SyntaxTarget target : flow::SyntaxTargets()) {
          PyStringMap one;
          one["target"] = std::string(flow::SyntaxTargetName(target));
          one["path"] = std::string(flow::SyntaxTargetPath(target));
          targets.append(one);
        }
        return targets;
      },
      R"doc(Every editor definition the language generates, and where each belongs.

One entry per target: its name, as ``syntax`` is asked for it, and the path the
generated file is checked in at. What the CLI offers as a choice and what it
walks when it is asked to check them all -- so a target added in C++ is a target
the check covers, rather than one a list in Python has to be told about.
)doc");

  flow.def(
      "scan",
      [](const std::vector<std::string>& paths) {
        const flow::discover::Result found = flow::discover::Discover(paths);
        auto value = JsonToPython(found.found.ToJson()).cast<PyJsonObject>();
        PyJsonObject scanned;
        scanned["files_read"] = static_cast<long long>(found.files_read);
        scanned["reached_file_limit"] = found.reached_file_limit;
        py::typing::List<py::str> too_large;
        for (const std::string& path : found.too_large) {
          too_large.append(path);
        }
        scanned["too_large"] = too_large;
        value["scanned"] = scanned;
        return value;
      },
      py::arg("paths"),
      R"doc(The actions a project declares, read out of its source.

Walks each path -- a file or a directory -- for ``ActionSchema`` declarations in
Python, C++ and TypeScript, and returns a ``flow.catalogue/v1`` payload in which
every entry carries the ``origin`` it was declared at. That is what makes an
action somebody wrote this afternoon hoverable in a flow, and what gives it
somewhere for "go to declaration" to land.

A tolerant textual read, not a parser for three languages: a schema written as a
constructor call with literal arguments comes back whole, one assembled statement
by statement comes back with thinner ports, and one whose *name* is computed is
not found. ``scanned`` says how many files were read and what was skipped, so a
caller can tell a half-read tree from a small one.

The result is what a frontend passes as ``context`` to the other methods, or
merges over the embedded snapshot itself.
)doc");

  flow.def(
      "run_program",
      [](const std::string& source, const std::string& source_name,
         const std::vector<std::string>& arguments,
         const std::vector<std::string>& roots, bool allow_write,
         bool allow_run, bool allow_net, bool allow_local_net,
         const std::vector<std::string>& allow_env, bool unrestricted,
         std::optional<double> timeout_seconds, bool standard_streams,
         const py::object& registry, const py::object& session,
         const py::object& dispatch_stream)
          -> py::typing::Dict<py::str, py::object> {
        namespace sdk_flow = a11::sdk::flow;

        // Built here rather than bound as a class: the policy a caller states is
        // the same short list the command line takes, and a bound struct would
        // be a second way to say it that could drift from the first.
        std::vector<std::string> where = roots;
        if (where.empty() && !unrestricted) {
          where.emplace_back(".");
        }
        sdk_flow::CapabilitiesBuilder capabilities =
            allow_write ? sdk_flow::WorkspaceCapabilities(std::move(where))
                        : sdk_flow::ReadOnlyCapabilities(std::move(where));
        if (unrestricted) {
          capabilities->filesystem.unrestricted = true;
          capabilities->filesystem.writable = allow_write;
        }
        capabilities->process.enabled = allow_run;
        capabilities->process.any_program = allow_run;
        capabilities->process.inherit_environment = allow_run;
        capabilities->process.sandbox = sdk_flow::SandboxRequest::kPreferred;
        capabilities->network.enabled = allow_net || allow_local_net;
        capabilities->network.any_host = capabilities->network.enabled;
        capabilities->network.may_listen = capabilities->network.enabled;
        capabilities->network.allow_loopback = allow_local_net;
        capabilities->network.allow_private = allow_local_net;
        capabilities->network.allow_link_local = allow_local_net;
        capabilities->environment.names = allow_env;

        a11::flow::interpreter::RunOptions how;
        how.arguments = arguments;
        how.capabilities = capabilities;
        if (!registry.is_none()) {
          how.registry =
              registry.cast<std::shared_ptr<actions::ActionRegistry>>();
        }
        how.standard_streams = standard_streams;
        // Where a `call` step goes, when it is to leave this process. Both or
        // neither: a stream with no session would dispatch calls whose replies
        // nothing routes back, which looks exactly like a hang.
        if (!session.is_none()) {
          how.session = session.cast<std::shared_ptr<service::Session>>();
        }
        if (!dispatch_stream.is_none()) {
          how.dispatch_stream =
              dispatch_stream.cast<std::shared_ptr<net::WireStream>>();
        }
        if ((how.dispatch_stream == nullptr) != (how.session == nullptr)) {
          ThrowStatus(absl::InvalidArgumentError(
              "session and dispatch_stream go together: a stream with no "
              "session dispatches calls whose replies nothing routes back, and "
              "a session with no stream has nowhere to dispatch them"));
        }
        // The same bridge `make_handler` installs. Without it the program runs
        // against A11's own registry, which is keyed by `typeid` and so knows
        // nothing about a type declared in Python -- so a flow saying
        // `a11.sdk.Interaction{..}` would fail `unimplemented` in the one place
        // it is most likely to be written, a program the host handed its own
        // `interact_with_llm` to.
        how.bridge = HostBridgeForPython();
        if (timeout_seconds.has_value()) {
          how.timeout = absl::Seconds(*timeout_seconds);
        }
        const a11::flow::interpreter::Source what{.text = source,
                                                  .name = source_name};

        // Without the GIL: the program runs to completion here, and everything
        // it does -- reading a file, waiting on a clock, calling an action the
        // host registered -- happens on A11's fibres. Holding the GIL across it
        // would deadlock the moment one of those needed to call back into
        // Python, which is exactly what a host-registered action does.
        absl::StatusOr<a11::flow::interpreter::RunOutcome> outcome =
            WithoutGil([&] { return a11::flow::interpreter::Run(what, how); });
        if (!outcome.ok()) {
          ThrowStatus(outcome.status());
        }

        py::list warnings;
        for (const flow::Diagnostic& diagnostic : outcome->diagnostics) {
          warnings.append(
              JsonToPython(flow::DiagnosticToJsonValue(diagnostic)));
        }
        py::dict result;
        result["exit_code"] = outcome->exit_code;
        result["diagnostics"] = warnings;
        return result;
      },
      py::arg("source"), py::arg("source_name") = "", py::kw_only(),
      py::arg("arguments") = std::vector<std::string>{},
      py::arg("roots") = std::vector<std::string>{},
      py::arg("allow_write") = false, py::arg("allow_run") = false,
      py::arg("allow_net") = false, py::arg("allow_local_net") = false,
      py::arg("allow_env") = std::vector<std::string>{},
      py::arg("unrestricted") = false,
      py::arg("timeout_seconds") = std::optional<double>(),
      py::arg("standard_streams") = true,
      // `py::none()`, not a null shared_ptr: a default argument is converted
      // when the function is *defined*, and BindFlow runs before BindActions
      // registers ActionRegistry -- so a typed default fails at import. The cast
      // happens at call time instead, when the type is certainly known.
      py::arg("registry") = py::none(),
      // Same reason as `registry` above: Session and WireStream are registered
      // by other Bind* functions, so these are cast at call time.
      py::arg("session") = py::none(), py::arg("dispatch_stream") = py::none(),
      R"doc(Run a Flow program's entry flow, and return what it did.

The same interpreter ``a11-flow-run`` is -- one implementation, called two ways
-- so a program behaves identically whichever started it.

Pass a ``registry`` that already holds your own actions and the program can call
them: a process that registered ``interact_with_llm`` can run a file that asks a
model, with no subprocess and nothing about the file changed. Names you
registered are never replaced by the standard library's.

What the program may *do* is the arguments here and nothing the file can say --
``roots``, ``allow_write``, ``allow_run``, ``allow_net``, ``allow_env`` -- for
the same reason they are command-line flags in the CLI.

Pass ``session`` and ``dispatch_stream`` together and the program's ``call``
steps are dispatched at that peer while its ``run`` steps stay here. The peer's
actions still have to be in ``registry`` for their schemas -- registered without
handlers, which is how "this one lives on the peer" is spelled -- because the
resolver looks a name up before it decides anything.
)doc");

  flow.def(
      "check_program",
      [](const std::string& source, const std::string& source_name) {
        absl::StatusOr<std::string> described =
            a11::flow::interpreter::DescribeEntry(
                a11::flow::interpreter::Source{.text = source,
                                               .name = source_name});
        if (!described.ok()) {
          ThrowStatus(described.status());
        }
        return *described;
      },
      py::arg("source"), py::arg("source_name") = "",
      R"doc(What a program's entry flow is, compiling it and running nothing.

Raises if the source will not compile or declares no ``flow { ... }``.
)doc");

  flow.def(
      "request",
      [](const PyJsonObject& request) {
        // Always an object: `{ok, result}` or `{ok, error}`, which is what
        // makes the answer safe to index rather than something to type-test.
        return JsonToPython(flow::Handle(PythonToJson(request)))
            .cast<PyJsonObject>();
      },
      py::arg("request"),
      R"doc(One request to the language service, answered.

The same method dispatch the standalone ``a11-flow serve`` speaks, reachable
without spawning it: ``{"method": "check", "source": "..."}`` gives
``{"ok": true, "result": {...}}``. Every method is available through a named
function here as well; this is for a frontend that is *relaying* -- a server, a
plugin, a test of the protocol -- and would otherwise have to keep its own table
of which method means which call.
)doc");
}

}  // namespace a11::python
