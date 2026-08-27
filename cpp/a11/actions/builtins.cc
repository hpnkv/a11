// Copyright 2026 The A11 Authors.

#include "a11/actions/builtins.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>

#include "a11/actions/action.h"
#include "a11/actions/describe.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/nodes/async_node.h"

namespace a11::actions {
namespace {

/// A chunk of @p text with @p mimetype stated.
data::Chunk TextChunk(std::string text, std::string_view mimetype) {
  data::Chunk chunk;
  data::ChunkMetadata metadata;
  metadata.mimetype = std::string(mimetype);
  chunk.metadata = std::move(metadata);
  chunk.data = std::move(text);
  return chunk;
}

/**
 * @brief The text on a unary input, or empty where it said nothing.
 *
 * Reads bytes rather than deserialising: a builtin has to work whatever
 * serialization registry the caller's side is carrying, and the two things it
 * accepts -- a JSON request document and an action name -- are text either way.
 */
absl::StatusOr<std::string> ReadUnaryText(const std::shared_ptr<Action>& action,
                                          std::string_view port) {
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<nodes::AsyncNode> input,
                        action->GetInput(std::string(port)));
  ABSL_ASSIGN_OR_RETURN(std::optional<data::Chunk> chunk,
                        input->NextChunk().Await());
  if (!chunk.has_value() || chunk->IsNull() || chunk->IsEmpty()) {
    return "";
  }
  ABSL_RETURN_IF_ERROR(chunk->Materialize());
  return chunk->data;
}

/// Finalizes @p port with one value, which is what a unary output means.
absl::Status WriteUnary(const std::shared_ptr<Action>& action,
                        std::string_view port, std::string text,
                        std::string_view mimetype) {
  ABSL_ASSIGN_OR_RETURN(std::shared_ptr<nodes::AsyncNode> output,
                        action->GetOutput(std::string(port)));
  return output->Finalize(TextChunk(std::move(text), mimetype))
      .Await()
      .status();
}

/// The registry the call arrived through, or FailedPrecondition.
absl::StatusOr<std::shared_ptr<ActionRegistry>> RegistryOf(
    const std::shared_ptr<Action>& action) {
  std::shared_ptr<ActionRegistry> registry = action->GetRegistry();
  if (registry == nullptr) {
    // Only reachable for an action built with no registry at all, which is a
    // caller that constructed the builtin by hand rather than resolving it.
    return absl::FailedPreconditionError(
        "This action was not dispatched through a registry, so there is "
        "nothing to describe");
  }
  return registry;
}

absl::Status RunListActions(const std::shared_ptr<Action>& action) {
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<ActionRegistry> registry,
                        RegistryOf(action));
  ABSL_ASSIGN_OR_RETURN(const std::string encoded,
                        ReadUnaryText(action, "request"));
  ABSL_ASSIGN_OR_RETURN(const SchemaQuery request, ParseSchemaQuery(encoded));
  ABSL_ASSIGN_OR_RETURN(std::string document,
                        RegistryToJsonText(*registry, request));
  return WriteUnary(action, "actions", std::move(document),
                    data::kJsonMimetype);
}

absl::Status RunGetSchema(const std::shared_ptr<Action>& action) {
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<ActionRegistry> registry,
                        RegistryOf(action));
  ABSL_ASSIGN_OR_RETURN(const std::string name,
                        ReadUnaryText(action, "action"));
  if (name.empty()) {
    return absl::InvalidArgumentError(
        "__get_schema__ needs the name of an action on its 'action' input");
  }
  // NotFound rather than an empty document, and distinct from the
  // InvalidArgument an unnameable id gets:
  ABSL_ASSIGN_OR_RETURN(const ActionSchema schema, registry->GetSchema(name));
  const bool runnable = registry->GetHandler(name).ok();
  ABSL_ASSIGN_OR_RETURN(std::string document,
                        SchemaToJsonText(schema, runnable, PortView::kAll));
  return WriteUnary(action, "schema", std::move(document), data::kJsonMimetype);
}

absl::Status RunPing(const std::shared_ptr<Action>& action) {
  ABSL_ASSIGN_OR_RETURN(std::string value, ReadUnaryText(action, "input"));
  return WriteUnary(action, "output", std::move(value), data::kTextMimetype);
}

/// Wraps a blocking handler as an asynchronous one, off the caller's frame.
ActionHandler Plain(absl::Status (*run)(const std::shared_ptr<Action>&)) {
  return [run](std::shared_ptr<Action> action) {
    return a11::SubmitTask(
        [run, action = std::move(action)]() { return run(action); });
  };
}

ActionPortSchema Port(std::string_view name, std::string_view type,
                      std::string_view description, bool required, bool unary) {
  ActionPortSchema port;
  port.name = std::string(name);
  port.type = std::string(type);
  port.description = std::string(description);
  port.required = required;
  port.unary = unary;
  return port;
}

ActionSchema ListActionsSchema() {
  ActionSchema schema;
  schema.name = std::string(kListActionsName);
  schema.description =
      "List the actions this peer serves, with their schemas, as one "
      "a11.actions/v1 document. Takes an optional request object on 'request': "
      "'names' (full-match patterns), 'exact' (names), 'ports' (\"callable\" "
      "or "
      "\"all\"), 'include_reserved', and 'runnable_only'.";
  schema.inputs.emplace(
      "request", Port("request", std::string(data::kJsonMimetype),
                      "Which actions to describe. Absent means all of them.",
                      /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "actions", Port("actions", std::string(data::kJsonMimetype),
                      "The a11.actions/v1 document, whole.", /*required=*/true,
                      /*unary=*/true));
  return schema;
}

ActionSchema GetSchemaSchema() {
  ActionSchema schema;
  schema.name = std::string(kGetSchemaName);
  schema.description =
      "Describe one action this peer serves, as an a11.actions/v1 document. "
      "Fails NOT_FOUND when the name is not registered here.";
  schema.inputs.emplace(
      "action", Port("action", std::string(data::kTextMimetype),
                     "Name of the action to describe.", /*required=*/true,
                     /*unary=*/true));
  schema.outputs.emplace(
      "schema", Port("schema", std::string(data::kJsonMimetype),
                     "The a11.actions/v1 document for that one action.",
                     /*required=*/true, /*unary=*/true));
  return schema;
}

ActionSchema PingSchema() {
  ActionSchema schema;
  schema.name = std::string(kPingName);
  // Wording kept close to the gateway's original: clients match on behaviour,
  // not on this text, but a probe that started describing itself differently
  // would be the first thing anybody diffing two peers noticed.
  schema.description =
      "Ping the server to check if it is alive. Requires a single value on the "
      "port `input`, which it returns as a single value on the port `output`.";
  schema.inputs.emplace("input", Port("input", std::string(data::kTextMimetype),
                                      "Ping input value", /*required=*/false,
                                      /*unary=*/false));
  schema.outputs.emplace("output",
                         Port("output", std::string(data::kTextMimetype),
                              "Pong response value", /*required=*/false,
                              /*unary=*/false));
  return schema;
}

/// The table. A function-local static so it is built once, on first use, and
/// holds nothing that could own a registry.
const absl::flat_hash_map<std::string, BuiltinAction>& Builtins() {
  static const auto* const table = [] {
    auto* built = new absl::flat_hash_map<std::string, BuiltinAction>();
    built->emplace(std::string(kListActionsName),
                   BuiltinAction{ListActionsSchema(), Plain(&RunListActions)});
    built->emplace(std::string(kGetSchemaName),
                   BuiltinAction{GetSchemaSchema(), Plain(&RunGetSchema)});
    built->emplace(std::string(kPingName),
                   BuiltinAction{PingSchema(), Plain(&RunPing)});
    return built;
  }();
  return *table;
}

}  // namespace

bool IsBuiltinAction(std::string_view name) {
  return Builtins().contains(name);
}

const BuiltinAction* absl_nullable GetBuiltinAction(std::string_view name) {
  const auto found = Builtins().find(name);
  if (found == Builtins().end()) {
    return nullptr;
  }
  return &found->second;
}

const std::vector<std::string>& BuiltinActionNames() {
  static const auto* const names = [] {
    auto* built = new std::vector<std::string>();
    built->reserve(Builtins().size());
    for (const auto& [name, unused] : Builtins()) {
      (void)unused;
      built->push_back(name);
    }
    std::sort(built->begin(), built->end());
    return built;
  }();
  return *names;
}

}  // namespace a11::actions
