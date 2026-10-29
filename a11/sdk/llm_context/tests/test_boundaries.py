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

"""Pin projection, host scope, header, and completed-result contracts."""

import asyncio
import contextlib
import json

import pytest

import a11
from a11 import flow, net
from a11.sdk.llm_context import (
    LlmContextConfig,
    LlmContextServices,
    bind_services,
)
from a11.sdk.llm_context.actions import register
from a11.sdk.llm_context.config import LlmContextHeaders, config_for
from a11.sdk.llm_context.proxy import create_proxy, project_request
from a11.sdk.llm_context.retention import RetainedContentStore
from a11.sdk.llm_context.shell import intercept_shell_result, shell_hook
from a11.status import StatusCode, StatusException


@pytest.mark.parametrize(
    "mode,changed", [("off", False), ("observe", False), ("compress", True)]
)
def test_shell_modes_preserve_execution_and_original(mode, changed):
    raw = {"stdout": "x\n" * 1000, "stderr": "", "exit_code": 0, "cwd": "/tmp"}
    result = intercept_shell_result(raw, LlmContextConfig(shell={"mode": mode}))
    assert (result["view"]["stdout"] != raw["stdout"]) == changed
    assert result["original"] is raw
    assert result["view"]["cwd"] == raw["cwd"]
    assert result["view"]["exit_code"] == 0


@pytest.mark.parametrize(
    "extra",
    [
        {"exit_code": 1},
        {"interactive": True},
        {"complete": False},
        {"stdout": b"\xff"},
        {"truncated": True},
    ],
)
def test_shell_ineligible_results_pass_through(extra):
    raw = {"stdout": "x\n" * 1000, "stderr": "", "exit_code": 0, **extra}
    result = intercept_shell_result(
        raw, LlmContextConfig(shell={"mode": "compress"})
    )
    assert result["view"] == raw
    assert result["reports"][0]["skip_reason"]


def test_pre_execution_hook_advertises_unsupported_contract():
    event = {"command": "cat x | head", "approved": True}
    result = shell_hook(event, LlmContextConfig(shell={"mode": "compress"}))
    assert result["event"] == event
    assert not result["supported"]


@pytest.mark.asyncio
async def test_proxy_projects_only_editable_request_spans():
    body = json.dumps(
        {
            "model": "example",
            "messages": [
                {"role": "system", "content": "keep  spacing"},
                {"role": "user", "content": "{ " + " " * 200 + '"x": 1 }'},
                {
                    "role": "assistant",
                    "content": [
                        {
                            "type": "thinking",
                            "signature": "signed",
                            "thinking": "untouched",
                        }
                    ],
                },
            ],
            "unknown": {"vendor": 1},
        }
    ).encode()
    result = json.loads(
        await project_request(
            body,
            path="/v1/chat/completions",
            profile="chat",
            config=LlmContextConfig(input_content="lossless", min_gain=0),
        )
    )
    original = json.loads(body)
    assert result["messages"][0] == original["messages"][0]
    assert result["messages"][2] == original["messages"][2]
    assert result["unknown"] == original["unknown"]
    assert result["messages"][1]["content"] == '{"x":1}'
    assert (
        await project_request(
            body, path="/unknown", profile="chat", config=LlmContextConfig()
        )
        == body
    )


@pytest.mark.asyncio
async def test_retained_pages_survive_reopen_and_scope_isolation(tmp_path):
    scope = {"owner": "host", "conversation": "one"}
    owner = RetainedContentStore(tmp_path, owner="host")
    ref = await owner.retain("éllo world", scope)
    reopened = RetainedContentStore(tmp_path, owner="host")
    page = await reopened.retrieve(ref, scope, max_bytes=3)
    assert page["text"] == "él"
    assert page["next_cursor"] == 3
    with pytest.raises(StatusException) as error:
        await reopened.retrieve(ref, {**scope, "conversation": "two"})
    assert error.value.status.code == StatusCode.NOT_FOUND


@pytest.mark.asyncio
async def test_view_revision_conflict_is_explicit(tmp_path):
    store = RetainedContentStore(tmp_path, owner="host")
    scope = {"owner": "host", "conversation": "one"}
    first = await store.save_view(
        scope,
        {"history": []},
        source_digest="x",
        expected_revision=0,
        operation_id="op1",
    )
    assert first["revision"] == 1
    again = await store.save_view(
        scope,
        {"history": []},
        source_digest="x",
        expected_revision=0,
        operation_id="op1",
    )
    assert again == first
    with pytest.raises(StatusException) as error:
        await store.save_view(
            scope,
            {},
            source_digest="y",
            expected_revision=0,
            operation_id="op2",
        )
    assert error.value.status.code == StatusCode.ABORTED


def test_nested_headers_reuse_immutable_policy_and_changed_header_overrides():
    registry = a11.ActionRegistry()
    register(registry)
    parent = registry.make_action("llm_context_compress")
    parent.set_header(LlmContextHeaders.CONFIG, '{"input_content":"lossless"}')
    child = parent.make_nested(registry.get_schema("llm_context_compress"))
    assert config_for(parent) is config_for(child)
    child.set_header(LlmContextHeaders.CONFIG, '{"enabled":false}')
    assert not config_for(child).enabled
    assert config_for(parent).enabled


@pytest.mark.asyncio
async def test_flow_uses_same_utility_and_policy_headers():
    registry = a11.ActionRegistry()
    register(registry)
    program = flow.loads("""flow shrink {
      in request: object
      out result: object
      nodes scratch
      x = run llm_context_compress(request: request) via scratch
      x.result -> result
    }""")
    program.register_all(registry)
    result = await program.main.invoke(
        {"request": {"text": "x\n" * 1000}},
        registry=registry,
        headers={LlmContextHeaders.CONFIG: '{"strategy":"lines"}'},
    )
    assert result["result"]["report"]["changed"]


@pytest.mark.asyncio
async def test_proxy_relays_error_body_duplicate_headers_and_auth():
    observed = []

    async def upstream(request, response):
        observed.append(request)
        response.send_headers(
            429, [("set-cookie", "a=1"), ("set-cookie", "b=2")]
        )
        response.write(b"data: untouched\n\n")
        response.finish_with_trailers([("x-end", "yes")])

    origin = net.Http2Server.create("127.0.0.1", 0, upstream)
    proxy = create_proxy(f"http://127.0.0.1:{origin.port}")
    client = await net.Http2Client.connect("127.0.0.1", proxy.port)
    try:
        response = client.request_stream(
            "GET",
            "/v1/check",
            [("authorization", "Bearer test"), ("x-a11-secret", "private")],
        )
        head = await response.headers()
        chunks = [chunk async for chunk in response]
        trailers = await response.trailers()
        assert head.status == 429
        assert b"".join(chunks) == b"data: untouched\n\n"
        assert [(k, v) for k, v in head.headers if k == "set-cookie"] == [
            ("set-cookie", "a=1"),
            ("set-cookie", "b=2"),
        ]
        assert ("x-end", "yes") in trailers
        assert ("authorization", "Bearer test") in observed[0].headers
        assert not any(k.startswith("x-a11-") for k, _ in observed[0].headers)
    finally:
        client.close()
        proxy.stop()
        origin.stop()


@pytest.mark.asyncio
async def test_durable_operation_claim_prevents_duplicate_summary_after_reopen(
    tmp_path,
):
    from a11.sdk.llm_context.runtime import prepare_messages

    calls = []

    async def summarize(prefix):
        calls.append(prefix)
        return "Prior work is complete."

    policy = LlmContextConfig(
        retention=True,
        compaction="summary",
        summarizer="sum",
        max_tokens=200,
        reserve_tokens=0,
        retain_recent=1,
    )
    scope = {"owner": "host", "conversation": "one", "operation_id": "op1"}
    messages = [
        {"role": "user", "content": "old " * 1000},
        {"role": "user", "content": "now"},
    ]

    async def receiving_host():
        registry = a11.ActionRegistry()
        register(
            registry,
            services=LlmContextServices(
                retention=RetainedContentStore(tmp_path, owner="host"),
                authorize_scope=lambda action, selected: selected[
                    "conversation"
                ]
                == "one",
                summarizers={"sum": summarize},
            ),
        )

        async def present(action):
            projected = await prepare_messages(action, messages)
            await action["result"].finalize({"history": projected})

        registry.register(
            "llm_context_compact",
            registry.get_schema("llm_context_compact"),
            present,
        )
        action = registry.make_action("llm_context_compact")
        action.set_header(LlmContextHeaders.CONFIG, policy.model_dump_json())
        action.set_header(LlmContextHeaders.SCOPE, json.dumps(scope))
        action.run()
        await action["request"].finalize({})
        result = await action["result"].consume(dict)
        await action.wait()
        return result["history"]

    first = await receiving_host()
    second = await receiving_host()
    assert first == second
    assert len(calls) == 1


@pytest.mark.asyncio
async def test_pending_operation_does_not_repeat_work(tmp_path):
    store = RetainedContentStore(tmp_path, owner="host")
    scope = {"owner": "host", "conversation": "one"}
    assert (
        await store.claim_operation(
            scope, operation_id="op", source_digest="x", expected_revision=0
        )
        is None
    )
    reopened = RetainedContentStore(tmp_path, owner="host")
    with pytest.raises(StatusException) as error:
        await reopened.claim_operation(
            scope, operation_id="op", source_digest="x", expected_revision=0
        )
    assert error.value.status.code == StatusCode.FAILED_PRECONDITION


@pytest.mark.asyncio
async def test_expiry_releases_quota_and_removes_unavailable_data(
    tmp_path, monkeypatch
):
    from a11.sdk.llm_context import retention

    now = [100.0]
    monkeypatch.setattr(retention.time, "time", lambda: now[0])
    store = RetainedContentStore(
        tmp_path, owner="host", max_bytes=8, ttl_seconds=10
    )
    scope = {"owner": "host", "conversation": "one"}
    ref = await store.retain("12345678", scope)
    with pytest.raises(StatusException) as error:
        await store.retain("x", scope)
    assert error.value.status.code == StatusCode.RESOURCE_EXHAUSTED
    now[0] = 111.0
    assert await store.purge_expired() == 1
    with pytest.raises(StatusException):
        await store.retrieve(ref, scope)
    assert (await store.retain("new", scope))["owner"] == "host"


def test_configured_schema_default_and_explicit_off_use_normal_headers():
    from a11.sdk.llm_context import configure_registry

    registry = a11.ActionRegistry()
    register(registry)
    configure_registry(
        registry,
        LlmContextConfig(tool_results="lossless"),
        names=["llm_context_compress"],
    )
    action = registry.make_action("llm_context_compress")
    assert config_for(action).tool_results == "lossless"
    action.set_header(LlmContextHeaders.CONFIG, '{"enabled":false}')
    assert config_for(action).enabled is False


@pytest.mark.asyncio
async def test_relay_deadline_cancels_stream_without_retry():
    seen = []
    released = asyncio.Event()

    async def upstream(request, response):
        seen.append(request.path)
        response.send_headers(200, [])
        try:
            await response.wait_done()
        except StatusException:
            released.set()

    origin = net.Http2Server.create("127.0.0.1", 0, upstream)
    proxy = create_proxy(
        f"http://127.0.0.1:{origin.port}", timeout_seconds=0.05
    )
    client = await net.Http2Client.connect("127.0.0.1", proxy.port)
    try:
        response = client.request_stream("GET", "/slow")
        await response.headers()
        with pytest.raises(StatusException):
            await response.read()
        await asyncio.wait_for(released.wait(), 2)
        assert seen == ["/slow"]
    finally:
        client.close()
        proxy.stop()
        origin.stop()


@pytest.mark.asyncio
async def test_cancelling_relay_leaves_another_exchange_running():
    slow_finished = asyncio.Event()

    async def upstream(request, response):
        if request.path == "/slow":
            response.send_headers(200, [])
            try:
                await response.wait_done()
            except StatusException:
                slow_finished.set()
        else:
            response.send_response(201, [], b"other stream")

    origin = net.Http2Server.create("127.0.0.1", 0, upstream)
    proxy = create_proxy(f"http://127.0.0.1:{origin.port}")
    client = await net.Http2Client.connect("127.0.0.1", proxy.port)
    try:
        slow = client.request_stream("GET", "/slow")
        await slow.headers()
        other = client.request_stream("GET", "/other")
        slow.cancel()
        head = await other.headers()
        assert head.status == 201
        assert b"".join([chunk async for chunk in other]) == b"other stream"
        await asyncio.wait_for(slow_finished.wait(), 2)
    finally:
        client.close()
        proxy.stop()
        origin.stop()


@pytest.mark.asyncio
async def test_remote_utility_keeps_host_services_through_registry_copy():
    registry = a11.ActionRegistry()
    register(
        registry,
        services=LlmContextServices(
            summarizers={"sum": lambda prefix: "Prior objective"}
        ),
    )
    service = a11.Service(action_registry=registry)
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.ensure_future(service.accept(server_stream))
    client = a11.Session(action_registry=a11.ActionRegistry())
    await client.add_stream(client_stream, mode="start")
    try:
        call = (
            a11.Action(registry.get_schema("llm_context_compact"))
            .bind_node_map(client.node_map)
            .bind_session(client)
            .bind_stream(client_stream)
        )
        call.set_header(
            LlmContextHeaders.CONFIG,
            json.dumps(
                {
                    "summarizer": "sum",
                    "max_tokens": 200,
                    "reserve_tokens": 0,
                    "retain_recent": 1,
                }
            ),
        )
        await call.call()
        await call["request"].finalize(
            {
                "history": [
                    {"role": "user", "content": "old " * 1000},
                    {"role": "user", "content": "now"},
                ]
            }
        )
        result = await call["result"].consume(dict)
        await call.wait()
        assert result["checkpoint"] == "Prior objective"
    finally:
        client_stream.half_close()
        await client_stream.drain_outgoing_messages()
        service.abort(
            a11.Status(code=StatusCode.CANCELLED, message="test complete")
        )
        with contextlib.suppress(StatusException):
            await serving


@pytest.mark.asyncio
async def test_sdk_shell_presentation_preserves_raw_ports_and_command_status():
    from a11.sdk.llm_context.runtime import prepare_tool_result

    registry = a11.ActionRegistry()
    register(registry)
    policy = LlmContextConfig(shell={"mode": "compress"})
    raw = {
        "result": {"exit_code": 0, "output_lines": ["same line"] * 1000},
        "output_lines": ["same line"] * 1000,
    }

    async def present(action):
        text = json.dumps(raw, separators=(",", ":"))
        projected = await prepare_tool_result(
            action, text, source="run_command"
        )
        await action["result"].finalize({"text": projected})

    registry.register(
        "llm_context_shell", registry.get_schema("llm_context_shell"), present
    )
    action = registry.make_action("llm_context_shell")
    action.set_header(LlmContextHeaders.CONFIG, policy.model_dump_json())
    action.run()
    await action["request"].finalize({})
    result = json.loads((await action["result"].consume(dict))["text"])
    await action.wait()
    assert result["result"]["exit_code"] == 0
    view = result["result"]["output_lines"]
    assert view["representation"] == "application/json"
    from a11.sdk.llm_context import expand_content

    assert (
        json.loads(expand_content(view["view"]))
        == raw["result"]["output_lines"]
    )
    assert raw["result"]["output_lines"] == ["same line"] * 1000


@pytest.mark.asyncio
async def test_relay_streams_request_body_without_reconstructing_it():
    options = net.Http2Options()
    options.set_stream_request_body(True)
    received = []

    async def upstream(request, response):
        received.append(
            b"".join([chunk async for chunk in request.body_stream])
        )
        response.send_response(202, [], received[-1])

    origin = net.Http2Server.create("127.0.0.1", 0, upstream, options)
    proxy = create_proxy(f"http://127.0.0.1:{origin.port}", options=options)
    client = await net.Http2Client.connect("127.0.0.1", proxy.port)
    try:
        exchange = client.request_streaming_body(
            "POST", "/upload", [("content-type", "application/octet-stream")]
        )
        exchange.write(b"first\x00")
        exchange.write(b"second")
        exchange.finish()
        assert (await exchange.headers()).status == 202
        assert (
            b"".join([chunk async for chunk in exchange]) == b"first\x00second"
        )
        assert received == [b"first\x00second"]
    finally:
        client.close()
        proxy.stop()
        origin.stop()


@pytest.mark.asyncio
async def test_proxy_uses_host_scope_and_persists_recoverable_view(tmp_path):
    store = RetainedContentStore(tmp_path, owner="host")
    scope = {
        "owner": "host",
        "conversation": "authenticated",
        "branch": "main",
        "expected_revision": 0,
        "operation_id": "request-one",
    }
    observed = []

    async def upstream(request, writer):
        observed.append(json.loads(bytes(request.body)))
        writer.send_headers(200, [])
        writer.write(b"unchanged response")
        writer.finish()

    origin = net.Http2Server.create("127.0.0.1", 0, upstream)
    proxy = create_proxy(
        f"http://127.0.0.1:{origin.port}",
        config=LlmContextConfig(retention=True, input_content="lossless"),
        services=LlmContextServices(
            retention=store,
            resolve_scope=lambda request: scope,
            authorize_scope=lambda request, selected: selected["conversation"]
            == "authenticated",
        ),
    )
    client = await net.Http2Client.connect("127.0.0.1", proxy.port)
    try:
        payload = json.dumps(
            {
                "messages": [
                    {"role": "user", "content": "{ " + " " * 200 + '"x":1 }'}
                ]
            }
        ).encode()
        response = client.request_stream(
            "POST",
            "/v1/chat/completions",
            [
                ("content-type", "application/json"),
                ("x-a11-llm-context-scope", '{"owner":"attacker"}'),
            ],
            payload,
        )
        head = await response.headers()
        assert head.status == 200
        assert (
            b"".join([part async for part in response]) == b"unchanged response"
        )
        await response.wait_done()
        saved = await store.load_view(scope)
        assert saved["revision"] == 1
        assert saved["view"]["history"][0]["content"] == '{"x":1}'
        raw = await store.retrieve(saved["view"]["source"], scope)
        assert json.loads(raw["text"])[0]["content"] != '{"x":1}'
        assert observed[0]["messages"][0]["content"] == '{"x":1}'
    finally:
        client.close()
        proxy.stop()
        origin.stop()
