# Copyright 2026 The A11 Authors.

"""The gateway serves the flow tools, and a flow really runs on it.

The interesting one is the last test: a client sends *source*, not a call, and
the composition runs on the gateway against the gateway's own actions. That is
the whole proposition -- what crosses the wire is a flow and its declared
outputs, not every intermediate value.
"""

import asyncio
import pathlib

import pytest

import a11
from a11 import flow, net
from a11.gateway import app as gateway_app
from a11.gateway import config, conversations
from a11.gateway.ping import PING_SCHEMA
from a11.sdk import flow_tools
from a11.sdk.llm import LlmHeaders
from a11.service.session import Session
from a11.status import StatusCode, StatusException


def _registry(
    root: pathlib.Path, **overrides
) -> a11.ActionRegistry:
    settings = config.GatewayConfig(
        a11_port=0,
        conversation_store_root=root,
        audio_capture=False,
        speech_recognition=False,
        **overrides,
    )
    store = conversations.ConversationStore(root)
    return gateway_app._make_action_registry(settings, store)


@pytest.mark.asyncio
async def test_the_gateway_serves_the_flow_tools(tmp_path: pathlib.Path):
    registered = set(_registry(tmp_path).list_registered_actions())
    assert flow_tools.FLOW_TOOL_NAMES <= registered


@pytest.mark.asyncio
async def test_no_flow_tools_takes_them_off(tmp_path: pathlib.Path):
    registered = set(
        _registry(tmp_path, flow_tools=False).list_registered_actions()
    )
    assert not (flow_tools.FLOW_TOOL_NAMES & registered)
    # Nothing else went with them.
    assert "__ping" in registered


def test_the_cli_flag_reaches_the_config():
    """A flag nothing reads is worse than no flag."""
    import argparse

    from a11.cli.commands.gateway import GATEWAY_COMMAND

    parser = argparse.ArgumentParser()
    GATEWAY_COMMAND.configure(parser)
    assert config.GatewayConfig.from_args(parser.parse_args([])).flow_tools
    assert not config.GatewayConfig.from_args(
        parser.parse_args(["--no-flow-tools"])
    ).flow_tools


@pytest.mark.asyncio
async def test_a_client_sends_a_flow_and_the_gateway_runs_it(
    tmp_path: pathlib.Path,
):
    """One call carries a composition of the gateway's own actions."""
    gateway = gateway_app.A11Gateway(
        conversations.ConversationStore(tmp_path), _registry(tmp_path)
    )
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.create_task(gateway.handle_stream(server_stream))

    client = Session(action_registry=a11.ActionRegistry())
    await client.add_stream(client_stream, mode="start")

    call = (
        a11.Action(flow_tools.FLOW_RUN_SCHEMA)
        .bind_node_map(client.node_map)
        .bind_session(client)
        .bind_stream(client_stream)
    )
    call.set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"__ping")
    await call.call()

    async with (
        call["source"] as source,
        call["inputs"] as inputs,
        call["flow"] as which,
    ):
        await source.put_final(
            """
            flow echo-twice {
              in  word: string required
              out said: string stream
              first  = run __ping(input: word)
              second = run __ping(input: first.output)
              first.output  -> said
              second.output -> said
            }
            """
        )
        await inputs.put_final({"word": "hello"})
        await which.put_null_final()

    result = await asyncio.wait_for(
        call["result"].next_object(), timeout=30
    )
    await asyncio.wait_for(call.wait(), timeout=30)
    assert result == {"said": ["hello", "hello"]}

    serving.cancel()
    with pytest.raises(asyncio.CancelledError):
        await serving


@pytest.mark.asyncio
async def test_a_flow_on_the_gateway_calls_back_to_the_client(
    tmp_path: pathlib.Path,
):
    """`run` is the gateway's, `call` is the client's -- in one composition.

    The flow runs on the gateway, and says `call __ping`, so the action is
    dispatched back down the stream the flow arrived on and served by the
    *client's* handler. That is what lets a composition running over there use
    a microphone or a shell over here, and it is the reason `flow_run` hands
    the composition its stream: a `call` step's reply fragments route through
    it, and without one the call never returns.
    """
    gateway = gateway_app.A11Gateway(
        conversations.ConversationStore(tmp_path), _registry(tmp_path)
    )
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.create_task(gateway.handle_stream(server_stream))

    async def my_ping(action: a11.Action) -> None:
        word = await action["input"].consume(str, allow_none=True)
        await (await action["output"].put(f"the client answered {word!r}"))
        await action["output"].drain_and_close()

    # The gateway has its own `__ping` with a real handler. `call` still goes
    # to the peer, which is the whole point of saying it.
    mine = a11.ActionRegistry()
    mine.register(PING_SCHEMA.name, PING_SCHEMA, my_ping)
    client = Session(action_registry=mine)
    await client.add_stream(client_stream, mode="start")

    call = (
        a11.Action(flow_tools.FLOW_RUN_SCHEMA)
        .bind_node_map(client.node_map)
        .bind_session(client)
        .bind_stream(client_stream)
    )
    call.set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"__ping")
    await call.call()
    async with (
        call["source"] as source,
        call["inputs"] as inputs,
        call["flow"] as which,
    ):
        await source.put_final(
            """
            flow ask-the-client {
              in  word: string required
              out said: string stream
              theirs = call __ping(input: word)
              theirs.output -> said
            }
            """
        )
        await inputs.put_final({"word": "over here"})
        await which.put_null_final()

    result = await asyncio.wait_for(call["result"].next_object(), timeout=30)
    await asyncio.wait_for(call.wait(), timeout=30)
    assert result == {"said": ["the client answered 'over here'"]}

    serving.cancel()
    with pytest.raises(asyncio.CancelledError):
        await serving


@pytest.mark.asyncio
async def test_a_flows_outputs_reach_the_caller_as_they_are_produced(
    tmp_path: pathlib.Path,
):
    """A flow's outputs are nodes, so they arrive as the flow fills them.

    `result` collecting the lot is a convenience for a tool call; it must not
    be the only way to see what a composition is producing. Here the caller
    reads every value *before* the flow has finished, which is what makes a
    model's answer usable while it is still being written.
    """
    # An action that takes its time, so "arrived early" is a fact rather than a
    # race: the third value cannot exist until at least 3 x 50ms have passed.
    slow = a11.ActionSchema.model_validate(
        {
            "name": "__slow",
            "inputs": {"count": {"type": int, "unary": True}},
            "outputs": {"tick": {"type": str}},
        }
    )

    async def emit(action: a11.Action) -> None:
        count = await action["count"].consume(int, allow_none=True) or 0
        for index in range(count):
            await asyncio.sleep(0.05)
            await (await action["tick"].put(f"tick-{index}"))
        await action["tick"].drain_and_close()

    registry = _registry(tmp_path)
    registry.register("__slow", slow, emit)
    gateway = gateway_app.A11Gateway(
        conversations.ConversationStore(tmp_path), registry
    )
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.create_task(gateway.handle_stream(server_stream))

    client = Session(action_registry=a11.ActionRegistry())
    await client.add_stream(client_stream, mode="start")

    call = (
        a11.Action(flow_tools.FLOW_RUN_SCHEMA)
        .bind_node_map(client.node_map)
        .bind_session(client)
        .bind_stream(client_stream)
    )
    call.set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"__slow")
    await call.call()

    # Worked out from this call's own id: nothing has to be announced.
    said = client.node_map.get(
        flow_tools.flow_output_node_id(call.get_id(), "said")
    )

    async with (
        call["source"] as source,
        call["inputs"] as inputs,
        call["flow"] as which,
    ):
        await source.put_final(
            """
            flow echo-each {
              in  howmany: integer required
              out said:    string stream
              one = run __slow(count: howmany)
              one.tick -> said
            }
            """
        )
        await inputs.put_final({"howmany": 3})
        await which.put_null_final()

    # The first value, while the flow is certainly still running: two more
    # sleeps have to happen before it can finish, so this cannot be the
    # collected result arriving early.
    first = await asyncio.wait_for(said.next_object(), timeout=30)
    assert first == "tick-0"

    rest = [
        await asyncio.wait_for(said.next_object(), timeout=30)
        for _ in range(2)
    ]
    assert rest == ["tick-1", "tick-2"]

    result = await asyncio.wait_for(call["result"].next_object(), timeout=30)
    await asyncio.wait_for(call.wait(), timeout=30)
    assert result == {"said": ["tick-0", "tick-1", "tick-2"]}

    serving.cancel()
    with pytest.raises(asyncio.CancelledError):
        await serving


@pytest.mark.asyncio
async def test_a_client_runs_the_flow_itself_and_calls_the_gateway(
    tmp_path: pathlib.Path,
):
    """The other direction: the flow runs here, its calls run there.

    What `scripts/flow_playground.py` does. The client registers the action's
    *schema* and no handler -- it has the ports, and the work is the gateway's
    -- and the flow dispatches it over the session on that basis.
    """
    gateway = gateway_app.A11Gateway(
        conversations.ConversationStore(tmp_path), _registry(tmp_path)
    )
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.create_task(gateway.handle_stream(server_stream))

    here = a11.ActionRegistry()
    here.register(PING_SCHEMA.name, PING_SCHEMA)
    client = Session(action_registry=here)
    await client.add_stream(client_stream, mode="start")

    plan = flow.loads(
        """
        flow ask-the-gateway {
          in  word: string required
          out said: string stream
          echo = call __ping(input: word)
          echo.output -> said
        }
        """,
        "client.flow",
    ).main
    result = await asyncio.wait_for(
        plan.invoke(
            {"word": "over there"},
            registry=here,
            session=client,
            node_map=client.node_map,
            stream=client_stream,
        ),
        timeout=30,
    )
    assert result == {"said": ["over there"]}

    serving.cancel()
    with pytest.raises(asyncio.CancelledError):
        await serving


@pytest.mark.asyncio
async def test_a_client_can_run_two_flows_over_one_connection(
    tmp_path: pathlib.Path,
):
    """The second one has to reach the peer too.

    It did not: the flow's own action was given the session's stream, and a
    locally-run action that holds a stream ends it when it finishes -- after
    which the session dispatched nothing and said nothing about why. The
    stream belongs to the calls, not to the flow.
    """
    gateway = gateway_app.A11Gateway(
        conversations.ConversationStore(tmp_path), _registry(tmp_path)
    )
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.create_task(gateway.handle_stream(server_stream))

    here = a11.ActionRegistry()
    here.register(PING_SCHEMA.name, PING_SCHEMA)
    client = Session(action_registry=here)
    await client.add_stream(client_stream, mode="start")

    plan = flow.loads(
        """
        flow echo {
          in  word: string required
          out said: string stream
          heard = call __ping(input: word)
          heard.output -> said
        }
        """,
        "client.flow",
    ).main
    where = {
        "registry": here,
        "session": client,
        "node_map": client.node_map,
        "dispatch_stream": client_stream,
    }
    first = await asyncio.wait_for(plan.invoke({"word": "one"}, **where), 30)
    second = await asyncio.wait_for(plan.invoke({"word": "two"}, **where), 30)
    assert (first, second) == ({"said": ["one"]}, {"said": ["two"]})

    # And a plain dispatch still works after them, which is the same property
    # from the other side.
    call = (
        a11.Action(PING_SCHEMA)
        .bind_node_map(client.node_map)
        .bind_session(client)
        .bind_stream(client_stream)
    )
    await call.call()
    async with call["input"] as port:
        await port.put_final("three")
    assert [str(value) async for value in call["output"]] == ["three"]
    await asyncio.wait_for(call.wait(), timeout=30)

    serving.cancel()
    with pytest.raises(asyncio.CancelledError):
        await serving


@pytest.mark.asyncio
async def test_the_gateway_refuses_a_flow_that_reaches_too_far(
    tmp_path: pathlib.Path,
):
    """The allow-list still decides, even when the call is a whole flow."""
    gateway = gateway_app.A11Gateway(
        conversations.ConversationStore(tmp_path), _registry(tmp_path)
    )
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.create_task(gateway.handle_stream(server_stream))

    client = Session(action_registry=a11.ActionRegistry())
    await client.add_stream(client_stream, mode="start")

    call = (
        a11.Action(flow_tools.FLOW_RUN_SCHEMA)
        .bind_node_map(client.node_map)
        .bind_session(client)
        .bind_stream(client_stream)
    )
    call.set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"__ping")
    await call.call()

    async with (
        call["source"] as source,
        call["inputs"] as inputs,
        call["flow"] as which,
    ):
        await source.put_final(
            """
            flow sneak {
              in  command: string required
              out out:     string stream
              sh = run shell_execute(command: command)
              sh.output_lines -> out
            }
            """
        )
        await inputs.put_final({"command": "echo no"})
        await which.put_null_final()

    with pytest.raises(StatusException) as raised:
        await asyncio.wait_for(call.wait(), timeout=30)
    assert raised.value.status.code == StatusCode.PERMISSION_DENIED
    assert "shell_execute" in raised.value.status.message

    serving.cancel()
    with pytest.raises(asyncio.CancelledError):
        await serving
