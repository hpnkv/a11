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

#include <absl/strings/escaping.h>
#include <absl/time/clock.h>

#include "a11/data/msgpack.h"
#include "a11/json_codec.h"
#include "a11/time.h"
#include "a11/uuid.h"
#include "jvm/bridge.h"
#include "thread/introspect.h"

namespace a11::jvm {
namespace {
template <typename T, typename From, typename To>
absl::StatusOr<nlohmann::json> Record(const nlohmann::json& args, From from,
                                      To to) {
  ABSL_ASSIGN_OR_RETURN(std::string action, Arg<std::string>(args, "action"));
  if (action == "decode") {
    ABSL_ASSIGN_OR_RETURN(std::string bytes, Arg<std::string>(args, "bytes"));
    ABSL_ASSIGN_OR_RETURN(T value, T::FromMsgpack(bytes));
    return to(value);
  }
  ABSL_ASSIGN_OR_RETURN(nlohmann::json fields,
                        Arg<nlohmann::json>(args, "fields"));
  ABSL_ASSIGN_OR_RETURN(T value, from(fields));
  if (action == "encode") {
    ABSL_ASSIGN_OR_RETURN(std::string bytes, value.ToMsgpack());
    return Binary(bytes);
  }
  if (action == "validate") {
    return Result(value.Validate());
  }
  if (action == "normalize") {
    return to(value);
  }
  return absl::InvalidArgumentError("Unknown wire value operation");
}
}  // namespace

void BindCore(absl::flat_hash_map<std::string, Operation>* operations) {
#define A11_JVM_RECORD(Type)                                      \
  (*operations)["data." #Type] = [](std::int64_t,                 \
                                    const nlohmann::json& args) { \
    return Record<data::Type>(args, data::Type##FromJsonValue,    \
                              data::Type##ToJsonValue);           \
  }
  A11_JVM_RECORD(ChunkMetadata);
  A11_JVM_RECORD(Chunk);
  A11_JVM_RECORD(NodeRef);
  A11_JVM_RECORD(NodeFragment);
  A11_JVM_RECORD(Port);
  A11_JVM_RECORD(ActionMessage);
  A11_JVM_RECORD(WireMessage);
#undef A11_JVM_RECORD
  (*operations)["json.parseOrdered"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto text, Arg<std::string>(args, "value"));
    ABSL_ASSIGN_OR_RETURN(auto value, ParseOrderedJson(text, "JSON data"));
    ABSL_ASSIGN_OR_RETURN(auto bytes, PackOrderedMsgpack(value, "JSON data"));
    return Binary(bytes);
  };
  (*operations)["json.parse"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(std::string value, Arg<std::string>(args, "value"));
    return ParseJson(value, "JSON data");
  };
  (*operations)["json.encodeOrdered"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto bytes, Arg<std::string>(args, "bytes"));
    auto ordered = nlohmann::ordered_json::from_msgpack(bytes, true, false);
    if (ordered.is_discarded()) {
      return absl::InvalidArgumentError("Invalid host fields");
    }
    ABSL_ASSIGN_OR_RETURN(auto text, DumpOrderedJson(ordered, "JSON data"));
    return nlohmann::json(std::move(text));
  };
  (*operations)["json.encode"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(nlohmann::json value,
                          Arg<nlohmann::json>(args, "value"));
    ABSL_ASSIGN_OR_RETURN(std::string encoded, DumpJson(value, "JSON data"));
    return nlohmann::json(encoded);
  };
  (*operations)["msgpack.encodeFields"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(nlohmann::json values,
                          Arg<nlohmann::json>(args, "values"));
    if (!values.is_array()) {
      return absl::InvalidArgumentError("Expected field array");
    }
    data::MsgpackWriter writer;
    for (const nlohmann::json& value : values) {
      ABSL_RETURN_IF_ERROR(writer.Pack(value));
    }
    return Binary(writer.TakeBytes());
  };
  (*operations)["msgpack.decodeFields"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(std::string bytes, Arg<std::string>(args, "bytes"));
    ABSL_ASSIGN_OR_RETURN(size_t count, Arg<size_t>(args, "count"));
    data::MsgpackReader reader(bytes);
    nlohmann::json values = nlohmann::json::array();
    for (size_t index = 0; index < count; ++index) {
      ABSL_ASSIGN_OR_RETURN(nlohmann::json value, reader.Read());
      values.push_back(std::move(value));
    }
    ABSL_RETURN_IF_ERROR(reader.EnsureFullyConsumed());
    return values;
  };
  (*operations)["msgpack.encode"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(nlohmann::json value,
                          Arg<nlohmann::json>(args, "value"));
    ABSL_ASSIGN_OR_RETURN(std::string bytes, PackMsgpack(value, "MessagePack"));
    return Binary(bytes);
  };
  (*operations)["msgpack.decode"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(std::string bytes, Arg<std::string>(args, "bytes"));
    return UnpackMsgpack(bytes, "MessagePack");
  };
  (*operations)["status.normalize"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(absl::Status status, StatusArg(args, "value"));
    return StatusToJson(status);
  };
  (*operations)["status.encode"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(absl::Status status, StatusArg(args, "value"));
    ABSL_ASSIGN_OR_RETURN(std::string bytes, data::PackStatus(status));
    return Binary(bytes);
  };
  (*operations)["debug.fiberReport"] =
      [](std::int64_t,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    return nlohmann::json(thread::FormatFiberReport());
  };
  (*operations)["debug.currentFiberId"] =
      [](std::int64_t,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    return nlohmann::json(thread::CurrentFiberId());
  };
  (*operations)["debug.completedWaits"] =
      [](std::int64_t,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    return nlohmann::json(thread::TotalCompletedWaits());
  };
}
}  // namespace a11::jvm
