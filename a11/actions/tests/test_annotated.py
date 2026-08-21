import asyncio
from collections.abc import AsyncIterator
from typing import Annotated, Any

import pydantic
import pytest

import a11
from a11.actions import (
    Action,
    ActionHeaderSchema,
    ActionRegistry,
    Header,
    InputPort,
    OutputPort,
    action_from_callable,
)
from a11.nodes.async_node import AsyncNode
from a11.status import StatusCode, StatusException


class Document(pydantic.BaseModel):
    title: str
    words: int = 0


async def _run(
    fn,
    *,
    inputs: dict[str, Any] | None = None,
    streams: dict[str, list[Any]] | None = None,
    headers: dict[str, bytes] | None = None,
    **build_kwargs: Any,
) -> Action:
    """Build ``fn``'s Action, fill its inputs, run it, and wait.

    Every declared input the caller did not fill is closed empty, which is what
    A11's client contract asks of a caller and what keeps an unsupplied optional
    input from being read as one whose value has not arrived yet.
    """
    schema, handler = action_from_callable(fn, **build_kwargs)
    action = Action(schema, handler=handler)
    for name, value in (headers or {}).items():
        action.set_header(name, value)
    for name, value in (inputs or {}).items():
        await action.get_input(name, bind_stream=False).finalize(value)
    for name, values in (streams or {}).items():
        node = action.get_input(name, bind_stream=False)
        for value in values:
            await node.put(value)
        await node.finalize()
    filled = {*(inputs or {}), *(streams or {})}
    for name in schema.inputs:
        if name not in filled:
            await action.get_input(name, bind_stream=False).close()
    action.run()
    await action.wait()
    return action


async def _drain(node: AsyncNode, obj_type: type | None = None) -> list[Any]:
    values = []
    while (value := await node.next_object(obj_type)) is not None:
        values.append(value)
    return values


# --- Unary inputs and the implicit output ------------------------------------


@pytest.mark.asyncio
async def test_unary_input_and_output_round_trip():
    async def shout(text: str) -> str:
        """Shout a line."""
        return text.upper()

    schema, _ = action_from_callable(shout)
    assert schema.name == "shout"
    assert schema.description == "Shout a line."
    assert schema.inputs["text"].required
    assert schema.inputs["text"].unary
    assert schema.inputs["text"].type == "text/plain"
    assert schema.inputs["text"].typeinfo is str
    assert schema.outputs["output"].unary
    assert schema.outputs["output"].required

    action = await _run(shout, inputs={"text": "hi"})
    assert await action["output"].consume(str) == "HI"


@pytest.mark.asyncio
async def test_optional_input_is_not_required_and_uses_its_default():
    async def greet(name: str | None = "world") -> str:
        return f"hello {name}"

    schema, _ = action_from_callable(greet)
    assert not schema.inputs["name"].required

    action = await _run(greet)
    assert await action["output"].consume(str) == "hello world"


@pytest.mark.asyncio
async def test_optional_input_without_a_default_is_none():
    async def greet(name: str | None) -> str:
        return f"hello {name}"

    assert not action_from_callable(greet)[0].inputs["name"].required
    action = await _run(greet)
    assert await action["output"].consume(str) == "hello None"


@pytest.mark.asyncio
async def test_a_required_input_left_empty_fails_the_action():
    async def greet(name: str) -> str:
        return f"hello {name}"

    schema, handler = action_from_callable(greet)
    action = Action(schema, handler=handler)
    await action.get_input("name", bind_stream=False).close()
    action.run()
    with pytest.raises(StatusException) as raised:
        await action.wait()
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT
    assert "name" in raised.value.status.message


@pytest.mark.asyncio
async def test_input_port_annotation_overrides_the_inferred_schema():
    async def fetch(
        target: Annotated[
            str,
            InputPort(
                name="url",
                mimetype="text/uri-list",
                description="What to fetch.",
                required=False,
            ),
        ] = "",
    ) -> str:
        return target

    schema, _ = action_from_callable(fetch)
    assert "target" not in schema.inputs
    assert schema.inputs["url"].name == "url"
    assert schema.inputs["url"].type == "text/uri-list"
    assert schema.inputs["url"].description == "What to fetch."
    assert not schema.inputs["url"].required

    action = await _run(fetch, inputs={"url": "https://example.invalid"})
    assert await action["output"].consume(str) == "https://example.invalid"


@pytest.mark.asyncio
async def test_a_model_input_and_output_use_json():
    async def retitle(document: Document) -> Document:
        return Document(title=document.title.title(), words=document.words)

    schema, _ = action_from_callable(retitle)
    assert schema.inputs["document"].type == "application/json"
    assert schema.inputs["document"].typeinfo is Document
    assert schema.outputs["output"].typeinfo is Document

    action = await _run(retitle, inputs={"document": Document(title="a note")})
    assert await action["output"].consume(Document) == Document(title="A Note")


@pytest.mark.asyncio
async def test_a_parametrised_annotation_is_validated_by_pydantic():
    async def total(values: list[int]) -> int:
        return sum(values)

    schema, _ = action_from_callable(total)
    # The registry's obj_type must be a class, so nothing is published, and the
    # decoded value is validated here instead.
    assert schema.inputs["values"].typeinfo is None
    assert schema.inputs["values"].type == "application/json"

    action = await _run(total, inputs={"values": [1, 2, 3]})
    assert await action["output"].consume(int) == 6


@pytest.mark.asyncio
async def test_an_asyncnode_input_is_handed_over_raw():
    async def count(
        lines: Annotated[AsyncNode, InputPort(mimetype="text/plain")],
    ) -> int:
        seen = 0
        while await lines.next_object(str) is not None:
            seen += 1
        return seen

    schema, _ = action_from_callable(count)
    assert not schema.inputs["lines"].unary

    action = await _run(count, streams={"lines": ["a", "b", "c"]})
    assert await action["output"].consume(int) == 3


# --- Streaming ---------------------------------------------------------------


@pytest.mark.asyncio
async def test_a_stream_input_is_a_lazy_iterator_of_its_element_type():
    async def widths(lines: AsyncIterator[str]) -> list[int]:
        return [len(line) async for line in lines]

    schema, _ = action_from_callable(widths)
    assert not schema.inputs["lines"].unary
    assert not schema.inputs["lines"].required
    assert schema.inputs["lines"].type == "text/plain"
    assert schema.inputs["lines"].typeinfo is str

    action = await _run(widths, streams={"lines": ["a", "bb", "ccc"]})
    assert await action["output"].consume() == [1, 2, 3]


@pytest.mark.asyncio
async def test_a_stream_input_of_models_decodes_each_value():
    async def titles(docs: AsyncIterator[Document]) -> list[str]:
        return [doc.title async for doc in docs]

    action = await _run(
        titles,
        streams={"docs": [Document(title="one"), Document(title="two")]},
    )
    assert await action["output"].consume() == ["one", "two"]


@pytest.mark.asyncio
async def test_an_async_generator_gets_a_streaming_output():
    async def split(text: str) -> AsyncIterator[str]:
        for word in text.split():
            yield word

    schema, _ = action_from_callable(split)
    assert not schema.outputs["output"].unary
    assert schema.outputs["output"].type == "text/plain"
    assert schema.outputs["output"].typeinfo is str

    action = await _run(split, inputs={"text": "one two three"})
    assert await _drain(action["output"], str) == ["one", "two", "three"]


@pytest.mark.asyncio
async def test_the_output_port_name_is_configurable():
    async def split(text: str) -> AsyncIterator[str]:
        for word in text.split():
            yield word

    schema, _ = action_from_callable(split, output="words")
    assert set(schema.outputs) == {"words"}

    schema, _ = action_from_callable(
        split, output=OutputPort(name="words", description="One each.")
    )
    assert schema.outputs["words"].description == "One each."

    action = await _run(split, inputs={"text": "a b"}, output="words")
    assert await _drain(action["words"], str) == ["a", "b"]


# --- Declared outputs --------------------------------------------------------


@pytest.mark.asyncio
async def test_declared_output_ports_are_handed_over_as_nodes():
    async def split(
        text: str,
        words: Annotated[
            AsyncNode, OutputPort(mimetype="text/plain", description="Each.")
        ],
        total: Annotated[AsyncNode, OutputPort()],
    ) -> None:
        parts = text.split()
        for word in parts:
            await words.put(word)
        await words.finalize()
        await total.finalize(len(parts))

    schema, _ = action_from_callable(split)
    assert set(schema.outputs) == {"words", "total"}
    assert schema.outputs["words"].type == "text/plain"
    assert schema.outputs["words"].description == "Each."
    assert schema.outputs["total"].type == "application/json"

    action = await _run(split, inputs={"text": "a b c"})
    assert await _drain(action["words"], str) == ["a", "b", "c"]
    assert await action["total"].consume(int) == 3


@pytest.mark.asyncio
async def test_a_none_returning_function_declares_no_output():
    seen: list[str] = []

    async def record(text: str) -> None:
        seen.append(text)

    schema, _ = action_from_callable(record)
    assert schema.outputs == {}

    await _run(record, inputs={"text": "noted"})
    assert seen == ["noted"]


@pytest.mark.asyncio
async def test_an_output_the_handler_never_wrote_is_still_closed():
    async def quiet(out: Annotated[AsyncNode, OutputPort()]) -> None:
        del out

    action = await _run(quiet)
    assert await action["out"].consume(allow_none=True) is None


# --- The action itself -------------------------------------------------------


@pytest.mark.asyncio
async def test_an_action_parameter_receives_the_running_action():
    async def describe(text: str, action: Action) -> str:
        await action.log("working")
        return f"{action.get_schema().name}:{text}"

    schema, _ = action_from_callable(describe)
    assert "action" not in schema.inputs

    action = await _run(describe, inputs={"text": "x"})
    assert await action["output"].consume(str) == "describe:x"


# --- Headers -----------------------------------------------------------------


@pytest.mark.asyncio
async def test_a_header_parameter_names_its_header_with_hyphens():
    async def run(
        x_a11_shell_id: Annotated[str, Header(description="Shell.")],
    ) -> str:
        return x_a11_shell_id

    schema, _ = action_from_callable(run)
    assert set(schema.headers) == {"x-a11-shell-id"}
    assert schema.headers["x-a11-shell-id"].description == "Shell."
    assert schema.inputs == {}

    action = await _run(run, headers={"x-a11-shell-id": b"sh-1"})
    assert await action["output"].consume(str) == "sh-1"


@pytest.mark.asyncio
async def test_headers_are_decoded_to_the_parameter_type():
    async def read(
        count: Annotated[int, Header()],
        ratio: Annotated[float, Header()],
        tags: Annotated[list[str], Header()],
        raw: Annotated[bytes, Header()],
        mode: Annotated[str, Header()],
    ) -> list[Any]:
        return [count, ratio, tags, raw.decode(), mode]

    action = await _run(
        read,
        headers={
            "count": b"3",
            "ratio": b"0.5",
            "tags": b'["a", "b"]',
            "raw": b"\x01\x02",
            # Not JSON: a header is text on the wire, and text is the fallback.
            "mode": b"live",
        },
    )
    assert await action["output"].consume() == [
        3,
        0.5,
        ["a", "b"],
        "\x01\x02",
        "live",
    ]


@pytest.mark.asyncio
async def test_a_static_header_default_is_published_on_the_schema():
    async def read(retries: Annotated[int, Header(default=3)]) -> int:
        return retries

    schema, _ = action_from_callable(read)
    assert schema.headers["retries"].default == b"3"

    action = await _run(read)
    assert await action["output"].consume(int) == 3


@pytest.mark.asyncio
async def test_a_header_default_factory_is_called_per_run():
    calls = []

    def factory() -> str:
        calls.append(len(calls))
        return f"run-{len(calls)}"

    async def read(
        tag: Annotated[str, Header(default_factory=factory)],
    ) -> str:
        return tag

    schema, _ = action_from_callable(read)
    assert schema.headers["tag"].default is None

    first = await _run(read)
    second = await _run(read)
    assert await first["output"].consume(str) == "run-1"
    assert await second["output"].consume(str) == "run-2"


@pytest.mark.asyncio
async def test_an_absent_header_is_none_when_the_parameter_admits_it():
    async def read(tag: Annotated[str | None, Header()]) -> str:
        return str(tag)

    action = await _run(read)
    assert await action["output"].consume(str) == "None"


@pytest.mark.asyncio
async def test_an_absent_header_with_no_default_fails_the_action():
    async def read(tag: Annotated[str, Header()]) -> str:
        return tag

    schema, handler = action_from_callable(read)
    action = Action(schema, handler=handler)
    action.run()
    with pytest.raises(StatusException) as raised:
        await action.wait()
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT
    assert "tag" in raised.value.status.message


@pytest.mark.asyncio
async def test_extra_header_schemas_are_merged_in():
    async def read(text: str) -> str:
        return text

    schema, _ = action_from_callable(
        read,
        headers={
            "x-tenant": ActionHeaderSchema("x-tenant", "Which tenant."),
        },
    )
    assert schema.headers["x-tenant"].description == "Which tenant."

    schema, _ = action_from_callable(read, headers=a11.DEFAULT_ACTION_HEADERS)
    assert "x-a11-deadline" in schema.headers


# --- Registration ------------------------------------------------------------


@pytest.mark.asyncio
async def test_the_registry_decorator_registers_the_derived_action():
    registry = ActionRegistry()

    @registry.action
    async def shout(text: str) -> str:
        """Shout a line."""
        return text.upper()

    assert registry.is_registered("shout")
    assert registry.get_schema("shout").description == "Shout a line."
    assert shout.action_schema == registry.get_schema("shout")
    # The function is returned unchanged, so it stays directly callable.
    assert await shout("hi") == "HI"

    action = registry.make_action("shout")
    await action.get_input("text", bind_stream=False).finalize("hi")
    action.run()
    await action.wait()
    assert await action["output"].consume(str) == "HI"


@pytest.mark.asyncio
async def test_the_registry_decorator_takes_arguments():
    registry = ActionRegistry()

    @registry.action(name="upper-case", output="shouted", description="Shout.")
    async def shout(text: str) -> str:
        return text.upper()

    assert registry.list_registered_actions() == ["upper-case"]
    schema = registry.get_schema("upper-case")
    assert schema.description == "Shout."
    assert set(schema.outputs) == {"shouted"}


# --- Build-time errors -------------------------------------------------------


def test_an_unannotated_parameter_is_rejected():
    async def fn(text) -> str:
        return text

    with pytest.raises(TypeError, match="no annotation"):
        action_from_callable(fn)


def test_a_missing_return_annotation_is_rejected():
    async def fn(text: str):
        return text

    with pytest.raises(TypeError, match="Annotate the return type"):
        action_from_callable(fn)


def test_declared_outputs_and_a_returned_value_cannot_be_mixed():
    async def fn(out: Annotated[AsyncNode, OutputPort()]) -> str:
        del out
        return "x"

    with pytest.raises(TypeError, match="must return None"):
        action_from_callable(fn)


def test_an_async_generator_cannot_also_declare_outputs():
    async def fn(
        out: Annotated[AsyncNode, OutputPort()],
    ) -> AsyncIterator[str]:
        del out
        yield "x"

    with pytest.raises(TypeError, match="cannot also take OutputPort"):
        action_from_callable(fn)


def test_two_parameters_cannot_claim_one_port_name():
    async def fn(a: Annotated[str, InputPort(name="x")], x: str) -> None:
        del a, x

    with pytest.raises(TypeError, match="same input port"):
        action_from_callable(fn)


def test_an_output_port_must_be_annotated_asyncnode():
    async def fn(out: Annotated[str, OutputPort()]) -> None:
        del out

    with pytest.raises(TypeError, match="annotated AsyncNode"):
        action_from_callable(fn)


def test_an_optional_stream_is_rejected():
    async def fn(lines: AsyncIterator[str] | None) -> None:
        del lines

    with pytest.raises(TypeError, match="cannot be Optional"):
        action_from_callable(fn)


def test_a_positional_only_parameter_is_rejected():
    async def fn(text: str, /) -> str:
        return text

    with pytest.raises(TypeError, match="positional-only"):
        action_from_callable(fn)


def test_var_args_are_rejected():
    async def fn(*args: str) -> None:
        del args

    with pytest.raises(TypeError, match=r"\*args"):
        action_from_callable(fn)


def test_a_header_cannot_have_both_kinds_of_default():
    with pytest.raises(TypeError, match="not both"):
        Header(default=1, default_factory=lambda: 1)


def test_a_parameter_takes_at_most_one_marker():
    async def fn(text: Annotated[str, InputPort(), Header()]) -> None:
        del text

    with pytest.raises(TypeError, match="at most one"):
        action_from_callable(fn)


# --- Plain callables ---------------------------------------------------------


@pytest.mark.asyncio
async def test_a_synchronous_function_is_accepted():
    def shout(text: str) -> str:
        return text.upper()

    action = await _run(shout, inputs={"text": "hi"})
    assert await action["output"].consume(str) == "HI"


@pytest.mark.asyncio
async def test_concurrent_runs_of_one_derived_handler_do_not_interfere():
    async def echo(text: str) -> str:
        await asyncio.sleep(0)
        return text

    schema, handler = action_from_callable(echo)
    actions = []
    for index in range(4):
        action = Action(schema, handler=handler)
        await action.get_input("text", bind_stream=False).finalize(str(index))
        actions.append(action.run())
    await asyncio.gather(*(action.wait() for action in actions))
    results = [await action["output"].consume(str) for action in actions]
    assert results == ["0", "1", "2", "3"]
