# Copyright 2026 The A11 Authors.

"""The three Flow Actions, driven the way the LLM tool runner drives them.

The interesting cases are the ones where a composition meets the rules that
exist because a *model* wrote it: an action it may not call, a flow that will
not compile, and a check that must not run anything.
"""

import asyncio
import re

import pytest
import pytest_asyncio

from a11 import _native
from a11 import flow
from a11.actions import Action, ActionRegistry, ActionSchema
from a11.sdk import flow_tools
from a11.sdk.llm import LlmHeaders
from a11.status import StatusCode, StatusException

UPPER = ActionSchema.model_validate(
    {
        "name": "text-upper",
        "description": "Upper-case each value.",
        "inputs": {"text": {"name": "text", "type": str}},
        "outputs": {"upper": {"name": "upper", "type": str}},
    }
)
SIZE = ActionSchema.model_validate(
    {
        "name": "text-size",
        "description": "Count the characters of one value.",
        "inputs": {
            "text": {"name": "text", "type": str, "unary": True},
        },
        "outputs": {"size": {"name": "size", "type": int, "unary": True}},
    }
)
SECRET = ActionSchema.model_validate(
    {
        "name": "launch-missiles",
        "description": "Something the caller is not allowed to reach.",
        "inputs": {"text": {"name": "text", "type": str}},
        "outputs": {"done": {"name": "done", "type": str}},
    }
)


@pytest_asyncio.fixture
async def registry() -> ActionRegistry:
    """The flow tools, plus small actions for a flow to compose.

    Async on purpose: a registry built with no running loop hands out actions
    that never complete, so the fixture has to be inside the loop its tests run
    in.
    """
    fired: list[str] = []

    async def upper(action: Action) -> None:
        async for value in action["text"]:
            await (await action["upper"].put(value.upper()))

    async def size(action: Action) -> None:
        text = await action["text"].consume(str)
        await (await action["size"].put(len(text)))

    async def launch(action: Action) -> None:
        async for value in action["text"]:
            fired.append(value)
            await (await action["done"].put("fired"))

    built = ActionRegistry()
    flow_tools.register(built)
    built.register("text-upper", UPPER, upper)
    built.register("text-size", SIZE, size)
    built.register("launch-missiles", SECRET, launch)
    built.fired = fired  # type: ignore[attr-defined]
    return built


async def drive(
    registry: ActionRegistry,
    name: str,
    *,
    allowed: str | None = None,
    **inputs,
) -> Action:
    """Run one flow tool to completion, closing its inputs like the runner."""
    action = registry.make_action(name)
    if allowed is not None:
        action.set_header(
            LlmHeaders.ALLOWED_LLM_ACTIONS.value, allowed.encode()
        )
    # Claimed before the run, as the tool runner does: an unclaimed log goes to
    # the process sink and never materialises a node to read.
    action.get_log_node()
    action.run()
    for port, value in inputs.items():
        await action[port].put(value, final=True)
    for port in action.get_schema().inputs:
        await action[port].close()
    await asyncio.wait_for(action.wait(), timeout=30)
    return action


async def result_of(action: Action, port: str):
    """The one JSON value a flow tool writes."""
    return await action[port].next_object()


async def log_of(action: Action) -> str:
    """What a flow tool narrated, off its log port."""
    chunk = await asyncio.wait_for(
        action.get_log_node().next_chunk(), timeout=30
    )
    assert chunk is not None, "the tool narrated nothing"
    return _native.log_record_from_chunk(chunk)["text"]


# --- flow_actions -------------------------------------------------------------


@pytest.mark.asyncio
async def test_flow_actions_reports_the_ports_a_flow_needs(registry):
    action = await drive(registry, "flow_actions")
    described = await result_of(action, "actions")
    by_name = {one["action"]: one for one in described}

    # The output ports are the point: they are what a pipe needs on the left of
    # a `->`, and a tool definition does not carry them.
    assert by_name["text-upper"]["outputs"] == [
        {"port": "upper", "type": "str", "stream": True}
    ]
    assert by_name["text-upper"]["inputs"] == [
        {"port": "text", "type": "str", "stream": True}
    ]
    assert by_name["text-size"]["inputs"] == [{"port": "text", "type": "str"}]
    assert by_name["text-upper"]["description"] == "Upper-case each value."
    # A run log is for the person watching, not for a flow to pipe -- and it
    # cannot be offered here even by accident, because it is not a schema port.
    for described in by_name.values():
        ports = {port["port"] for port in described["inputs"]}
        ports |= {port["port"] for port in described["outputs"]}
        assert _native.ACTION_LOG_OUTPUT not in ports
    # The flow tools are not composable into a flow.
    assert not set(by_name) & set(flow_tools.FLOW_TOOL_NAMES)


@pytest.mark.asyncio
async def test_flow_actions_lists_only_what_the_caller_may_call(registry):
    action = await drive(registry, "flow_actions", allowed="text-.*")
    described = await result_of(action, "actions")
    assert sorted(one["action"] for one in described) == [
        "text-size",
        "text-upper",
    ]


# --- flow_run -----------------------------------------------------------------


COMPOSITION = """
flow shout-and-measure {
  in  words: string stream required
  out loud:  string stream
  out size:  integer

  up = run text-upper(text: words)
  counted = run text-size(text: up.upper | join " ")
  up.upper -> loud
  counted.size -> size
}
"""


@pytest.mark.asyncio
async def test_flow_run_composes_actions_and_returns_only_its_outputs(
    registry,
):
    action = await drive(
        registry,
        "flow_run",
        source=COMPOSITION,
        inputs={"words": ["one", "two"]},
    )
    assert await result_of(action, "result") == {
        "loud": ["ONE", "TWO"],
        "size": 7,
    }


@pytest.mark.asyncio
async def test_flow_run_takes_the_flow_it_is_told_to(registry):
    source = """
    flow first  { out a: string
                  "one" -> a }
    flow second { out a: string
                  "two" -> a }
    """
    default = await drive(registry, "flow_run", source=source)
    assert await result_of(default, "result") == {"a": "one"}

    chosen = await drive(registry, "flow_run", source=source, flow="second")
    assert await result_of(chosen, "result") == {"a": "two"}

    with pytest.raises(StatusException) as raised:
        await drive(registry, "flow_run", source=source, flow="third")
    assert raised.value.status.code == StatusCode.NOT_FOUND
    assert "third" in raised.value.status.message


@pytest.mark.asyncio
async def test_flow_run_narrates_its_run_for_the_person_watching(registry):
    action = await drive(
        registry, "flow_run", source=COMPOSITION, inputs={"words": ["hi"]}
    )
    log = await log_of(action)
    assert log.startswith("Ran the flow `shout-and-measure`")
    assert "text-upper" in log


# --- flow_run, with the ports the caller fills itself -------------------------


async def drive_streaming(
    registry: ActionRegistry,
    *,
    source: str,
    feed: dict[str, list] | None = None,
    flow_name: str | None = None,
    allowed: str | None = None,
) -> Action:
    """Run `flow_run` with its ports named on `input_streams` and written here.

    Which is the whole difference from [drive][]: there is no object of values to
    hand over, only ports to write and close -- one value or several, the same
    way either way.
    """
    action = registry.make_action("flow_run")
    if allowed is not None:
        action.set_header(
            LlmHeaders.ALLOWED_LLM_ACTIONS.value, allowed.encode()
        )
    action.get_log_node()
    action.run()
    await action["source"].put(source, final=True)
    if flow_name is not None:
        await action["flow"].put(flow_name, final=True)
    if feed:
        await action["input_streams"].put(sorted(feed), final=True)
    for port in action.get_schema().inputs:
        await action[port].close()

    for port, values in (feed or {}).items():
        node = action.get_node(
            flow_tools.flow_input_node_id(action.get_id(), port)
        )
        for value in values:
            await (await node.put(value))
        # The caller's close, because nothing else will do it.
        await node.finalize()

    await asyncio.wait_for(action.wait(), timeout=30)
    return action


@pytest.mark.asyncio
async def test_a_port_written_as_a_node_reaches_the_same_flow(registry):
    """The two ways of filling a port differ in who writes, in nothing else."""
    action = await drive_streaming(
        registry, source=COMPOSITION, feed={"words": ["one", "two"]}
    )
    assert await result_of(action, "result") == {
        "loud": ["ONE", "TWO"],
        "size": 7,
    }


@pytest.mark.asyncio
async def test_a_one_value_port_is_filled_the_same_way_as_a_stream(registry):
    """A port that carries one value is a stream that carries one."""
    source = """
    flow both {
      in  many: string stream required
      in  one:  string required
      out said: string stream

      up = run text-upper(text: many)
      up.upper -> said
      one -> said
    }
    """
    action = await drive_streaming(
        registry, source=source, feed={"many": ["a", "b"], "one": ["solo"]}
    )
    said = (await result_of(action, "result"))["said"]
    # `one` is written by the same put/close as `many`, and lands once.
    assert sorted(said) == ["A", "B", "solo"]


@pytest.mark.asyncio
async def test_a_value_and_a_node_can_fill_different_ports_of_one_flow(
    registry,
):
    """The two mechanisms are not exclusive; only per port are they."""
    source = """
    flow both {
      in  many: string stream required
      in  one:  string required
      out said: string stream

      up = run text-upper(text: many)
      up.upper -> said
      one -> said
    }
    """
    action = registry.make_action("flow_run")
    action.run()
    await action["source"].put(source, final=True)
    await action["inputs"].put({"many": ["a", "b"]}, final=True)
    await action["input_streams"].put(["one"], final=True)
    for port in action.get_schema().inputs:
        await action[port].close()
    node = action.get_node(
        flow_tools.flow_input_node_id(action.get_id(), "one")
    )
    await (await node.put("solo"))
    await node.finalize()
    await asyncio.wait_for(action.wait(), timeout=30)
    assert sorted((await result_of(action, "result"))["said"]) == [
        "A",
        "B",
        "solo",
    ]


@pytest.mark.asyncio
async def test_a_port_cannot_be_given_a_value_and_named_as_a_stream(registry):
    """One way in per port: the runtime says so, for every caller of it."""
    with pytest.raises(StatusException) as raised:
        action = registry.make_action("flow_run")
        action.run()
        await action["source"].put(COMPOSITION, final=True)
        await action["inputs"].put({"words": ["one"]}, final=True)
        await action["input_streams"].put(["words"], final=True)
        for port in action.get_schema().inputs:
            await action[port].close()
        await asyncio.wait_for(action.wait(), timeout=30)
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT
    assert "given a value and left open" in raised.value.status.message


@pytest.mark.asyncio
async def test_input_streams_must_be_a_list_of_port_names(registry):
    for named in ("words", [""], [1], ["words", "words"]):
        with pytest.raises(StatusException) as raised:
            action = registry.make_action("flow_run")
            action.run()
            await action["source"].put(COMPOSITION, final=True)
            await action["input_streams"].put(named, final=True)
            for port in action.get_schema().inputs:
                await action[port].close()
            await asyncio.wait_for(action.wait(), timeout=30)
        assert raised.value.status.code == StatusCode.INVALID_ARGUMENT, named


@pytest.mark.asyncio
async def test_the_run_log_says_which_ports_the_caller_filled(registry):
    action = await drive_streaming(
        registry, source=COMPOSITION, feed={"words": ["hi"]}
    )
    log = await log_of(action)
    assert log.startswith("Ran the flow `shout-and-measure`")
    assert "The caller filled words." in log


# --- what a model is not allowed to compose -----------------------------------


@pytest.mark.asyncio
async def test_a_flow_may_not_call_what_the_caller_may_not_call(registry):
    source = """
    flow sneak {
      in  order: string required
      out done:  string stream
      go = run launch-missiles(text: order)
      go.done -> done
    }
    """
    with pytest.raises(StatusException) as raised:
        await drive(
            registry,
            "flow_run",
            allowed="text-.*",
            source=source,
            inputs={"order": "now"},
        )
    assert raised.value.status.code == StatusCode.PERMISSION_DENIED
    assert "launch-missiles" in raised.value.status.message
    # Refused before anything ran, which is the only refusal worth having.
    assert registry.fired == []


@pytest.mark.asyncio
async def test_a_flow_may_not_call_the_flow_tools(registry):
    source = """
    flow recurse {
      in  again: string required
      out out:   string stream
      inner = run flow_run(source: again)
      inner.result -> out
    }
    """
    with pytest.raises(StatusException) as raised:
        await drive(
            registry, "flow_run", source=source, inputs={"again": "flow x {}"}
        )
    assert raised.value.status.code == StatusCode.PERMISSION_DENIED
    assert "flow_run" in raised.value.status.message




@pytest.mark.asyncio
async def test_without_the_header_nothing_here_restricts_the_calls(registry):
    """A script driving these handlers is not a model being kept to a list."""
    action = await drive(
        registry,
        "flow_run",
        source="""
        flow allowed {
          in  order: string required
          out done:  string stream
          go = run launch-missiles(text: order)
          go.done -> done
        }
        """,
        inputs={"order": "now"},
    )
    assert await result_of(action, "result") == {"done": ["fired"]}
    assert registry.fired == ["now"]


# --- flow_check ---------------------------------------------------------------


@pytest.mark.asyncio
async def test_flow_check_describes_the_composition_without_running_it(
    registry,
):
    action = await drive(
        registry,
        "flow_check",
        source="""
        flow careful {
          in  order: string required
          out done:  string stream
          go = run launch-missiles(text: order)
          go.done -> done
        }
        """,
    )
    described = await result_of(action, "plan")
    assert [one["flow"] for one in described["flows"]] == ["careful"]
    assert "run" in {step["step"] for step in described["flows"][0]["steps"]}
    # Nothing was dispatched, which is the whole point of checking first.
    assert registry.fired == []


@pytest.mark.asyncio
async def test_flow_check_refuses_the_same_calls_flow_run_would(registry):
    with pytest.raises(StatusException) as raised:
        await drive(
            registry,
            "flow_check",
            allowed="text-.*",
            source="""
            flow sneak {
              in  order: string required
              out done:  string stream
              go = run launch-missiles(text: order)
              go.done -> done
            }
            """,
        )
    assert raised.value.status.code == StatusCode.PERMISSION_DENIED


@pytest.mark.parametrize("tool", ["flow_check", "flow_run"])
@pytest.mark.asyncio
async def test_a_flow_that_will_not_compile_says_where(registry, tool: str):
    with pytest.raises(StatusException) as raised:
        await drive(
            registry, tool, source="flow broken {\n  in a: nonsense\n}"
        )
    status = raised.value.status
    assert status.code == StatusCode.INVALID_ARGUMENT
    assert "Unknown type" in status.message
    # The line and column are what a model needs to fix its own flow.
    assert re.search(r"flow:2:\d+:", status.message), status.message


@pytest.mark.parametrize("tool", ["flow_check", "flow_run"])
@pytest.mark.asyncio
async def test_the_source_is_required(registry, tool: str):
    with pytest.raises(StatusException) as raised:
        await drive(registry, tool, source="   ")
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT
    assert "source" in raised.value.status.message


# --- the skill ----------------------------------------------------------------


def test_the_skill_teaches_the_language_that_is_implemented():
    text = flow_tools.get_system_prompt()
    # The reference is the language's own, not a second copy of it.
    assert flow.REFERENCE.strip() in text
    for tool in flow_tools.FLOW_TOOL_NAMES:
        assert tool in text
    # What the skill does *not* teach: filling a port by node id, which needs a
    # session and a node map the model has no way to reach.
    assert "input_streams" not in text


def test_every_flow_the_skill_shows_compiles():
    """A skill that teaches a flow the compiler rejects is worse than none."""
    blocks = re.findall(
        r"```\n(flow [\s\S]*?)```", flow_tools.get_system_prompt()
    )
    assert blocks, "the skill shows no example flow"
    for block in blocks:
        program = flow.loads(block, "skill.flow")
        assert len(program) >= 1


def test_the_checked_in_skill_md_is_the_one_the_code_generates():
    """The file on disk is generated; this is what keeps it from drifting."""
    pytest.importorskip("yaml")
    written = flow_tools.SKILL_MD_PATH.read_text(encoding="utf-8")
    assert written == flow_tools.get_skill().to_skill_md()


def test_the_skill_md_reads_back_as_the_skill_it_came_from():
    pytest.importorskip("yaml")
    from a11.sdk.skill import Skill

    parsed = Skill.from_text(flow_tools.SKILL_MD_PATH.read_text())
    assert parsed.name == flow_tools.SKILL_NAME
    assert parsed.description == flow_tools.SKILL_DESCRIPTION
    assert parsed.body.strip() == flow_tools.get_system_prompt().strip()


@pytest.mark.asyncio
async def test_the_tools_are_offered_to_a_model_the_way_any_action_is(
    registry,
):
    from a11.sdk.llm_tools.runner import get_tool_definitions

    definitions = get_tool_definitions(
        registry, sorted(flow_tools.FLOW_TOOL_NAMES)
    )
    by_name = {one["name"]: one for one in definitions}
    assert set(by_name) == set(flow_tools.FLOW_TOOL_NAMES)
    run = by_name["flow_run"]["input_schema"]
    assert run["required"] == ["source"]
    assert set(run["properties"]) == {
        "source",
        "inputs",
        "flow",
        "input_streams",
    }
    # A model *can* see the port a client fills by node id, because a schema is
    # one document and a tool definition carries every input that is not
    # autofilled. What keeps it away is the skill, which teaches the other three
    # and never mentions this one -- see the skill test above. The port's own
    # description says the same thing for anyone reading the schema itself.
    streamed = flow_tools.FLOW_RUN_SCHEMA.inputs["input_streams"].description
    assert "model calling this as a tool -- wants `inputs`" in streamed


def test_a11_is_importable_without_the_yaml_the_skill_format_needs():
    """The three tools must not drag PyYAML in; only `get_skill` needs it."""
    import a11.sdk.flow_tools.handlers as handlers
    import a11.sdk.flow_tools.prompt as prompt

    assert "yaml" not in dir(handlers)
    assert "Skill" not in prompt.__dict__
