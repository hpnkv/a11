# Copyright 2026 The A11 Authors.

"""One MCP tool, read as an A11 `ActionSchema`.

The translation, and nothing else: no transport, no session, no `mcp` import.
It works on the wire shapes -- the `tools/list` entries, whether they arrive as
SDK models or as plain JSON -- so a schema can be derived, tested and shipped to
a peer without the MCP SDK installed anywhere.

The shape of the answer is decided by what happens to it afterwards. An Action
derived here is registered like any other, so it is offered to a model by
[definition_from_schema][a11.sdk.llm_tools.runner.definition_from_schema],
called through
[ActionCallAdapter][a11.sdk.llm.ActionCallAdapter], and described to a peer by
[a11.actions.describe][a11.actions.describe]. Each of those reads an
`ActionSchema` in a particular way, and the rules below are what make the round
trip MCP tool -> Action -> tool definition come back out looking like the tool
the server declared:

* **A tool's arguments become ports, one per top-level property.** A single
  `arguments` port would show every MCP tool to a model as an object with one
  opaque field; ports are A11's argument surface, and one per property is what
  lets a flow wire a single argument and a model see the tool's real signature.
  Each port carries the property's own JSON Schema, made self-contained, so the
  types survive even though no Python type ever existed to derive them from.
* **A homogeneous array becomes a streaming port carrying its item schema.**
  Not cosmetic: `ActionCallAdapter.get_action_inputs` writes a list argument as
  one fragment per element, so only a non-unary port can tell "the argument is
  a list of one" from "the argument is that one value". A positional tuple
  (`prefixItems`) is a value rather than a stream and stays unary.
* **A result becomes three ports**, because an MCP result is three things: text
  blocks, other content blocks, and -- when the tool declares an output schema
  -- one structured document. The structured document is then mapped to the
  whole JSON result, which is MCP's own rule that a client should prefer
  `structuredContent` where a tool declares a schema for it.
* **What is not an argument becomes a header**: which tool, which server, and
  MCP's per-request `_meta`. See [McpHeaders][a11.sdk.mcp.schemas.McpHeaders].

What cannot survive is named where it is dropped: object-level composition
(`anyOf` and friends *around* the properties) cannot be split into ports at all,
so such a tool keeps its whole argument object on one port; and the array
keywords other than `items` (`maxItems`, `uniqueItems`, a `minItems` above one)
have nowhere to live on a streaming port, though `minItems: 1` is carried by the
port being required.
"""

from __future__ import annotations

import dataclasses
import enum
import json
from collections.abc import Collection, Mapping, Sequence
from typing import Any

import a11
from a11 import _native
from a11.actions.jsonschema import organise_and_deduplicate_jsonschema
from a11.status import Status, StatusCode

#: Port carrying the text blocks of a tool result, in order.
TEXT_OUTPUT = "text"
#: Port carrying the result's non-text content blocks, verbatim.
CONTENT_OUTPUT = "content"
#: Port carrying `structuredContent`, present when the tool declares a schema
#: for it.
STRUCTURED_OUTPUT = "structured_content"
#: Port carrying a whole argument object, for a tool whose input schema cannot
#: be split into one port per property.
ARGUMENTS_INPUT = "arguments"

#: Port names A11 reserves for an action's own lifecycle streams.
RESERVED_PORT_NAMES = frozenset(
    {
        _native.ACTION_STATUS_OUTPUT,
        _native.ACTION_DISPATCH_STATUS_OUTPUT,
        _native.ACTION_LOG_OUTPUT,
    }
)

# Keywords that say something about the argument *object* rather than about one
# of its properties. Any of them means the object cannot be taken apart into
# ports without changing what the tool accepts, so it is kept whole.
_OBJECT_COMPOSITION_KEYWORDS = (
    "anyOf",
    "oneOf",
    "allOf",
    "not",
    "if",
    "then",
    "else",
    "dependentRequired",
    "dependentSchemas",
    "patternProperties",
    "propertyNames",
    "minProperties",
    "maxProperties",
    "unevaluatedProperties",
)

# Array keywords that make a schema a positional tuple rather than a sequence of
# like values. A tuple is one value, so it stays on a unary port with its whole
# schema intact.
_TUPLE_KEYWORDS = ("prefixItems", "additionalItems")

# Keywords by which an input schema says something about what arguments a tool
# takes. A schema with nothing to say -- `{}`, `{"type": "object"}`, or the
# `{"properties": {}}` that a no-argument tool actually gets from a server built
# on pydantic -- describes a tool that takes nothing, and gets no input ports.
_ARGUMENT_KEYWORDS = (
    "properties",
    "required",
    "additionalProperties",
    "$ref",
    *_OBJECT_COMPOSITION_KEYWORDS,
)

_JSON_MIMETYPE = "application/json"
_TEXT_MIMETYPE = "text/plain"


class McpHeaders(enum.StrEnum):
    """MCP context that is not an argument, and so travels as a header.

    Headers describe one call and flow into nested actions, which is what makes
    them the right home for this: the tool being called and the server it lives
    on are properties of the call rather than data the model supplies, and
    `_meta` is MCP's own per-request bag.
    """

    #: Name of the MCP tool to call. Defaults, on a derived schema, to the
    #: tool's verbatim name -- so an action renamed to satisfy A11's identifier
    #: rules still reaches the right tool, and the mapping is visible to anyone
    #: reading the schema rather than buried in a handler.
    TOOL = "x-a11-mcp-tool"
    #: The MCP server these handlers are bound to. Defaults to the target the
    #: tools were discovered from; a call naming a different one is refused
    #: rather than quietly sent somewhere else.
    SERVER = "x-a11-mcp-server"
    #: A JSON object merged into the request's `_meta`. MCP reserves the
    #: `io.modelcontextprotocol/*` keys for the SDK, so this is for a caller's
    #: own per-request metadata.
    META = "x-a11-mcp-meta"


#: Header schemas an MCP-derived action always carries, before the defaults that
#: name its own tool and server are filled in.
MCP_HEADERS = {
    McpHeaders.META: a11.ActionHeaderSchema(
        McpHeaders.META,
        "JSON object merged into the MCP request's _meta.",
    ),
}


@dataclasses.dataclass(frozen=True)
class McpArgument:
    """One MCP tool argument, and the port it is fed from."""

    #: The A11 port name.
    port: str
    #: The property name the MCP server expects, which the port name may have
    #: had to differ from.
    property: str
    #: Whether the port carries one value rather than a sequence of them.
    unary: bool


@dataclasses.dataclass(frozen=True)
class McpTool:
    """One MCP tool as an A11 Action: the schema, and how to feed it.

    The schema is the public half -- register it, describe it, show it to a
    model. The rest is what a handler needs to turn ports back into a
    `tools/call` and its result back into ports, and is why the translation
    returns an object rather than a bare `ActionSchema`.
    """

    #: The tool's name as the server spells it.
    tool_name: str
    #: The server the tool was discovered from, as a display string.
    server: str
    #: The Action interface derived from the tool.
    schema: a11.ActionSchema
    #: One entry per argument port, in the order they were derived.
    arguments: tuple[McpArgument, ...] = ()
    #: Port carrying the whole argument object, when the input schema could not
    #: be split into one port per property.
    whole_arguments: str | None = None
    #: Port carrying the result's text blocks.
    text_output: str = TEXT_OUTPUT
    #: Port carrying the result's other content blocks.
    content_output: str = CONTENT_OUTPUT
    #: Port carrying `structuredContent`, when the tool declares a schema for
    #: it.
    structured_output: str | None = None

    @property
    def action_name(self) -> str:
        """The name the Action is registered under."""
        return self.schema.name


def sanitise_name(name: str, *, fallback: str = "tool") -> str:
    """Coerce ``name`` into an A11 identifier, keeping it recognisable.

    A11 names allow letters, digits and underscores anywhere, and hyphens and
    `#` in the middle (`a11.data.types.NameString`). MCP puts no such
    restriction on a tool name, and servers do use dots and slashes to namespace
    them, so anything else collapses to an underscore rather than being
    rejected. The tool's real name is not lost: it rides the
    [TOOL][a11.sdk.mcp.schemas.McpHeaders.TOOL] header.
    """
    kept = []
    for character in name:
        allowed = character in "-_#" or (
            character.isascii() and character.isalnum()
        )
        kept.append(character if allowed else "_")
    # Hyphens and `#` are middle-only, so a name that starts or ends with one
    # loses it rather than being rejected.
    sanitised = "".join(kept).strip("-#")[:255]
    bare = sanitised.lstrip("_")
    if not bare:
        # Nothing survived that says which tool this is -- `!!!` sanitises to
        # underscores -- so the caller's word for a tool is better than that.
        return fallback
    if sanitised.startswith("__"):
        # A `__` prefix is A11's mark for its own actions
        # ([is_reserved_action][a11.actions.describe.is_reserved_action]), and a
        # tool wearing it would be quietly left out of what a model is offered.
        sanitised = f"_{bare}"
    return sanitised


def _unique(name: str, taken: Collection[str]) -> str:
    """``name``, suffixed until it is not one of ``taken``."""
    if name not in taken:
        return name
    index = 2
    while f"{name}_{index}" in taken:
        index += 1
    return f"{name}_{index}"


def _field(data: Mapping[str, Any], *names: str) -> Any:
    """The first of ``names`` present in ``data``.

    A tool entry reaches us either as the SDK's model, dumped by alias into the
    wire's camel case, or as the JSON a server actually sent. Both are read the
    same way, and a caller assembling one by hand in snake case is read too.
    """
    for name in names:
        if name in data and data[name] is not None:
            return data[name]
    return None


def _mapping(value: Any) -> dict[str, Any]:
    """``value`` as a JSON-Schema mapping; `{}` for anything else.

    JSON Schema allows `true` and `false` as whole schemas, and a server may
    send a property with no schema at all. None of those constrain anything, so
    they all read as "unconstrained" rather than as a failure.
    """
    return dict(value) if isinstance(value, Mapping) else {}


def tool_document(tool: Any) -> dict[str, Any]:
    """One `tools/list` entry as a plain mapping.

    Accepts the MCP SDK's `Tool`, or the JSON of one. The SDK model is dumped
    by alias, so both spellings arrive in the wire's camel case.
    """
    dump = getattr(tool, "model_dump", None)
    if callable(dump):
        return dump(mode="json", by_alias=True, exclude_none=True)
    if isinstance(tool, Mapping):
        return dict(tool)
    raise Status(
        code=StatusCode.INVALID_ARGUMENT,
        message="An MCP tool must be a Tool model or a mapping.",
    ).to_exception()


def _annotation_hints(annotations: Mapping[str, Any]) -> str:
    """The tool's behaviour hints as one sentence, or ``""``.

    Only what the server actually stated. These are hints rather than
    guarantees -- MCP says so, and says a client should not make tool-use
    decisions on them for an untrusted server -- but they are exactly what
    somebody choosing whether to call a tool wants to know, and an
    `ActionSchema` has nowhere but its description to put them.
    """
    read_only = _field(annotations, "readOnlyHint", "read_only_hint")
    destructive = _field(annotations, "destructiveHint", "destructive_hint")
    idempotent = _field(annotations, "idempotentHint", "idempotent_hint")
    open_world = _field(annotations, "openWorldHint", "open_world_hint")

    hints: list[str] = []
    if read_only is True:
        hints.append("does not modify its environment")
    if destructive is True:
        hints.append("may perform destructive updates")
    elif destructive is False:
        hints.append("only makes additive updates")
    if idempotent is True:
        hints.append("repeating a call with the same arguments does nothing")
    if open_world is True:
        hints.append("may interact with entities outside this server")
    elif open_world is False:
        hints.append("interacts with a closed set of entities")
    if not hints:
        return ""
    return f"Hints declared by the server: {'; '.join(hints)}."


def tool_description(document: Mapping[str, Any]) -> str:
    """What a model is told about the tool.

    The server's description, introduced by its display title when that says
    something the description does not, and closed by whatever its annotations
    claim about its behaviour.
    """
    description = str(_field(document, "description") or "").strip()
    annotations = _mapping(_field(document, "annotations"))
    title = str(
        _field(document, "title") or _field(annotations, "title") or ""
    ).strip()

    parts: list[str] = []
    if title and title.casefold() not in description.casefold():
        parts.append(title)
    if description:
        parts.append(description)
    if hints := _annotation_hints(annotations):
        parts.append(hints)
    return "\n\n".join(parts)


def _type_names(schema: Mapping[str, Any]) -> list[str]:
    """The `type` keyword as a list, however it was spelled."""
    declared = schema.get("type")
    if isinstance(declared, str):
        return [declared]
    if isinstance(declared, Sequence):
        return [item for item in declared if isinstance(item, str)]
    return []


def _branches(schema: Mapping[str, Any]) -> list[dict[str, Any]]:
    """The alternatives of a union schema, or the schema itself.

    `null` alternatives are dropped: an absent optional argument is how null is
    said on a port, so a `T | null` property is read as a `T`.
    """
    union = _field(schema, "anyOf", "oneOf")
    if not isinstance(union, Sequence) or isinstance(union, (str, bytes)):
        candidates = [schema]
    else:
        candidates = [_mapping(branch) for branch in union]
    return [branch for branch in candidates if _type_names(branch) != ["null"]]


def array_item_schema(schema: Mapping[str, Any]) -> dict[str, Any] | None:
    """The item schema, when ``schema`` is unambiguously a sequence.

    "Unambiguously" is the whole question, because the answer decides whether
    the argument gets a streaming port. Every alternative left after the `null`
    ones has to be an array of like items: a property that is *either* a list or
    a scalar stays unary, where one fragment reads back as the scalar and more
    than one as the list, and a positional tuple stays unary because it is one
    value rather than a sequence of them.

    Returns:
        The schema of one item -- possibly empty, meaning unconstrained -- or
        None when the property is not a sequence.
    """
    branches = _branches(schema)
    if not branches:
        return None
    items: list[dict[str, Any]] = []
    for branch in branches:
        if "array" not in _type_names(branch):
            return None
        if any(keyword in branch for keyword in _TUPLE_KEYWORDS):
            return None
        item = branch.get("items")
        if isinstance(item, Sequence) and not isinstance(item, (str, bytes)):
            # Draft-04 tuple spelling: `items` as a list of positional schemas.
            return None
        items.append(_mapping(item))
    if len(items) == 1:
        return items[0]
    return {"anyOf": items}


def _port_mimetype(schema: Mapping[str, Any]) -> str:
    """The media type a port declares for values of this schema.

    Descriptive rather than enforced -- a port reads whatever media type the
    chunk actually carries -- but `text/plain` for a string is what the rest of
    the SDK declares (`SHELL_EXECUTE_SCHEMA`'s `command` port), and it is what
    tells a reader whether a value on the port is a document or a line of text.
    """
    branches = _branches(schema)
    if branches and all(
        _type_names(branch) == ["string"] for branch in branches
    ):
        return _TEXT_MIMETYPE
    return _JSON_MIMETYPE


def self_contained_schema(
    schema: Mapping[str, Any], defs: Mapping[str, Any] | None
) -> dict[str, Any]:
    """One property's schema, resolved against its parent's `$defs`.

    A port's `json_schema` is embedded in bigger documents by everything that
    reads it, so it has to stand on its own:
    [organise_and_deduplicate_jsonschema][a11.actions.jsonschema.organise_and_deduplicate_jsonschema]
    resolves the property's `$ref` against the enclosing `$defs`, re-homes what
    it actually referenced at this schema's own root, and drops the rest of the
    server's definitions.
    """
    document = dict(schema)
    if defs:
        document = {"$defs": dict(defs), **document}
    return organise_and_deduplicate_jsonschema(document)


def _json_schema_text(schema: Mapping[str, Any]) -> str:
    """A port's `json_schema` field: the document, or `""` for no constraint."""
    return json.dumps(schema, sort_keys=True) if schema else ""


def _declares_arguments(input_schema: Mapping[str, Any]) -> bool:
    """Whether an input schema says the tool takes anything at all.

    A keyword that is *present but empty* says nothing: a no-argument tool on a
    pydantic-backed server is described as `{"properties": {}}`, and reading
    that as an opaque object would give it an argument port no caller could
    ever fill sensibly -- and which every caller would then have to close.
    """
    return any(
        bool(_field(input_schema, keyword)) for keyword in _ARGUMENT_KEYWORDS
    )


def _splittable(input_schema: Mapping[str, Any]) -> bool:
    """Whether an input schema can become one port per property.

    Only a plain object of independent properties can. Composition around the
    properties makes which arguments are accepted depend on the others, and
    open `additionalProperties` means the declared properties are not the whole
    story -- in both cases the object has to stay whole to stay faithful.
    """
    properties = _field(input_schema, "properties")
    if not isinstance(properties, Mapping) or not properties:
        return False
    if any(keyword in input_schema for keyword in _OBJECT_COMPOSITION_KEYWORDS):
        return False
    additional = _field(
        input_schema, "additionalProperties", "additional_properties"
    )
    if additional is True or isinstance(additional, Mapping):
        return False
    types = _type_names(input_schema)
    return not types or "object" in types


def _argument_ports(
    input_schema: Mapping[str, Any],
) -> tuple[
    dict[str, a11.ActionPortSchema], tuple[McpArgument, ...], str | None
]:
    """The input ports for a tool, and how they map back to its arguments."""
    defs = _mapping(_field(input_schema, "$defs", "definitions"))

    if not _splittable(input_schema):
        if not _declares_arguments(input_schema):
            # A tool that takes nothing. Nothing to declare, and a port nobody
            # can fill would only have to be closed by every caller.
            return {}, (), None
        # Everything else keeps its argument object whole: a schema whose
        # properties are not independent, or one that names no properties but
        # accepts arbitrary ones, cannot be split without changing what the
        # tool accepts.
        whole = ARGUMENTS_INPUT
        port = a11.ActionPortSchema(
            whole,
            _JSON_MIMETYPE,
            description=(
                "The whole argument object for this tool, whose schema"
                " constrains its properties together in a way individual"
                " ports cannot express."
            ),
            required=bool(_field(input_schema, "required")),
            unary=True,
            json_schema=_json_schema_text(
                organise_and_deduplicate_jsonschema(dict(input_schema))
            ),
        )
        return {whole: port}, (), whole

    required = _field(input_schema, "required") or []
    required_names = {name for name in required if isinstance(name, str)}

    ports: dict[str, a11.ActionPortSchema] = {}
    arguments: list[McpArgument] = []
    for property_name, raw in _field(input_schema, "properties").items():
        schema = _mapping(raw)
        port_name = _unique(
            sanitise_name(str(property_name), fallback="argument"),
            {*ports, *RESERVED_PORT_NAMES},
        )
        item_schema = array_item_schema(schema)
        unary = item_schema is None
        value_schema = schema if unary else item_schema
        description = str(_field(schema, "description") or "")
        if port_name != property_name:
            note = f"MCP argument {property_name!r}."
            description = f"{description} {note}".strip()
        ports[port_name] = a11.ActionPortSchema(
            port_name,
            _port_mimetype(value_schema),
            description=description,
            required=str(property_name) in required_names,
            unary=unary,
            json_schema=_json_schema_text(
                self_contained_schema(value_schema, defs)
            ),
        )
        arguments.append(
            McpArgument(
                port=port_name, property=str(property_name), unary=unary
            )
        )
    return ports, tuple(arguments), None


def _result_ports(
    output_schema: Mapping[str, Any] | None, taken: Collection[str]
) -> tuple[dict[str, a11.ActionPortSchema], str, str, str | None]:
    """The output ports for a tool result, avoiding the names already used.

    An input and an output sharing a name would share one node, which
    `ActionSchema::Validate` refuses, and a tool with a `text` argument is
    perfectly ordinary -- so the argument keeps the name it was given and the
    result port moves.
    """
    reserved = {*taken, *RESERVED_PORT_NAMES}
    text = _unique(TEXT_OUTPUT, reserved)
    content = _unique(CONTENT_OUTPUT, {*reserved, text})
    ports = {
        text: a11.ActionPortSchema(
            text,
            _TEXT_MIMETYPE,
            description="Text blocks of the tool result, in order.",
            unary=False,
            json_schema=_json_schema_text({"type": "string"}),
        ),
        content: a11.ActionPortSchema(
            content,
            _JSON_MIMETYPE,
            description=(
                "Content blocks of the tool result that are not text --"
                " images, audio, resource links and embedded resources -- as"
                " the server sent them."
            ),
            unary=False,
        ),
    }
    if output_schema is None:
        return ports, text, content, None

    structured = _unique(STRUCTURED_OUTPUT, {*reserved, text, content})
    ports[structured] = a11.ActionPortSchema(
        structured,
        _JSON_MIMETYPE,
        description="The tool's structured result.",
        unary=True,
        json_schema=_json_schema_text(
            organise_and_deduplicate_jsonschema(dict(output_schema))
        ),
    )
    return ports, text, content, structured


def action_schema_from_tool(
    tool: Any,
    *,
    server: str = "",
    name: str = "",
    prefix: str = "",
    taken: Collection[str] = (),
) -> McpTool:
    """Read one MCP tool as an Action.

    Args:
        tool: A `tools/list` entry -- the MCP SDK's `Tool`, or the JSON of one.
        server: How to name the server in the schema, for the
            [SERVER][a11.sdk.mcp.schemas.McpHeaders.SERVER] header's default.
        name: Register under this name instead of one derived from the tool's.
        prefix: Prepended to the derived name, to keep two servers' tools apart
            in one registry.
        taken: Names already in use, to derive a distinct one from.

    Returns:
        The [McpTool][a11.sdk.mcp.schemas.McpTool] holding the schema and the
        mapping a handler needs to call the tool with it.

    Raises:
        StatusException: INVALID_ARGUMENT if the entry has no usable name.
    """
    document = tool_document(tool)
    tool_name = str(_field(document, "name") or "")
    if not tool_name:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="An MCP tool must have a name.",
        ).to_exception()

    action_name = name or _unique(
        f"{prefix}{sanitise_name(tool_name)}"[:255], taken
    )

    input_schema = _mapping(_field(document, "inputSchema", "input_schema"))
    raw_output = _field(document, "outputSchema", "output_schema")
    output_schema = _mapping(raw_output) if raw_output is not None else None

    inputs, arguments, whole_arguments = _argument_ports(input_schema)
    outputs, text, content, structured = _result_ports(output_schema, inputs)

    headers = dict(a11.DEFAULT_ACTION_HEADERS)
    headers.update(MCP_HEADERS)
    headers[McpHeaders.TOOL] = a11.ActionHeaderSchema(
        McpHeaders.TOOL,
        "The MCP tool this action calls.",
        tool_name.encode(),
    )
    headers[McpHeaders.SERVER] = a11.ActionHeaderSchema(
        McpHeaders.SERVER,
        "The MCP server this action's handler is bound to.",
        server.encode() if server else None,
    )

    schema = a11.ActionSchema(
        name=action_name,
        description=tool_description(document),
        inputs=inputs,
        outputs=outputs,
        headers=headers,
        # The structured result *is* the result when the tool declares a schema
        # for it, which is what MCP tells a client to prefer. The other ports
        # stay readable for a flow; only the model's view narrows.
        output_to_json_field=(
            {structured: a11.ActionSchema.WHOLE_JSON} if structured else {}
        ),
    )
    schema.validate()

    return McpTool(
        tool_name=tool_name,
        server=server,
        schema=schema,
        arguments=arguments,
        whole_arguments=whole_arguments,
        text_output=text,
        content_output=content,
        structured_output=structured,
    )


__all__ = [
    "ARGUMENTS_INPUT",
    "CONTENT_OUTPUT",
    "MCP_HEADERS",
    "RESERVED_PORT_NAMES",
    "STRUCTURED_OUTPUT",
    "TEXT_OUTPUT",
    "McpArgument",
    "McpHeaders",
    "McpTool",
    "action_schema_from_tool",
    "array_item_schema",
    "sanitise_name",
    "self_contained_schema",
    "tool_description",
    "tool_document",
]
