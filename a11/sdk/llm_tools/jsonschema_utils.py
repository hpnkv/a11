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

"""Re-export of [a11.actions.jsonschema][a11.actions.jsonschema].

The module moved down into the actions layer, because deriving a port's
`json_schema` is part of describing an action and the describer cannot import an
SDK. This name stays for the tool adapters that were its first callers.
"""

from __future__ import annotations

from a11.actions.jsonschema import (
    get_json_schema_type,
    organise_and_deduplicate_jsonschema,
)

__all__ = ["get_json_schema_type", "organise_and_deduplicate_jsonschema"]
