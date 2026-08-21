# Copyright 2026 The A11 Authors.

"""An `ActionSchema` in JSON, which is how one travels.

One concept in two representations, and this is the crossing between them. An
`ActionSchema` is the live object; a port's `typeinfo` is a Python type and an
input's autofills are receiver-owned values, so neither can go on a wire. The
`a11.actions/v1` document is the same schema written as text, with `typeinfo`
replaced by the port's `json_schema` and the autofill values by an `autofilled`
flag. Its shape lives in `cpp/a11/actions/describe.h`; these are thin wrappers
over it.

The one field a document carries that a schema does not is `runnable`, which
says whether the answering side holds a handler. It is the registry's annotation
on a schema rather than part of one -- the same schema is runnable here and
schema-only there, and that difference is what Flow reads to choose `run` over
`call`.

What Python adds is the one thing C++ cannot do: derive a port's `json_schema`
from its `typeinfo`, which only a Python runtime can read. That happens at
**registration**, not when a schema is written out, and that is the whole
design. A peer asking `__list_actions__` is answered by the native writer, which
sees only what is on the schema; if the derivation happened in a Python path
instead, a schema written locally would carry types and the same schema written
for a peer would not. Deriving once, when the action becomes discoverable, is
what makes those two answers the same document.
"""

from __future__ import annotations

import json
import logging
from typing import Any

from a11 import _native
from a11._native import ActionPortSchema, ActionRegistry, ActionSchema
from a11.actions.jsonschema import (
    get_json_schema_type,
    organise_and_deduplicate_jsonschema,
)

#: The `format` field every document produced here carries.
SCHEMA_DOCUMENT_FORMAT = "a11.actions/v1"

#: Output-to-JSON target meaning "this port is the whole result document".
#: Mirrors C++ `ActionSchema::kWholeJson` and Kotlin's `WHOLE_JSON_OUTPUT`.
WHOLE_JSON_OUTPUT = "$"

#: Name of the builtin that lists what a peer serves.
LIST_ACTIONS_ACTION = "__list_actions__"
#: Name of the builtin that describes one action.
GET_SCHEMA_ACTION = "__get_schema__"

# The builtins' schemas, for a *caller*. A client needs these to build the call
# before it has asked anything, and it cannot get them from its own registry's
# copy of the native table without a round trip through the describer -- so they
# are spelled here, and pinned against the native table by a test.
LIST_ACTIONS_SCHEMA = ActionSchema(
    name=LIST_ACTIONS_ACTION,
    description="List the actions this peer serves, with their schemas.",
    inputs={
        "request": ActionPortSchema(
            name="request",
            type="application/json",
            description="Which actions to describe. Absent means all of them.",
            unary=True,
            typeinfo=dict,
        )
    },
    outputs={
        "actions": ActionPortSchema(
            name="actions",
            type="application/json",
            description="The a11.actions/v1 document, whole.",
            required=True,
            unary=True,
            typeinfo=dict,
        )
    },
)

GET_SCHEMA_SCHEMA = ActionSchema(
    name=GET_SCHEMA_ACTION,
    description="Describe one action this peer serves.",
    inputs={
        "action": ActionPortSchema(
            name="action",
            type="text/plain",
            description="Name of the action to describe.",
            required=True,
            unary=True,
            typeinfo=str,
        )
    },
    outputs={
        "schema": ActionPortSchema(
            name="schema",
            type="application/json",
            description="The a11.actions/v1 document for that one action.",
            required=True,
            unary=True,
            typeinfo=dict,
        )
    },
)


def json_schema_for(port: ActionPortSchema) -> str:
    """A JSON Schema for ``port``'s payload, as text, or ``""``.

    Empty when the port has no `typeinfo` to derive from -- which is not a
    failure. A port whose type nobody stated is described without one, and a
    model shown that port gets `{"type": "object"}` from the adapter, exactly as
    before this field existed.
    """
    if port.json_schema:
        return port.json_schema
    if port.typeinfo is None:
        return ""
    try:
        derived = organise_and_deduplicate_jsonschema(
            get_json_schema_type(port.typeinfo)
        )
        return json.dumps(derived)
    except Exception:
        # A type that will not describe itself is worth a line and nothing more:
        # the action still works, and the alternative is refusing to register it
        # over a field only a model reads.
        logging.debug(
            "could not derive a JSON Schema for port %r", port.name,
            exc_info=True,
        )
        return ""


def fill_json_schemas(schema: ActionSchema) -> ActionSchema:
    """Derives `json_schema` for every port of ``schema`` that lacks one.

    Mutates and returns ``schema``. Idempotent: a port that already has one is
    left alone, so a caller that stated a schema by hand keeps theirs.
    """
    for ports in (schema.inputs, schema.outputs):
        for name in list(ports):
            port = ports[name]
            derived = json_schema_for(port)
            if derived and not port.json_schema:
                port.json_schema = derived
                # The map is a view over the native schema; assigning back is
                # what commits a mutated port on every binding.
                ports[name] = port
    return schema


def schema_to_json(
    schema: ActionSchema,
    *,
    runnable: bool = True,
    all_ports: bool = False,
) -> dict[str, Any]:
    """One action's `a11.actions/v1` **entry**.

    The entry rather than the envelope: a caller with one schema in hand wants
    the thing that goes in an `actions` array, and
    [registry_to_json][a11.actions.describe.registry_to_json] is what produces
    a whole document. The native call returns the envelope so that the HTTP
    endpoint's item route and its collection route have the same shape; this
    unwraps it.

    Args:
        schema: The action's interface.
        runnable: Whether this side holds a handler for it. A schema registered
            without one means "this action lives on the peer".
        all_ports: Keep inputs the receiver autofills, flagged. A caller cannot
            write them, so they are omitted by default.
    """
    envelope = json.loads(
        _native.schema_to_json(schema, runnable=runnable, all_ports=all_ports)
    )
    entries = schemas_in_document(envelope)
    return entries[0] if entries else {}


def registry_to_json(
    registry: ActionRegistry,
    request: Any = None,
) -> dict[str, Any]:
    """Every action in ``registry`` as one `a11.actions/v1` document.

    Args:
        registry: The registry to describe.
        request: What `__list_actions__` takes on its `request` port -- a
            mapping with any of ``names`` (full-match patterns), ``exact``,
            ``ports`` (``"callable"`` or ``"all"``), ``include_reserved`` and
            ``runnable_only``; or a bare list of patterns; or None for all of
            them.
    """
    return json.loads(_native.registry_to_json(registry, request))


def schemas_in_document(document: Any) -> list[dict[str, Any]]:
    """The entries of a document, accepting the envelope or a bare list.

    A caller handed one or the other -- a whole document from `__list_actions__`
    or just its entries -- should not have to care which.
    """
    if isinstance(document, list):
        return list(document)
    if isinstance(document, dict):
        entries = document.get("actions")
        if isinstance(entries, list):
            return list(entries)
    raise ValueError(
        "An action schema document must be an a11.actions/v1 envelope or a"
        " list of its entries"
    )


def schema_from_json(described: dict[str, Any]) -> ActionSchema:
    """Rebuilds an `ActionSchema` from one described action.

    For a side that has to *call* what it was told about: a tool bridge
    registering a reverse-dispatch proxy, or a flow run against a peer
    registering the peer's actions for their schemas alone.

    What cannot survive the trip does not: a port's `typeinfo` is a local handle
    and comes back None, and an input's autofills are receiver-owned defaults
    that deliberately never travel. A port's `json_schema` does survive, which
    is what lets a model be shown a remote tool's real argument types.
    """
    return _native.schema_from_json(json.dumps(described))


def builtin_action_names() -> list[str]:
    """The actions every registry answers for, whatever it was built to do."""
    return list(_native.builtin_action_names())


def is_reserved_action(name: str) -> bool:
    """Whether ``name`` is one of A11's own, rather than an application's.

    The `__`-prefix rule, which replaces the hand-maintained exclusion lists
    that each discovery workaround kept for itself.
    """
    return len(name) > 4 and name.startswith("__")


__all__ = [
    "SCHEMA_DOCUMENT_FORMAT",
    "GET_SCHEMA_ACTION",
    "GET_SCHEMA_SCHEMA",
    "LIST_ACTIONS_ACTION",
    "LIST_ACTIONS_SCHEMA",
    "WHOLE_JSON_OUTPUT",
    "builtin_action_names",
    "registry_to_json",
    "schema_to_json",
    "schemas_in_document",
    "fill_json_schemas",
    "is_reserved_action",
    "json_schema_for",
    "schema_from_json",
]
