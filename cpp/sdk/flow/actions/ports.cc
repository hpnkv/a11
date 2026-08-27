// Copyright 2026 The A11 Authors.

#include "sdk/flow/actions/ports.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>
#include <nlohmann/json.hpp>

#include "a11/actions/action.h"
#include "a11/actions/schema.h"
#include "a11/data/msgpack.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/json_codec.h"
#include "a11/nodes/async_node.h"

namespace a11::sdk::flow {
namespace {

using ::a11::nodes::AsyncNode;

/// The message a value that will not fit JSON gets, naming both ways out.
absl::Status NotEncodableAsJson(std::string_view what) {
  return absl::InvalidArgumentError(absl::StrCat(
      what,
      " holds bytes that are not valid UTF-8, which JSON has no spelling for. "
      "Set options.encoding to \"msgpack\", which carries them exactly, or "
      "read the `bytes` port, which is exact by construction."));
}

}  // namespace

// ---------------------------------------------------------------------------
// Chunks
// ---------------------------------------------------------------------------

absl::StatusOr<Encoding> EncodingFromName(std::string_view name) {
  if (name.empty() || name == "json") {
    return Encoding::kJson;
  }
  if (name == "msgpack" || name == "packb") {
    return Encoding::kMsgpack;
  }
  return absl::InvalidArgumentError(absl::StrCat(
      R"(options.encoding must be "json" or "msgpack", got ')", name, "'"));
}

absl::StatusOr<data::Chunk> ValueChunk(const nlohmann::json& value,
                                       Encoding encoding) {
  data::Chunk chunk;
  if (encoding == Encoding::kMsgpack) {
    ABSL_ASSIGN_OR_RETURN(chunk.data,
                          a11::PackMsgpack(value, "a Flow action's value"));
    chunk.metadata =
        data::ChunkMetadata{.mimetype = std::string(data::kMsgpackMimetype)};
    return chunk;
  }
  // Checked before nlohmann is asked.
  if (FindUnencodableString(value) != nullptr) {
    return NotEncodableAsJson("this value");
  }
  ABSL_ASSIGN_OR_RETURN(chunk.data,
                        a11::DumpJson(value, "a Flow action's value"));
  chunk.metadata =
      data::ChunkMetadata{.mimetype = std::string(data::kJsonMimetype)};
  return chunk;
}

absl::StatusOr<data::Chunk> JsonChunk(const nlohmann::json& value) {
  return ValueChunk(value, Encoding::kJson);
}

data::Chunk BytesChunk(std::string bytes) {
  data::Chunk chunk;
  chunk.metadata = data::ChunkMetadata{.mimetype = std::string(kOctetStream)};
  chunk.data = std::move(bytes);
  return chunk;
}

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------

absl::StatusOr<std::string> BytesOfChunk(const data::Chunk& chunk) {
  const std::string mimetype = chunk.GetMimetype();
  if (absl::StartsWith(mimetype, data::kJsonMimetype)) {
    absl::StatusOr<nlohmann::json> parsed =
        a11::ParseJson(chunk.data, "a byte port's value");
    if (!parsed.ok()) {
      // Labelled JSON and not JSON. The bytes are what there is, and refusing
      // them would lose data over a mislabelling nobody can act on.
      return chunk.data;
    }
    if (parsed->is_string()) {
      return parsed->get<std::string>();
    }
    return chunk.data;
  }
  if (absl::StartsWith(mimetype, data::kMsgpackMimetype)) {
    absl::StatusOr<nlohmann::json> unpacked =
        a11::UnpackMsgpack(chunk.data, "a byte port's value");
    if (!unpacked.ok()) {
      return chunk.data;
    }
    if (unpacked->is_string()) {
      return unpacked->get<std::string>();
    }
    if (unpacked->is_binary()) {
      const nlohmann::json::binary_t& bytes = unpacked->get_binary();
      return std::string(bytes.begin(), bytes.end());
    }
    return chunk.data;
  }
  return chunk.data;
}

absl::StatusOr<std::optional<nlohmann::json>> ReadJsonInput(
    const std::shared_ptr<AsyncNode>& node) {
  if (node == nullptr) {
    return std::optional<nlohmann::json>(std::nullopt);
  }
  ABSL_ASSIGN_OR_RETURN(std::optional<data::Chunk> chunk,
                        node->NextChunk().Await());
  if (!chunk.has_value() || chunk->IsNull()) {
    return std::optional<nlohmann::json>(std::nullopt);
  }
  const std::string mimetype = chunk->GetMimetype();
  // MessagePack first, and not as an afterthought: A11 treats it as a
  // first-class codec, `| packb` puts it in front of any port, and a producer
  // upstream may have chosen it for reasons of its own.
  if (absl::StartsWith(mimetype, data::kMsgpackMimetype)) {
    ABSL_ASSIGN_OR_RETURN(
        nlohmann::json unpacked,
        a11::UnpackMsgpack(chunk->data, "a Flow action's input"));
    return std::optional<nlohmann::json>(std::move(unpacked));
  }
  if (!absl::StartsWith(mimetype, data::kJsonMimetype)) {
    // Text or bytes: a string either way, which is what makes `path: "/tmp/x"`
    // work whichever of those the writer chose.
    return std::optional<nlohmann::json>(nlohmann::json(chunk->data));
  }
  ABSL_ASSIGN_OR_RETURN(nlohmann::json parsed,
                        a11::ParseJson(chunk->data, "a Flow action's input"));
  return std::optional<nlohmann::json>(std::move(parsed));
}

absl::StatusOr<std::optional<std::string>> ReadTextInput(
    const std::shared_ptr<AsyncNode>& node) {
  ABSL_ASSIGN_OR_RETURN(std::optional<nlohmann::json> value,
                        ReadJsonInput(node));
  if (!value.has_value() || value->is_null()) {
    return std::optional<std::string>(std::nullopt);
  }
  if (value->is_string()) {
    return std::optional<std::string>(value->get<std::string>());
  }
  return std::optional<std::string>(value->dump());
}

absl::StatusOr<std::optional<nlohmann::json>> ReadJsonInput(
    const std::shared_ptr<actions::Action>& action, std::string_view port) {
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<AsyncNode> node,
                        action->GetInput(std::string(port)));
  return ReadJsonInput(node);
}

absl::StatusOr<std::optional<std::string>> ReadTextInput(
    const std::shared_ptr<actions::Action>& action, std::string_view port) {
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<AsyncNode> node,
                        action->GetInput(std::string(port)));
  return ReadTextInput(node);
}

absl::StatusOr<std::string> ReadRequiredTextInput(
    const std::shared_ptr<actions::Action>& action, std::string_view port) {
  ABSL_ASSIGN_OR_RETURN(std::optional<std::string> value,
                        ReadTextInput(action, port));
  if (!value.has_value() || value->empty()) {
    return absl::InvalidArgumentError(
        absl::StrCat(action->GetSchema().name, " requires ", port));
  }
  return *std::move(value);
}

// ---------------------------------------------------------------------------
// Sink
// ---------------------------------------------------------------------------

bool Sink::present() const {
  return entry_ != nullptr && !entry_->closed && entry_->node != nullptr;
}

absl::Status Sink::Put(data::Chunk chunk, bool final) const {
  if (!present()) {
    return absl::OkStatus();
  }
  if (final) {
    entry_->final = true;
  }
  return entry_->node->PutChunk(std::move(chunk), std::nullopt, final)
      .Await()
      .status();
}

absl::Status Sink::PutValue(const nlohmann::json& value, bool final) const {
  if (!present()) {
    return absl::OkStatus();
  }
  absl::StatusOr<data::Chunk> chunk = ValueChunk(value, entry_->encoding);
  if (!chunk.ok()) {
    return chunk.status();
  }
  return Put(*std::move(chunk), final);
}

absl::Status Sink::PutText(std::string_view text, bool final) const {
  if (!present()) {
    return absl::OkStatus();
  }
  if (entry_->encoding == Encoding::kMsgpack) {
    // A byte string, which is what a path or a line of a file is. MessagePack
    // has a type for that and JSON does not.
    return Put(*ValueChunk(data::Binary(text), Encoding::kMsgpack), final);
  }
  if (!IsValidUtf8(text)) {
    // Named, because this is a file the caller can perfectly well read -- just
    // not as JSON text.
    return NotEncodableAsJson(absl::StrCat("the port '", entry_->name, "'"));
  }
  absl::StatusOr<data::Chunk> chunk =
      ValueChunk(nlohmann::json(text), Encoding::kJson);
  if (!chunk.ok()) {
    return chunk.status();
  }
  return Put(*std::move(chunk), final);
}

absl::Status Sink::PutBytes(std::string bytes, bool final) const {
  if (!present()) {
    return absl::OkStatus();
  }
  return Put(BytesChunk(std::move(bytes)), final);
}

absl::Status Sink::PutOnly(const nlohmann::json& value) const {
  ABSL_RETURN_IF_ERROR(PutValue(value, /*final=*/true));
  return Close();
}

absl::Status Sink::PutOnlyText(std::string_view text) const {
  ABSL_RETURN_IF_ERROR(PutText(text, /*final=*/true));
  return Close();
}

absl::Status Sink::Close() const {
  if (entry_ == nullptr || entry_->closed) {
    return absl::OkStatus();
  }
  return OutputPorts::CloseEntry(*entry_);
}

// ---------------------------------------------------------------------------
// OutputPorts
// ---------------------------------------------------------------------------

absl::StatusOr<OutputPorts> OutputPorts::Open(
    const std::shared_ptr<actions::Action>& action,
    const std::vector<std::string>& omitted, Encoding encoding) {
  if (action == nullptr) {
    return absl::InvalidArgumentError("an action is required");
  }
  const actions::ActionSchema schema = action->GetSchema();

  // A misspelled omission is reported before anything is written: the caller
  // meant to skip a port and is instead about to wait on one.
  for (const std::string& name : omitted) {
    if (!schema.outputs.contains(name)) {
      return absl::InvalidArgumentError(
          absl::StrCat("options.omit names '", name,
                       "', which is not an output of ", schema.name));
    }
  }

  OutputPorts ports;
  ports.entries_.reserve(schema.outputs.size());
  for (const auto& [name, port] : schema.outputs) {
    ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> node,
                          action->GetOutput(name));
    auto entry = std::make_unique<Sink::Entry>(Sink::Entry{
        .name = name, .node = std::move(node), .encoding = encoding});
    if (std::find(omitted.begin(), omitted.end(), name) != omitted.end()) {
      // Nothing will be written here, and saying so now is what lets a caller
      // ask for three ports without draining the other five.
      ABSL_RETURN_IF_ERROR(CloseEntry(*entry));
    }
    ports.entries_.push_back(std::move(entry));
  }
  return ports;
}

Sink OutputPorts::operator[](std::string_view name) {
  for (const std::unique_ptr<Sink::Entry>& entry : entries_) {
    if (entry->name == name) {
      return Sink(entry.get());
    }
  }
  if (misuse_.ok()) {
    misuse_ = absl::InternalError(absl::StrCat("no output port named '", name,
                                               "'; this action declares ",
                                               absl::StrJoin(Names(), ", ")));
  }
  return {};
}

std::vector<std::string> OutputPorts::Names() const {
  std::vector<std::string> names;
  names.reserve(entries_.size());
  for (const std::unique_ptr<Sink::Entry>& entry : entries_) {
    names.push_back(entry->name);
  }
  return names;
}

absl::Status OutputPorts::Finish() {
  absl::Status first = misuse_;
  for (const std::unique_ptr<Sink::Entry>& entry : entries_) {
    const absl::Status closed = CloseEntry(*entry);
    if (first.ok()) {
      first = closed;
    }
  }
  entries_.clear();
  return first;
}

absl::Status OutputPorts::Abort(const absl::Status& reason) {
  if (reason.ok()) {
    return Finish();
  }
  absl::Status first = absl::OkStatus();
  for (const std::unique_ptr<Sink::Entry>& entry : entries_) {
    if (entry->closed) {
      continue;
    }
    entry->closed = true;
    // A port whose value is already complete has nothing to warn a reader
    // about, so it is closed rather than aborted: `status_code` was true even
    // though the body stopped early.
    const absl::Status ended =
        entry->final ? entry->node->Close().Await().status()
                     : entry->node->AbortWithStatus(reason).Await().status();
    if (first.ok()) {
      first = ended;
    }
  }
  entries_.clear();
  return first;
}

absl::Status OutputPorts::CloseEntry(Sink::Entry& entry) {
  if (entry.closed) {
    return absl::OkStatus();
  }
  entry.closed = true;
  if (entry.node == nullptr) {
    return absl::OkStatus();
  }
  // A port whose last value was already marked final has its end established;
  // a null terminator on top of that would claim a second, different one, and
  // the store rejects that rather than guess which was meant.
  if (entry.final) {
    return entry.node->Close().Await().status();
  }
  return entry.node->Finalize({.wait = true}).Await().status();
}

// ---------------------------------------------------------------------------
// Schema helpers
// ---------------------------------------------------------------------------

actions::ActionPortSchema Port(std::string_view name, std::string_view type,
                               std::string_view description, bool required,
                               bool unary) {
  return actions::ActionPortSchema{.name = std::string(name),
                                   .type = std::string(type),
                                   .description = std::string(description),
                                   .required = required,
                                   .unary = unary};
}

std::string JsonType() {
  return std::string(data::kJsonMimetype);
}

absl::StatusOr<Encoding> EncodingOption(const Options& options) {
  ABSL_ASSIGN_OR_RETURN(
      const std::string name,
      options.Enum("encoding", "json", {"json", "msgpack", "packb"}));
  return EncodingFromName(name);
}

absl::StatusOr<OutputPorts> OpenOutputs(
    const std::shared_ptr<actions::Action>& action, const Options& options) {
  ABSL_ASSIGN_OR_RETURN(const std::vector<std::string> omitted, options.Omit());
  ABSL_ASSIGN_OR_RETURN(const Encoding encoding, EncodingOption(options));
  return OutputPorts::Open(action, omitted, encoding);
}

std::string_view SharedOptionsHelp() {
  return "omit -- output port names to close immediately rather than write; "
         "and encoding -- \"json\" (the default) or \"msgpack\", which is both "
         "cheaper downstream of a `| packb` and the only one of the two that "
         "can carry a path or a line that is not valid UTF-8.";
}

void AddDeadlineHeader(actions::ActionSchema& schema, std::string_view effect) {
  schema.headers.emplace(
      std::string(actions::kActionHeaderPrefix) + "deadline",
      actions::ActionHeaderSchema{
          .name = absl::StrCat(actions::kActionHeaderPrefix, "deadline"),
          .description = absl::StrCat(
              "Absolute execution deadline: a base-10 count of milliseconds "
              "since the Unix epoch, or nanoseconds with an 'ns' suffix. ",
              effect)});
}

}  // namespace a11::sdk::flow
