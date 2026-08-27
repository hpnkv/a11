# Copyright 2026 The A11 Authors.

"""Asking a peer what it can do, and taking it at its word.

Every A11 peer answers `__list_actions__` and `__get_schema__` -- they are
builtins, not registrations, so there is nothing to install and nothing to
announce (see `a11/actions/builtins.h`). This is the client half: call them, and
turn the answer into schemas a local registry can resolve.

`install_schemas` registers what came back **without handlers**, which is not a
shortcut but the contract Flow already reads: a name registered for its schema
alone means "this action lives on the peer, say `call` rather than `run`". It is
what makes `a11 flow run --peer` work, and the reason discovery had to exist
before that could.
"""

from __future__ import annotations

import logging
from collections.abc import Sequence
from typing import TYPE_CHECKING, Any

import a11
from a11 import timing
from a11.actions import describe

if TYPE_CHECKING:  # pragma: no cover - import cycle at runtime only
    from a11.client.connection import GatewayConnection

#: How long a discovery round trip is given before it is called a failure.
DISCOVERY_TIMEOUT = timing.Duration.seconds(30)


async def _consume_or_explain(
    call, port: str, timeout: timing.Duration
) -> dict:
    """The document on ``port``, or the reason the call could not produce one.

    A failed call writes nothing to its output, so the read first reports
    "AsyncNode is empty" instead of the peer's status. The terminal wait carries
    the peer's status and is checked after an empty read. The read error is
    re-raised only when the call completed successfully.
    """
    try:
        return await call[port].consume(dict)
    except Exception:
        await call.wait(timeout)
        raise


def _request(
    names: Sequence[str] = (),
    *,
    exact: Sequence[str] = (),
    all_ports: bool = False,
    include_reserved: bool = False,
    runnable_only: bool = False,
) -> dict[str, Any]:
    request: dict[str, Any] = {}
    if names:
        request["names"] = list(names)
    if exact:
        request["exact"] = list(exact)
    if all_ports:
        request["ports"] = "all"
    if include_reserved:
        request["include_reserved"] = True
    if runnable_only:
        request["runnable_only"] = True
    return request


async def fetch_schemas(
    connection: GatewayConnection,
    *,
    names: Sequence[str] = (),
    exact: Sequence[str] = (),
    all_ports: bool = False,
    include_reserved: bool = False,
    runnable_only: bool = False,
    timeout: timing.Duration | None = None,
) -> list[dict[str, Any]]:
    """Ask the peer what it serves.

    Args:
        connection: An open connection to the peer.
        names: Full-match patterns; empty asks for everything.
        exact: Exact names to include, whether or not they match ``names``.
        all_ports: Keep inputs the peer autofills, flagged. Off by default,
            because a caller cannot write them.
        include_reserved: Include A11's own `__`-prefixed actions.
        runnable_only: Skip actions the peer registered for their schema alone.
        timeout: Bound on the round trip.

    Returns:
        The `actions` entries of the peer's `a11.actions/v1` document.
    """
    bound = timeout or DISCOVERY_TIMEOUT
    call = connection.action(
        describe.LIST_ACTIONS_ACTION, describe.LIST_ACTIONS_SCHEMA
    )
    await call.call()
    await call["request"].finalize(
        _request(
            names,
            exact=exact,
            all_ports=all_ports,
            include_reserved=include_reserved,
            runnable_only=runnable_only,
        )
    )
    document = await _consume_or_explain(call, "actions", bound)
    await call.wait(bound)
    return describe.schemas_in_document(document)


async def fetch_schema(
    connection: GatewayConnection,
    name: str,
    *,
    timeout: timing.Duration | None = None,
) -> dict[str, Any]:
    """Ask the peer about one action.

    Raises:
        StatusException: NOT_FOUND when the peer does not serve ``name``.
    """
    bound = timeout or DISCOVERY_TIMEOUT
    call = connection.action(
        describe.GET_SCHEMA_ACTION, describe.GET_SCHEMA_SCHEMA
    )
    await call.call()
    await call["action"].finalize(name)
    document = await _consume_or_explain(call, "schema", bound)
    await call.wait(bound)
    entries = describe.schemas_in_document(document)
    if not entries:
        raise a11.Status(
            code=a11.StatusCode.NOT_FOUND,
            message=f"The peer described no action named {name!r}.",
        ).to_exception()
    return entries[0]


def install_schemas(
    registry: a11.ActionRegistry,
    described: Sequence[dict[str, Any]],
    *,
    overwrite: bool = False,
) -> list[str]:
    """Register peer action schemas without local handlers.

    Handler-free entries allow a Flow `call` step to resolve an action that runs
    on the peer.

    Args:
        registry: Where to register them.
        described: Entries from [fetch_schemas][].
        overwrite: Replace a name this side already serves. Off by default: a
            local action with a handler is more useful than a remote schema, and
            silently shadowing it would move where the work happens.

    Returns:
        The names registered, in the order they were.
    """
    installed: list[str] = []
    for entry in described:
        name = entry.get("name")
        if not name:
            continue
        if describe.is_reserved_action(name):
            # A builtin is already answerable here; registering the peer's copy
            # would be refused anyway.
            continue
        if not overwrite and registry.is_registered(name):
            logging.debug("keeping the local %r over the peer's", name)
            continue
        try:
            schema = describe.schema_from_json(entry)
        except Exception:
            # Skip one unreadable entry without discarding valid peer schemas.
            logging.warning(
                "could not read the peer's description of %r",
                name,
                exc_info=True,
            )
            continue
        registry.register(name, schema)
        installed.append(name)
    return installed


async def install_peer_actions(
    connection: GatewayConnection,
    registry: a11.ActionRegistry,
    *,
    names: Sequence[str] = (),
    timeout: timing.Duration | None = None,
) -> list[str]:
    """[fetch_schemas][] then [install_schemas][], the common pairing."""
    described = await fetch_schemas(
        connection, names=names, runnable_only=True, timeout=timeout
    )
    return install_schemas(registry, described)


__all__ = [
    "DISCOVERY_TIMEOUT",
    "fetch_schemas",
    "fetch_schema",
    "install_peer_actions",
    "install_schemas",
]
