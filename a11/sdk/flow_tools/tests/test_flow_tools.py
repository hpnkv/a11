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

from a11 import flow
from a11.actions import Action, ActionRegistry, ActionSchema
from a11.sdk import flow_tools
from a11.sdk.llm import USER_FACING_LOG_PORT, LlmHeaders
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
            USER_FACING_LOG_PORT: {"name": USER_FACING_LOG_PORT, "type": str},
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
    action.run()
    for port, value in inputs.items():
        await action[port].put(value, final=True)
    for port in action.get_schema().inputs:
        await action[port].drain_and_close()
    await asyncio.wait_for(action.wait(), timeout=30)
    return action


async def result_of(action: Action, port: str):
    """The one JSON value a flow tool writes."""
    return await action[port].next_object()


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
    # A run log is for the person watching, not for a flow to pipe.
    assert USER_FACING_LOG_PORT not in {
        port["port"] for port in by_name["text-size"]["inputs"]
    }
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
    log = await action[USER_FACING_LOG_PORT].next_object(str)
    assert log.startswith("Ran the flow `shout-and-measure`")
    assert "text-upper" in log


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
    assert "Unknown port type" in status.message
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
    assert set(run["properties"]) == {"source", "inputs", "flow"}


def test_a11_is_importable_without_the_yaml_the_skill_format_needs():
    """The three tools must not drag PyYAML in; only `get_skill` needs it."""
    import a11.sdk.flow_tools.handlers as handlers
    import a11.sdk.flow_tools.prompt as prompt

    assert "yaml" not in dir(handlers)
    assert "Skill" not in prompt.__dict__
