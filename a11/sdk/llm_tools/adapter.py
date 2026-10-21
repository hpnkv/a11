# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import a11
from a11.actions.describe import fill_json_schemas, schema_to_json

#: A port mapped to "$" supplies the whole JSON result.
WHOLE_JSON_OUTPUT = "$"


class ToolAdapter:
    """Expose a native action through the shared discovery-schema projection."""

    def __init__(self, schema: a11.ActionSchema):
        self._schema = schema

    def _entry(self):
        schema = fill_json_schemas(self._schema.model_copy(deep=True))
        return schema_to_json(schema)

    @property
    def input_schema(self):
        return self._get_input_json_schema()

    def _get_input_json_schema(self):
        from a11.sdk.llm_tools.runner import definition_from_schema

        return definition_from_schema(self._entry())["input_schema"]

    @property
    def output_schema(self):
        return self._get_output_json_schema()

    def _get_output_json_schema(self):
        from a11.sdk.llm_tools.runner import output_definition_from_schema

        return output_definition_from_schema(self._entry())
