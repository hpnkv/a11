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

"""Exercise encoding guarantees and bounded context transformations."""

import json

import pytest

from a11 import _native
from a11.sdk.llm_context import (
    LlmContextConfig,
    compress_content,
    decode_config,
    expand_content,
)
from a11.status import StatusException


def test_json_preserves_numeric_tokens_duplicate_keys_and_strings():
    text = (
        '{ "huge": 123456789012345678901234567890, "n": 1.2300, "n": -0, "s": "'
        ' a  b \\" \u00e9" }'
    )
    compact = _native.llm_context_compact_json(text)
    assert "123456789012345678901234567890" in compact
    assert "1.2300" in compact
    assert '"n":-0' in compact
    assert json.loads(compact) == json.loads(text)


@pytest.mark.parametrize(
    "text", ["", "\n", "hello", "x\nx\nx\n", "a\r\na\r\n", "é\né\n"]
)
def test_line_encoding_roundtrips_bytes(text):
    encoded = _native.llm_context_encode(text, "a11.lines/v1")
    assert expand_content(encoded) == text


def test_dictionary_roundtrip_retains_json_lexemes():
    text = (
        '[ { "repeated_key": 1234567890123456789 }, {"repeated_key": 1.2300} ]'
    )
    encoded = _native.llm_context_encode(text, "a11.json-keys/v1")
    assert expand_content(encoded) == text


def test_expansion_rejects_bombs():
    encoded = json.dumps(
        {"format": "a11.lines/v1", "parts": [[999999999999, "x"]]}
    )
    with pytest.raises(StatusException):
        expand_content(encoded, max_bytes=256)


def test_invalid_and_overdeep_json_return_status():
    with pytest.raises(StatusException):
        _native.llm_context_compact_json("[" * 130 + "0" + "]" * 130)
    assert (
        compress_content("plain user instruction")["text"]
        == "plain user instruction"
    )


def test_shadow_records_projection_but_preserves_input():
    text = "{" + (" " * 200) + '"x": 1 }'
    result = compress_content(
        text,
        {"strategy": "json", "min_gain": 0, "measurement": {"mode": "shadow"}},
    )
    assert result["text"] == text
    assert (
        result["report"]["projected_tokens_after"]
        < result["report"]["tokens_before"]
    )
    assert result["report"]["changed"] is False


def test_header_cache_compares_equal_bytes_and_off_overrides():
    raw = b'{"input_content":"lossless"}'
    assert decode_config(raw) is decode_config(bytes(bytearray(raw)))
    assert decode_config(None).enabled is False
    assert decode_config(b'{"enabled":false}').enabled is False
    with pytest.raises(StatusException):
        decode_config(b'{"enabled":"yes"}')
