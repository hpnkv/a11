# Copyright 2026 The A11 Authors.

"""One A11 action, declared as an MCP tool.

The mirror of [a11.sdk.mcp.schemas][a11.sdk.mcp.schemas], and like it a pure
translation: no transport, no session, no `mcp` import. It reads the
`a11.actions/v1` document a registry describes itself with and writes the
`tools/list` entry an MCP client reads, so a declaration can be derived and
tested wherever A11 runs.

Deriving from the *document* rather than from the live `ActionSchema` is what
keeps three views of one action identical: what a model is offered through
[definition_from_schema][a11.sdk.llm_tools.runner.definition_from_schema], what
a peer is told by [a11.actions.describe][a11.actions.describe], and what an MCP
client discovers here. It also means a peer's action -- registered for its
schema alone, with no Python type anywhere in this process -- declares as well
as a local one.

The rules:

* **An input port becomes a property of `inputSchema`.** The whole document
  comes from `definition_from_schema`, which already skips autofilled inputs,
  carries each port's own `json_schema`, and gives a streaming port an array of
  its item schema.
* **The declared outputs become `outputSchema`**, through
  [output_definition_from_schema][a11.sdk.llm_tools.runner.output_definition_from_schema],
  and only when what they describe is a JSON object: MCP's `structuredContent`
  is an object, so an action whose whole result is a string declares no output
  schema and answers in text.
* **A port carrying pictures or sound is left out of `outputSchema`**, because
  MCP has content blocks for those and a base64 string inside a JSON document
  is not one. See [media_outputs][a11.sdk.mcp.tools.media_outputs].
* **The action's own entry rides `_meta`**, under
  [McpMeta.ACTION][a11.sdk.mcp.schemas.McpMeta.ACTION]. Any client may ignore
  it; an A11 client reads it and rebuilds the real `ActionSchema` -- port
  names, streaming ports, header schemas -- rather than re-deriving an
  approximation from JSON Schema.

An MCP tool name has no character restrictions to satisfy, so an A11 action
name travels verbatim and `sanitise_name` is a no-op on the way back.

Behaviour hints (`readOnlyHint` and the rest) are not declared. An
`ActionSchema` states nothing about whether an action modifies its
environment; an absent hint is what a client reads as unknown.
"""

from __future__ import annotations

import dataclasses
from collections.abc import Collection, Mapping, Sequence
from typing import Any

import a11
from a11.actions import describe
from a11.sdk.llm_tools.runner import (
    definition_from_schema,
    output_definition_from_schema,
    whole_json_port,
)
from a11.sdk.mcp.schemas import McpMeta
from a11.status import Status, StatusCode

#: Port media types that become MCP content blocks rather than JSON.
MEDIA_PREFIXES = ("image/", "audio/")

#: Which actions a server exposes when it is given no patterns.
ALL_ACTIONS = (".*",)


def is_media_type(mimetype: str) -> bool:
    """Whether a port of this media type becomes an MCP content block."""
    return any(mimetype.startswith(prefix) for prefix in MEDIA_PREFIXES)


def media_outputs(entry: Mapping[str, Any]) -> dict[str, str]:
    """The output ports that carry pictures or sound, by media type.

    MCP has `ImageContent` and `AudioContent` for these, and a client shows
    them to a person. Base64 inside a `structuredContent` document is a string
    to everything downstream, so such a port leaves the structured result and
    becomes a content block.

    A schema with an ``output_to_json_field`` mapping states exactly what its
    result document is, and that statement is honoured whole: no port is taken
    out of it, and this returns ``{}``.
    """
    if entry.get("output_to_json_field"):
        return {}
    found: dict[str, str] = {}
    for port in entry.get("outputs", ()):
        name = port.get("name")
        if name and is_media_type(str(port.get("type", ""))):
            found[name] = str(port.get("type", ""))
    return found


def _structured_entry(
    entry: Mapping[str, Any], media: Mapping[str, str]
) -> dict[str, Any]:
    """``entry`` with the media ports removed, for the output schema."""
    if not media:
        return dict(entry)
    document = dict(entry)
    document["outputs"] = [
        port
        for port in entry.get("outputs", ())
        if port.get("name") not in media
    ]
    return document


@dataclasses.dataclass(frozen=True)
class ActionTool:
    """One A11 action as an MCP tool: the declaration, and how to answer it.

    The declaration is the public half -- it is what `tools/list` returns. The
    rest is what [a11.sdk.mcp.calls][a11.sdk.mcp.calls] needs to turn one
    `tools/call` into an action run and its outputs back into a result, and is
    why the translation returns an object rather than a bare document.
    """

    #: The action's name, which is also the tool's.
    action_name: str
    #: The `a11.actions/v1` entry the declaration was derived from.
    entry: dict[str, Any]
    #: The `tools/list` entry, as MCP's wire spells it.
    tool: dict[str, Any]
    #: Output ports carried as content blocks, by media type.
    media: Mapping[str, str] = dataclasses.field(default_factory=dict)
    #: Result fields the output schema declares as sequences.
    sequences: frozenset[str] = frozenset()

    @property
    def structured(self) -> bool:
        """Whether the tool declares an output schema."""
        return "outputSchema" in self.tool

    @property
    def whole_json(self) -> str | None:
        """The port whose payload is the whole result, when there is one."""
        return whole_json_port(self.entry.get("output_to_json_field") or {})


def tool_from_entry(
    entry: Mapping[str, Any], *, describe_action: bool = True
) -> ActionTool:
    """Declare one described action as an MCP tool.

    Args:
        entry: One `a11.actions/v1` entry, as
            [schemas_in_document][a11.actions.describe.schemas_in_document]
            yields them.
        describe_action: Carry the entry itself in the tool's `_meta`, so an
            A11 client can rebuild the action's real schema. Pass False for a
            declaration with nothing in it but MCP's own fields.

    Returns:
        The [ActionTool][a11.sdk.mcp.tools.ActionTool] holding the declaration
        and what answering a call to it needs.
    """
    name = str(entry.get("name") or "")
    if not name:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="A described action must have a name.",
        ).to_exception()

    tool: dict[str, Any] = {
        "name": name,
        "inputSchema": definition_from_schema(dict(entry))["input_schema"],
    }
    if description := str(entry.get("description") or ""):
        tool["description"] = description

    media = media_outputs(entry)
    output_schema = output_definition_from_schema(
        _structured_entry(entry, media)
    )
    # MCP's structured result is an object. An action whose result is a string,
    # a number or a picture answers in content blocks instead.
    if output_schema.get("type") == "object":
        tool["outputSchema"] = output_schema
    if describe_action:
        tool["_meta"] = {McpMeta.ACTION.value: dict(entry)}

    return ActionTool(
        action_name=name,
        entry=dict(entry),
        tool=tool,
        media=media,
        sequences=sequence_fields(entry),
    )


def sequence_fields(entry: Mapping[str, Any]) -> frozenset[str]:
    """The result fields a streaming output port fills.

    A port that carried one value decodes to that value rather than to a list
    of one, which a client validating the result against the output schema
    reads as the wrong type. Naming the sequences is what lets
    [call_action][a11.sdk.mcp.calls.call_action] send the list it declared.
    """
    mapping = entry.get("output_to_json_field") or {}
    streaming = {
        str(port.get("name"))
        for port in entry.get("outputs", ())
        if port.get("name") and not port.get("unary", False)
    }
    if not mapping:
        return frozenset(streaming)
    return frozenset(
        field for port, field in mapping.items() if port in streaming
    )


def tools_from_registry(
    registry: a11.ActionRegistry,
    patterns: Sequence[str] = ALL_ACTIONS,
    *,
    describe_action: bool = True,
    skip: Collection[str] = (),
) -> list[ActionTool]:
    """Declare everything ``registry`` serves and ``patterns`` admits.

    Asks the registry to describe itself and translates each answer, which is
    the same route
    [get_tool_definitions][a11.sdk.llm_tools.runner.get_tool_definitions] takes
    to a model -- so what an MCP client discovers and what a model is offered
    cannot drift.

    Args:
        registry: The actions to serve.
        patterns: Full-match regular expressions naming what to expose, the
            same shape as `x-a11-allowed-llm-actions`. Everything, by default.
        describe_action: Carry each action's entry in its tool `_meta`.
        skip: Action names to leave out whatever the patterns say.

    Returns:
        One [ActionTool][a11.sdk.mcp.tools.ActionTool] per exposed action, by
        name. A11's own reserved actions are protocol operations rather than
        tools and are never included, and neither is an action this side holds
        no handler for.
    """
    document = describe.registry_to_json(
        registry, {"names": list(patterns), "runnable_only": True}
    )
    tools: list[ActionTool] = []
    for entry in describe.schemas_in_document(document):
        if str(entry.get("name") or "") in skip:
            continue
        tools.append(tool_from_entry(entry, describe_action=describe_action))
    return tools


__all__ = [
    "ALL_ACTIONS",
    "MEDIA_PREFIXES",
    "ActionTool",
    "is_media_type",
    "media_outputs",
    "sequence_fields",
    "tool_from_entry",
    "tools_from_registry",
]
