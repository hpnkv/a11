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

"""Observe response usage without changing streamed provider output."""

import json
from types import SimpleNamespace

import pytest

from a11 import net
from a11.net.http import relay_http
from a11.sdk.llm_context import LlmContextServices
from a11.sdk.llm_context.observation import ResponseUsageObserver
from a11.sdk.llm_context.proxy import create_proxy


def observer(profile="chat", *, sse=True):
    records = []
    result = ResponseUsageObserver(profile, "request", records.append)
    result(
        "headers",
        SimpleNamespace(
            status=200,
            headers=[
                (
                    "content-type",
                    "text/event-stream" if sse else "application/json",
                ),
            ],
        ),
        b"",
    )
    return result, records


def test_sse_usage_revisions_survive_arbitrary_byte_splits():
    capture, records = observer()
    wire = (
        b"data:"
        b' {"usage":{"prompt_tokens":10,"completion_tokens":2,'
        b'"prompt_tokens_details":{"cached_tokens":3}}}\r\n\r\ndata:'
        b" [DONE]\n\n"
    )
    for byte in wire:
        capture("body", None, bytes([byte]))
    capture("complete", None, b"")
    usage = [r for r in records if r.get("type") == "a11.usage"]
    assert usage[-1]["usage"]["input_tokens"] == 7
    assert usage[-1]["usage"].get("complete", True)
    assert usage[-1]["revision"] > usage[0]["revision"]
    assert records[-1]["coverage"] == "complete"


@pytest.mark.parametrize("ending", ["limit", "error", "complete"])
def test_incomplete_sse_never_reports_complete_usage(ending):
    capture, records = observer("anthropic")
    capture(
        "body", None, b'data: {"message":{"usage":{"input_tokens":10}}}\n\n'
    )
    capture("body", None, b'data: {"usage":')
    capture(ending, None, b"")
    usage = [r for r in records if r.get("type") == "a11.usage"]
    assert usage[-1]["usage"].get("complete", True) is False
    assert records[-1]["coverage"] == "partial"


def test_anthropic_start_and_delta_counts_merge():
    capture, records = observer("anthropic")
    for payload in (
        {"message": {"usage": {"input_tokens": 10}}},
        {"usage": {"output_tokens": 5}},
    ):
        capture(
            "body", None, b"data: " + json.dumps(payload).encode() + b"\n\n"
        )
    capture("body", None, b'data: {"type":"message_stop"}\n\n')
    capture("complete", None, b"")
    usage = [r for r in records if r.get("type") == "a11.usage"][-1]
    assert usage["usage"]["input_tokens"] == 10
    assert usage["usage"]["output_tokens"] == 5


@pytest.mark.asyncio
async def test_proxy_passively_records_usage_and_preserves_wire_bytes():
    wire = (
        b'data: {"usage":{"prompt_tokens":10,"completion_tokens":2}}\n\ndata:'
        b" [DONE]\n\n"
    )

    async def upstream(request, writer):
        writer.send_headers(200, [("content-type", "text/event-stream")])
        writer.write(wire)
        writer.finish()

    records = []
    origin = net.Http2Server.create("127.0.0.1", 0, upstream)
    proxy = create_proxy(
        f"http://127.0.0.1:{origin.port}",
        services=LlmContextServices(
            recorder=SimpleNamespace(record=records.append)
        ),
    )
    client = await net.Http2Client.connect("127.0.0.1", proxy.port)
    try:
        response = client.request_stream("GET", "/v1/check", [])
        assert b"".join([part async for part in response]) == wire
        await response.wait_done()
        assert any(r.get("type") == "a11.usage" for r in records)
    finally:
        client.close()
        proxy.stop()
        origin.stop()


@pytest.mark.asyncio
async def test_native_relay_observer_failure_preserves_response():
    wire = b"response"

    async def upstream(request, writer):
        writer.send_headers(200, [])
        writer.write(wire)
        writer.finish()

    origin = net.Http2Server.create("127.0.0.1", 0, upstream)

    def broken(event, head, data):
        raise ValueError("observer failure")

    async def relay(request, writer):
        await relay_http(
            f"http://127.0.0.1:{origin.port}/", request, writer, observer=broken
        )

    proxy = net.Http2Server.create("127.0.0.1", 0, relay)
    client = await net.Http2Client.connect("127.0.0.1", proxy.port)
    try:
        response = client.request_stream("GET", "/", [])
        assert b"".join([part async for part in response]) == wire
        await response.wait_done()
    finally:
        client.close()
        proxy.stop()
        origin.stop()


def test_http_completion_without_provider_terminal_stays_partial():
    capture, records = observer()
    capture("body", None, b'data: {"usage":{"prompt_tokens":10}}\n\n')
    capture("complete", None, b"")
    assert records[-1]["coverage"] == "partial"


def test_provider_error_after_usage_stays_partial():
    capture, records = observer("responses")
    capture(
        "body", None, b'data: {"response":{"usage":{"input_tokens":10}}}\n\n'
    )
    capture("body", None, b'data: {"type":"response.failed"}\n\n')
    capture("complete", None, b"")
    assert records[-1]["coverage"] == "partial"
