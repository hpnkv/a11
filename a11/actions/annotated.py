"""Build an Action's schema and handler from a callable's annotations.

The same trade FastAPI makes for request handlers: write an ordinary function
whose parameters are the values it actually wants, and let the framework work
out the wire shape from the annotations and do the marshalling.

```python
async def summarise(
    document: str,
    style: Annotated[str | None, InputPort(description="Tone to use.")] = None,
) -> str:
    # Whatever the docstring says becomes the Action's description.
    return model.summarise(document, style or "neutral")

schema, handler = action_from_callable(summarise)
```

That declares a ``summarise`` Action with a required unary ``document`` input,
an optional unary ``style`` input, and one unary ``output`` port; the handler
consumes the inputs, calls the function, and finalises its return value onto
the output.
[`ActionRegistry.action`][a11.actions.registry.ActionRegistry.action] is the
same thing as a decorator that registers the result.

## How a parameter is read

Each parameter is classified once, when the Action is built:

* [`Action`][a11.actions.action.Action] -- the running action itself, the way
  FastAPI hands over its ``Request``. Nothing is declared for it.
* Annotated with [`Header`][a11.actions.annotated.Header] -- an action header,
  decoded to the parameter's type.
* Annotated with [`OutputPort`][a11.actions.annotated.OutputPort] -- an output
  port, handed over as a live [`AsyncNode`][a11.nodes.async_node.AsyncNode] for
  the function to write itself.
* anything else -- an input port, optionally annotated with
  [`InputPort`][a11.actions.annotated.InputPort] to say more about it.

An input's *arity* is its annotation: ``AsyncIterator[T]`` (or
``AsyncIterable``/``AsyncGenerator``) is a stream and the parameter receives a
lazy async iterator of ``T``; anything else is unary and the parameter receives
one value. A unary input is required unless its type admits ``None`` (``T |
None``, ``Optional[T]``) or the parameter has a default. Annotate a parameter
``AsyncNode`` to be handed the node itself instead of decoded values.

Every declared name defaults to the parameter's name, with a header turning
underscores into hyphens -- so ``x_a11_shell_id`` is the ``x-a11-shell-id``
header -- and every one can be overridden on the annotation.

## How the outputs are decided

If no parameter is annotated with `OutputPort`, the function's own result is the
Action's single output, named ``output`` unless ``action_from_callable``'s
``output`` argument says otherwise:

* an ``async def`` returning ``T`` gets a unary port, finalised with the
  returned value (a ``None`` from a ``T | None`` return finalises the port
  empty);
* an async generator gets a streaming port, each yielded value written to it in
  turn;
* an ``async def`` annotated ``-> None`` declares no output at all.

A function that wants several outputs takes them as parameters instead, and
must then return nothing:

```python
async def split(
    text: str,
    words: Annotated[AsyncNode, OutputPort(description="One word each.")],
    total: Annotated[AsyncNode, OutputPort(mimetype="application/json")],
) -> None:
    for word in text.split():
        await words.put(word)
    await words.finalize()
    await total.finalize(len(text.split()))
```

Those nodes are ordinary ones: write, `finalize`, `close`, or leave them to the
runner, which closes any output the handler did not write.

## What a caller owes

A11's client contract already asks a caller to close every input port it
declares, and a derived handler needs that too: an input port left open with
nothing in it is indistinguishable from one whose value has not arrived yet, so
reading it waits. The action's ``x-a11-deadline`` bounds that wait. A caller
that neither fills nor closes an optional input and sets no deadline waits
indefinitely.
"""

from __future__ import annotations

import dataclasses
import enum
import inspect
import types as builtin_types
import typing
from collections.abc import (
    AsyncGenerator,
    AsyncIterable,
    AsyncIterator,
    Callable,
    Mapping,
    Sequence,
)
from typing import Any, get_args, get_origin

import pydantic
import pydantic_core

from a11 import timing
from a11.actions.action import (
    Action,
    ActionHandler,
    ActionHeaderSchema,
    ActionPortSchema,
    ActionSchema,
)
from a11.data import serialization
from a11.data import types as data_types
from a11.nodes.async_node import AsyncNode
from a11.status import Status, StatusCode

#: Default name of the port a function's own result is written to.
DEFAULT_OUTPUT_NAME = "output"

#: Annotations that make a parameter a stream rather than a single value.
_STREAM_ORIGINS = frozenset({
    AsyncIterator,
    AsyncIterable,
    AsyncGenerator,
    typing.AsyncIterator,
    typing.AsyncIterable,
    typing.AsyncGenerator,
})

_NONE_TYPE = type(None)


class _Unset(enum.Enum):
    """The one-member enum spelling of "nothing was supplied"."""

    TOKEN = "unset"

    def __repr__(self) -> str:  # pragma: no cover -- debugging aid
        return "UNSET"


#: Sentinel distinguishing "no default" from a default that is ``None``.
UNSET = _Unset.TOKEN


# --- Annotation markers ------------------------------------------------------


@dataclasses.dataclass(frozen=True, kw_only=True)
class InputPort:
    """Describe the input port a parameter is read from.

    Every field is optional: what is left out is inferred from the parameter
    (its name, its type, whether it admits ``None``). Attach one with
    ``Annotated[T, InputPort(...)]``.

    Args:
        name: Port name; defaults to the parameter's name.
        mimetype: Declared media type; defaults to the one the parameter's type
            serialises as (``text/plain`` for ``str``,
            ``application/octet-stream`` for ``bytes``, ``application/json``
            otherwise).
        description: What the port carries. Reaches an LLM that is offered this
            Action as a tool, so write it for that reader.
        typeinfo: Python type published on the schema; defaults to the
            parameter's own type when it is a plain class.
        required: Whether a caller must supply the port. Defaults to ``False``
            for a parameter that admits ``None`` or has a default, and for
            streams, and ``True`` otherwise.
        unary: Whether the port carries a single whole value. Defaults to the
            parameter's arity and normally needs no override.
        autofills: Fragments the runtime fills the port with, as on
            [`ActionPortSchema`][a11.actions.action.ActionPortSchema].
    """

    name: str | None = None
    mimetype: str | None = None
    description: str = ""
    typeinfo: type | None = None
    required: bool | None = None
    unary: bool | None = None
    autofills: Sequence[Any] | None = None


@dataclasses.dataclass(frozen=True, kw_only=True)
class OutputPort:
    """Declare an output port handed to the function as an `AsyncNode`.

    Annotate a parameter ``Annotated[AsyncNode, OutputPort(...)]`` and the
    handler passes the live output node, which makes the function responsible
    for what goes on it. A function that declares any of these must return
    ``None``: its own result no longer has a port to go to.

    The fields are `InputPort`'s, read the same way, except that ``typeinfo``
    and ``mimetype`` have no parameter type to be inferred from -- a node is
    just a node -- so an undeclared ``mimetype`` is ``application/json``.
    """

    name: str | None = None
    mimetype: str | None = None
    description: str = ""
    typeinfo: type | None = None
    required: bool | None = None
    unary: bool | None = None
    autofills: Sequence[Any] | None = None


@dataclasses.dataclass(frozen=True, kw_only=True)
class Header:
    """Describe the action header a parameter is read from.

    Attach one with ``Annotated[T, Header(...)]``. The raw header bytes are
    decoded to the parameter's type: ``bytes`` verbatim, ``str`` as UTF-8,
    anything else validated by Pydantic from the value as JSON and then, if that
    is not JSON, from the decoded text -- so ``int``, an enum, a model, and a
    ``list[str]`` all work.

    A header the caller omitted uses ``default``, or ``default_factory()``, or
    is ``None`` if the parameter admits it; with none of those the action fails
    with ``INVALID_ARGUMENT``, which is the only sense in which a header is
    "required". A static ``default`` is also published on the schema, so the
    runtime seeds it into the action's headers and a nested call inherits it.

    Args:
        name: Header name; defaults to the parameter's name with underscores
            turned into hyphens.
        description: What the header means, for whoever calls the Action.
        default: Value used when the header is absent.
        default_factory: Called for that value instead, for one that must not
            be shared (or cannot be computed until the call).
    """

    name: str | None = None
    description: str = ""
    default: Any = UNSET
    default_factory: Callable[[], Any] | None = None

    def __post_init__(self) -> None:
        if self.default is not UNSET and self.default_factory is not None:
            raise TypeError(
                "Header takes default or default_factory, not both."
            )


# --- Annotation analysis -----------------------------------------------------


def _strip_optional(annotation: Any) -> tuple[Any, bool]:
    """Split ``T | None`` into ``T`` and "admits None"."""
    origin = get_origin(annotation)
    if origin is not typing.Union and origin is not builtin_types.UnionType:
        return annotation, False
    args = [arg for arg in get_args(annotation) if arg is not _NONE_TYPE]
    if len(args) == len(get_args(annotation)):
        return annotation, False
    if not args:
        return _NONE_TYPE, True
    if len(args) == 1:
        return args[0], True
    return typing.Union[tuple(args)], True


def _stream_element(annotation: Any) -> tuple[Any, bool]:
    """Split a stream annotation into its element type and "is a stream"."""
    if annotation in _STREAM_ORIGINS:
        return Any, True
    origin = get_origin(annotation)
    if origin is None or origin not in _STREAM_ORIGINS:
        return annotation, False
    args = get_args(annotation)
    return (args[0] if args else Any), True


def _default_mimetype(annotation: Any) -> str:
    """The media type a value of ``annotation`` serialises as by default."""
    if annotation is str:
        return serialization.TEXT_MIMETYPE
    if annotation in (bytes, bytearray, memoryview):
        return serialization.BYTES_MIMETYPE
    if isinstance(annotation, type) and issubclass(
        annotation, (data_types.Chunk, data_types.NodeFragment)
    ):
        # Raw chunks are opaque, so use the generic octet-stream representation.
        return serialization.BYTES_MIMETYPE
    return serialization.JSON_MIMETYPE


def _schema_typeinfo(annotation: Any) -> type | None:
    """``annotation`` as a plain class, or ``None`` if it is not one."""
    if isinstance(annotation, type) and annotation is not _NONE_TYPE:
        return annotation
    return None


def _value_adapter(annotation: Any) -> Callable[[Any], Any] | None:
    """A validator for an annotation the serialization registry cannot take.

    The registry's ``obj_type`` must be a class, so a parametrised annotation
    (``list[int]``, a union, a ``TypedDict``) is deserialized generically and
    then validated here instead.
    """
    if annotation is Any or _schema_typeinfo(annotation) is not None:
        return None
    try:
        adapter = pydantic.TypeAdapter(annotation)
    except Exception:  # pragma: no cover -- unadaptable annotation
        return None
    return adapter.validate_python


def _header_decoder(annotation: Any) -> Callable[[bytes], Any]:
    """A decoder from raw header bytes to ``annotation``."""
    if annotation in (bytes, bytearray, memoryview):
        return bytes
    if annotation is str or annotation is Any:
        return lambda raw: raw.decode()
    try:
        adapter = pydantic.TypeAdapter(annotation)
    except Exception:  # pragma: no cover -- unadaptable annotation
        return lambda raw: raw.decode()

    def decode(raw: bytes) -> Any:
        try:
            return adapter.validate_json(raw)
        except pydantic.ValidationError:
            # A header is text on the wire, so its natural spelling is often not
            # JSON at all: `x-count: 5` parses either way, but `x-mode: live`
            # only as text.
            return adapter.validate_python(raw.decode())

    return decode


def _encode_header_default(value: Any) -> bytes:
    """A header default as the bytes the schema publishes."""
    if isinstance(value, (bytes, bytearray, memoryview)):
        return bytes(value)
    if isinstance(value, str):
        return value.encode()
    if isinstance(value, enum.Enum) and isinstance(value.value, str):
        return value.value.encode()
    return pydantic_core.to_json(value)


# --- Bindings ----------------------------------------------------------------


@dataclasses.dataclass(frozen=True, kw_only=True)
class _InputBinding:
    parameter: str
    port: str
    obj_type: type | None
    adapter: Callable[[Any], Any] | None
    streaming: bool
    raw_node: bool
    required: bool
    default: Any


@dataclasses.dataclass(frozen=True, kw_only=True)
class _OutputBinding:
    parameter: str
    port: str


@dataclasses.dataclass(frozen=True, kw_only=True)
class _HeaderBinding:
    parameter: str
    header: str
    decode: Callable[[bytes], Any]
    default: Any
    default_factory: Callable[[], Any] | None
    optional: bool


@dataclasses.dataclass(frozen=True, kw_only=True)
class _ResultBinding:
    """Where the function's own result goes, when it has a port of its own."""

    port: str
    streaming: bool


def _marker(
    annotation: Any,
) -> tuple[Any, InputPort | OutputPort | Header | None]:
    """Split an ``Annotated`` type into its type and its A11 marker."""
    if get_origin(annotation) is not typing.Annotated:
        return annotation, None
    args = get_args(annotation)
    found: InputPort | OutputPort | Header | None = None
    for extra in args[1:]:
        if isinstance(extra, (InputPort, OutputPort, Header)):
            if found is not None:
                raise TypeError(
                    "A parameter takes at most one of InputPort, OutputPort, "
                    "or Header."
                )
            found = extra
    return args[0], found


def _is_action_annotation(annotation: Any) -> bool:
    return isinstance(annotation, type) and issubclass(annotation, Action)


def _is_node_annotation(annotation: Any) -> bool:
    return isinstance(annotation, type) and issubclass(annotation, AsyncNode)


def _port_schema(
    *,
    name: str,
    mimetype: str,
    description: str,
    required: bool,
    unary: bool,
    typeinfo: type | None,
    autofills: Sequence[Any] | None,
) -> ActionPortSchema:
    return ActionPortSchema(
        name=name,
        type=mimetype,
        description=description,
        required=required,
        unary=unary,
        autofills=list(autofills) if autofills is not None else None,
        typeinfo=typeinfo,
    )


def _claim(names: dict[str, str], name: str, parameter: str, what: str) -> None:
    if name in names:
        raise TypeError(
            f"Two parameters ({names[name]} and {parameter}) declare the "
            f"same {what} {name!r}."
        )
    names[name] = parameter


class _Plan:
    """Everything the handler needs, worked out once at build time."""

    def __init__(self) -> None:
        self.action_parameters: list[str] = []
        self.inputs: list[_InputBinding] = []
        self.outputs: list[_OutputBinding] = []
        self.headers: list[_HeaderBinding] = []
        self.result: _ResultBinding | None = None
        self.is_async_gen = False


def _analyse_parameters(
    signature: inspect.Signature,
    hints: Mapping[str, Any],
    plan: _Plan,
    schema_inputs: dict[str, ActionPortSchema],
    schema_outputs: dict[str, ActionPortSchema],
    schema_headers: dict[str, ActionHeaderSchema],
) -> None:
    input_names: dict[str, str] = {}
    output_names: dict[str, str] = {}
    header_names: dict[str, str] = {}

    for parameter in signature.parameters.values():
        if parameter.kind is inspect.Parameter.VAR_POSITIONAL:
            raise TypeError(f"*{parameter.name} cannot be bound to an Action.")
        if parameter.kind is inspect.Parameter.VAR_KEYWORD:
            raise TypeError(f"**{parameter.name} cannot be bound to an Action.")
        if parameter.kind is inspect.Parameter.POSITIONAL_ONLY:
            raise TypeError(
                f"{parameter.name} is positional-only; the generated handler "
                "passes every argument by keyword."
            )
        if parameter.name not in hints:
            raise TypeError(
                f"{parameter.name} has no annotation, so there is nothing to "
                "derive its port from."
            )

        annotation, marker = _marker(hints[parameter.name])
        has_default = parameter.default is not inspect.Parameter.empty

        if isinstance(marker, Header):
            inner, optional = _strip_optional(annotation)
            name = marker.name or parameter.name.replace("_", "-")
            _claim(header_names, name, parameter.name, "header")
            default = marker.default
            if default is UNSET and has_default:
                default = parameter.default
            plan.headers.append(
                _HeaderBinding(
                    parameter=parameter.name,
                    header=name,
                    decode=_header_decoder(inner),
                    default=default,
                    default_factory=marker.default_factory,
                    optional=optional,
                )
            )
            schema_headers[name] = ActionHeaderSchema(
                name=name,
                description=marker.description,
                default=(
                    None
                    if default is UNSET
                    else _encode_header_default(default)
                ),
            )
            continue

        if isinstance(marker, OutputPort):
            if not _is_node_annotation(annotation):
                raise TypeError(
                    f"{parameter.name} is an OutputPort, so it must be "
                    "annotated AsyncNode -- the handler hands over the node "
                    "itself."
                )
            name = marker.name or parameter.name
            _claim(output_names, name, parameter.name, "output port")
            plan.outputs.append(
                _OutputBinding(parameter=parameter.name, port=name)
            )
            schema_outputs[name] = _port_schema(
                name=name,
                mimetype=marker.mimetype or serialization.JSON_MIMETYPE,
                description=marker.description,
                required=(True if marker.required is None else marker.required),
                unary=False if marker.unary is None else marker.unary,
                typeinfo=marker.typeinfo,
                autofills=marker.autofills,
            )
            continue

        if marker is None and _is_action_annotation(annotation):
            plan.action_parameters.append(parameter.name)
            continue

        # Everything left is an input port.
        stripped, optional = _strip_optional(annotation)
        element, streaming = _stream_element(stripped)
        if streaming and optional:
            raise TypeError(
                f"{parameter.name} is a stream, which is empty rather than "
                "absent, so it cannot be Optional. Say "
                "InputPort(required=False) instead."
            )
        raw_node = _is_node_annotation(element)
        if raw_node and streaming:
            raise TypeError(
                f"{parameter.name} cannot be a stream of AsyncNodes; annotate "
                "it AsyncNode to be handed the input node itself."
            )

        name = (marker.name if marker else None) or parameter.name
        _claim(input_names, name, parameter.name, "input port")
        if marker is not None and marker.required is not None:
            required = marker.required
        elif streaming:
            required = False
        else:
            required = not optional and not has_default
        unary = not streaming and not raw_node
        if marker is not None and marker.unary is not None:
            unary = marker.unary

        value_type = Any if raw_node else element
        # What an empty port yields: the parameter's own default, then None for
        # a type that admits it, and otherwise nothing -- which the handler
        # reports as a missing required input.
        if has_default:
            empty = parameter.default
        elif optional:
            empty = None
        else:
            empty = UNSET
        plan.inputs.append(
            _InputBinding(
                parameter=parameter.name,
                port=name,
                obj_type=_schema_typeinfo(value_type),
                adapter=_value_adapter(value_type),
                streaming=streaming,
                raw_node=raw_node,
                required=required,
                default=empty,
            )
        )
        schema_inputs[name] = _port_schema(
            name=name,
            mimetype=(marker.mimetype if marker else None)
            or _default_mimetype(value_type),
            description=marker.description if marker else "",
            required=required,
            unary=unary,
            typeinfo=(marker.typeinfo if marker else None)
            or _schema_typeinfo(value_type),
            autofills=marker.autofills if marker else None,
        )


def _analyse_result(
    fn: Callable[..., Any],
    hints: Mapping[str, Any],
    plan: _Plan,
    schema_outputs: dict[str, ActionPortSchema],
    output: str | OutputPort,
) -> None:
    """Decide where the function's own result goes, and declare its port."""
    spec = OutputPort(name=output) if isinstance(output, str) else output
    name = spec.name or DEFAULT_OUTPUT_NAME
    plan.is_async_gen = inspect.isasyncgenfunction(fn)
    declared = bool(plan.outputs)
    annotation = hints.get("return", UNSET)

    if plan.is_async_gen:
        if declared:
            raise TypeError(
                "An async generator writes its single output by yielding, so "
                "it cannot also take OutputPort parameters."
            )
        element = Any
        if annotation is not UNSET:
            element, streaming = _stream_element(annotation)
            if not streaming:
                raise TypeError(
                    "An async generator must be annotated to return an "
                    "AsyncIterator/AsyncGenerator."
                )
        if name in schema_outputs:
            raise TypeError(f"The output port {name!r} is declared twice.")
        plan.result = _ResultBinding(port=name, streaming=True)
        schema_outputs[name] = _port_schema(
            name=name,
            mimetype=spec.mimetype or _default_mimetype(element),
            description=spec.description,
            required=True if spec.required is None else spec.required,
            unary=False if spec.unary is None else spec.unary,
            typeinfo=spec.typeinfo or _schema_typeinfo(element),
            autofills=spec.autofills,
        )
        return

    if annotation is UNSET:
        if declared:
            return
        raise TypeError(
            "Annotate the return type: a value type gives the Action its "
            "single output port, and None says it has none (which is what a "
            "function writing its own OutputPort parameters returns)."
        )
    if annotation is None or annotation is _NONE_TYPE:
        return
    if declared:
        raise TypeError(
            "A function with OutputPort parameters must return None: its own "
            "result has no port left to go to."
        )

    value_type, optional = _strip_optional(annotation)
    if name in schema_outputs:
        raise TypeError(f"The output port {name!r} is declared twice.")
    plan.result = _ResultBinding(port=name, streaming=False)
    schema_outputs[name] = _port_schema(
        name=name,
        mimetype=spec.mimetype or _default_mimetype(value_type),
        description=spec.description,
        required=(not optional) if spec.required is None else spec.required,
        unary=True if spec.unary is None else spec.unary,
        typeinfo=spec.typeinfo or _schema_typeinfo(value_type),
        autofills=spec.autofills,
    )


# --- Runtime marshalling -----------------------------------------------------


def _remaining(action: Action) -> timing.Duration:
    """What is left of the action's deadline, so no read can hang forever.

    An unbounded read on an input a caller neither supplies nor closes would
    wait for good; the deadline is the bound the caller already agreed to, and
    is infinite when there is none.
    """
    from a11 import get_deadline

    deadline = get_deadline(action)
    if deadline == timing.infinite_future():
        return timing.infinite_duration()
    return max(deadline - timing.now(), timing.zero_duration())


def _read_header(action: Action, binding: _HeaderBinding) -> Any:
    raw = action.get_header(binding.header)
    if raw is not None:
        return binding.decode(raw)
    if binding.default_factory is not None:
        return binding.default_factory()
    if binding.default is not UNSET:
        return binding.default
    if binding.optional:
        return None
    raise Status(
        code=StatusCode.INVALID_ARGUMENT,
        message=f"The {binding.header} header is required.",
    ).to_exception()


async def _iterate_input(
    node: AsyncNode, binding: _InputBinding
) -> AsyncIterator[Any]:
    """Yield an input port's values, decoded to the parameter's type."""
    if binding.obj_type is not None:
        node.set_expected_types("", binding.obj_type)
    async for value in node:
        yield binding.adapter(value) if binding.adapter is not None else value


async def _read_input(action: Action, binding: _InputBinding) -> Any:
    # ``allow_none`` even for a required input: an empty port is a caller's
    # mistake, and INVALID_ARGUMENT naming the port says so better than
    # ``consume``'s own FAILED_PRECONDITION about a reader offset.
    value = await action.get_input(binding.port).consume(
        obj_type=binding.obj_type,
        timeout=_remaining(action),
        allow_none=True,
    )
    if value is None:
        if binding.default is not UNSET:
            return binding.default
        if binding.required:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"The {binding.port} input is required.",
            ).to_exception()
        return None
    return binding.adapter(value) if binding.adapter is not None else value


def _make_handler(fn: Callable[..., Any], plan: _Plan) -> ActionHandler:
    async def handler(action: Action) -> None:
        kwargs: dict[str, Any] = dict.fromkeys(plan.action_parameters, action)
        for binding in plan.outputs:
            kwargs[binding.parameter] = action.get_output(binding.port)
        for header in plan.headers:
            kwargs[header.parameter] = _read_header(action, header)
        # Streams and raw nodes bind without reading, so a handler that only
        # wants headers never waits on a caller that is still producing.
        for binding in plan.inputs:
            if binding.raw_node:
                kwargs[binding.parameter] = action.get_input(binding.port)
            elif binding.streaming:
                kwargs[binding.parameter] = _iterate_input(
                    action.get_input(binding.port), binding
                )
        for binding in plan.inputs:
            if not binding.streaming and not binding.raw_node:
                kwargs[binding.parameter] = await _read_input(action, binding)

        result = fn(**kwargs)
        if plan.result is None:
            if inspect.isawaitable(result):
                await result
            return

        node = action.get_output(plan.result.port)
        if plan.result.streaming:
            async for value in result:
                await node.put(value)
            await node.finalize()
            return
        if inspect.isawaitable(result):
            result = await result
        # A null terminator when there is nothing: the port still ends, so a
        # caller reading an optional result gets None rather than a hang.
        await node.finalize(result)

    handler.__name__ = getattr(fn, "__name__", "handler")
    handler.__qualname__ = getattr(fn, "__qualname__", handler.__name__)
    handler.__doc__ = fn.__doc__
    # No ``__wrapped__`` is set: this ensures ``inspect.signature`` reports
    # the handler wrapper's own signature rather than the underlying function's,
    # which the runtime reads to decide invocation arguments.
    return handler


# --- Public entry point ------------------------------------------------------


def action_from_callable(
    fn: Callable[..., Any],
    *,
    name: str | None = None,
    description: str | None = None,
    output: str | OutputPort = DEFAULT_OUTPUT_NAME,
    headers: Mapping[str, ActionHeaderSchema] | None = None,
) -> tuple[ActionSchema, ActionHandler]:
    """Derive an Action's schema and handler from ``fn``'s annotations.

    See the [module documentation][a11.actions.annotated] for how each
    parameter and the return type are read. Everything is worked out here, once:
    the returned handler only marshals.

    Args:
        fn: The function to bind. Usually ``async def``, either returning a
            value or an async generator yielding them; a plain ``def`` is
            accepted too.
        name: Action name; defaults to ``fn.__name__``.
        description: Action description; defaults to ``fn``'s docstring.
        output: Where ``fn``'s own result goes -- a port name, or an
            `OutputPort` to say more about it. Ignored by a function that
            declares `OutputPort` parameters, which has no such result.
        headers: Extra header schemas to merge in, for headers the function does
            not itself take a parameter for. Pass
            [`DEFAULT_ACTION_HEADERS`][a11.actions.action.DEFAULT_HEADERS] to
            declare A11's own.

    Returns:
        The schema and the handler, in the order
        [`register`][a11.actions.registry.ActionRegistry.register] takes them.

    Raises:
        TypeError: If an annotation cannot be turned into a port -- an
            unannotated parameter, two parameters claiming one name, a function
            that both declares outputs and returns a value.
    """
    if not callable(fn):
        raise TypeError("fn must be callable.")
    signature = inspect.signature(fn)
    hints = typing.get_type_hints(fn, include_extras=True)

    plan = _Plan()
    schema_inputs: dict[str, ActionPortSchema] = {}
    schema_outputs: dict[str, ActionPortSchema] = {}
    schema_headers: dict[str, ActionHeaderSchema] = dict(headers or {})

    _analyse_parameters(
        signature,
        hints,
        plan,
        schema_inputs,
        schema_outputs,
        schema_headers,
    )
    _analyse_result(fn, hints, plan, schema_outputs, output)

    schema = ActionSchema(
        name=name or getattr(fn, "__name__", None) or "action",
        description=(
            description
            if description is not None
            else (inspect.getdoc(fn) or "")
        ),
        inputs=schema_inputs,
        outputs=schema_outputs,
        headers=schema_headers,
    )
    schema.validate()
    return schema, _make_handler(fn, plan)


__all__ = [
    "DEFAULT_OUTPUT_NAME",
    "UNSET",
    "Header",
    "InputPort",
    "OutputPort",
    "action_from_callable",
]
