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

"""What a call's `_meta` asks for, read as action headers.

MCP has no header on a `tools/call`, A11 has no per-call bag, and this is the
crossing between them. No server and no MCP SDK: the mapping is a function of
the mapping the wire carried.
"""

import json

from a11.sdk.mcp.calls import headers_from_meta
from a11.sdk.mcp.schemas import McpHeaders, McpMeta


def test_nothing_asks_for_nothing():
    assert headers_from_meta(None) == {}
    assert headers_from_meta({}) == {}


def test_the_headers_object_names_them_one_by_one():
    headers = headers_from_meta(
        {McpMeta.HEADERS.value: {"x-a11-tenant": "acme", "x-trace": "1"}}
    )

    assert headers["x-a11-tenant"] == "acme"
    assert headers["x-trace"] == "1"


def test_an_a11_key_at_the_top_level_is_that_header():
    headers = headers_from_meta(
        {"x-a11-tenant": "acme", "x-otel-traceparent": "00-abc"}
    )

    assert headers["x-a11-tenant"] == "acme"
    assert headers["x-otel-traceparent"] == "00-abc"


def test_the_whole_meta_travels_as_one_json_header():
    headers = headers_from_meta({"tenant": "acme", "attempt": 2})

    assert json.loads(headers[McpHeaders.META.value]) == {
        "tenant": "acme",
        "attempt": 2,
    }


def test_what_mcp_reserved_for_itself_is_left_out():
    headers = headers_from_meta(
        {
            "progress_token": "tok",
            "io.modelcontextprotocol/clientInfo": {"name": "somebody"},
            "tenant": "acme",
        }
    )

    assert json.loads(headers[McpHeaders.META.value]) == {"tenant": "acme"}


def test_the_headers_object_is_not_repeated_inside_the_meta_header():
    headers = headers_from_meta(
        {McpMeta.HEADERS.value: {"x-a11-tenant": "acme"}, "note": "hello"}
    )

    assert json.loads(headers[McpHeaders.META.value]) == {"note": "hello"}


def test_a_value_that_is_not_text_is_carried_as_json():
    headers = headers_from_meta(
        {McpMeta.HEADERS.value: {"x-a11-limits": {"calls": 3}}}
    )

    assert json.loads(headers["x-a11-limits"]) == {"calls": 3}
