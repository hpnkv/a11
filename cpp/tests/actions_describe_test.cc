// Copyright 2026 The A11 Authors.

#include "a11/actions/describe.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/actions/builtins.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/json_codec.h"
#include "a11/nodes/async_node.h"

namespace a11::actions {
namespace {

/// An action with one port of every interesting shape.
ActionSchema SampleSchema() {
  ActionSchema schema;
  schema.name = "sample";
  schema.description = "A sample action.";

  ActionPortSchema command;
  command.name = "command";
  command.type = "text/plain";
  command.description = "What to run.";
  command.required = true;
  command.unary = true;
  command.json_schema = R"({"type":"string"})";
  schema.inputs.emplace("command", command);

  ActionPortSchema hidden;
  hidden.name = "hidden";
  hidden.type = "text/plain";
  hidden.autofills.push_back(std::nullopt);
  schema.inputs.emplace("hidden", hidden);

  ActionPortSchema lines;
  lines.name = "lines";
  lines.type = "text/plain";
  lines.unary = false;
  schema.outputs.emplace("lines", lines);

  ActionHeaderSchema header;
  header.name = "x-a11-thing";
  header.description = "A thing.";
  header.default_value = "yes";
  schema.headers.emplace("x-a11-thing", header);

  (void)schema.MapOutputToJson("lines", std::string(ActionSchema::kWholeJson));
  return schema;
}

ActionHandler NoopHandler() {
  return [](std::shared_ptr<Action>) {
    return a11::SubmitTask([]() -> absl::Status { return absl::OkStatus(); });
  };
}

const nlohmann::json& FindPort(const nlohmann::json& ports,
                               std::string_view name) {
  static const nlohmann::json kNone = nlohmann::json::object();
  for (const nlohmann::json& one : ports) {
    if (one.value("name", "") == name) return one;
  }
  ADD_FAILURE() << "no port named " << name << " in " << ports.dump();
  return kNone;
}

TEST(DescribeSchemaTest, WritesEveryPortField) {
  const nlohmann::json described =
      SchemaToJson(SampleSchema(), /*runnable=*/true);
  EXPECT_EQ(described["name"], "sample");
  EXPECT_EQ(described["description"], "A sample action.");
  EXPECT_TRUE(described["runnable"].get<bool>());

  const nlohmann::json& command = FindPort(described["inputs"], "command");
  EXPECT_EQ(command["type"], "text/plain");
  EXPECT_EQ(command["description"], "What to run.");
  EXPECT_TRUE(command["required"].get<bool>());
  EXPECT_TRUE(command["unary"].get<bool>());
  // Spliced in as a value, not as the text it is stored as, so a consumer
  // building a tool definition need not parse it again.
  EXPECT_EQ(command["json_schema"]["type"], "string");

  EXPECT_EQ(described["headers"][0]["name"], "x-a11-thing");
  EXPECT_TRUE(described["headers"][0]["has_default"].get<bool>());
  EXPECT_EQ(described["headers"][0]["default"], "yes");
  EXPECT_EQ(described["output_to_json_field"]["lines"], "$");
}

TEST(DescribeSchemaTest, AlwaysStatesUnaryExplicitly) {
  // The regression guard named in describe.h: ActionPortSchema defaults `unary`
  // to false and flow.catalogue/v1's reader defaults it to true, so a document
  // that omitted it would mean opposite things on the two sides of one wire.
  const nlohmann::json described =
      SchemaToJson(SampleSchema(), /*runnable=*/true);
  for (const nlohmann::json& port : described["inputs"]) {
    EXPECT_TRUE(port.contains("unary")) << port.dump();
  }
  for (const nlohmann::json& port : described["outputs"]) {
    EXPECT_TRUE(port.contains("unary")) << port.dump();
  }
  EXPECT_FALSE(FindPort(described["outputs"], "lines")["unary"].get<bool>());
}

TEST(DescribeSchemaTest, HidesAutofilledInputsFromCallers) {
  const nlohmann::json callable =
      SchemaToJson(SampleSchema(), /*runnable=*/true, PortView::kCallable);
  for (const nlohmann::json& port : callable["inputs"]) {
    EXPECT_NE(port.value("name", ""), "hidden");
  }

  const nlohmann::json all =
      SchemaToJson(SampleSchema(), /*runnable=*/true, PortView::kAll);
  EXPECT_TRUE(FindPort(all["inputs"], "hidden")["autofilled"].get<bool>());
}

TEST(SchemaFromDescriptionTest, RoundTripsWhatCanTravel) {
  const ActionSchema original = SampleSchema();
  const nlohmann::json described =
      SchemaToJson(original, /*runnable=*/true, PortView::kAll);
  const absl::StatusOr<ActionSchema> rebuilt =
      SchemaFromJson(described);
  ASSERT_TRUE(rebuilt.ok()) << rebuilt.status();

  EXPECT_EQ(rebuilt->name, original.name);
  EXPECT_EQ(rebuilt->description, original.description);
  EXPECT_EQ(rebuilt->inputs.size(), original.inputs.size());
  EXPECT_EQ(rebuilt->outputs.size(), original.outputs.size());

  const ActionPortSchema& command = rebuilt->inputs.at("command");
  EXPECT_EQ(command.type, "text/plain");
  EXPECT_TRUE(command.required);
  EXPECT_TRUE(command.unary);
  EXPECT_FALSE(command.json_schema.empty());
  EXPECT_FALSE(rebuilt->outputs.at("lines").unary);
  EXPECT_EQ(rebuilt->output_to_json_field.at("lines"), "$");

  // Autofills deliberately do not travel: they are receiver-owned defaults, and
  // a caller that could set one could override the receiver's own.
  EXPECT_TRUE(rebuilt->inputs.at("hidden").autofills.empty());
}

TEST(SchemaFromDescriptionTest, DropsAnOlderClientsUserFacingFlag) {
  // The flag marked an output as narration, from before every action had a
  // reserved log port to narrate on. Read and dropped: the port comes back as
  // the ordinary output it now is, rather than failing the whole descriptor.
  const absl::StatusOr<ActionSchema> rebuilt = SchemaFromJsonText(R"({
      "name": "legacy",
      "outputs": [{"name": "progress", "type": "text/plain", "unary": false,
                   "user_facing": true}]
  })");
  ASSERT_TRUE(rebuilt.ok()) << rebuilt.status();
  EXPECT_TRUE(rebuilt->outputs.contains("progress"));
}

TEST(DescribeSchemaTest, NeverWritesAUserFacingFlag) {
  ActionSchema schema;
  schema.name = "sample";
  ActionPortSchema progress;
  progress.name = "progress";
  progress.type = "text/plain";
  schema.outputs.emplace("progress", progress);

  const nlohmann::json described = SchemaToJson(schema, /*runnable=*/true);
  EXPECT_FALSE(described["outputs"][0].contains("user_facing"));
}

TEST(ParseDescribeRequestTest, AbsentIsTheDefaultRequest) {
  for (const std::string_view encoded : {"", "  ", "null", "{}"}) {
    const absl::StatusOr<SchemaQuery> request =
        ParseSchemaQuery(encoded);
    ASSERT_TRUE(request.ok()) << encoded << ": " << request.status();
    EXPECT_TRUE(request->names.empty());
    EXPECT_FALSE(request->include_reserved);
    EXPECT_FALSE(request->runnable_only);
    EXPECT_EQ(request->ports, PortView::kCallable);
  }
}

TEST(ParseDescribeRequestTest, ReadsAnObjectAndABareArray) {
  const absl::StatusOr<SchemaQuery> object = ParseSchemaQuery(
      R"({"names":["shell_.*"],"ports":"all","include_reserved":true,
          "runnable_only":true,"exact":["ping"]})");
  ASSERT_TRUE(object.ok()) << object.status();
  EXPECT_EQ(object->names, std::vector<std::string>{"shell_.*"});
  EXPECT_EQ(object->exact, std::vector<std::string>{"ping"});
  EXPECT_EQ(object->ports, PortView::kAll);
  EXPECT_TRUE(object->include_reserved);
  EXPECT_TRUE(object->runnable_only);

  const absl::StatusOr<SchemaQuery> array =
      ParseSchemaQuery(R"(["a.*","b.*"])");
  ASSERT_TRUE(array.ok()) << array.status();
  EXPECT_EQ(array->names.size(), 2U);
}

TEST(ParseDescribeRequestTest, RejectsAPatternThatIsNotOne) {
  const absl::StatusOr<SchemaQuery> request =
      ParseSchemaQuery(R"({"names":["*unclosed("]})");
  EXPECT_EQ(request.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(ParseDescribeQueryTest, ReadsTheQueryString) {
  const absl::StatusOr<SchemaQuery> request = ParseSchemaQueryString(
      "name=shell_.%2A&name=read_.%2A&ports=all&reserved=1&runnable=1");
  ASSERT_TRUE(request.ok()) << request.status();
  EXPECT_EQ(request->names.size(), 2U);
  EXPECT_EQ(request->names[0], "shell_.*");
  EXPECT_EQ(request->ports, PortView::kAll);
  EXPECT_TRUE(request->include_reserved);
  EXPECT_TRUE(request->runnable_only);
}

TEST(ParseDescribeQueryTest, EmptyIsTheDefaultRequest) {
  const absl::StatusOr<SchemaQuery> request = ParseSchemaQueryString("");
  ASSERT_TRUE(request.ok()) << request.status();
  EXPECT_TRUE(request->names.empty());
}

TEST(DescribeRequestTest, HidesReservedNamesUnlessAsked) {
  SchemaQuery request;
  EXPECT_TRUE(SchemaQueryAccepts(request, "shell_execute"));
  EXPECT_FALSE(SchemaQueryAccepts(request, "__list_actions__"));

  request.include_reserved = true;
  EXPECT_TRUE(SchemaQueryAccepts(request, "__list_actions__"));

  // Named exactly, a reserved action is what the caller asked for.
  SchemaQuery named;
  named.exact.emplace_back("__ping");
  EXPECT_TRUE(SchemaQueryAccepts(named, "__ping"));
  EXPECT_FALSE(SchemaQueryAccepts(named, "shell_execute"));
}

TEST(DescribeRegistryTest, DescribesWhatIsRegistered) {
  auto registry = std::make_shared<ActionRegistry>();
  ASSERT_TRUE(registry->Register("sample", SampleSchema(), NoopHandler()).ok());

  ActionSchema peer_only = SampleSchema();
  peer_only.name = "on_the_peer";
  ASSERT_TRUE(registry->Register("on_the_peer", peer_only).ok());

  const nlohmann::json described =
      RegistryToJson(*registry, SchemaQuery{});
  EXPECT_EQ(described["format"], "a11.actions/v1");
  ASSERT_EQ(described["actions"].size(), 2U);
  // Sorted, so the document is diffable between calls.
  EXPECT_EQ(described["actions"][0]["name"], "on_the_peer");
  EXPECT_FALSE(described["actions"][0]["runnable"].get<bool>());
  EXPECT_EQ(described["actions"][1]["name"], "sample");
  EXPECT_TRUE(described["actions"][1]["runnable"].get<bool>());
}

TEST(DescribeRegistryTest, RunnableOnlyDropsPeerActions) {
  auto registry = std::make_shared<ActionRegistry>();
  ASSERT_TRUE(registry->Register("sample", SampleSchema(), NoopHandler()).ok());
  ActionSchema peer_only = SampleSchema();
  peer_only.name = "on_the_peer";
  ASSERT_TRUE(registry->Register("on_the_peer", peer_only).ok());

  SchemaQuery request;
  request.runnable_only = true;
  const nlohmann::json described = RegistryToJson(*registry, request);
  ASSERT_EQ(described["actions"].size(), 1U);
  EXPECT_EQ(described["actions"][0]["name"], "sample");
}

TEST(DescribeRegistryTest, FiltersByPattern) {
  auto registry = std::make_shared<ActionRegistry>();
  for (const std::string name : {"shell_execute", "shell_kill", "read_file"}) {
    ActionSchema schema = SampleSchema();
    schema.name = name;
    ASSERT_TRUE(registry->Register(name, schema, NoopHandler()).ok());
  }
  SchemaQuery request;
  request.names.emplace_back("shell_.*");
  const nlohmann::json described = RegistryToJson(*registry, request);
  ASSERT_EQ(described["actions"].size(), 2U);
  EXPECT_EQ(described["actions"][0]["name"], "shell_execute");
  EXPECT_EQ(described["actions"][1]["name"], "shell_kill");
}

TEST(DescribedActionsTest, AcceptsAnEnvelopeOrABareArray) {
  const absl::StatusOr<std::string> encoded =
      SchemaToJsonText(SampleSchema(), /*runnable=*/true);
  ASSERT_TRUE(encoded.ok()) << encoded.status();
  const absl::StatusOr<nlohmann::json> envelope =
      a11::ParseJson(*encoded, "test");
  ASSERT_TRUE(envelope.ok()) << envelope.status();

  absl::StatusOr<std::vector<nlohmann::json>> from_envelope =
      SchemasInDocument(*envelope);
  ASSERT_TRUE(from_envelope.ok()) << from_envelope.status();
  EXPECT_EQ(from_envelope->size(), 1U);

  absl::StatusOr<std::vector<nlohmann::json>> from_array =
      SchemasInDocument((*envelope)["actions"]);
  ASSERT_TRUE(from_array.ok()) << from_array.status();
  EXPECT_EQ(from_array->size(), 1U);

  EXPECT_EQ(SchemasInDocument(nlohmann::json{{"nope", 1}}).status().code(),
            absl::StatusCode::kInvalidArgument);
}

// ---------------------------------------------------------------------------
// The builtins
// ---------------------------------------------------------------------------

TEST(BuiltinActionsTest, ABareRegistryAnswersForThem) {
  const auto registry = std::make_shared<ActionRegistry>();
  for (const std::string_view name :
       {kListActionsName, kGetSchemaName, kPingName}) {
    EXPECT_TRUE(registry->IsRegistered(name)) << name;
    EXPECT_TRUE(registry->GetSchema(name).ok()) << name;
    EXPECT_TRUE(registry->GetHandler(name).ok()) << name;
  }
}

TEST(BuiltinActionsTest, AreListedAndSurviveACopy) {
  const auto registry = std::make_shared<ActionRegistry>();
  ASSERT_TRUE(registry->Register("sample", SampleSchema(), NoopHandler()).ok());

  const std::vector<std::string> names = registry->ListRegisteredActions();
  EXPECT_EQ(std::count(names.begin(), names.end(),
                       std::string(kListActionsName)),
            1);

  const std::shared_ptr<ActionRegistry> copy = registry->Copy();
  EXPECT_TRUE(copy->IsRegistered(kListActionsName));
  EXPECT_TRUE(copy->GetHandler(kGetSchemaName).ok());
}

TEST(BuiltinActionsTest, CannotBeShadowedOrRemoved) {
  const auto registry = std::make_shared<ActionRegistry>();
  ActionSchema impostor = SampleSchema();
  impostor.name = std::string(kListActionsName);
  EXPECT_EQ(registry
                ->Register(std::string(kListActionsName), impostor,
                           NoopHandler())
                .code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(registry->Unregister(kListActionsName).code(),
            absl::StatusCode::kInvalidArgument);
  // Still the real one.
  const absl::StatusOr<ActionSchema> schema =
      registry->GetSchema(kListActionsName);
  ASSERT_TRUE(schema.ok()) << schema.status();
  EXPECT_TRUE(schema->inputs.contains("request"));
}

TEST(BuiltinActionsTest, AreHiddenFromAListingByDefault) {
  const auto registry = std::make_shared<ActionRegistry>();
  ASSERT_TRUE(registry->Register("sample", SampleSchema(), NoopHandler()).ok());

  const nlohmann::json described =
      RegistryToJson(*registry, SchemaQuery{});
  ASSERT_EQ(described["actions"].size(), 1U);
  EXPECT_EQ(described["actions"][0]["name"], "sample");

  SchemaQuery reserved;
  reserved.include_reserved = true;
  const nlohmann::json all = RegistryToJson(*registry, reserved);
  EXPECT_EQ(all["actions"].size(), 4U);
}

/// Runs a builtin locally and returns the text it finalized on @p output.
absl::StatusOr<std::string> RunBuiltin(
    const std::shared_ptr<ActionRegistry>& registry, std::string_view name,
    std::string_view input_port, std::string_view input,
    std::string_view output_port) {
  absl::StatusOr<std::shared_ptr<Action>> action =
      registry->MakeAction(name);
  if (!action.ok()) return action.status();
  if (!input.empty()) {
    absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> in =
        (*action)->GetInput(std::string(input_port));
    if (!in.ok()) return in.status();
    data::Chunk chunk;
    data::ChunkMetadata metadata;
    metadata.mimetype = std::string(data::kTextMimetype);
    chunk.metadata = std::move(metadata);
    chunk.data = std::string(input);
    absl::StatusOr<a11::Unit> put = (*in)->Finalize(std::move(chunk)).Await();
    if (!put.ok()) return put.status();
  } else {
    absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> in =
        (*action)->GetInput(std::string(input_port));
    if (!in.ok()) return in.status();
    absl::StatusOr<a11::Unit> done = (*in)->Finalize().Await();
    if (!done.ok()) return done.status();
  }
  absl::StatusOr<std::shared_ptr<Action>> started = (*action)->Run();
  if (!started.ok()) return started.status();
  absl::StatusOr<std::shared_ptr<Action>> finished =
      (*action)->Wait(absl::Seconds(10)).Await();
  if (!finished.ok()) return finished.status();

  absl::StatusOr<std::shared_ptr<nodes::AsyncNode>> out =
      (*action)->GetOutput(std::string(output_port));
  if (!out.ok()) return out.status();
  absl::StatusOr<std::optional<data::Chunk>> chunk = (*out)->NextChunk().Await();
  if (!chunk.ok()) return chunk.status();
  if (!chunk->has_value()) {
    return absl::InternalError("the builtin wrote nothing");
  }
  if (absl::Status materialized = (*chunk)->Materialize();
      !materialized.ok()) {
    return materialized;
  }
  return (*chunk)->data;
}

TEST(BuiltinActionsTest, ListActionsDescribesTheRegistry) {
  const auto registry = std::make_shared<ActionRegistry>();
  ASSERT_TRUE(registry->Register("sample", SampleSchema(), NoopHandler()).ok());

  const absl::StatusOr<std::string> answered =
      RunBuiltin(registry, kListActionsName, "request", "", "actions");
  ASSERT_TRUE(answered.ok()) << answered.status();
  const absl::StatusOr<nlohmann::json> document =
      a11::ParseJson(*answered, "listing");
  ASSERT_TRUE(document.ok()) << document.status();
  EXPECT_EQ((*document)["format"], "a11.actions/v1");
  ASSERT_EQ((*document)["actions"].size(), 1U);
  EXPECT_EQ((*document)["actions"][0]["name"], "sample");
}

TEST(BuiltinActionsTest, GetSchemaDescribesOneAction) {
  const auto registry = std::make_shared<ActionRegistry>();
  ASSERT_TRUE(registry->Register("sample", SampleSchema(), NoopHandler()).ok());

  const absl::StatusOr<std::string> answered =
      RunBuiltin(registry, kGetSchemaName, "action", "sample", "schema");
  ASSERT_TRUE(answered.ok()) << answered.status();
  const absl::StatusOr<nlohmann::json> document =
      a11::ParseJson(*answered, "schema");
  ASSERT_TRUE(document.ok()) << document.status();
  ASSERT_EQ((*document)["actions"].size(), 1U);
  EXPECT_EQ((*document)["actions"][0]["name"], "sample");
  // The one-action view is `all`: a caller asking about a named action is
  // inspecting it, and wants to see the ports it cannot write as well.
  EXPECT_TRUE(
      FindPort((*document)["actions"][0]["inputs"], "hidden")["autofilled"]
          .get<bool>());
}

TEST(BuiltinActionsTest, GetSchemaIsNotFoundForAnUnknownName) {
  const auto registry = std::make_shared<ActionRegistry>();
  const absl::StatusOr<std::string> answered =
      RunBuiltin(registry, kGetSchemaName, "action", "nope", "schema");
  EXPECT_EQ(answered.status().code(), absl::StatusCode::kNotFound)
      << answered.status();
}

TEST(BuiltinActionsTest, GetSchemaIsInvalidArgumentForNoName) {
  const auto registry = std::make_shared<ActionRegistry>();
  const absl::StatusOr<std::string> answered =
      RunBuiltin(registry, kGetSchemaName, "action", "", "schema");
  EXPECT_EQ(answered.status().code(), absl::StatusCode::kInvalidArgument)
      << answered.status();
}

TEST(BuiltinActionsTest, PingEchoes) {
  const auto registry = std::make_shared<ActionRegistry>();
  const absl::StatusOr<std::string> answered =
      RunBuiltin(registry, kPingName, "input", "hello", "output");
  ASSERT_TRUE(answered.ok()) << answered.status();
  EXPECT_EQ(*answered, "hello");
}

TEST(BuiltinActionsTest, ListActionsHonoursItsRequest) {
  const auto registry = std::make_shared<ActionRegistry>();
  for (const std::string name : {"shell_execute", "read_file"}) {
    ActionSchema schema = SampleSchema();
    schema.name = name;
    ASSERT_TRUE(registry->Register(name, schema, NoopHandler()).ok());
  }
  const absl::StatusOr<std::string> answered =
      RunBuiltin(registry, kListActionsName, "request",
                 R"({"names":["shell_.*"]})", "actions");
  ASSERT_TRUE(answered.ok()) << answered.status();
  const absl::StatusOr<nlohmann::json> document =
      a11::ParseJson(*answered, "listing");
  ASSERT_TRUE(document.ok()) << document.status();
  ASSERT_EQ((*document)["actions"].size(), 1U);
  EXPECT_EQ((*document)["actions"][0]["name"], "shell_execute");
}

}  // namespace
}  // namespace a11::actions
