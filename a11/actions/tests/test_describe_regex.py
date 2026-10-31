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

"""RE2 discovery patterns cross the native Python status boundary."""

import pytest

import a11
from a11.actions.describe import registry_to_json
from a11.status import StatusCode, StatusException


def test_discovery_uses_native_re2_patterns():
    registry = a11.ActionRegistry()
    for name in ["read_file", "prefix_read_file"]:
        registry.register(name, a11.ActionSchema(name=name))
    document = registry_to_json(registry, {"names": ["(?i)READ_.*"]})
    assert [entry["name"] for entry in document["actions"]] == ["read_file"]


@pytest.mark.parametrize("pattern", ["[", "(?=read).*", r"(read)_\1"])
def test_invalid_discovery_patterns_raise_status_without_logging(
    pattern, capfd
):
    with pytest.raises(StatusException) as raised:
        registry_to_json(a11.ActionRegistry(), {"names": [pattern]})
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT
    assert "RE2" in raised.value.status.message
    assert capfd.readouterr().err == ""
