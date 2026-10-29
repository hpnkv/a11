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

#include "a11/flow/emit_json.h"
#include "a11/flow/runtime.h"
#include "a11/flow/service.h"
#include "jvm/bridge.h"

namespace a11::jvm {
void BindFlow(absl::flat_hash_map<std::string, Operation>* operations) {
  (*operations)["flow.request"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(nlohmann::json request,
                          Arg<nlohmann::json>(args, "value"));
    return flow::Handle(request);
  };
  (*operations)["flow.compile"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(std::string source, Arg<std::string>(args, "source"));
    ABSL_ASSIGN_OR_RETURN(std::string path,
                          DefaultArg<std::string>(args, "path", ""));
    ABSL_ASSIGN_OR_RETURN(auto program,
                          flow::CompiledProgram::Compile(source, path));
    return Save(std::move(program));
  };
  (*operations)["flow.plan"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto program, Object<flow::CompiledProgram>(id));
    return flow::PlanToJsonValue(program->source_name(), program->program());
  };
}
}  // namespace a11::jvm
