"""Tests for the Flow language: compiling, and running on A11."""

import asyncio
import pathlib

import pytest
import pytest_asyncio

from a11 import flow, timing
from a11.actions import Action, ActionRegistry, ActionSchema
from a11.flow.lexer import FlowSyntaxError
from a11.net.wire_stream import OnDone, OnMessage, WireStream
from a11.nodes.async_node import NodeMap
from a11.status import Status, StatusCode, StatusException


def _schema(name: str, inputs: dict, outputs: dict) -> ActionSchema:
    return ActionSchema.model_validate(
        {"name": name, "inputs": inputs, "outputs": outputs}
    )


# --- Toy actions the flows below compose -------------------------------------


UPPER = _schema("text-upper", {"text": {"type": str}}, {"upper": {"type": str}})
SPLIT = _schema(
    "text-split",
    {"text": {"type": str, "unary": True}},
    {"words": {"type": str}},
)
SIZE = _schema(
    "text-size",
    {"text": {"type": str, "unary": True}},
    {"size": {"type": int, "unary": True}},
)
TOTAL = _schema(
    "sum-numbers",
    {"numbers": {"type": float}},
    {"total": {"type": float, "unary": True}},
)
NOISY = _schema(
    "noisy",
    {"text": {"type": str}},
    {"result": {"type": str}, "log": {"type": str}},
)
BOOM = _schema("boom", {"text": {"type": str}}, {"result": {"type": str}})
STEP = _schema(
    "agent-step",
    {"state": {"type": dict, "unary": True}},
    {"next": {"type": dict, "unary": True}},
)
MIMES = _schema(
    "show-mimetypes",
    {"values": {"type": str}},
    {"mimetypes": {"type": str}},
)
HEADERS = _schema(
    "show-header",
    {"text": {"type": str, "unary": True}},
    {"seen": {"type": str, "unary": True}},
)
PEEK = _schema(
    "header-peek",
    {"go": {"type": str, "unary": True}},
    {"seen": {"type": dict, "unary": True}},
)


@pytest_asyncio.fixture
async def registry() -> ActionRegistry:
    """A registry holding the small actions the test flows call.

    Async on purpose: registering an action wants a running event loop, and a
    sync fixture only finds one when some earlier test happened to leave it
    current -- which makes these tests pass alone and fail in a full run.
    """
    finished: list[str] = []

    async def upper(action: Action) -> None:
        async for value in action["text"]:
            await (await action["upper"].put(value.upper()))

    async def split(action: Action) -> None:
        text = await action["text"].consume(str)
        for word in text.split():
            await (await action["words"].put(word))

    async def size(action: Action) -> None:
        text = await action["text"].consume(str)
        await (await action["size"].put(len(text)))

    async def total(action: Action) -> None:
        running = 0.0
        async for value in action["numbers"]:
            running += float(value)
        await (await action["total"].put(running))

    async def noisy(action: Action) -> None:
        async for value in action["text"]:
            await (await action["result"].put(f"result:{value}"))
            for index in range(5):
                await (await action["log"].put(f"log:{value}:{index}"))
        finished.append("noisy")

    async def boom(action: Action) -> None:
        del action
        raise Status(
            code=StatusCode.DATA_LOSS, message="boom went the action"
        ).to_exception()

    async def agent_step(action: Action) -> None:
        state = await action.get_input("state").consume(dict)
        turns = int(state.get("turns", 0)) + 1
        await (
            await action.get_output("next").put(
                {
                    "turns": turns,
                    "done": turns >= 3,
                    "trace": state.get("trace", "") + ".",
                }
            )
        )

    async def show_mimetypes(action: Action) -> None:
        """Report how each value arrived, rather than what it was."""
        async for chunk in action["values"].iter_chunks():
            # The stream's own end marker says nothing about a value.
            if chunk.is_null():
                continue
            await (await action["mimetypes"].put(chunk.metadata.mimetype))

    async def show_header(action: Action) -> None:
        await action["text"].consume(str, allow_none=True)
        await (
            await action["seen"].put(
                action.get_header("x-passed-on", decode=True) or ""
            )
        )

    async def header_peek(action: Action) -> None:
        """Report every header this step was given, whatever it is called.

        `show_header` asks after one it knows the name of; this one is for
        `forward headers`, where the question is *which* headers arrived.
        Framework headers are left out: A11 forwards every ``x-a11-`` one to a
        nested action of its own accord, and they would drown the answer.
        """
        await action["go"].consume(str, allow_none=True)
        seen = {
            name: value.decode()
            for name, value in dict(action.headers).items()
            if not name.lower().startswith("x-a11-")
        }
        await (await action["seen"].put(seen))

    built = ActionRegistry()
    built.register("header-peek", PEEK, header_peek)
    built.register("text-upper", UPPER, upper)
    built.register("text-split", SPLIT, split)
    built.register("text-size", SIZE, size)
    built.register("sum-numbers", TOTAL, total)
    built.register("noisy", NOISY, noisy)
    built.register("boom", BOOM, boom)
    built.register("agent-step", STEP, agent_step)
    built.register("show-mimetypes", MIMES, show_mimetypes)
    built.register("show-header", HEADERS, show_header)
    built.finished = finished  # type: ignore[attr-defined]
    return built


async def run_flow(
    source: str,
    registry: ActionRegistry,
    inputs: dict | None = None,
    *,
    headers: dict | None = None,
    **keyword_inputs,
):
    """Compile ``source``, register it, and invoke its first flow."""
    program = flow.loads(source, "test.flow")
    program.register_all(registry)
    return await program.main.invoke(
        {**(inputs or {}), **keyword_inputs},
        registry=registry,
        headers=headers,
    )


# --- Compiling ---------------------------------------------------------------


EXAMPLES = pathlib.Path(__file__).parents[3] / "examples" / "003-flow-dsl"


@pytest.mark.parametrize(
    "path", sorted(EXAMPLES.glob("*.flow")), ids=lambda path: path.name
)
def test_the_example_flows_compile(path: pathlib.Path):
    program = flow.load(path)
    assert len(program) >= 1
    for compiled in program:
        assert compiled.schema.name == compiled.name


def test_ports_headers_and_description_become_an_action_schema():
    program = flow.loads(
        """
        flow greet {
          describe "Say hello."
          in  name:  string required "Who to greet."
          out lines: string stream
          header "x-a11-deadline" as deadline
        }
        """,
        "greet.flow",
    )
    schema = program["greet"].schema
    assert schema.name == "greet"
    assert schema.description == "Say hello."
    assert schema.inputs["name"].unary and schema.inputs["name"].required
    assert schema.inputs["name"].typeinfo is str
    assert schema.inputs["name"].description == "Who to greet."
    assert not schema.outputs["lines"].unary
    assert "x-a11-deadline" in schema.headers


def test_a_flow_describes_itself_as_data():
    program = flow.loads("""
        flow work {
          in  q: string
          out a: string
          step = run text-upper(text: q)
          step.upper | first 1 -> a
          skip step.other
        }
        """)
    described = program["work"].describe()
    assert described["flow"] == "work"
    assert described["inputs"]["q"] == {
        "type": "string",
        "unary": True,
        "required": False,
        "description": "",
    }
    kinds = [step["step"] for step in described["steps"]]
    # A described step names the verb it was written with, not just "a call".
    assert kinds == ["run", "pipe", "pipe", "skip"]
    assert described["steps"][2]["from"] == "step.upper | first 1"


def test_a_port_says_what_it_is_after_it_says_what_it_holds():
    """`stream` and `required` follow the type, in either order."""
    program = flow.loads("""
        flow shapes {
          in  passes:  object stream required
          in  reversed: object required stream
          in  one:     string required
          out plain:   string
        }
        """)
    schema = program["shapes"].schema
    for name in ("passes", "reversed"):
        assert not schema.inputs[name].unary
        assert schema.inputs[name].required
    assert schema.inputs["one"].unary and schema.inputs["one"].required
    assert schema.outputs["plain"].unary
    assert not schema.outputs["plain"].required


def test_a_port_type_may_be_generic_or_a_serialisation_tag():
    program = flow.loads("""
        flow typed {
          in  names:  list[string]
          in  frames: list[a11.NodeFragment] stream
          out audio:  a11.sdk.AudioBuffer stream
          out raw:    "application/x-msgpack"
        }
        """)
    schema = program["typed"].schema
    # A container still carries the Python type its JSON schema comes from.
    assert schema.inputs["names"].typeinfo is list
    # A tag is the name a registry knows the type by, and travels as written --
    # the module defining it need not be imported to compile the flow.
    assert schema.outputs["audio"].type == "a11.sdk.AudioBuffer"
    assert schema.outputs["raw"].type == "application/x-msgpack"
    described = program["typed"].describe()
    assert described["inputs"]["frames"]["type"] == "list[a11.NodeFragment]"


@pytest.mark.parametrize(
    ("source", "message"),
    [
        (
            "flow f { in a: stream string\n }",
            "'stream' follows the type: write 'a: TYPE stream'",
        ),
        ("flow f { in a: list[string, object]\n }", "type parameter"),
        ("flow f { in a: string[object]\n }", "type parameter"),
        ("flow f { in a: list[wat]\n }", "Unknown port type"),
        ("flow f { in a: a11.sdk.AudioBuffer[string]\n }", "type parameter"),
        ("flow f { in a: list[string\n }", "Expected ']'"),
    ],
)
def test_a_badly_written_type_says_so(source: str, message: str):
    with pytest.raises(FlowSyntaxError) as raised:
        flow.loads(source, "bad.flow")
    assert message in str(raised.value)


@pytest.mark.parametrize(
    ("source", "message"),
    [
        ("flow f { out a: string\n missing -> a }", "Unknown name"),
        ("flow f { in a: string\n a -> a }", "cannot be written"),
        ("flow f { in a: string\n a | nope -> a }", "Unknown stage"),
        ("flow f { in a: wat\n }", "Unknown port type"),
        (
            (
                "flow f { in a: string\n x = run text-upper(text: a)\n"
                " x.upper -> a }"
            ),
            "cannot be written",
        ),
        (
            "flow f { in a: string\n out b: string\n a -> b b -> b }",
            "one statement per line",
        ),
        ("flow f { in a: string\n wait nobody }", "Unknown name"),
        ("flow f { in a: string\n s <- a }", "no repeat here"),
        ("flow f { in a: string", "missing its closing"),
    ],
)
def test_bad_flows_report_where_and_why(source: str, message: str):
    with pytest.raises(FlowSyntaxError) as raised:
        flow.loads(source, "bad.flow")
    assert message in str(raised.value)
    assert raised.value.source_name == "bad.flow"
    assert raised.value.line >= 1
    assert raised.value.to_status().code == StatusCode.INVALID_ARGUMENT


INNER = """
    flow inner { in a: string
                 out b: string }
"""

OUTER = """
    flow outer { in a: string
                 out b: string
                 x = run inner(nope: a)
                 x.b -> b }
"""


@pytest.mark.parametrize(
    ("note", "source"),
    [
        ("callee first", INNER + OUTER),
        ("caller first", OUTER + INNER),
    ],
)
def test_a_calls_ports_are_checked_against_a_flow_in_the_same_program(
    note: str, source: str
):
    """Whichever way round they are written: a program is not a header file."""
    del note
    with pytest.raises(FlowSyntaxError) as raised:
        flow.loads(source)
    assert "has no input port 'nope'" in str(raised.value)


def test_a_flow_that_calls_itself_is_checked_like_any_other_call():
    """A flow is in its own program, so its own ports are known to it."""
    with pytest.raises(FlowSyntaxError) as raised:
        flow.loads("""
            flow loop { in a: string
                        out b: string
                        x = run loop(nope: a)
                        x.b -> b }
            """)
    assert "has no input port 'nope'" in str(raised.value)


def test_a_pipeline_may_wrap_across_lines():
    program = flow.loads("""
        flow wrapped {
          in  words: string stream
          out out:   string
          words
            | where it != "skip"
            | join " "
            -> out
        }
        """)
    assert len(program["wrapped"].root.steps) == 1


# --- Running -----------------------------------------------------------------


@pytest.mark.asyncio
async def test_a_flow_pipes_one_action_into_another(registry):
    result = await run_flow(
        """
        flow shout-words {
          in  sentence: string
          out words:    string stream
          out loudest:  string

          split = run text-split(text: sentence)
          shout = run text-upper(text: split.words)
          shout.upper -> words
          shout.upper | first 1 -> loudest
        }
        """,
        registry,
        sentence="hello wide world",
    )
    assert result["words"] == ["HELLO", "WIDE", "WORLD"]
    assert result["loudest"] == "HELLO"


@pytest.mark.asyncio
async def test_a_literal_and_a_path_are_streams_too(registry):
    result = await run_flow(
        """
        flow constants {
          out greeting: string
          out picked:   string
          hello = run text-upper(text: "hi there")
          hello.upper -> greeting
          {"a": {"b": "deep"}}.a.b -> picked
        }
        """,
        registry,
    )
    assert result["greeting"] == "HI THERE"
    assert result["picked"] == "deep"


@pytest.mark.asyncio
async def test_stages_reshape_a_stream(registry):
    result = await run_flow(
        """
        flow shapes {
          in  words: string stream
          out kept:     string stream
          out numbered: object stream
          out joined:   string
          out counted:  number
          out batched:  list stream
          out clipped:  string stream

          words | where it != "no" -> kept
          words | map {"word": it, "size": len(it)} -> numbered
          words | join "," -> joined
          (words | count) -> counted
          words | batch 2 -> batched
          words | truncate 2 -> clipped
        }
        """,
        registry,
        words=["one", "no", "three"],
    )
    assert result["kept"] == ["one", "three"]
    assert result["numbered"] == [
        {"word": "one", "size": 3},
        {"word": "no", "size": 2},
        {"word": "three", "size": 5},
    ]
    assert result["joined"] == "one,no,three"
    assert result["counted"] == 3
    assert result["batched"] == [["one", "no"], ["three"]]
    assert result["clipped"] == ["on", "no", "th"]


@pytest.mark.asyncio
async def test_first_stops_reading_without_stalling_the_producer(registry):
    result = await run_flow(
        """
        flow peek {
          in  words: string stream
          out head:  string
          noise = run noisy(text: words)
          noise.result | first 1 -> head
          skip noise.log
        }
        """,
        registry,
        words=["a", "b", "c"],
    )
    assert result["head"] == "result:a"
    assert registry.finished == ["noisy"]


@pytest.mark.asyncio
async def test_an_output_nobody_mentions_is_drained_anyway(registry):
    result = await run_flow(
        """
        flow quiet {
          in  words: string stream
          out head:  string
          noise = run noisy(text: words)
          noise.result | first 1 -> head
        }
        """,
        registry,
        words=["a", "b"],
    )
    assert result["head"] == "result:a"
    # The action wrote five log values per input that the flow never asked for;
    # it could only have finished if somebody read them.
    assert registry.finished == ["noisy"]


@pytest.mark.asyncio
async def test_a_stream_can_fan_out_to_several_destinations(registry):
    result = await run_flow(
        """
        flow twice {
          in  words: string stream
          out a: string stream
          out b: string stream
          shout = run text-upper(text: words)
          shout.upper -> a, b
        }
        """,
        registry,
        words=["x", "y"],
    )
    assert result["a"] == ["X", "Y"]
    assert result["b"] == ["X", "Y"]


@pytest.mark.asyncio
async def test_several_writers_share_one_port_and_close_it_once(registry):
    result = await run_flow(
        """
        flow both {
          in  words: string stream
          out total: number
          add = run sum-numbers(numbers: words | map len(it))
          100 -> add.numbers
          add.total -> total
        }
        """,
        registry,
        words=["abc"],
    )
    assert result["total"] == 103.0


@pytest.mark.asyncio
async def test_for_each_runs_a_body_per_value(registry):
    result = await run_flow(
        """
        flow sizes {
          in  words: string stream
          out sizes: object stream
          for word in words {
            measure = run text-size(text: word)
            measure.size | map {"word": word, "size": it, "at": index} -> sizes
          }
        }
        """,
        registry,
        words=["a", "bb", "ccc"],
    )
    assert result["sizes"] == [
        {"word": "a", "size": 1, "at": 0},
        {"word": "bb", "size": 2, "at": 1},
        {"word": "ccc", "size": 3, "at": 2},
    ]


@pytest.mark.asyncio
async def test_for_each_can_run_passes_in_parallel(registry):
    result = await run_flow(
        """
        flow parallel-sizes {
          in  words: string stream
          out sizes: number stream
          for word in words parallel 3 {
            measure = run text-size(text: word)
            measure.size -> sizes
          }
        }
        """,
        registry,
        words=["a", "bb", "ccc", "dddd"],
    )
    assert sorted(result["sizes"]) == [1, 2, 3, 4]


@pytest.mark.asyncio
async def test_a_loop_body_rereads_an_outer_stream_on_every_pass(registry):
    result = await run_flow(
        """
        flow cross {
          in  words:  string stream
          in  suffix: string
          out pairs:  string stream
          for word in words {
            word | map text(it) -> pairs
            suffix -> pairs
          }
        }
        """,
        registry,
        words=["a", "b"],
        suffix="!",
    )
    # One pass at a time, and the outer `suffix` is replayed for both of them.
    # Within a pass the two pipes race, so only the passes are ordered.
    assert sorted(result["pairs"][:2]) == ["!", "a"]
    assert sorted(result["pairs"][2:]) == ["!", "b"]


@pytest.mark.asyncio
async def test_repeat_carries_a_value_until_a_condition_holds(registry):
    result = await run_flow(
        """
        flow think {
          out turns: number stream
          out trace: string
          repeat state = {"turns": 0, "trace": ""} max 8 {
            step = run agent-step(state: state)
            state <- step.next
            until step.next.done
            step.next.turns -> turns
            if step.next.done {
              step.next.trace -> trace
            }
          }
        }
        """,
        registry,
    )
    assert result["turns"] == [1, 2, 3]
    assert result["trace"] == "..."


@pytest.mark.asyncio
async def test_repeat_stops_at_its_bound(registry):
    result = await run_flow(
        """
        flow bounded {
          out seen: number stream
          repeat state = {"turns": 0} max 2 {
            step = run agent-step(state: state)
            state <- step.next
            step.next.turns -> seen
          }
        }
        """,
        registry,
    )
    assert result["seen"] == [1, 2]


@pytest.mark.asyncio
async def test_a_branch_reads_a_called_actions_status(registry):
    result = await run_flow(
        """
        flow tolerate {
          in  words:   string stream
          out outcome: string
          risky = try run boom(text: words)
          if risky.status.ok {
            "went fine" -> outcome
          } else {
            risky.status.message -> outcome
          }
        }
        """,
        registry,
        words=["x"],
    )
    assert result["outcome"] == "boom went the action"


@pytest.mark.asyncio
async def test_a_failing_call_fails_the_flow_unless_it_says_try(registry):
    with pytest.raises(StatusException) as raised:
        await run_flow(
            """
            flow strict {
              in  words: string stream
              out out:   string
              risky = run boom(text: words)
              risky.result -> out
            }
            """,
            registry,
            words=["x"],
        )
    assert raised.value.status.code == StatusCode.DATA_LOSS


@pytest.mark.asyncio
async def test_fail_ends_the_flow_with_a_status(registry):
    with pytest.raises(StatusException) as raised:
        await run_flow(
            """
            flow refuse {
              in  words: string stream
              out out:   string
              if (words | count) > 1 {
                fail resource-exhausted "too many words"
              }
              words -> out
            }
            """,
            registry,
            words=["a", "b"],
        )
    assert raised.value.status.code == StatusCode.RESOURCE_EXHAUSTED
    assert raised.value.status.message == "too many words"


@pytest.mark.asyncio
async def test_a_flow_can_call_another_flow_in_the_same_program(registry):
    program = flow.loads(
        """
        flow loud {
          in  text: string stream
          out out:  string stream
          shout = run text-upper(text: text)
          shout.upper -> out
        }

        flow twice-loud {
          in  text: string stream
          out out:  string stream
          once = run loud(text: text)
          again = run loud(text: once.out)
          again.out -> out
        }
        """,
        "nested.flow",
    )
    program.register_all(registry)
    result = await program["twice-loud"].invoke(
        {"text": ["hi"]}, registry=registry
    )
    assert result["out"] == ["HI"]


@pytest.mark.asyncio
async def test_headers_reach_a_called_action(registry):
    program = flow.loads("""
        flow forward {
          out seen: string
          header "x-tenant" as tenant default "anonymous"
          inner = run show-header(text: "ignored") with "x-passed-on": tenant
          inner.seen -> seen
        }
        """)
    program.register_all(registry)
    named = await program.main.invoke(
        registry=registry, headers={"x-tenant": "acme"}
    )
    assert named["seen"] == "acme"
    assert (await program.main.invoke(registry=registry))["seen"] == "anonymous"


@pytest.mark.asyncio
async def test_after_and_wait_order_two_calls(registry):
    order: list[str] = []

    async def record(action: Action) -> None:
        name = await action["text"].consume(str)
        order.append(f"start:{name}")
        await asyncio.sleep(0.02)
        order.append(f"end:{name}")
        await (await action["upper"].put(name))

    registry.register(
        "record", UPPER.model_copy(update={"name": "record"}), record
    )
    await run_flow(
        """
        flow ordered {
          out done: string stream
          first = run record(text: "one")
          second = run record(text: "two") after first
          first.upper -> done
          second.upper -> done
        }
        """,
        registry,
    )
    assert order == ["start:one", "end:one", "start:two", "end:two"]


@pytest.mark.asyncio
async def test_a_named_drain_gates_a_later_step(registry):
    order: list[str] = []

    async def slowly(action: Action) -> None:
        await action["text"].consume(str, allow_none=True)
        for value in ("1", "2", "3"):
            await asyncio.sleep(0.01)
            await (await action["upper"].put(value))
        order.append("written")

    async def gate(action: Action) -> None:
        order.append("gated")
        await (await action["upper"].put(await action["text"].consume(str)))

    registry.register(
        "slowly", UPPER.model_copy(update={"name": "slowly"}), slowly
    )
    registry.register("gate", UPPER.model_copy(update={"name": "gate"}), gate)
    result = await run_flow(
        """
        flow landed {
          out out: string
          source = run slowly(text: "go")
          add = run sum-numbers(numbers: source.upper)
          landed = drain add.numbers
          gated = run gate(text: "ok") after landed
          gated.upper -> out
        }
        """,
        registry,
    )
    assert result["out"] == "ok"
    # The gate could only start once everything written into the summing
    # action's port had landed, which is only true once its writer was done.
    assert order == ["written", "gated"]


@pytest.mark.asyncio
async def test_cancel_stops_a_call(registry):
    async def forever(action: Action) -> None:
        await action["text"].consume(str, allow_none=True)
        await asyncio.Event().wait()

    registry.register(
        "forever", UPPER.model_copy(update={"name": "forever"}), forever
    )
    result = await run_flow(
        """
        flow give-up {
          out outcome: string
          slow = try run forever(text: "wait")
          quick = run text-upper(text: "now")
          cancel slow after quick
          quick.upper -> outcome
        }
        """,
        registry,
    )
    assert result["outcome"] == "NOW"


@pytest.mark.asyncio
async def test_an_input_the_flow_never_feeds_is_closed_for_the_callee(registry):
    async def optional(action: Action) -> None:
        text = await action["text"].consume(str, allow_none=True)
        await (await action["upper"].put(f"got:{text}"))

    registry.register(
        "optional", UPPER.model_copy(update={"name": "optional"}), optional
    )
    result = await run_flow(
        """
        flow unfed {
          out out: string
          probe = run optional()
          probe.upper -> out
        }
        """,
        registry,
    )
    assert result["out"] == "got:None"


@pytest.mark.asyncio
async def test_naming_a_port_the_action_does_not_have_says_so(registry):
    with pytest.raises(StatusException) as raised:
        await run_flow(
            """
            flow wrong {
              out out: string
              x = run text-upper(text: "hi")
              x.nope -> out
            }
            """,
            registry,
        )
    assert raised.value.status.code == StatusCode.NOT_FOUND
    assert "no output port 'nope'" in raised.value.status.message


@pytest.mark.asyncio
async def test_calling_something_nobody_registered_says_so(registry):
    with pytest.raises(StatusException) as raised:
        await run_flow(
            """
            flow missing {
              out out: string
              x = run not-a-thing(text: "hi")
              x.upper -> out
            }
            """,
            registry,
        )
    assert raised.value.status.code == StatusCode.NOT_FOUND
    assert "not registered here" in raised.value.status.message


@pytest.mark.asyncio
async def test_a_call_without_a_peer_says_which_flow_and_action(registry):
    """`call` goes to the session even when a handler is sitting right here.

    The verb is the dispatch, not a hint: `text-upper` is registered with a
    handler in this very registry, and saying `call` still puts it on the
    stream the flow is attached to -- which, there being no peer, is where it
    fails. Anything else would make the two verbs mean the same thing whenever
    the local registry happened to be full.
    """
    with pytest.raises(StatusException) as raised:
        await run_flow(
            """
            flow away {
              out out: string
              x = call text-upper(text: "hi")
              x.upper -> out
            }
            """,
            registry,
        )
    assert "could not call 'text-upper'" in raised.value.status.message


@pytest.mark.asyncio
async def test_run_needs_a_handler_and_says_so_when_there_is_none(registry):
    """The other half: `run` will not quietly go to the session instead."""
    registry.register(
        "elsewhere", _schema("elsewhere", {}, {"out": {"type": str}})
    )
    with pytest.raises(StatusException) as raised:
        await run_flow(
            """
            flow here {
              out out: string
              x = run elsewhere()
              x.out -> out
            }
            """,
            registry,
        )
    assert raised.value.status.code is StatusCode.FAILED_PRECONDITION
    assert "has no handler to run" in raised.value.status.message
    assert "Say 'call'" in raised.value.status.message


@pytest.mark.asyncio
async def test_an_action_registered_for_its_schema_alone_is_called_away(
    registry,
):
    """Schema here, work over there: how a flow composes a peer's actions.

    A client writing a flow against a gateway's actions has their schemas --
    the SDK ships them -- and no handlers, which is exactly what says "this
    one is not mine to run". Without a peer it still fails, but on the
    dispatch rather than on the name: reaching that message is the point.
    """
    registry.register("text-upper", UPPER)  # schema, no handler
    with pytest.raises(StatusException) as raised:
        await run_flow(
            """
            flow away {
              out out: string
              x = call text-upper(text: "hi")
              x.upper -> out
            }
            """,
            registry,
        )
    assert "could not call 'text-upper'" in raised.value.status.message
    assert "not registered here" not in raised.value.status.message


@pytest.mark.asyncio
async def test_while_and_else_if_read_the_way_they_look(registry):
    result = await run_flow(
        """
        flow classify {
          in  words: string stream
          out size:  string
          out seen:  number stream
          if (words | count) > 2 {
            "many" -> size
          } else if (words | count) == 2 {
            "two" -> size
          } else {
            "few" -> size
          }
          repeat state = {"turns": 0} max 5 {
            step = run agent-step(state: state)
            state <- step.next
            while not step.next.done
            step.next.turns -> seen
          }
        }
        """,
        registry,
        words=["a", "b"],
    )
    assert result["size"] == "two"
    assert result["seen"] == [1, 2, 3]


@pytest.mark.asyncio
async def test_mime_and_distinct_and_json_filter_a_stream(registry):
    result = await run_flow(
        """
        flow filtered {
          in  words:  string stream
          out kept:   string stream
          out unique: string stream
          out parsed: list stream
          words | mime "application/json" -> kept
          words | distinct -> unique
          "[1, 2]" | json -> parsed
        }
        """,
        registry,
        words=["x", "x", "y"],
    )
    assert result["kept"] == ["x", "x", "y"]
    assert result["unique"] == ["x", "y"]
    assert result["parsed"] == [[1, 2]]


@pytest.mark.asyncio
async def test_then_reads_one_stream_after_another(registry):
    """Order, which two writers to one node cannot give you."""
    result = await run_flow(
        """
        flow conversation {
          in  history: string stream required
          in  said:    string required
          out sent:    string stream

          asked = node()
          said | map upper(it) -> asked

          history | then asked -> sent
        }
        """,
        registry,
        history=["one", "two"],
        said="and now?",
    )
    assert result["sent"] == ["one", "two", "AND NOW?"]


@pytest.mark.asyncio
async def test_then_chains_and_reads_a_calls_port(registry):
    result = await run_flow(
        """
        flow three {
          in  words: string stream required
          out all:   string stream
          shout = run text-upper(text: words)
          "first" | then shout.upper | then "last" -> all
        }
        """,
        registry,
        words=["a", "b"],
    )
    assert result["all"] == ["first", "A", "B", "last"]


@pytest.mark.asyncio
async def test_group_accumulates_until_a_value_closes_the_group(registry):
    """What `batch` does by counting, `group` does by asking."""
    result = await run_flow(
        """
        flow sentences {
          in  pieces: string stream required
          out said:   string stream
          pieces
            | group ends-with(trim(it), [".", "?", "!"])
            | map trim(join(it, " "))
            -> said
        }
        """,
        registry,
        pieces=["um", "so", "what is a fiber?", "a stack", "one you park."],
    )
    # The fragments before the closing piece are part of the sentence, which
    # is the whole point: a piece is not a sentence.
    assert result["said"] == [
        "um so what is a fiber?",
        "a stack one you park.",
    ]


@pytest.mark.asyncio
async def test_a_group_still_open_at_the_end_is_given_up(registry):
    result = await run_flow(
        """
        flow trailing {
          in  pieces: string stream required
          out said:   string stream
          pieces | group ends-with(it, ".") | map join(it, " ") -> said
        }
        """,
        registry,
        pieces=["one.", "and", "then"],
    )
    assert result["said"] == ["one.", "and then"]


@pytest.mark.asyncio
async def test_ends_with_takes_any_of_several_endings(registry):
    result = await run_flow(
        """
        flow enders {
          in  words: string stream required
          out kept:  string stream
          words | where ends-with(it, ["?", "!"]) -> kept
        }
        """,
        registry,
        words=["a.", "b?", "c!", "d"],
    )
    assert result["kept"] == ["b?", "c!"]


@pytest.mark.asyncio
async def test_a_value_can_be_made_into_a_registered_type(registry):
    """Both spellings, and the chunk builtin that makes the content."""
    # A tag resolves to a type this process has been told about, and importing
    # the module is what tells it. A flow cannot import anything itself, which
    # is what keeps the set of types the host's decision rather than the
    # flow's.
    from a11.sdk.llm import Interaction  # noqa: F401

    result = await run_flow(
        """
        flow typed {
          in  text:  string required
          out role:  string
          out said:  string
          out plain: string

          spoken = node()
          a11.sdk.Interaction{
            role: "user",
            content: [to_chunk({
              role: "user",
              content: [{type: "text", text: text}]
            })]
          } -> spoken

          spoken | map text(it.role) -> role
          spoken | map from_chunk(it.content[0]).content[0].text -> said
          ({"role": "model"} as a11.sdk.Interaction)
            | map text(it.role) -> plain
        }
        """,
        registry,
        text="what is a fiber?",
    )
    assert result["role"] == "user"
    assert result["said"] == "what is a fiber?"
    # The cast validates as well as fills in: `Role.ASSISTANT` is "model", and
    # a role the type does not have would have failed here.
    assert result["plain"] == "model"


@pytest.mark.asyncio
async def test_a_cast_to_a_built_in_type_coerces_the_way_the_builtin_does(
    registry,
):
    result = await run_flow(
        """
        flow coerced {
          out n:     number
          out t:     string
          out items: list
          "42" as number -> n
          7 as string -> t
          ["1", "2"] as list[number] -> items
        }
        """,
        registry,
    )
    assert result["n"] == 42
    assert result["t"] == "7"
    assert result["items"] == [1, 2]


@pytest.mark.asyncio
async def test_a_type_nothing_here_knows_says_so(registry):
    with pytest.raises(StatusException) as raised:
        await run_flow(
            """
            flow unknown {
              out out: string
              {"a": 1} as not.a.Type | map text(it) -> out
            }
            """,
            registry,
        )
    assert "Nothing here knows the type 'not.a.Type'" in (
        raised.value.status.message
    )


def test_a_brace_after_a_name_still_opens_a_block():
    """`if step.next.done {` cannot become a typed value; Go says so too."""
    program = flow.loads("""
        flow blocks {
          in  step: object required
          out out:  string
          if step.next.done {
            "done" -> out
          } else {
            "not yet" -> out
          }
          for one in step.items {
            skip one
          }
        }
        """)
    kinds = [step["step"] for step in program.main.describe()["steps"]]
    assert kinds == ["if", "for"]


def test_a_typed_value_is_still_reachable_in_a_condition():
    """Inside brackets a '{' cannot be a block, so the restriction lifts."""
    program = flow.loads("""
        flow guarded {
          out out: string
          if (a11.sdk.Interaction{role: "user"}).role == "user" {
            "yes" -> out
          }
        }
        """)
    assert program.main.name == "guarded"


@pytest.mark.asyncio
async def test_packb_writes_values_as_messagepack(registry):
    """`packb` says how a value travels, not what it is."""
    result = await run_flow(
        """
        flow packed {
          in  words: string stream
          out how:   string stream
          out plain: string stream
          seen = run show-mimetypes(values: words | packb)
          seen.mimetypes -> how
          words -> plain
        }
        """,
        registry,
        words=["one", "two"],
    )
    assert result["how"] == ["application/x-msgpack"] * 2
    # The values themselves are unchanged: only their encoding is.
    assert result["plain"] == ["one", "two"]


@pytest.mark.asyncio
async def test_packb_leaves_a_value_that_is_already_packed_alone(registry):
    """A second `packb` is a no-op, tag and all."""

    async def pack(action: Action) -> None:
        from a11.data.serialization import (
            MSGPACK_MIMETYPE,
            get_global_serialization_registry,
        )

        chunk = get_global_serialization_registry().to_chunk(
            b"already", MSGPACK_MIMETYPE
        )
        await (await action["packed"].put_chunk(chunk))

    registry.register(
        "pack-bytes",
        _schema("pack-bytes", {}, {"packed": {"type": bytes}}),
        pack,
    )
    result = await run_flow(
        """
        flow twice {
          out how: string stream
          source = run pack-bytes()
          seen = run show-mimetypes(values: source.packed | packb | packb)
          seen.mimetypes -> how
        }
        """,
        registry,
    )
    assert result["how"] == ["application/x-msgpack;type=bytes"]


@pytest.mark.asyncio
async def test_wait_with_a_timeout_gives_up(registry):
    async def slow(action: Action) -> None:
        await action["text"].consume(str, allow_none=True)
        await asyncio.sleep(30)

    registry.register(
        "sleepy", UPPER.model_copy(update={"name": "sleepy"}), slow
    )
    with pytest.raises(StatusException) as raised:
        await run_flow(
            """
            flow impatient {
              out out: string
              slow = try run sleepy(text: "hi")
              wait slow timeout 50ms
              "unreachable" -> out
            }
            """,
            registry,
        )
    assert raised.value.status.code == StatusCode.DEADLINE_EXCEEDED
    assert "Waiting for slow timed out" in raised.value.status.message


@pytest.mark.asyncio
async def test_a_call_can_be_given_an_id_and_its_own_node_map(registry):
    outputs, node_map, _ = await _run_with_runtime(
        """
        flow named {
          out out: string
          nodes scratch {
            x = run text-upper(text: "hi") via scratch id "fixed-id"
          }
          x.upper -> out
        }
        """,
        registry,
    )
    assert outputs["out"] == "HI"
    assert Action.make_node_id("fixed-id", "upper") not in node_map


# --- Node maps and traffic ---------------------------------------------------


class _RecordingStream(WireStream):
    """A wire stream that remembers what a flow tried to send over it."""

    def __init__(self) -> None:
        super().__init__()
        self.sent: list = []

    def send(self, message) -> None:
        self.sent.append(message)

    async def start(self, on_message: OnMessage, on_done: OnDone) -> None:
        return

    async def accept(self, on_message: OnMessage, on_done: OnDone) -> None:
        return

    def half_close(self, trailers=None) -> None:
        return

    async def drain_outgoing_messages(self) -> None:
        return

    def abort(self, status: Status) -> None:
        return

    def set_deadline(self, deadline=None) -> None:
        return

    @property
    def deadline(self):
        return timing.infinite_future()

    def get_status(self) -> Status:
        return Status.ok()

    def get_trailers(self):
        return None

    def get_id(self) -> str:
        return "recording"

    def get_impl(self):
        return None

    def node_ids(self) -> set[str]:
        return {
            fragment.id
            for message in self.sent
            for fragment in message.node_fragments
        }


async def _run_with_runtime(source: str, registry: ActionRegistry, **inputs):
    program = flow.loads(source, "traffic.flow")
    program.register_all(registry)
    node_map = NodeMap()
    stream = _RecordingStream()
    outputs = await program.main.invoke(
        inputs, registry=registry, node_map=node_map, stream=stream
    )
    return outputs, node_map, stream


@pytest.mark.asyncio
async def test_a_run_steps_streams_stay_off_the_wire(registry):
    outputs, node_map, stream = await _run_with_runtime(
        """
        flow summarise {
          in  words:   string stream
          out summary: string
          shout = run text-upper(text: words)
          shout.upper | join "," -> summary
        }
        """,
        registry,
        words=["a", "b"],
    )
    assert outputs["summary"] == "A,B"
    sent = stream.node_ids()
    # The flow's own output is what the caller asked for and is teed; the
    # intermediate nodes of the step it ran locally are not.
    assert any(name.endswith("summary") for name in sent)
    assert not any("shout" in name or "upper" in name for name in sent)


@pytest.mark.asyncio
async def test_a_nodes_block_keeps_a_steps_nodes_out_of_the_session(registry):
    outputs, node_map, _ = await _run_with_runtime(
        """
        flow private {
          in  words:   string stream
          out summary: string
          nodes scratch {
            shout = run text-upper(text: words)
          }
          shout.upper | join "," -> summary
        }
        """,
        registry,
        words=["a", "b"],
    )
    assert outputs["summary"] == "A,B"
    assert not [name for name in _node_names(node_map) if "upper" in name]


@pytest.mark.asyncio
async def test_without_a_nodes_block_a_steps_nodes_are_in_the_session(registry):
    outputs, node_map, _ = await _run_with_runtime(
        """
        flow shared {
          in  words:   string stream
          out summary: string
          shout = run text-upper(text: words)
          shout.upper | join "," -> summary
        }
        """,
        registry,
        words=["a", "b"],
    )
    assert outputs["summary"] == "A,B"
    assert [name for name in _node_names(node_map) if "upper" in name]


def _node_names(node_map: NodeMap) -> list[str]:
    # NodeMap has no iteration protocol; the flow's nodes are named after the
    # nested action and its port, so probing the ids the test cares about is
    # enough to tell whether the step's nodes landed in the session's map.
    found = []
    for suffix in ("upper", "text", "summary"):
        for prefix in _seen_action_ids:
            candidate = Action.make_node_id(prefix, suffix)
            if candidate in node_map:
                found.append(candidate)
    return found


_seen_action_ids: list[str] = []


@pytest.fixture(autouse=True)
def _remember_action_ids(monkeypatch):
    """Remember the ids A11 hands out, so node-map probes can find them."""
    original = Action.make_nested

    def make_nested(self, *args, **kwargs):
        nested = original(self, *args, **kwargs)
        _seen_action_ids.append(nested.id)
        return nested

    monkeypatch.setattr(Action, "make_nested", make_nested)
    _seen_action_ids.clear()
    yield


# --- Casing ------------------------------------------------------------------


@pytest.mark.asyncio
async def test_a_flow_may_be_written_entirely_in_upper_case(registry):
    result = await run_flow(
        """
        FLOW shouted {
          DESCRIBE "Every keyword in upper case."
          IN  words: STRING STREAM REQUIRED
          OUT loud:  STRING STREAM
          OUT total: NUMBER

          say = RUN text-upper(text: words)
          say.upper | WHERE it != "SKIP" -> loud
          (words | COUNT) -> total
        }
        """,
        registry,
        words=["a", "skip", "b"],
    )
    assert result["loud"] == ["A", "B"]
    assert result["total"] == 3


@pytest.mark.asyncio
async def test_upper_case_reaches_the_newer_statements_too(registry):
    result = await run_flow(
        """
        FLOW loud-recovery {
          IN  words:   STRING STREAM
          OUT outcome: STRING
          OUT kept:    STRING STREAM

          held = NODE()
          risky = TRY RUN boom(text: words)
          check = WAIT risky
          IF NOT check.ok {
            check.code -> outcome
          } ELSE {
            "fine" -> outcome
          }
          words -> held
          held -> kept
        }
        """,
        registry,
        words=["x"],
    )
    assert result["outcome"] == "DATA_LOSS"
    assert result["kept"] == ["x"]


def test_mixed_case_is_a_name_and_not_a_keyword():
    # `TRUE` is the literal; `True` is a name, and nothing is called that.
    assert flow.loads("flow f { out a: bool\n TRUE -> a }")
    with pytest.raises(FlowSyntaxError) as raised:
        flow.loads("flow f { out a: bool\n True -> a }", "mixed.flow")
    assert "Unknown name 'True'" in str(raised.value)


def test_a_port_carries_one_value_unless_it_says_stream():
    schema = flow.loads("""
        flow shapes {
          in  single: string
          in  several: string stream
          out answer: object
        }
        """)["shapes"].schema
    assert schema.inputs["single"].unary
    assert not schema.inputs["several"].unary
    assert schema.outputs["answer"].unary


# --- Failing with a status ---------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("written", "code"),
    [
        ('fail resource_exhausted "nope"', StatusCode.RESOURCE_EXHAUSTED),
        ('fail RESOURCE_EXHAUSTED "nope"', StatusCode.RESOURCE_EXHAUSTED),
        ('fail 8 "nope"', StatusCode.RESOURCE_EXHAUSTED),
        ('fail "nope"', StatusCode.INTERNAL),
    ],
)
async def test_fail_takes_a_canonical_code_by_name_or_by_number(
    registry, written: str, code: StatusCode
):
    with pytest.raises(StatusException) as raised:
        await run_flow(
            f"""
            flow refuse {{
              out out: string
              {written}
              "unreachable" -> out
            }}
            """,
            registry,
        )
    assert raised.value.status.code == code
    assert raised.value.status.message == "nope"


@pytest.mark.asyncio
async def test_a_computed_code_is_accepted(registry):
    with pytest.raises(StatusException) as raised:
        await run_flow(
            """
            flow refuse {
              in  code: number
              out out:  string
              fail code "computed"
              "unreachable" -> out
            }
            """,
            registry,
            code=7,
        )
    assert raised.value.status.code == StatusCode.PERMISSION_DENIED


def test_an_unknown_code_word_is_rejected_while_compiling():
    with pytest.raises(FlowSyntaxError) as raised:
        flow.loads('flow f { out a: string\n fail wat "x" }')
    assert "Unknown status code 'wat'" in str(raised.value)


# --- Statuses, and recovering from them --------------------------------------


@pytest.mark.asyncio
async def test_a_status_is_a_record_a_flow_can_read(registry):
    result = await run_flow(
        """
        flow look {
          in  words:   string stream
          out ok:      bool
          out code:    string
          out number:  number
          out message: string

          risky = try run boom(text: words)
          check = wait risky
          check.ok      -> ok
          check.code    -> code
          check.number  -> number
          check.message -> message
        }
        """,
        registry,
        words=["x"],
    )
    assert result == {
        "ok": False,
        "code": "DATA_LOSS",
        "number": int(StatusCode.DATA_LOSS),
        "message": "boom went the action",
    }


@pytest.mark.asyncio
async def test_the_status_keyword_reads_the_same_outcome(registry):
    result = await run_flow(
        """
        flow look {
          in  words: string stream
          out code:  string
          out same:  bool

          risky = try run boom(text: words)
          status risky.code -> code
          (status risky).ok -> same
        }
        """,
        registry,
        words=["x"],
    )
    assert result["code"] == "DATA_LOSS"
    assert result["same"] is False


@pytest.mark.asyncio
async def test_a_recovered_failure_can_be_translated_and_re_raised(registry):
    with pytest.raises(StatusException) as raised:
        await run_flow(
            """
            flow recover {
              in  words: string stream
              out out:   string

              risky = try run boom(text: words)
              check = wait risky
              if check.ok {
                "went fine" -> out
              } else {
                fail unavailable check.message
              }
            }
            """,
            registry,
            words=["x"],
        )
    assert raised.value.status.code == StatusCode.UNAVAILABLE
    assert raised.value.status.message == "boom went the action"


@pytest.mark.asyncio
async def test_a_status_can_be_raised_exactly_as_it_arrived(registry):
    with pytest.raises(StatusException) as raised:
        await run_flow(
            """
            flow rethrow {
              in  words: string stream
              out out:   string
              risky = try run boom(text: words)
              check = wait risky
              if not check.ok {
                fail check
              }
              "unreachable" -> out
            }
            """,
            registry,
            words=["x"],
        )
    assert raised.value.status.code == StatusCode.DATA_LOSS
    assert raised.value.status.message == "boom went the action"


@pytest.mark.asyncio
async def test_waiting_on_a_try_call_does_not_fail_the_flow(registry):
    """`try` says the flow will handle it, and `wait` is how it finds out."""
    result = await run_flow(
        """
        flow patient {
          in  words: string stream
          out out:   string
          risky = try run boom(text: words)
          wait risky
          "carried on" -> out
        }
        """,
        registry,
        words=["x"],
    )
    assert result["out"] == "carried on"


# --- Flows running flows -----------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("note", "order"),
    [
        ("callee first", ("inner", "outer")),
        ("caller first", ("outer", "inner")),
    ],
)
async def test_a_flow_runs_a_sibling_that_is_in_no_registry(
    registry, note, order
):
    """A program is a scope: `run` finds a flow declared beside the caller.

    Nothing registers these. A composition arrives as one text and its
    declarations resolve against each other, so a flow may be factored into
    two without either half having to exist as a deployed action first --
    which is the whole reason `flow_run` takes source rather than a name.
    Declaration order does not matter either; a program is not a header file.
    """
    del note
    bodies = {
        "inner": """
        flow inner {
          in  t: string stream
          out o: string stream
          u = run text-upper(text: t)
          u.upper -> o
        }
        """,
        "outer": """
        flow outer {
          in  t: string stream
          out o: string stream
          i = run inner(t: t)
          i.o -> o
        }
        """,
    }
    program = flow.loads("\n".join(bodies[name] for name in order), "two.flow")
    # Deliberately not registered: only the toy actions are.
    result = await program["outer"].invoke(
        {"t": ["a", "b"]}, registry=registry
    )
    assert result == {"o": ["A", "B"]}


# --- then, where, strformat, and time ----------------------------------------


@pytest.mark.asyncio
async def test_then_and_where_read_without_their_pipe(registry):
    """Both join two things, so both read as words between them."""
    result = await run_flow(
        """
        flow plainly {
          in  words: string stream
          out all:   string stream
          out long:  string stream
          shout = run text-upper(text: words)
          "first" then shout.upper then "last" -> all
          words where len(it) > 1 -> long
        }
        """,
        registry,
        words=["a", "bb"],
    )
    assert result["all"] == ["first", "A", "BB", "last"]
    assert result["long"] == ["bb"]


def test_a_stage_name_is_still_a_port_name_where_one_belongs():
    """`then` unpiped must not swallow a port that is called `then`."""
    program = flow.loads(
        """
        flow named {
          in  then: string stream
          out out:  string stream
          then -> out
        }
        """,
        "named.flow",
    )
    assert "then" in program["named"].schema.inputs


@pytest.mark.asyncio
async def test_strformat_fills_slots_in_order_and_by_number(registry):
    result = await run_flow(
        """
        flow formatted {
          in  words: string stream
          out one:     string
          out both:    string
          out printfy: string
          out piped:   string stream
          strformat("%s and %s", "a", "b") -> both
          strformat("%2$s before %1$s", "a", "b") -> one
          words | strformat "<%s>" -> piped
          strformat("%-4s|%06.2f|%d|%x", "ab", 3.14159, "42", 255) -> printfy
        }
        """,
        registry,
        words=["x", "y"],
    )
    assert result["both"] == "a and b"
    assert result["one"] == "b before a"
    # The pipe form is `map strformat(fmt, it)`, which is what it is for.
    assert result["piped"] == ["<x>", "<y>"]
    # printf's own flags, width and precision, and a value coerced to what the
    # conversion asks for rather than the template failing over it.
    assert result["printfy"] == "ab  |003.14|42|ff"


def test_strformat_does_not_let_a_template_walk_into_a_value():
    """A template is printf's, not Python's: conversions and nothing else.

    Flow templates can come from a model, and `str.format` would make
    `{0.__class__.__init__.__globals__}` a way out of the sandbox. printf has
    nowhere to walk to, and a conversion with no value behind it is left as
    written rather than raising.
    """
    from a11.flow.values import strformat

    assert strformat("{0.__class__}", ["x"]) == "{0.__class__}"
    assert strformat("%3$s missing", ["a"]) == "%3$s missing"
    assert strformat("100%% sure", []) == "100% sure"
    # A percent that starts nothing is a percent.
    assert strformat("100% sure", []) == "100% sure"
    # And a Python-style slot is now just text, which is the point of moving.
    assert strformat("{} and {}", ["a", "b"]) == "{} and {}"


@pytest.mark.asyncio
async def test_durations_add_subtract_compare_and_format(registry):
    result = await run_flow(
        """
        flow timed {
          out sum:    string
          out diff:   string
          out longer: bool
          out secs:   number
          out milli:  string
          30s + 15s | text -> sum
          90s - 30s | text -> diff
          (90s > 30s)     -> longer
          seconds(1500ms) -> secs
          strformat("%(ms)dms", 1500ms) -> milli
        }
        """,
        registry,
    )
    assert result["sum"] == "45s"
    assert result["diff"] == "1m"
    assert result["longer"] is True
    assert result["secs"] == 1.5
    assert result["milli"] == "1500ms"


@pytest.mark.asyncio
async def test_an_instant_minus_an_instant_is_a_duration(registry):
    """What a flow needs to time itself, and the only use of `now()`."""
    result = await run_flow(
        """
        flow clocked {
          out ahead:  bool
          out year:   string
          out gap:    number
          (now() + 60s > now())            -> ahead
          strformat("%(%Y)s", now())       -> year
          seconds((now() + 90s) - now())   -> gap
        }
        """,
        registry,
    )
    assert result["ahead"] is True
    assert result["year"].isdigit() and len(result["year"]) == 4
    # Two separate readings of the clock, so a shade under the 90 added.
    assert 89.0 < result["gap"] <= 90.0


@pytest.mark.asyncio
async def test_forward_headers_sends_them_on_without_naming_a_value(registry):
    """`forward headers` is what a flow says when a step needs what it got.

    A11 already gives a nested action every ``x-a11-`` header of its parent, so
    this is for the ones outside that prefix -- an ``authorization``, a tenant
    id -- which otherwise took a `header` declaration *and* a `with` per step to
    move one hop. A pattern names a family of them.
    """
    result = await run_flow(
        """
        flow forwarding {
          out seen: json
          x = run header-peek(go: "now")
              forward headers "authorization", "x-tenant-*"
          x.seen -> seen
        }
        """,
        registry,
        headers={
            "authorization": "a-secret",
            "x-tenant-id": "acme",
            "x-tenant-plan": "pro",
            "x-unmentioned": "no",
        },
    )
    assert result["seen"] == {
        "authorization": "a-secret",
        "x-tenant-id": "acme",
        "x-tenant-plan": "pro",
    }


@pytest.mark.asyncio
async def test_an_explicit_with_beats_a_forwarded_header(registry):
    """The more specific of the two wins, whichever order they are written."""
    result = await run_flow(
        """
        flow both-ways {
          out seen: json
          x = run header-peek(go: "now")
              with "authorization": "computed"
              forward headers "authorization"
          x.seen -> seen
        }
        """,
        registry,
        headers={"authorization": "a-secret"},
    )
    assert result["seen"] == {"authorization": "computed"}


def test_forward_wants_the_word_headers_after_it():
    with pytest.raises(FlowSyntaxError) as raised:
        flow.loads(
            'flow wrong { out o: string\n'
            ' x = run thing(a: "b") forward "x-name"\n'
            ' x.out -> o }',
            "wrong.flow",
        )
    assert "Expected 'headers'" in str(raised.value)


@pytest.mark.asyncio
async def test_a_duration_is_written_and_read_back_in_the_same_units(registry):
    """What the language writes, `duration` reads: the same spelling both ways.

    A duration reaches a flow as text often enough -- a header, a JSON field, a
    model's answer -- that reading one back has to be as ordinary as writing
    one, and in the units the flow would have written itself.
    """
    result = await run_flow(
        """
        flow parsed {
          in  given:   string
          out compact: string
          out fine:    string
          out tiny:    string
          out secs:    number
          out counted: number
          duration(given)          | text -> compact
          duration("1ms500us")     | text -> fine
          500ns + 1500ns           | text -> tiny
          seconds("250ms")                -> secs
          seconds(duration(90))           -> counted
        }
        """,
        registry,
        given="1m30s500ms",
    )
    assert result["compact"] == "1m30s500ms"
    assert result["fine"] == "1ms500us"
    assert result["tiny"] == "2us"
    assert result["secs"] == 0.25
    assert result["counted"] == 90


@pytest.mark.asyncio
async def test_an_instant_can_be_read_from_text_and_compared(registry):
    """`time` is the other half of formatting one: text in, instant out."""
    result = await run_flow(
        """
        flow stamped {
          in  stamp:  string
          out same:   string
          out older:  bool
          out gap:    string
          out at:     string
          time(stamp)                    | text -> same
          (time(stamp) < now())                 -> older
          (time("1970-01-01T00:01:00Z") - time("1970-01-01T00:00:00Z"))
              | text -> gap
          (time(stamp) + 1h) | strformat "%(%H:%M)s" -> at
        }
        """,
        registry,
        stamp="2020-03-04T05:06:07Z",
    )
    assert result["same"] == "2020-03-04T05:06:07Z"
    assert result["older"] is True
    assert result["gap"] == "1m"
    assert result["at"] == "06:06"


@pytest.mark.asyncio
async def test_a_duration_below_zero_says_so_rather_than_meaning_forever(
    registry,
):
    """A11 reads a negative duration handed to a factory as an infinite one.

    The language cannot: `finished - started` the wrong way round is a length
    below zero, and a flow comparing or printing it must see that rather than
    "forever".
    """
    result = await run_flow(
        """
        flow backwards {
          out behind: string
          out number: number
          out under:  bool
          (30s - 1m)   | text -> behind
          seconds(30s - 1m)   -> number
          (30s - 1m < 0s)     -> under
        }
        """,
        registry,
    )
    assert result["behind"] == "-30s"
    assert result["number"] == -30.0
    assert result["under"] is True


@pytest.mark.asyncio
async def test_after_waits_for_a_node_as_well_as_for_a_step(registry):
    """`after some-node` is the barrier an author reaches for unprompted.

    "Stop the microphone once we have a sentence" is `after sentence`, and
    making that mean something spares a `x = wait sentence` binding whose only
    job was to give the barrier a name.
    """
    result = await run_flow(
        """
        flow ordered {
          in  words: string stream
          out seen:  string stream
          out last:  string
          gathered = node()
          words -> gathered
          gathered -> seen
          "everything landed" -> last after gathered
        }
        """,
        registry,
        words=["a", "b"],
    )
    assert result["seen"] == ["a", "b"]
    assert result["last"] == "everything landed"


def test_after_says_so_when_the_name_is_nothing_to_wait_for():
    with pytest.raises(FlowSyntaxError) as raised:
        flow.loads(
            "flow wrong { in w: string stream\n out o: string stream\n"
            " w -> o after nowhere }",
            "wrong.flow",
        )
    assert "Unknown name 'nowhere'" in str(raised.value)


def test_making_a_node_needs_its_parentheses():
    with pytest.raises(FlowSyntaxError) as raised:
        flow.loads(
            "flow wrong { out o: string stream\n n = node\n n -> o }",
            "wrong.flow",
        )
    assert "takes parentheses" in str(raised.value)


# --- Negation ----------------------------------------------------------------


@pytest.mark.asyncio
async def test_not_negates_a_condition(registry):
    result = await run_flow(
        """
        flow judge {
          in  words: string stream
          out took:  string
          risky = try run boom(text: words)
          outcome = wait risky
          if not outcome.ok { "recovered" -> took } else { "fine" -> took }
          skip risky.result
        }
        """,
        registry,
        words=["x"],
    )
    assert result["took"] == "recovered"


@pytest.mark.asyncio
async def test_a_failing_guard_ends_the_flow_before_the_rest_of_it(registry):
    """`if not ok { fail }` as an early exit, with the happy path after it.

    Dataflow has no "return", so the guard does not stop the statements below
    it from starting -- it ends the whole flow out from under them, which is
    the same thing from the caller's side and is what the shape is for.
    """
    with pytest.raises(StatusException) as raised:
        await run_flow(
            """
            flow guard {
              in  words:   string stream
              out shouted: string stream
              risky = try run boom(text: words)
              outcome = wait risky
              if not outcome.ok { fail unavailable outcome.message }
              ok = run text-upper(text: words)
              ok.upper -> shouted
              skip risky.result
            }
            """,
            registry,
            words=["a"],
        )
    assert raised.value.status.code is StatusCode.UNAVAILABLE
    assert "boom went the action" in raised.value.status.message


# --- Skipping the front of a stream ------------------------------------------


@pytest.mark.asyncio
async def test_a_counted_skip_takes_values_off_the_node_for_every_reader(
    registry,
):
    """The point of the count: it is not this reader's, it is the node's.

    `| drop 1` would trim only the pipeline that said it. A count on `skip`
    happens where the stream is produced, so the reader below -- which says
    nothing about skipping -- starts after the skipped value too.
    """
    result = await run_flow(
        """
        flow headerless {
          in  words: string stream
          out kept:  string stream
          out seen:  number
          x = run text-upper(text: words)
          skip 1 x.upper
          x.upper -> kept
          x.upper | count -> seen
        }
        """,
        registry,
        words=["a", "b", "c"],
    )
    assert result["kept"] == ["B", "C"]
    assert result["seen"] == 2


@pytest.mark.asyncio
@pytest.mark.parametrize("order", [("1", "2"), ("2", "1")])
async def test_counted_skips_of_one_node_add_up(registry, order):
    """Two of them naming one node leave three values unread, either way round.

    The count belongs to the ref, and the ref is one object however many times
    the flow mentions it, so this is summed while the flow is compiled rather
    than raced at runtime.
    """
    first, second = order
    result = await run_flow(
        f"""
        flow tail {{
          in  words: string stream
          out kept:  string stream
          x = run text-upper(text: words)
          skip {first} x.upper
          skip {second} x.upper
          x.upper -> kept
        }}
        """,
        registry,
        words=["a", "b", "c", "d", "e"],
    )
    assert result["kept"] == ["D", "E"]


@pytest.mark.asyncio
async def test_a_counted_skip_alone_does_not_stall_the_call(registry):
    """It claims no reader, so the runtime's auto-drain still has the port."""
    result = await run_flow(
        """
        flow quiet {
          in  words: string stream
          out seen:  number
          x = run text-upper(text: words)
          skip 1 x.upper
          words | count -> seen
        }
        """,
        registry,
        words=["a", "b", "c"],
    )
    assert result["seen"] == 3


def test_a_counted_skip_needs_a_port_or_a_node():
    """A loop variable has no front to take values off: each pass is handed
    its own."""
    with pytest.raises(FlowSyntaxError) as raised:
        flow.loads(
            """
            flow wrong {
              in  words: string stream
              out out:   string stream
              for word in words { skip 1 word }
              words -> out
            }
            """,
            "wrong.flow",
        )
    assert "takes a port or a node" in str(raised.value)
    assert "| drop 1" in str(raised.value)


@pytest.mark.parametrize("count", ["0", "-1", "1.5"])
def test_a_counted_skip_counts_whole_values(count):
    with pytest.raises(FlowSyntaxError) as raised:
        flow.loads(
            f"flow wrong {{ in w: string stream\n skip {count} w }}",
            "wrong.flow",
        )
    assert "counts whole values" in str(raised.value)


# --- Nodes of the flow's own -------------------------------------------------


@pytest.mark.asyncio
async def test_a_flow_can_keep_a_stream_in_a_node_of_its_own(registry):
    result = await run_flow(
        """
        flow scratchpad {
          in  words: string stream
          out size:  number
          out text:  string

          kept = node()
          shout = run text-upper(text: words)
          shout.upper -> kept
          (kept | count) -> size
          kept | join "," -> text
        }
        """,
        registry,
        words=["a", "b"],
    )
    assert result["size"] == 2
    assert result["text"] == "A,B"


@pytest.mark.asyncio
async def test_a_node_of_the_flows_own_can_live_in_its_own_node_map(registry):
    outputs, node_map, _ = await _run_with_runtime(
        """
        flow private-node {
          in  words: string stream
          out text:  string

          nodes scratch
          kept = node() in scratch
          words -> kept
          kept | join "," -> text
        }
        """,
        registry,
        words=["a", "b"],
    )
    assert outputs["text"] == "a,b"
    assert not [name for name in _node_names(node_map) if "kept" in name]


@pytest.mark.asyncio
async def test_a_node_nothing_writes_is_an_empty_stream(registry):
    result = await run_flow(
        """
        flow empty {
          out size: number
          unused = node()
          (unused | count) -> size
        }
        """,
        registry,
    )
    assert result["size"] == 0


@pytest.mark.asyncio
async def test_a_flow_can_attach_to_a_node_its_caller_named(registry):
    """The `x-a11-user-log-node` pattern: the caller says where to write."""

    async def progress(action: Action) -> None:
        target = action.get_header("x-a11-progress-node", decode=True)
        assert target is not None
        node = action.get_node(target)
        async for value in action["text"]:
            await (await node.put(f"progress:{value}"))
        await (await action["upper"].put("done"))

    registry.register(
        "progress", UPPER.model_copy(update={"name": "progress"}), progress
    )
    result = await run_flow(
        """
        flow watched {
          in  words:  string stream
          out log:    string stream
          out status: string

          seen = node()
          worker = run progress(text: words)
              with "x-a11-progress-node": seen.id
          worker.upper -> status
          seen -> log
          drain seen after worker
        }
        """,
        registry,
        words=["a", "b"],
    )
    assert result["status"] == "done"
    assert result["log"] == ["progress:a", "progress:b"]


@pytest.mark.asyncio
async def test_a_node_can_be_named_by_a_header(registry):
    result = await run_flow(
        """
        flow told-where {
          in  words: string stream
          out text:  string
          header "x-a11-scratch-node" as scratch-id

          target = node(scratch-id)
          words -> target
          target | join "," -> text
        }
        """,
        registry,
        {"words": ["a", "b"]},
        headers={"x-a11-scratch-node": "a-node-the-caller-chose"},
    )
    assert result["text"] == "a,b"


def test_a_node_needs_a_node_map_that_exists():
    with pytest.raises(FlowSyntaxError) as raised:
        flow.loads("""
            flow bad {
              out out: string
              kept = node() in nowhere
              "x" -> out
            }
            """)
    assert "Unknown node map 'nowhere'" in str(raised.value)
