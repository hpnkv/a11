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


#: An action the *client* serves, for the reverse-dispatch tests. Its name is an
#: ordinary one on purpose: A11's own `__`-prefixed actions are answered by
#: every registry and cannot be re-registered, so they cannot stand in for "a
#: tool the peer owns" -- which is what these tests need.
_CLIENT_TOOL = a11.ActionSchema(
    name="ask_the_human",
    description="Put a question to whoever is running the client.",
    inputs={
        "input": a11.ActionPortSchema(
            name="input", type="text/plain", typeinfo=str
        )
    },
    outputs={
        "output": a11.ActionPortSchema(
            name="output", type="text/plain", typeinfo=str
        )
    },
)


async def _send(
    call: a11.Action,
    source: str,
    *,
    inputs: dict | None = None,
    input_streams: list[str] | None = None,
    flow: str | None = None,
) -> None:
    """Send a `flow_run` call its source, and close every input port.

    Every one, including the ports this caller has nothing for: an input nobody
    writes and nobody closes is one the handler waits on, and with no deadline
    on the call it waits for good. Callers of a remote action own that, the way
    the LLM tool runner does for a model's.
    """
    await call["source"].finalize(source)
    await call["inputs"].finalize(inputs)
    await call["input_streams"].finalize(input_streams)
    await call["flow"].finalize(flow)


def _registry(root: pathlib.Path, **overrides) -> a11.ActionRegistry:
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
        a11
        .Action(flow_tools.FLOW_RUN_SCHEMA)
        .bind_node_map(client.node_map)
        .bind_session(client)
        .bind_stream(client_stream)
    )
    call.set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"__ping")
    await call.call()

    await _send(
        call,
        """
        flow echo-twice {
          in  word: string required
          out said: string stream
          first  = run __ping(input: word)
          second = run __ping(input: first.output)
          first.output  -> said
          second.output -> said
        }
        """,
        inputs={"word": "hello"},
    )

    result = await asyncio.wait_for(call["result"].next_object(), timeout=30)
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

    The flow runs on the gateway, and says `call ask_the_human`, so the action
    is dispatched back down the stream the flow arrived on and served by the
    *client's* handler. This allows a remote composition to use client-local
    resources such as a microphone or shell. `flow_run` supplies the stream
    used to route the `call` step's reply fragments.

    The two sides register the same name differently, which is the contract:
    the gateway holds its *schema* alone, so the step resolves and means "not
    here"; the client holds the handler, so the work happens there. Answering
    in the client's own words is what proves whose handler ran.
    """
    gateway_registry = _registry(tmp_path)
    gateway_registry.register(_CLIENT_TOOL.name, _CLIENT_TOOL)
    gateway = gateway_app.A11Gateway(
        conversations.ConversationStore(tmp_path), gateway_registry
    )
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.create_task(gateway.handle_stream(server_stream))

    async def answer(action: a11.Action) -> None:
        word = await action["input"].consume(str, allow_none=True)
        await action["output"].finalize(f"the client answered {word!r}")

    mine = a11.ActionRegistry()
    mine.register(_CLIENT_TOOL.name, _CLIENT_TOOL, answer)
    client = Session(action_registry=mine)
    await client.add_stream(client_stream, mode="start")

    call = (
        a11
        .Action(flow_tools.FLOW_RUN_SCHEMA)
        .bind_node_map(client.node_map)
        .bind_session(client)
        .bind_stream(client_stream)
    )
    call.set_header(
        LlmHeaders.ALLOWED_LLM_ACTIONS.value, _CLIENT_TOOL.name.encode()
    )
    await call.call()
    await _send(
        call,
        """
        flow ask-the-client {
          in  word: string required
          out said: string stream
          theirs = call ask_the_human(input: word)
          theirs.output -> said
        }
        """,
        inputs={"word": "over here"},
    )

    result = await asyncio.wait_for(call["result"].next_object(), timeout=30)
    await asyncio.wait_for(call.wait(), timeout=30)
    assert result == {"said": ["the client answered 'over here'"]}

    serving.cancel()
    with pytest.raises(asyncio.CancelledError):
        await serving


ECHO = a11.ActionSchema.model_validate({
    "name": "__echo",
    "inputs": {"text": {"type": str}},
    "outputs": {"echoed": {"type": str}},
})


async def _echo(action: a11.Action) -> None:
    """Answer each value as it arrives, so a reply proves the input arrived."""
    async for value in action["text"]:
        await (await action["echoed"].put(str(value).upper()))
    await action["echoed"].finalize()


#: A flow whose ports are one of each: a stream, and one value.
STREAMED_BOTH_WAYS = """
flow echo-each {
  in  words: string stream required
  in  once:  string
  out said:  string stream
  back = run __echo(text: words)
  back.echoed -> said
  once -> said
}
"""


async def _run_flow_call(client: Session, stream, allowed: bytes = b"__echo"):
    """A `flow_run` call on the client's session, dispatched and ready."""
    call = (
        a11
        .Action(flow_tools.FLOW_RUN_SCHEMA)
        .bind_node_map(client.node_map)
        .bind_session(client)
        .bind_stream(stream)
    )
    call.set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, allowed)
    await call.call()
    return call


def _port(client: Session, call, stream, port: str, *, write: bool):
    """One of the flow's own ports, by the id both ends work out."""
    node_id = (
        flow_tools.flow_input_node_id(call.get_id(), port)
        if write
        else flow_tools.flow_output_node_id(call.get_id(), port)
    )
    node = client.node_map.get(node_id)
    if write:
        # A port this end writes has to reach the other end.
        node.attach_stream(stream)
    return node


@pytest.mark.asyncio
async def test_a_port_named_on_input_streams_takes_values_as_it_runs(
    tmp_path: pathlib.Path,
):
    """The streaming contract, both directions, over a real connection.

    The client writes a value *after* the flow is running and reads the answer
    back *before* it closes the port. Neither half of that can be expressed by
    sending an object of values and waiting for another one -- and the collected
    `result` still lands at the end for callers that want the lot.
    """
    registry = _registry(tmp_path)
    registry.register(ECHO.name, ECHO, _echo)
    gateway = gateway_app.A11Gateway(
        conversations.ConversationStore(tmp_path), registry
    )
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.create_task(gateway.handle_stream(server_stream))

    client = Session(action_registry=a11.ActionRegistry())
    await client.add_stream(client_stream, mode="start")
    call = await _run_flow_call(client, client_stream)

    # Every port of the flow, at the id derived from this call's own: nothing is
    # announced, and subscribing before the source is sent misses nothing.
    said = _port(client, call, client_stream, "said", write=False)
    words = _port(client, call, client_stream, "words", write=True)
    once = _port(client, call, client_stream, "once", write=True)

    await _send(call, STREAMED_BOTH_WAYS, input_streams=["words", "once"])

    # One value in, one value back, with the port still open: the flow is
    # certainly still running, so this cannot be a collected result.
    await (await words.put("one"))
    assert (await asyncio.wait_for(said.next_object(), timeout=30)) == "ONE"
    await (await words.put("two"))
    assert (await asyncio.wait_for(said.next_object(), timeout=30)) == "TWO"

    # A port that carries one value is filled the same way, and closed the same
    # way. The close is the caller's: nothing else ends either port.
    await (await once.put("solo"))
    for node in (words, once):
        await node.finalize()

    result = await asyncio.wait_for(call["result"].next_object(), timeout=30)
    await asyncio.wait_for(call.wait(), timeout=30)
    assert result == {"said": ["ONE", "TWO", "solo"]}

    serving.cancel()
    with pytest.raises(asyncio.CancelledError):
        await serving


@pytest.mark.asyncio
async def test_a_streamed_port_does_not_lose_a_value_written_early(
    tmp_path: pathlib.Path,
):
    """A caller need not wait for anything before writing a port.

    The node holds what it was given from the moment it exists, on either end,
    so a client that writes its inputs and *then* sends the source is not racing
    the flow it is about to start.
    """
    registry = _registry(tmp_path)
    registry.register(ECHO.name, ECHO, _echo)
    gateway = gateway_app.A11Gateway(
        conversations.ConversationStore(tmp_path), registry
    )
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.create_task(gateway.handle_stream(server_stream))

    client = Session(action_registry=a11.ActionRegistry())
    await client.add_stream(client_stream, mode="start")
    call = await _run_flow_call(client, client_stream)

    words = _port(client, call, client_stream, "words", write=True)
    once = _port(client, call, client_stream, "once", write=True)
    # Written and closed before the gateway has even been told which flow to
    # run.
    for node, value in ((words, "early"), (once, "also early")):
        await (await node.put(value))
        await node.finalize()

    await _send(call, STREAMED_BOTH_WAYS, input_streams=["words", "once"])

    result = await asyncio.wait_for(call["result"].next_object(), timeout=30)
    await asyncio.wait_for(call.wait(), timeout=30)
    # Sorted, because `said` has two writers and two writers to one node
    # interleave by arrival. What is being tested is that neither value was
    # dropped for having been written too early.
    assert sorted(result["said"]) == ["EARLY", "also early"]

    serving.cancel()
    with pytest.raises(asyncio.CancelledError):
        await serving


@pytest.mark.asyncio
async def test_a_streamed_port_the_caller_never_closes_keeps_it_waiting(
    tmp_path: pathlib.Path,
):
    """The other side of "the caller owns the close", stated as a test."""
    registry = _registry(tmp_path)
    registry.register(ECHO.name, ECHO, _echo)
    gateway = gateway_app.A11Gateway(
        conversations.ConversationStore(tmp_path), registry
    )
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.create_task(gateway.handle_stream(server_stream))

    client = Session(action_registry=a11.ActionRegistry())
    await client.add_stream(client_stream, mode="start")
    call = await _run_flow_call(client, client_stream)
    words = _port(client, call, client_stream, "words", write=True)
    once = _port(client, call, client_stream, "once", write=True)

    await _send(call, STREAMED_BOTH_WAYS, input_streams=["words", "once"])

    await (await words.put("one"))
    with pytest.raises(asyncio.TimeoutError):
        await asyncio.wait_for(call.wait(), timeout=0.5)

    for node in (words, once):
        await node.finalize()
    await asyncio.wait_for(call.wait(), timeout=30)

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
    slow = a11.ActionSchema.model_validate({
        "name": "__slow",
        "inputs": {"count": {"type": int, "unary": True}},
        "outputs": {"tick": {"type": str}},
    })

    async def emit(action: a11.Action) -> None:
        count = await action["count"].consume(int, allow_none=True) or 0
        for index in range(count):
            await asyncio.sleep(0.05)
            await (await action["tick"].put(f"tick-{index}"))
        await action["tick"].finalize()

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
        a11
        .Action(flow_tools.FLOW_RUN_SCHEMA)
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

    await _send(
        call,
        """
        flow echo-each {
          in  howmany: integer required
          out said:    string stream
          one = run __slow(count: howmany)
          one.tick -> said
        }
        """,
        inputs={"howmany": 3},
    )

    # The first value, while the flow is certainly still running: two more
    # sleeps have to happen before it can finish, so this cannot be the
    # collected result arriving early.
    first = await asyncio.wait_for(said.next_object(), timeout=30)
    assert first == "tick-0"

    rest = [
        await asyncio.wait_for(said.next_object(), timeout=30) for _ in range(2)
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
    # `__ping` is a builtin, so the client registry already has the schema that
    # a remote `call` step requires.
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
    """Two flows reuse a connection because calls, not flows, own streams."""
    gateway = gateway_app.A11Gateway(
        conversations.ConversationStore(tmp_path), _registry(tmp_path)
    )
    server_stream, client_stream = net.create_in_process_wire_stream_pair()
    serving = asyncio.create_task(gateway.handle_stream(server_stream))

    here = a11.ActionRegistry()
    # `__ping` is a builtin, so the client registry already has the schema that
    # a remote `call` step requires.
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
        a11
        .Action(PING_SCHEMA)
        .bind_node_map(client.node_map)
        .bind_session(client)
        .bind_stream(client_stream)
    )
    await call.call()
    await call["input"].finalize("three")
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
        a11
        .Action(flow_tools.FLOW_RUN_SCHEMA)
        .bind_node_map(client.node_map)
        .bind_session(client)
        .bind_stream(client_stream)
    )
    call.set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"__ping")
    await call.call()

    await _send(
        call,
        """
        flow sneak {
          in  command: string required
          out out:     string stream
          sh = run shell_execute(command: command)
          sh.output_lines -> out
        }
        """,
        inputs={"command": "echo no"},
    )

    with pytest.raises(StatusException) as raised:
        await asyncio.wait_for(call.wait(), timeout=30)
    assert raised.value.status.code == StatusCode.PERMISSION_DENIED
    assert "shell_execute" in raised.value.status.message

    serving.cancel()
    with pytest.raises(asyncio.CancelledError):
        await serving
