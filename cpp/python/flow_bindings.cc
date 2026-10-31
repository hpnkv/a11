// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
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
#include "a11/flow/document.h"
#include "a11/flow/emit_json.h"
#include "a11/flow/file_modules.h"
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
#include "native_types.h"
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
        value["value"] = token.integer;
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
  if (py::isinstance<NativeStatus>(value)) {
    return flow::StatusRecord(value.cast<const NativeStatus&>().value());
  }
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
  if (value.native_status() != nullptr) {
    return StatusToPython(*value.native_status());
  }
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
/// Every one of these reaches into CPython from a flow's fiber, which is why
/// the runtime gives its fibers a stack an interpreter frame chain fits in. The
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
    if (data::IsStatusChunk(chunk)) {
      ABSL_ASSIGN_OR_RETURN(const absl::Status status,
                            data::StatusFromStatusChunk(chunk));
      return flow::StatusRecord(status);
    }
    const py::gil_scoped_acquire acquire;
    absl::Status from_registry;
    try {
      return ValueFromPython(Registry().attr("from_chunk")(py::cast(chunk)));
    } catch (py::error_already_set& error) {
      from_registry = StatusFromPythonException(error);
    }
    // Nothing is registered for a media type that describes bytes rather than a
    // structure -- `application/octet-stream` for a response body, `text/plain`
    // for a log line -- and the default C++ bridge reads those as a bytes or.
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
    if (value.native_status() != nullptr) {
      return data::MakeStatusChunk(*value.native_status());
    }
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
  /// the GIL from a flow's fiber puts it behind the interpreter thread that
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
      if (data::IsStatusChunk(*chunk)) {
        const auto status = data::StatusFromStatusChunk(*chunk);
        if (status.ok()) {
          values.emplace_back(flow::StatusRecord(*status));
        } else {
          values.emplace_back(status.status());
        }
        continue;
      }
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
      if (value->native_status() != nullptr) {
        chunks.emplace_back(data::MakeStatusChunk(*value->native_status()));
        continue;
      }
      try {
        chunks.emplace_back(
            to_chunk(ValueToPython(*value), requested).cast<data::Chunk>());
      } catch (py::error_already_set& error) {
        chunks.emplace_back(StatusFromPythonException(error));
      }
    }
    return chunks;
  }

  // A value of a `struct` becomes an instance of the pydantic model that shape
  // describes.
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
  /// Cache the getter, not the registry. `set_global_serialization_registry`
  /// rebinds the module global, so each conversion calls the cached getter to
  /// observe replacements without repeating module and attribute lookup.
  ///
  /// REQUIRES: the GIL is held.
  py::object Registry() {
    if (!getter_.has_value()) {
      getter_ = py::module_::import("a11.data.serialization")
                    .attr("get_global_serialization_registry");
    }
    return (*getter_)();
  }

  // The leaked HostBridgeForPython singleton owns this reference beyond
  // interpreter teardown, avoiding release without the GIL. DeferredPythonRefs
  // handles objects with shorter lifetimes.
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

  py::class_<flow::CompilationLimits>(
      flow, "CompilationLimits",
      "Host-controlled bounds for parsing, imports and semantic analysis.")
      .def(py::init<>())
      .def_readwrite("max_modules", &flow::CompilationLimits::max_modules,
                     "Compilation bound for modules.")
      .def_readwrite("max_source_bytes",
                     &flow::CompilationLimits::max_source_bytes,
                     "Compilation bound for source bytes.")
      .def_readwrite("max_depth", &flow::CompilationLimits::max_depth,
                     "Compilation bound for depth.")
      .def_readwrite("max_nesting", &flow::CompilationLimits::max_nesting,
                     "Compilation bound for nesting.")
      .def_readwrite("max_tokens", &flow::CompilationLimits::max_tokens,
                     "Compilation bound for tokens.")
      .def_readwrite("max_work", &flow::CompilationLimits::max_work,
                     "Compilation bound for work.")
      .def_readwrite("max_linked_name_bytes",
                     &flow::CompilationLimits::max_linked_name_bytes,
                     "Compilation bound for linked name bytes.")
      .def_readwrite("max_duration_ms",
                     &flow::CompilationLimits::max_duration_ms,
                     "Compilation bound for duration ms.");

  py::class_<flow::syntax::TypeExpression>(flow, "Type",
                                           "A native Flow type expression.")
      .def(py::init([](std::string name,
                       std::vector<flow::syntax::TypeExpression> parameters,
                       bool quoted) {
             flow::syntax::TypeExpression type;
             type.name = std::move(name);
             type.parameters = std::move(parameters);
             type.quoted = quoted;
             const auto source = ValueOrThrow(type.ToSource());
             const auto parsed = flow::Parse(absl::StrCat(
                 "declare flow validate { in value: ", source, " }"));
             if (const auto* error = parsed.FirstError(); error != nullptr) {
               ThrowIfNotOk(absl::InvalidArgumentError(error->message));
             }
             return type;
           }),
           py::arg("name"),
           py::arg("parameters") = std::vector<flow::syntax::TypeExpression>{},
           py::arg("quoted") = false,
           "Construct a type, optionally with generic arguments or a quoted "
           "media type.")
      .def_property_readonly(
          "source",
          [](const flow::syntax::TypeExpression& type) {
            return ValueOrThrow(type.ToSource());
          },
          "The grammar-aware type spelling.");

  auto call_options = py::class_<flow::CallOptions>(
      flow, "CallOptions", "Typed modifiers for an invocation expression.");

  auto expression =
      py::class_<flow::Expression>(
          flow, "Expression",
          "A native Flow expression with grammar-aware source emission.")
          .def_static(
              "reference",
              [](std::string name) {
                return ValueOrThrow(
                    flow::Expression::Reference(std::move(name)));
              },
              py::arg("name"), "Refer to a name or a qualified field.")
          .def(
              "transfer",
              [](const flow::Expression& source,
                 const flow::Expression& destination, bool tolerant) {
                return ValueOrThrow(source.Transfer(destination, tolerant));
              },
              py::arg("destination"), py::arg("tolerant") = false,
              "Construct a grouped transfer returning a continuation sink.")
          .def_static(
              "literal",
              [](py::object value) {
                return ValueOrThrow(
                    flow::Expression::Literal(PythonToJson(value)));
              },
              py::arg("value"), "Construct a Flow literal from a JSON value.")
          .def_static(
              "invoke",
              [](std::string action,
                 const std::map<std::string, flow::Expression>& arguments,
                 bool remote, bool tolerant, std::string node_map) {
                std::vector<std::pair<std::string, flow::Expression>> ordered(
                    arguments.begin(), arguments.end());
                return ValueOrThrow(flow::Expression::Invoke(
                    std::move(action), std::move(ordered), remote, tolerant,
                    std::move(node_map)));
              },
              py::arg("action"),
              py::arg("arguments") = std::map<std::string, flow::Expression>{},
              py::arg("remote") = false, py::arg("tolerant") = false,
              py::arg("node_map") = "",
              "Construct an action invocation, grouped for result access.")
          .def_static(
              "object",
              [](const py::typing::Dict<py::str, flow::Expression>& fields) {
                std::vector<std::pair<std::string, flow::Expression>> ordered;
                ordered.reserve(fields.size());
                for (const auto item : fields) {
                  ordered.emplace_back(py::cast<std::string>(item.first),
                                       py::cast<flow::Expression>(item.second));
                }
                return ValueOrThrow(
                    flow::Expression::Object(std::move(ordered)));
              },
              py::arg("fields"),
              "Construct an object, retaining field evaluation order.")
          .def_static(
              "array",
              [](std::vector<flow::Expression> items) {
                return ValueOrThrow(flow::Expression::Array(std::move(items)));
              },
              py::arg("items"), "Construct an array of expressions.")
          .def_static(
              "builtin",
              [](std::string name, std::vector<flow::Expression> arguments) {
                return ValueOrThrow(flow::Expression::Builtin(
                    std::move(name), std::move(arguments)));
              },
              py::arg("name"), py::arg("arguments"),
              "Apply a language builtin.")
          .def_static(
              "node",
              [](std::optional<flow::Expression> id, std::string node_map) {
                return ValueOrThrow(
                    flow::Expression::Node(std::move(id), std::move(node_map)));
              },
              py::arg("id") = std::nullopt, py::arg("node_map") = "",
              "Allocate or attach a node.")
          .def_static(
              "completion",
              [](std::string operation, const flow::Expression& subject) {
                return ValueOrThrow(flow::Expression::Completion(
                    std::move(operation), subject));
              },
              py::arg("operation"), py::arg("subject"),
              "Read status, wait for, or close a subject.")
          .def(
              "cast",
              [](const flow::Expression& self,
                 const flow::syntax::TypeExpression& type) {
                return ValueOrThrow(self.Cast(type));
              },
              py::arg("type"), "Coerce an expression to a type.")
          .def(
              "index",
              [](const flow::Expression& self, const flow::Expression& index) {
                return ValueOrThrow(self.Index(index));
              },
              py::arg("index"), "Select an indexed item.")
          .def(
              "field",
              [](const flow::Expression& self, std::string name) {
                return ValueOrThrow(self.Field(std::move(name)));
              },
              py::arg("name"), "Select a port or field.")
          .def(
              "pipeline",
              [](const flow::Expression& self, std::string stage,
                 std::optional<flow::Expression> argument) {
                return ValueOrThrow(
                    self.Pipeline(std::move(stage), std::move(argument)));
              },
              py::arg("stage"), py::arg("argument") = std::nullopt,
              "Apply a pipeline stage with required grouping.")
          .def(
              "pipeline_invoke",
              [](const flow::Expression& self, std::string action,
                 std::vector<std::pair<std::string, flow::Expression>>
                     arguments,
                 std::optional<flow::CallOptions> options) {
                return ValueOrThrow(
                    self.PipelineInvoke(std::move(action), std::move(arguments),
                                        options.value_or(flow::CallOptions{})));
              },
              py::arg("action"),
              py::arg("arguments") =
                  std::vector<std::pair<std::string, flow::Expression>>{},
              py::arg("options") = py::none(),
              "Invoke an action once for the whole pipeline stream.")
          .def_property_readonly("source", &flow::Expression::source,
                                 "The emitted Flow expression.");

  call_options.def(py::init<>(), "Create default local invocation options.")
      .def_readwrite("remote", &flow::CallOptions::remote)
      .def_readwrite("tolerant", &flow::CallOptions::tolerant)
      .def_readwrite("tee", &flow::CallOptions::tee)
      .def_readwrite("node_map", &flow::CallOptions::node_map)
      .def_readwrite("id", &flow::CallOptions::id)
      .def_readwrite("deadline", &flow::CallOptions::deadline,
                     "An eager absolute std::time expression.")
      .def_readwrite("after", &flow::CallOptions::after)
      .def_readwrite("forward", &flow::CallOptions::forward)
      .def_property(
          "headers",
          [](const flow::CallOptions& self) {
            py::typing::Dict<py::str, flow::Expression> result;
            for (const auto& [name, value] : self.headers) {
              result[py::str(name)] = py::cast(value);
            }
            return result;
          },
          [](flow::CallOptions& self,
             const py::typing::Dict<py::str, flow::Expression>& values) {
            self.headers.clear();
            self.headers.reserve(values.size());
            for (const auto item : values) {
              self.headers.emplace_back(
                  py::cast<std::string>(item.first),
                  py::cast<flow::Expression>(item.second));
            }
          },
          "Explicit header expressions in evaluation order.")
      .def_property(
          "timeout",
          [](const flow::CallOptions& self) -> py::object {
            return self.timeout.has_value() ? DurationToPython(*self.timeout)
                                            : py::none();
          },
          [](flow::CallOptions& self, py::object value) {
            if (value.is_none()) {
              self.timeout.reset();
            } else if (py::isinstance<py::str>(value)) {
              absl::Duration duration;
              if (!absl::ParseDuration(value.cast<std::string>(), &duration)) {
                ThrowIfNotOk(
                    absl::InvalidArgumentError("Invalid duration string."));
              }
              self.timeout = duration;
            } else {
              self.timeout = ValueOrThrow(DurationFromPython(value, true));
            }
          },
          "An optional A11 duration or duration string.");
  expression.def(
      "take", &flow::Expression::Take, py::arg("optional") = false,
      "Extract the next item, optionally representing EOF as absence.");
  expression.def_static(
      "invoke_with",
      [](std::string action,
         const std::map<std::string, flow::Expression>& arguments,
         const flow::CallOptions& options) {
        return ValueOrThrow(flow::Expression::InvokeWith(
            std::move(action), {arguments.begin(), arguments.end()}, options));
      },
      py::arg("action"), py::arg("arguments"), py::arg("options"),
      "Construct an invocation with typed modifiers.");

  py::class_<flow::Statement>(flow, "Statement",
                              "A typed statement in an executable Flow body.")
      .def_static(
          "bind",
          [](std::string name, const flow::Expression& value,
             std::optional<flow::syntax::TypeExpression> type) {
            return ValueOrThrow(flow::Statement::Initialize(
                std::move(name), value, std::move(type)));
          },
          py::arg("name"), py::arg("value"), py::arg("type") = std::nullopt,
          "Initialize or reassign a value with an optional type guarantee.")
      .def_static(
          "pipe",
          [](const flow::Expression& value,
             const flow::Expression& destination) {
            return ValueOrThrow(flow::Statement::Pipe(value, destination));
          },
          py::arg("value"), py::arg("destination"),
          "Write a stream to a destination.")
      .def_static(
          "wait",
          [](const flow::Expression& subject, bool close) {
            return ValueOrThrow(flow::Statement::Wait(subject, close));
          },
          py::arg("subject"), py::arg("close") = false,
          "Wait for or close a subject.")
      .def_static(
          "wait_all", []() { return ValueOrThrow(flow::Statement::WaitAll()); },
          "Wait for all preceding work in this scope before activating "
          "following statements.")
      .def_static(
          "cancel",
          [](const flow::Expression& subject) {
            return ValueOrThrow(flow::Statement::Cancel(subject));
          },
          py::arg("subject"), "Cancel an owned action.")
      .def_static(
          "fail",
          [](const flow::Expression& code,
             std::optional<flow::Expression> message) {
            return ValueOrThrow(
                flow::Statement::Fail(code, std::move(message)));
          },
          py::arg("code"), py::arg("message") = std::nullopt,
          "Fail with a status and optional message.")
      .def_static(
          "if_",
          [](const flow::Expression& condition,
             const std::vector<flow::Statement>& then_body,
             const std::vector<flow::Statement>& else_body) {
            return ValueOrThrow(
                flow::Statement::If(condition, then_body, else_body));
          },
          py::arg("condition"), py::arg("then_body"),
          py::arg("else_body") = std::vector<flow::Statement>{},
          "Construct conditional statement bodies.")
      .def_static(
          "if_some",
          [](std::string name, const flow::Expression& option,
             const std::vector<flow::Statement>& then_body,
             const std::vector<flow::Statement>& else_body) {
            return ValueOrThrow(flow::Statement::IfSome(std::move(name), option,
                                                        then_body, else_body));
          },
          py::arg("name"), py::arg("option"), py::arg("then_body"),
          py::arg("else_body") = std::vector<flow::Statement>{},
          "Bind an option payload only in its success branch; evaluate once.")
      .def_static(
          "for_",
          [](std::string variable, const flow::Expression& source,
             const std::vector<flow::Statement>& body, int parallel) {
            return ValueOrThrow(flow::Statement::For(std::move(variable),
                                                     source, body, parallel));
          },
          py::arg("variable"), py::arg("source"), py::arg("body"),
          py::arg("parallel") = 1, "Construct a loop over a stream.")
      .def_static(
          "after",
          [](const std::vector<flow::Expression>& subjects,
             const std::vector<flow::Statement>& body, bool tolerant) {
            return ValueOrThrow(
                flow::Statement::After(subjects, body, tolerant));
          },
          py::arg("subjects"), py::arg("body"), py::arg("tolerant") = false,
          "Gate a concurrent block on completion subjects.")
      .def_static(
          "block",
          [](const std::vector<flow::Statement>& body, bool tolerant) {
            return ValueOrThrow(flow::Statement::Block(body, tolerant));
          },
          py::arg("body"), py::arg("tolerant") = false,
          "Group concurrent statements with one outcome.")
      .def_property_readonly("source", &flow::Statement::source,
                             "The emitted statement.");

  py::class_<flow::StructBuilder>(flow, "StructBuilder",
                                  "Construct a native Flow struct.")
      .def(py::init<std::string>(), py::arg("name"), "Create a struct builder.")
      .def_property(
          "exported",
          [](const flow::StructBuilder& self) { return self.shape().exported; },
          [](flow::StructBuilder& self, bool value) {
            self.shape().exported = value;
          },
          "The module visibility.")
      .def_property(
          "description",
          [](const flow::StructBuilder& self) {
            return self.shape().description;
          },
          [](flow::StructBuilder& self, std::string value) {
            self.shape().description = std::move(value);
          },
          "The struct description.")
      .def(
          "field",
          [](flow::StructBuilder& self, std::string name,
             const flow::syntax::TypeExpression& type, bool required,
             std::optional<flow::Expression> default_value,
             std::string description) {
            ThrowIfNotOk(self.AddField(std::move(name), type, required,
                                       std::move(default_value),
                                       std::move(description)));
          },
          py::arg("name"), py::arg("type"), py::arg("required") = false,
          py::arg("default_value") = std::nullopt, py::arg("description") = "",
          "Append a typed field with an optional literal default.")
      .def(
          "emit",
          [](const flow::StructBuilder& self) {
            return ValueOrThrow(self.Emit());
          },
          "Emit validated struct source.");

  py::class_<flow::FlowBuilder>(
      flow, "FlowBuilder",
      "Construct an implemented flow or an interface declaration.")
      .def(py::init<std::string, bool>(), py::arg("name"),
           py::arg("interface_only") = false, "Create a flow builder.")
      .def_property(
          "exported",
          [](const flow::FlowBuilder& self) {
            return self.interface().exported;
          },
          [](flow::FlowBuilder& self, bool value) {
            self.interface().exported = value;
          },
          "Whether this flow belongs to the module's public API.")
      .def_property(
          "description",
          [](const flow::FlowBuilder& self) {
            return self.interface().description;
          },
          [](flow::FlowBuilder& self, std::string value) {
            self.interface().description = std::move(value);
          },
          "The action description.")
      .def(
          "port",
          [](flow::FlowBuilder& self, std::string name, std::string type,
             bool output, bool unary, bool required, std::string description) {
            flow::PortPlan port;
            port.name = std::move(name);
            port.declared = std::move(type);
            port.type = port.declared;
            port.direction = output ? flow::syntax::PortDirection::kOutput
                                    : flow::syntax::PortDirection::kInput;
            port.unary = unary;
            port.required = required;
            port.description = std::move(description);
            self.interface().ports.push_back(std::move(port));
          },
          py::arg("name"), py::arg("type"), py::arg("output") = false,
          py::arg("unary") = true, py::arg("required") = false,
          py::arg("description") = "", "Append an input or output port.")
      .def(
          "port",
          [](flow::FlowBuilder& self, std::string name,
             const flow::syntax::TypeExpression& type, bool output, bool unary,
             bool required, std::string description) {
            ThrowIfNotOk(self.AddPort(std::move(name), type,
                                      output
                                          ? flow::syntax::PortDirection::kOutput
                                          : flow::syntax::PortDirection::kInput,
                                      unary, required, std::move(description)));
          },
          py::arg("name"), py::arg("type"), py::arg("output") = false,
          py::arg("unary") = true, py::arg("required") = false,
          py::arg("description") = "",
          "Append a port with a typed grammar descriptor.")
      .def(
          "header",
          [](flow::FlowBuilder& self, std::string name, std::string alias,
             std::optional<std::string> default_value,
             std::string description) {
            flow::HeaderPlan header;
            header.name = std::move(name);
            header.alias = std::move(alias);
            header.has_default = default_value.has_value();
            if (default_value.has_value()) {
              header.default_value =
                  flow::syntax::Constant::String(*default_value);
            }
            header.description = std::move(description);
            self.interface().headers.push_back(std::move(header));
          },
          py::arg("name"), py::arg("alias"),
          py::arg("default_value") = std::nullopt, py::arg("description") = "",
          "Append a documented header with an optional byte-text default.")
      .def(
          "bind",
          [](flow::FlowBuilder& self, std::string name,
             const flow::Expression& value,
             std::optional<flow::syntax::TypeExpression> type) {
            ThrowIfNotOk(
                self.AddInitialize(std::move(name), value, std::move(type)));
          },
          py::arg("name"), py::arg("value"), py::arg("type") = std::nullopt,
          "Initialize or reassign an expression value.")
      .def(
          "pipe",
          [](flow::FlowBuilder& self, const flow::Expression& value,
             const flow::Expression& destination) {
            ThrowIfNotOk(self.AddPipe(value, destination));
          },
          py::arg("value"), py::arg("destination"),
          "Write an expression to a destination.")
      .def(
          "add_import",
          [](flow::FlowBuilder& self, std::string reference, std::string alias,
             std::string symbol) {
            ThrowIfNotOk(self.AddImport(std::move(reference), std::move(alias),
                                        std::move(symbol)));
          },
          py::arg("reference"), py::arg("alias"), py::arg("symbol") = "",
          "Import a namespace or one named symbol into this flow.")
      .def(
          "add_declaration",
          [](flow::FlowBuilder& self, const flow::FlowBuilder& interface) {
            ThrowIfNotOk(self.AddDeclaration(interface.interface()));
          },
          py::arg("interface"), "Add a flow-local callable declaration.")
      .def(
          "add_struct",
          [](flow::FlowBuilder& self, const flow::StructBuilder& shape) {
            ThrowIfNotOk(self.AddStruct(shape.shape()));
          },
          py::arg("shape"), "Add a flow-local struct.")
      .def(
          "add",
          [](flow::FlowBuilder& self, const flow::Statement& statement) {
            ThrowIfNotOk(self.AddStatement(statement));
          },
          py::arg("statement"), "Append a typed executable statement.")
      .def(
          "emit",
          [](const flow::FlowBuilder& self) {
            return ValueOrThrow(self.Emit());
          },
          "Emit and validate Flow source.");

  py::class_<flow::DocumentSymbolHandle>(
      flow, "DocumentSymbolHandle",
      "A symbol in one document revision; edits reject stale handles.")
      .def_readonly("document", &flow::DocumentSymbolHandle::document)
      .def_readonly("revision", &flow::DocumentSymbolHandle::revision)
      .def_readonly("name", &flow::DocumentSymbolHandle::name)
      .def_readonly("kind", &flow::DocumentSymbolHandle::kind)
      .def_readonly("occurrence", &flow::DocumentSymbolHandle::occurrence);
  py::class_<flow::Edit>(flow, "SourceEdit",
                         "A byte-range replacement in a source document.")
      .def(py::init<size_t, size_t, std::string>(), py::arg("start"),
           py::arg("end"), py::arg("text"), "Construct a source replacement.")
      .def_readwrite("start", &flow::Edit::start)
      .def_readwrite("end", &flow::Edit::end)
      .def_readwrite("text", &flow::Edit::text);
  py::class_<flow::DocumentChange>(
      flow, "DocumentChange",
      "The edits and diagnostics returned by an atomic document transaction.")
      .def_readonly("revision", &flow::DocumentChange::revision)
      .def_readonly("edits", &flow::DocumentChange::edits)
      .def_property_readonly(
          "diagnostics",
          [](const flow::DocumentChange& self) {
            auto result = nlohmann::json::array();
            for (const auto& diagnostic : self.diagnostics) {
              result.push_back(flow::DiagnosticToJsonValue(diagnostic));
            }
            return JsonToPython(result);
          },
          "Diagnostics after the transaction.");
  py::class_<flow::ModuleGraphChange>(
      flow, "ModuleGraphChange",
      "A validated source rename across an in-memory module graph.")
      .def_property_readonly(
          "sources",
          [](const flow::ModuleGraphChange& self) {
            return std::map<std::string, std::string>(self.sources.begin(),
                                                      self.sources.end());
          },
          "The updated source snapshots.")
      .def_property_readonly(
          "edits",
          [](const flow::ModuleGraphChange& self) {
            return std::map<std::string, std::vector<flow::Edit>>(
                self.edits.begin(), self.edits.end());
          },
          "Byte edits grouped by source identity.")
      .def_property_readonly(
          "diagnostics",
          [](const flow::ModuleGraphChange& self) {
            auto value = nlohmann::json::array();
            for (const auto& diagnostic : self.diagnostics) {
              value.push_back(flow::DiagnosticToJsonValue(diagnostic));
            }
            return JsonToPython(value);
          },
          "Public API and graph boundary information.");
  auto document_class =
      py::class_<flow::ModuleDocument>(flow, "ModuleDocument",
                                       "An owning editable Flow module, with "
                                       "revisioned native source transactions.")
          .def(py::init<std::string, std::string>(),
               py::arg("identity") = "<memory>", py::arg("source") = "",
               "Create a module or open source for editing.")
          .def_property_readonly("identity", &flow::ModuleDocument::identity,
                                 "The module's canonical host identity.")
          .def_property_readonly("source", &flow::ModuleDocument::source,
                                 "The current source text.")
          .def_property_readonly("revision", &flow::ModuleDocument::revision,
                                 "The current document revision.")
          .def("symbols", &flow::ModuleDocument::Symbols,
               "Find the contributing declarations and definitions.")
          .def(
              "apply",
              [](flow::ModuleDocument& self, uint64_t revision,
                 std::vector<flow::Edit> edits) {
                return ValueOrThrow(self.Apply(revision, std::move(edits)));
              },
              py::arg("revision"), py::arg("edits"),
              "Commit nonoverlapping edits against an expected revision.")
          .def(
              "insert",
              [](flow::ModuleDocument& self, const flow::FlowBuilder& builder) {
                return ValueOrThrow(self.Insert(builder));
              },
              py::arg("flow"), "Insert one flow contribution.")
          .def(
              "insert_struct",
              [](flow::ModuleDocument& self, const flow::StructBuilder& shape) {
                return ValueOrThrow(self.InsertStruct(shape.shape()));
              },
              py::arg("shape"), "Insert a struct contribution.")
          .def(
              "update",
              [](flow::ModuleDocument& self,
                 const flow::DocumentSymbolHandle& symbol,
                 const flow::FlowBuilder& builder) {
                return ValueOrThrow(self.Update(symbol, builder));
              },
              py::arg("symbol"), py::arg("flow"),
              "Replace one owned contribution, retaining visibility and "
              "declaration kind.")
          .def(
              "update_struct",
              [](flow::ModuleDocument& self,
                 const flow::DocumentSymbolHandle& symbol,
                 const flow::StructBuilder& shape) {
                return ValueOrThrow(self.UpdateStruct(symbol, shape.shape()));
              },
              py::arg("symbol"), py::arg("shape"),
              "Update one struct contribution, retaining visibility and "
              "unrelated "
              "source text.")
          .def(
              "remove",
              [](flow::ModuleDocument& self,
                 const flow::DocumentSymbolHandle& symbol) {
                return ValueOrThrow(self.Remove(symbol));
              },
              py::arg("symbol"), "Remove one contributing symbol.")
          .def(
              "set_exported",
              [](flow::ModuleDocument& self,
                 const flow::DocumentSymbolHandle& symbol, bool value) {
                return ValueOrThrow(self.SetExported(symbol, value));
              },
              py::arg("symbol"), py::arg("value"),
              "Set visibility with a local source edit.")
          .def(
              "upsert_import",
              [](flow::ModuleDocument& self, std::string reference,
                 std::string alias,
                 std::optional<flow::DocumentSymbolHandle> scope,
                 std::string symbol) {
                return ValueOrThrow(
                    self.UpsertImport(std::move(reference), std::move(alias),
                                      std::move(scope), std::move(symbol)));
              },
              py::arg("reference"), py::arg("alias"),
              py::arg("scope") = std::nullopt, py::arg("symbol") = "",
              "Insert or update an import in the module or an implemented "
              "flow.")
          .def(
              "remove_import",
              [](flow::ModuleDocument& self, std::string alias,
                 std::optional<flow::DocumentSymbolHandle> scope) {
                return ValueOrThrow(
                    self.RemoveImport(std::move(alias), std::move(scope)));
              },
              py::arg("alias"), py::arg("scope") = std::nullopt,
              "Remove an import from its lexical scope.")
          .def(
              "upsert_declaration",
              [](flow::ModuleDocument& self,
                 const actions::ActionSchema& schema,
                 std::optional<flow::DocumentSymbolHandle> symbol,
                 bool exported) {
                auto declaration = flow::DeclareAction(schema);
                if (!declaration.diagnostics.empty()) {
                  return flow::DocumentChange{
                      self.revision(), {}, std::move(declaration.diagnostics)};
                }
                declaration.interface.exported = exported;
                for (auto& type : declaration.types) {
                  type.exported = exported;
                }
                return ValueOrThrow(self.UpsertDeclaration(
                    declaration.interface, declaration.types,
                    std::move(symbol)));
              },
              py::arg("schema"), py::arg("symbol") = std::nullopt,
              py::arg("exported") = false,
              "Synchronize one owned interface from a native action schema "
              "without "
              "weakening unrepresentable metadata.")
          .def(
              "rename",
              [](flow::ModuleDocument& self,
                 const flow::DocumentSymbolHandle& symbol, std::string name) {
                return ValueOrThrow(self.Rename(symbol, std::move(name)));
              },
              py::arg("symbol"), py::arg("name"),
              "Rename a local symbol and its references.")
          .def(
              "rename_in_graph",
              [](const flow::ModuleDocument& self,
                 const flow::DocumentSymbolHandle& symbol, std::string name,
                 const std::map<std::string, std::string>& modules) {
                return ValueOrThrow(self.RenameInGraph(
                    symbol, std::move(name),
                    flow::ModuleSources(modules.begin(), modules.end())));
              },
              py::arg("symbol"), py::arg("name"),
              py::arg("modules") = std::map<std::string, std::string>{},
              "Prepare validated reference edits across supplied modules "
              "without "
              "mutating this document.")
          .def(
              "rename_import",
              [](flow::ModuleDocument& self, std::string alias,
                 std::string name,
                 std::optional<flow::DocumentSymbolHandle> scope) {
                return ValueOrThrow(self.RenameImport(
                    std::move(alias), std::move(name), std::move(scope)));
              },
              py::arg("alias"), py::arg("name"),
              py::arg("scope") = std::nullopt,
              "Rename a namespace and its lexically bound action/type "
              "references.")
          .def(
              "effective_schema",
              [](const flow::ModuleDocument& self, std::string name,
                 const std::map<std::string, std::string>& modules,
                 const std::vector<std::string>& source_roots,
                 flow::CompilationLimits limits) {
                flow::ModuleSources sources(modules.begin(), modules.end());
                flow::Program program;
                program.flows.push_back(ValueOrThrow(self.EffectiveSchema(
                    std::move(name), sources,
                    flow::FileModuleResolver(limits.max_source_bytes, {},
                                             source_roots),
                    limits)));
                return JsonToPython(
                    flow::PlanToJsonValue(self.identity(), program)
                        .at("flows")
                        .at(0));
              },
              py::arg("name"),
              py::arg("modules") = std::map<std::string, std::string>{},
              py::arg("source_roots") = std::vector<std::string>{},
              py::arg("limits") = flow::CompilationLimits{},
              "Query the effective conjunction of compatible callable "
              "contributions.")
          .def(
              "check",
              [](const flow::ModuleDocument& self,
                 const std::map<std::string, std::string>& modules,
                 const std::vector<std::string>& source_roots,
                 flow::CompilationLimits limits) {
                flow::ModuleSources sources(modules.begin(), modules.end());
                auto result = nlohmann::json::array();
                for (const auto& diagnostic :
                     self.Check(sources,
                                flow::FileModuleResolver(
                                    limits.max_source_bytes, {}, source_roots),
                                limits)) {
                  result.push_back(flow::DiagnosticToJsonValue(diagnostic));
                }
                return JsonToPython(result);
              },
              py::arg("modules") = std::map<std::string, std::string>{},
              py::arg("source_roots") = std::vector<std::string>{},
              py::arg("limits") = flow::CompilationLimits{},
              "Analyze this module and its supplied import graph.");

  py::class_<BoundFlow>(
      flow, "FlowPlan",
      R"doc(One compiled flow: an action schema, and the graph implementing it.

A handle onto the program it came from, which keeps the program -- and the syntax
tree its graph borrows -- alive for as long as anything holds the flow. So a
handler taken from one still runs after the program variable has gone.
)doc")
      .def_property_readonly("name",
                             [](const BoundFlow& self) { return self.name; })
      .def_property_readonly(
          "exported",
          [](const BoundFlow& self) { return self.plan().exported; },
          "Whether the flow is part of its module's public API.")
      .def_property_readonly(
          "registerable",
          [](const BoundFlow& self) { return self.plan().registerable; },
          "Whether bulk registration exposes this root or exported "
          "implementation.")
      .def_property_readonly(
          "imported",
          [](const BoundFlow& self) { return self.plan().imported; },
          "Whether the implementation belongs to an imported module.")
      .def_property_readonly(
          "interface_only",
          [](const BoundFlow& self) { return self.plan().interface_only; },
          "Whether this is an interface declaration.")
      .def_property_readonly(
          "description",
          [](const BoundFlow& self) { return self.plan().description; })
      .def_property_readonly(
          "source_name",
          [](const BoundFlow& self) { return self.program->source_name(); })
      .def_property_readonly(
          "node_maps",
          [](const BoundFlow& self) { return self.plan().node_maps; })
      .def(
          "action_schema",
          [](const BoundFlow& self) {
            const auto* resolved = self.program->Flow(self.name);
            return ValueOrThrow(flow::FlowSchema(
                resolved->implementation.value_or(resolved->plan),
                &self.program->program()));
          },
          "The selected native implementation's action schema, including "
          "header defaults.")
      .def("describe", &DescribeFlow,
           R"doc(The whole composition as plain data.

The `flow.plan/v1` entry for this flow: its ports, headers, node maps and steps,
nested bodies and all.
)doc")
      .def(
          "make_handler",
          [](const BoundFlow& self,
             const py::typing::Optional<PyLike<net::WireStream>>&
                 dispatch_stream,
             size_t frame_cache_bytes,
             std::optional<size_t> frame_budget_bytes) -> PyActionHandler {
            flow::RunOptions options;
            options.bridge = HostBridgeForPython();
            options.frames.cache_limit_bytes = frame_cache_bytes;
            if (frame_budget_bytes.has_value()) {
              options.frames.max_reserved_bytes = *frame_budget_bytes;
            }
            if (!dispatch_stream.is_none()) {
              options.dispatch_stream =
                  dispatch_stream.cast<std::shared_ptr<net::WireStream>>();
            }
            // A native handler, handed over as the opaque holder the bindings
            // accept anywhere a handler is taken:
            return py::cast(NativeActionHandler(ValueOrThrow(flow::MakeHandler(
                self.program, self.name, std::move(options)))));
          },
          py::arg("dispatch_stream") = py::none(), py::kw_only(),
          py::arg("frame_cache_bytes") = 256 * 1024,
          py::arg("frame_budget_bytes") = py::none(),
          R"doc(The action handler that runs this flow.

``dispatch_stream`` is only for a flow a *client* runs over a session it already
holds: the calls that belong to the peer are bound to that stream, and the flow's
own action is not. An action that is run locally *and* holds a stream ends that
stream when it finishes, after which the session can dispatch nothing.

``frame_cache_bytes`` caps cached frame capacity per runner; zero disables reuse.
``frame_budget_bytes`` bounds reserved frame capacity across the invocation and
its local Flow calls. Payloads, fiber stacks and ordinary containers are excluded.
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
              if (one.plan.entry || one.plan.imported) {
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

Returns a bool because an entry flow has no name and cannot be accessed through
`program["..."]`, `run`, or `call`. Use this to choose between `run_program` and
a named flow from `names`.
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
      .def(
          "external_actions",
          [](const BoundProgram& self) {
            return self.program->ExternalActions();
          },
          "Runtime action identities reachable through external calls and "
          "contracts.")
      .def(
          "optimizations",
          [](const BoundProgram& self) {
            return JsonToPython(self.program->Optimizations())
                .cast<PyJsonObject>();
          },
          "Explain verified single-item transport choices for this "
          "compilation.")
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

  const auto compile_snapshot =
      [](std::string source, std::string identity,
         const std::map<std::string, std::string>& modules,
         const std::vector<std::string>& source_roots,
         flow::CompilationLimits limits, bool single_item_slots) {
        flow::ModuleSources sources(modules.begin(), modules.end());
        return BoundProgram{ValueOrThrow(flow::CompiledProgram::Compile(
            std::move(source), std::move(identity), sources,
            flow::FileModuleResolver(limits.max_source_bytes, {}, source_roots),
            limits, single_item_slots))};
      };
  document_class.def(
      "compile",
      [compile_snapshot](const flow::ModuleDocument& self,
                         const std::map<std::string, std::string>& modules,
                         const std::vector<std::string>& source_roots,
                         flow::CompilationLimits limits,
                         bool single_item_slots) {
        return compile_snapshot(self.source(), self.identity(), modules,
                                source_roots, limits, single_item_slots);
      },
      py::arg("modules") = std::map<std::string, std::string>{},
      py::arg("source_roots") = std::vector<std::string>{},
      py::arg("limits") = flow::CompilationLimits{},
      py::arg("single_item_slots") = true,
      "Compile an immutable snapshot with explicit source roots and "
      "compilation limits.");
  flow.def(
      "compile", compile_snapshot, py::arg("source"),
      py::arg("source_name") = "",
      py::arg("modules") = std::map<std::string, std::string>{},
      py::arg("source_roots") = std::vector<std::string>{},
      py::arg("limits") = flow::CompilationLimits{},
      py::arg("single_item_slots") = true,
      "Compile Flow source with explicit source roots and compilation limits.");

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
      [](std::string_view source, std::string_view source_name,
         const std::map<std::string, std::string>& modules,
         const std::vector<std::string>& source_roots,
         flow::CompilationLimits limits) {
        nlohmann::json request{{"method", "check"},
                               {"source", source},
                               {"path", source_name},
                               {"modules", modules}};
        return JsonToPython(
                   flow::Handle(request,
                                flow::FileModuleResolver(
                                    limits.max_source_bytes, {}, source_roots),
                                limits)
                       .at("result"))
            .cast<PyJsonObject>();
      },
      py::arg("source"), py::arg("source_name") = "-",
      py::arg("modules") = std::map<std::string, std::string>{},
      py::arg("source_roots") = std::vector<std::string>{},
      py::arg("limits") = flow::CompilationLimits{},
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
        return JsonToPython(flow::Handle({{"method", "complete"},
                                          {"source", source},
                                          {"offset", offset}})
                                .at("result"))
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
        return JsonToPython(flow::Handle({{"method", "plan"},
                                          {"source", source},
                                          {"path", source_name}})
                                .at("result"))
            .cast<PyJsonObject>();
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
every entry carries its declaration ``origin``. Editors use that location for
hover information and "go to declaration".

A tolerant textual read, not a parser for three languages: a schema written as a
constructor call with literal arguments comes back whole, one assembled statement
by statement comes back with thinner ports, and one whose *name* is computed is
not found. ``scanned`` says how many files were read and what was skipped, so a
caller can tell a half-read tree from a small one.

The result is what a frontend passes as ``context`` to the other methods, or
merges over the embedded snapshot itself.
)doc");

  flow.def(
      "standard_action_schemas",
      []() {
        actions::ActionRegistry registry;
        const auto capabilities = sdk::flow::SystemCapabilities();
        const auto status = flow::interpreter::RegisterStandardLibrary(
            registry, capabilities, true);
        if (!status.ok()) {
          ThrowStatus(status);
        }
        std::vector<actions::ActionSchema> schemas;
        for (const auto& name : registry.ListRegisteredActions()) {
          auto schema = registry.GetSchema(name);
          if (!schema.ok()) {
            ThrowStatus(schema.status());
          }
          schemas.push_back(std::move(*schema));
        }
        return schemas;
      },
      "Return native standard-library schemas without running any actions.");

  flow.def(
      "register_standard_actions",
      [](const std::shared_ptr<actions::ActionRegistry>& registry,
         const std::vector<std::string>& roots, bool allow_write,
         bool allow_run, bool require_sandbox, bool inherit_environment,
         std::int64_t max_seconds, const std::string& current_directory,
         bool unrestricted) {
        namespace sdk_flow = a11::sdk::flow;
        if (registry == nullptr) {
          ThrowStatus(absl::InvalidArgumentError("a registry is required"));
        }
        std::vector<std::string> where = roots;
        if (where.empty()) {
          where.emplace_back(".");
        }
        sdk_flow::CapabilitiesBuilder capabilities =
            allow_write ? sdk_flow::WorkspaceCapabilities(std::move(where))
                        : sdk_flow::ReadOnlyCapabilities(std::move(where));
        capabilities->filesystem.current_directory = current_directory;
        capabilities->filesystem.unrestricted = unrestricted;
        capabilities->process.enabled = allow_run;
        capabilities->process.any_program = allow_run;
        capabilities->process.inherit_environment = inherit_environment;
        capabilities->process.max_seconds = max_seconds;
        capabilities->process.sandbox =
            unrestricted      ? sdk_flow::SandboxRequest::kNever
            : require_sandbox ? sdk_flow::SandboxRequest::kRequired
                              : sdk_flow::SandboxRequest::kPreferred;
        const auto add_runtime_root = [&capabilities](std::string path) {
          std::error_code error;
          if (std::filesystem::exists(path, error) && !error) {
            capabilities->process.read_roots.push_back(std::move(path));
          }
        };
        if (const char* home = std::getenv("HOME");
            home != nullptr && *home != '\0') {
          for (const std::string_view suffix :
               {"/.local/bin", "/.local/lib", "/.local/share/uv", "/.pyenv",
                "/.conda", "/.cargo", "/.rustup", "/.nvm", "/.fnm", "/.npm",
                "/.bun", "/.sdkman", "/.m2", "/.gradle", "/go", "/.nix-profile",
                "/Library/pnpm", "/.local/share/pnpm"}) {
            add_runtime_root(absl::StrCat(home, suffix));
          }
        }
        for (const std::string_view root :
             {"/opt/homebrew", "/usr/local/go", "/nix/store",
              "/run/current-system/sw", "/home/linuxbrew/.linuxbrew",
              "/Library/Developer", "/System/Library/Developer",
              "/Applications/Xcode.app"}) {
          add_runtime_root(std::string(root));
        }
        capabilities->network.enabled = true;
        capabilities->network.any_host = true;
        const absl::Status status =
            sdk_flow::RegisterFlowActions(*registry, capabilities);
        if (!status.ok()) {
          ThrowStatus(status);
        }
      },
      py::arg("registry"), py::arg("roots"), py::arg("allow_write") = false,
      py::arg("allow_run") = false, py::arg("require_sandbox") = true,
      py::arg("inherit_environment") = false, py::arg("max_seconds") = 600,
      py::arg("current_directory") = ".", py::arg("unrestricted") = false,
      R"doc(Register the native filesystem and process actions on a registry.

The policy is captured by the handlers at registration. A caller may narrow one
run through action options but cannot widen the roots, write access, process
access, environment inheritance, deadline ceiling, or kernel-sandbox requirement.
Relative filesystem paths resolve against ``current_directory``. Spawned
processes may make outbound network connections by default.
``unrestricted=True`` disables filesystem containment and the kernel sandbox;
write and process permissions, environment filtering, and limits still apply.
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
         const py::object& dispatch_stream,
         const std::vector<std::string>& source_roots,
         flow::CompilationLimits limits, bool single_item_slots,
         size_t frame_cache_bytes, std::optional<size_t> frame_budget_bytes)
          -> py::typing::Dict<py::str, py::object> {
        namespace sdk_flow = a11::sdk::flow;

        // Built here rather than bound as a class: the policy a caller states
        // is the same short list the command line takes, and a bound struct
        // would be a second way to say it that could drift from the first.
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
        how.frames.cache_limit_bytes = frame_cache_bytes;
        if (frame_budget_bytes.has_value()) {
          how.frames.max_reserved_bytes = *frame_budget_bytes;
        }
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
        // The same bridge `make_handler` installs.
        how.bridge = HostBridgeForPython();
        if (timeout_seconds.has_value()) {
          how.timeout = absl::Seconds(*timeout_seconds);
        }
        const a11::flow::interpreter::Source what{
            .text = source,
            .name = source_name,
            .source_roots = source_roots,
            .limits = limits,
            .single_item_slots = single_item_slots};

        // Without the GIL: the program runs to completion here, and everything
        // it does -- reading a file, waiting on a clock, calling an action the
        // host registered -- happens on A11's fibers.
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
      // None selects the interpreter's host registry and transport defaults.
      py::arg("registry") = py::none(), py::arg("session") = py::none(),
      py::arg("dispatch_stream") = py::none(),
      py::arg("source_roots") = std::vector<std::string>{},
      py::arg("limits") = flow::CompilationLimits{},
      py::arg("single_item_slots") = true,
      py::arg("frame_cache_bytes") = 256 * 1024,
      py::arg("frame_budget_bytes") = py::none(),
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
      [](const PyJsonObject& request,
         const std::vector<std::string>& source_roots,
         flow::CompilationLimits limits) {
        // Always an object: `{ok, result}` or `{ok, error}`, which is what
        // makes the answer safe to index rather than something to type-test.
        auto payload = PythonToJson(request);
        if (payload.contains("path") && payload["path"].is_string()) {
          payload["path"] =
              flow::FileModuleIdentity(payload["path"].get<std::string>());
        }
        return JsonToPython(
                   flow::Handle(payload,
                                flow::FileModuleResolver(
                                    limits.max_source_bytes, {}, source_roots),
                                limits))
            .cast<PyJsonObject>();
      },
      py::arg("request"), py::arg("source_roots") = std::vector<std::string>{},
      py::arg("limits") = flow::CompilationLimits{},
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
