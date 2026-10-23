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

"""Exercise Codex's MCP tools over the turn's HTTP transport."""

import asyncio
import base64
import contextlib

import pytest

import a11
from a11.sdk import llm
from a11.sdk.interact_with_llm import (
    INTERACT_WITH_LLM_SCHEMA,
    interact_with_llm,
)
from a11.sdk.openai import interact_with_codex as mod
from a11.sdk.openai._codex_mcp import SERVER_NAME, serve_tools

pytest.importorskip("mcp")

import httpx2
import mcp
from mcp.client.streamable_http import streamable_http_client
from mcp.shared.exceptions import MCPError


@contextlib.asynccontextmanager
async def _client(overrides, env):
    prefix = f"mcp_servers.{SERVER_NAME}"
    token = env[overrides[f"{prefix}.bearer_token_env_var"]]
    async with httpx2.AsyncClient(
        headers={"Authorization": f"Bearer {token}"}
    ) as http:
        async with mcp.Client(
            streamable_http_client(overrides[f"{prefix}.url"], http_client=http)
        ) as client:
            yield client


@pytest.mark.asyncio
@pytest.mark.parametrize("second_fails", [False, True])
async def test_mcp_calls_run_concurrently_through_llm_router(
    monkeypatch, second_fails
):
    seen = []
    started = asyncio.Event()
    results = []
    declarations = []
    deadline = a11.now() + a11.Duration.seconds(30)
    registry = a11.ActionRegistry()
    schema = a11.ActionSchema(
        name="files#lookup",
        inputs={"key": a11.ActionPortSchema("key", "text/plain", unary=True)},
        outputs={
            "value": a11.ActionPortSchema("value", "text/plain", unary=True),
            "picture": a11.ActionPortSchema("picture", "image/png"),
        },
    )

    async def lookup(action):
        assert a11.get_deadline(action) == deadline
        key = await action["key"].consume(str)
        seen.append(key)
        if len(seen) == 2:
            started.set()
        await asyncio.wait_for(started.wait(), 2)
        await action.log(f"looked up {key}")
        if key == "second" and second_fails:
            raise a11.Status(
                code=a11.StatusCode.INVALID_ARGUMENT, message="lookup failed"
            ).to_exception()
        await action["value"].finalize(f"value of {key}")
        if key == "first":
            await action["picture"].finalize(b"PNG", mimetype="image/png")

    registry.register(schema.name, schema, lookup)

    async def fake_session(*args):
        config, tools, env, details = args[1], args[6], args[8], args[10]
        assert (
            0
            < config.config_overrides[
                f"mcp_servers.{SERVER_NAME}.tool_timeout_sec"
            ]
            <= 30
        )
        details["root_thread_id"] = b"root-thread"
        async with _client(config.config_overrides, env) as client:
            declarations.extend((await client.list_tools()).tools)
            results.extend(
                await asyncio.gather(
                    *[
                        client.call_tool(
                            "files_lookup",
                            {"key": key},
                            meta={
                                "callId": f"call-{key}",
                                "threadId": (
                                    "child-thread"
                                    if key == "second"
                                    else "root-thread"
                                ),
                            },
                        )
                        for key in ("first", "second")
                    ]
                )
            )
        assert tools[0]["inputSchema"] == declarations[0].input_schema
        return "Done.", "root-thread", None

    monkeypatch.setattr(mod, "_run_codex_session", fake_session)
    action = (
        a11.Action(INTERACT_WITH_LLM_SCHEMA)
        .bind_registry(registry)
        .bind_handler(interact_with_llm)
        .set_header(llm.LlmHeaders.PROVIDER.value, b"codex")
        .set_header(llm.LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"files#lookup")
    )
    a11.set_deadline_header(action, deadline)
    deadline = a11.get_deadline(action)
    action.run()
    original = llm.Interaction(
        role=llm.Role.USER,
        content=[a11.to_chunk({"role": "user", "content": "look up both"})],
    )
    await action["interactions"].finalize(original)
    await action["tools"].finalize()
    await action["config"].finalize({})
    produced = [item async for item in action["new_interactions"]]
    await action.wait()

    assert set(seen) == {"first", "second"}
    assert [tool.name for tool in declarations] == ["files_lookup"]
    assert not results[0].is_error
    assert results[1].is_error == second_fails
    assert results[0].content[0].text == '{"value":"value of first"}'
    assert base64.b64decode(results[0].content[1].data) == b"PNG"
    if second_fails:
        assert "lookup failed" in results[1].content[0].text
    assert len(produced) == 5
    assert produced[0].previous_interaction_id == original.id
    for previous, current in zip(produced, produced[1:]):
        assert current.previous_interaction_id == previous.id
    assert {call.id for item in produced for call in item.action_calls} == {
        "call-first",
        "call-second",
    }
    assert produced[-1].backend_specific_metadata["thread_id"] == b"root-thread"
    child = [
        item
        for item in produced
        if item.backend_specific_metadata.get("thread_id") == b"child-thread"
    ]
    assert len(child) == 2
    assert all(
        item.backend_specific_metadata["parent_thread_id"] == b"root-thread"
        for item in child
    )
    assert any(
        llm.TOOL_LOGS_METADATA_KEY in item.backend_specific_metadata
        for item in produced
    )


@pytest.mark.asyncio
async def test_mcp_endpoint_requires_its_token_and_closes_after_turn():
    async def handler(params, thread_id):
        return {"content": [{"type": "text", "text": thread_id}]}

    tools = [{"name": "lookup", "inputSchema": {"type": "object"}}]
    async with serve_tools(tools, handler, lambda: "root") as (overrides, env):
        url = overrides[f"mcp_servers.{SERVER_NAME}.url"]
        async with httpx2.AsyncClient() as http:
            response = await http.post(
                url, json={"jsonrpc": "2.0", "id": 1, "method": "tools/list"}
            )
            assert response.status_code == 401
        async with _client(overrides, env) as client:
            result = await client.call_tool("lookup", {})
            assert result.content[0].text == "root"
            with pytest.raises(MCPError, match="Unknown A11 tool"):
                await client.call_tool("unadvertised", {})
    async with httpx2.AsyncClient() as http:
        with pytest.raises(httpx2.ConnectError):
            await http.get(url)


@pytest.mark.asyncio
async def test_cancelling_turn_cancels_pending_mcp_calls():
    called = asyncio.Event()
    cancelled = asyncio.Event()
    finish = asyncio.Event()
    ready = asyncio.Future()

    async def handler(params, thread_id):
        called.set()
        try:
            await finish.wait()
        finally:
            cancelled.set()

    async def turn():
        async with serve_tools(
            [{"name": "lookup", "inputSchema": {"type": "object"}}],
            handler,
            lambda: "root",
        ) as connection:
            ready.set_result(connection)
            await finish.wait()

    serving = asyncio.create_task(turn())
    overrides, env = await ready
    async with _client(overrides, env) as client:
        request = asyncio.create_task(client.call_tool("lookup", {}))
        try:
            await asyncio.wait_for(called.wait(), 2)
            serving.cancel()
            await asyncio.gather(serving, return_exceptions=True)
            assert cancelled.is_set()
        finally:
            request.cancel()
            await asyncio.gather(request, return_exceptions=True)
