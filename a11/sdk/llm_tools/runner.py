import asyncio
import dataclasses
import json
from collections import defaultdict
from collections.abc import AsyncIterator, Mapping
from typing import Any

from absl import logging

from a11 import _native
from a11._native import NodeFragment

import a11
from a11.actions import describe
from a11.actions.jsonschema import organise_and_deduplicate_jsonschema
from a11.status import Status, StatusCode, StatusException

from a11.sdk.llm import (
    action_name_matches_allowed,
    get_allowed_llm_action_patterns,
    Interaction,
    TOOL_LOGS_METADATA_KEY,
    WHOLE_JSON_FRAGMENT_ID,
)
from a11.sdk.llm_tools.adapter import WHOLE_JSON_OUTPUT, ToolAdapter


def definition_from_schema(entry: dict[str, Any]) -> dict[str, Any]:
    """The model's view of one action, from its `a11.actions/v1` entry.

    Derive the model definition from the written schema, including each port's
    ``json_schema``. This also supports peer actions whose Python types are not
    available locally.
    """
    properties: dict[str, Any] = {}
    required: list[str] = []
    for port in entry.get("inputs", ()):
        name = port.get("name")
        if not name or port.get("autofilled"):
            # An autofilled input is the receiver's own: the runtime requires it
            # empty before applying the default, so a model writing it fails.
            continue
        # `{"type": "object"}` where the port stated no type, which is what the
        # adapter has always shown for an untyped port.
        schema = port.get("json_schema") or {"type": "object"}
        if not port.get("unary", False):
            schema = {"type": "array", "items": schema}
            if port.get("required"):
                schema["minItems"] = 1
        properties[name] = schema
        if port.get("required"):
            required.append(name)

    input_schema: dict[str, Any] = {"type": "object", "properties": properties}
    if required:
        input_schema["required"] = required
    return {
        "name": entry.get("name", ""),
        "description": entry.get("description", ""),
        # Hoist each port schema's root `$defs` into the assembled document so
        # its `$ref` values resolve against the correct root.
        "input_schema": organise_and_deduplicate_jsonschema(input_schema),
    }


def output_definition_from_schema(entry: dict[str, Any]) -> dict[str, Any]:
    """The JSON Schema of what one action returns, from its entry.

    The output half of
    [definition_from_schema][a11.sdk.llm_tools.runner.definition_from_schema],
    read the same way -- from the written document, so it holds for a peer's
    action as well as a local one. A backend that declares what a tool returns
    (MCP's `outputSchema`, a structured-output request) states this document.

    ``output_to_json_field`` decides the shape, matching what
    [collect_action_outputs][a11.sdk.llm_tools.runner.collect_action_outputs]
    actually produces: no mapping describes an object of the declared outputs;
    a field mapping renames them and drops the ports it does not name; and
    ``{"port": "$"}`` makes that port's own schema the whole document.

    Two things the input half states are left out here, because an output
    schema is a claim the result has to satisfy and neither claim holds. A port
    with no `json_schema` is described as unconstrained rather than as an
    object, and no field is `required`: a port the handler closed empty carries
    no value, and a result missing that field is an ordinary outcome rather
    than a broken promise.

    Returns:
        The schema, or ``{}`` when the action declares no outputs.
    """
    properties: dict[str, Any] = {}
    for port in entry.get("outputs", ()):
        name = port.get("name")
        if not name:
            continue
        schema = port.get("json_schema") or {}
        if not port.get("unary", False):
            schema = {"type": "array", "items": schema}
        properties[name] = schema

    mapping = entry.get("output_to_json_field") or {}
    whole = whole_json_port(mapping)
    if whole is not None:
        return organise_and_deduplicate_jsonschema(properties.get(whole, {}))
    if mapping:
        properties = {
            mapping[name]: schema
            for name, schema in properties.items()
            if name in mapping
        }
    if not properties:
        return {}

    return organise_and_deduplicate_jsonschema(
        {"type": "object", "properties": properties}
    )


def whole_json_port(mapping: Mapping[str, str]) -> str | None:
    """The port an ``output_to_json_field`` mapping makes the whole result.

    ``{"port": "$"}`` means the port's payload *is* the result document rather
    than a field of one. The sentinel is the mapped-to field, matching
    `ActionSchema::kWholeJson` in `cpp/a11/actions/schema.cc` and
    [WHOLE_JSON_OUTPUT][a11.sdk.llm_tools.adapter.WHOLE_JSON_OUTPUT].
    """
    if len(mapping) != 1:
        return None
    port, field = next(iter(mapping.items()))
    return port if field == WHOLE_JSON_OUTPUT else None


def get_tool_definitions(
    registry: a11.ActionRegistry | None,
    allowed_actions: list[str] | None = None,
) -> list[dict[str, Any]]:
    """What the model is shown, for each of ``allowed_actions`` registered here.

    Built by describing the registry and deriving each definition, so the
    document a model sees and the document a peer is told cannot drift.
    """
    if registry is None or not allowed_actions:
        return []
    document = describe.registry_to_json(
        registry, {"exact": list(allowed_actions)}
    )
    return [
        definition_from_schema(entry)
        for entry in describe.schemas_in_document(document)
    ]


async def collect_tools(
    action: a11.Action, deadline: a11.Duration | None = None
) -> list[dict[str, Any]]:
    """The tools this turn may use: the caller's, plus the registry's own.

    Two sources meet here, and the allowed-action patterns are what admits
    either. A caller streams tool definitions onto the ``tools`` input -- its
    own tools, which it will serve itself when the model calls them -- and
    anything it sends that the patterns do not match is dropped. The actions
    registered *here* are then offered too, for every registered name the
    patterns match that the caller did not describe: a peer that says
    ``shell_.*`` is asking for this side's shell tools, and the alternative
    would be for it to reproduce their schemas by hand to be offered them.

    A registry action can only be *called* if it matches the same patterns
    (:func:`execute_actions_from_interaction` checks them again at call time),
    so this widens what the model is shown as far as the caller allowed.

    Between the two, the peer is *asked*. If a tool bridge is bound to this
    session and has not asked yet, it calls `__list_actions__` on the caller and
    registers a proxy per answer, allowing the model to use the caller's tools
    without prior registration. Discovery occurs here, after the session pump
    starts, and is skipped for connections without model interactions.
    """
    deadline = deadline if deadline is not None else a11.get_deadline(action)
    allowed_patterns = get_allowed_llm_action_patterns(action)

    tools: list[dict[str, Any]] = []
    async for tool in action["tools"].iter_with_deadline(deadline):
        if not action_name_matches_allowed(tool["name"], allowed_patterns):
            logging.warning(
                "Tool `%s` was requested, but isn't allowed.", tool["name"]
            )
            continue
        tools.append(tool)

    registry = action.get_registry()
    if registry is None:
        return tools

    await _ask_the_peer(action)

    requested = {tool["name"] for tool in tools}
    local = sorted(
        name
        for name in registry.list_registered_actions()
        if name not in requested and name != action.get_schema().name
        # A11's reserved actions are protocol operations, not model tools.
        and not describe.is_reserved_action(name)
        and action_name_matches_allowed(name, allowed_patterns)
    )
    tools.extend(get_tool_definitions(registry, local))
    return tools


async def _ask_the_peer(action: a11.Action) -> None:
    """Have this connection's tool bridge discover the caller's tools, once.

    Imported here rather than at module scope: the SDK must not depend on the
    gateway, and a client running `interact_with_llm` locally has no bridge at
    all. A missing bridge is the ordinary case, not a failure.
    """
    try:
        from a11.gateway.tool_bridge import RemoteToolBridge
    except ImportError:
        return
    bridge = RemoteToolBridge.of(action.get_session())
    if bridge is None:
        return
    discovered = await bridge.discover()
    if discovered:
        logging.info(
            "the caller serves %d tool(s): %s",
            len(discovered),
            ", ".join(discovered),
        )


@dataclasses.dataclass(frozen=True)
class ExecutedActions:
    """The result of running one interaction's tool calls, keyed by call id.

    ``outputs`` holds each call's output fragments and ``errors`` the status
    of each call that failed. A call appears in ``outputs`` either way — a
    backend owes the model one result per call it made, and for a failed call
    that result is the failure.

    ``logs`` holds what the called actions wrote through
    [log][a11.actions.action.Action.log]: narration for the person watching. It
    is not in ``outputs`` and cannot get there -- the log port is not one of the
    action's declared outputs -- so it can never reach the model. Only the calls
    that logged something user-facing appear; a log marked ``internal`` is A11's
    own bookkeeping and is left out.
    """

    outputs: dict[str, list[NodeFragment]]
    errors: dict[str, Status]
    logs: dict[str, str] = dataclasses.field(default_factory=dict)

    def log_metadata(self) -> dict[str, bytes]:
        """This round's logs as interaction metadata; ``{}`` if there are none.

        Merge into the ``backend_specific_metadata`` of the interaction carrying
        these calls' results, which is where a consumer looks for them; see
        [TOOL_LOGS_METADATA_KEY][a11.sdk.llm.TOOL_LOGS_METADATA_KEY].
        """
        if not self.logs:
            return {}
        return {TOOL_LOGS_METADATA_KEY: json.dumps(self.logs).encode()}

    def error_message(self, call_id: str) -> str | None:
        """A caller-facing description of a call's failure, if it failed."""
        status = self.errors.get(call_id)
        if status is None:
            return None
        if not status.message:
            return status.code.name
        return f"{status.code.name}: {status.message}"


DRAIN_AFTER_COMPLETION = a11.Duration.seconds(5)


def _drain_timeout(deadline: a11.Time) -> a11.Duration:
    """How long to wait for what a finished call still owes us."""
    remaining = max(deadline - a11.now(), a11.zero_duration())
    return min(remaining, DRAIN_AFTER_COMPLETION)


async def user_facing_log_entries(
    node: a11.AsyncNode, timeout: a11.Duration
) -> AsyncIterator[str]:
    """What a call logs for the person watching, one entry at a time.

    Reads the call's log port and keeps the entries that are *not* marked
    internal -- "user facing" is the absence of that flag, so a handler
    narrating itself needs no second port and no second decision. Each entry is
    rendered by
    [log_record_from_chunk][a11._native.log_record_from_chunk], which is also
    what the ``a11.action`` logger uses, so a line reads the same wherever it is
    shown.

    Yields as the entries arrive, for a caller relaying narration while the
    action is still running -- an MCP server turning them into progress
    notifications, say.
    [user_facing_log][a11.sdk.llm_tools.runner.user_facing_log] is the whole
    block, for a caller that reports once at the end.
    """
    try:
        async for chunk in node.iter_chunks(timeout=timeout):
            if (
                chunk is None
                or chunk.is_null()
                or _native.is_status_chunk(chunk)
            ):
                continue
            try:
                record = _native.log_record_from_chunk(chunk)
            except Exception:
                logging.debug("undecodable log chunk", exc_info=True)
                continue
            if record["internal"]:
                continue
            yield record["text"]
    except StatusException:
        logging.debug("a tool's log port did not end", exc_info=True)


async def user_facing_log(node: a11.AsyncNode, timeout: a11.Duration) -> str:
    """What a call logged for the person watching, as one block of text."""
    return "".join(
        [text async for text in user_facing_log_entries(node, timeout)]
    )


async def feed_action_inputs(
    action: a11.Action, fragments: list[NodeFragment | None]
) -> None:
    """Write one call's encoded arguments onto the action, then close them.

    Takes what
    [ActionCallAdapter.get_action_inputs][a11.sdk.llm.ActionCallAdapter.get_action_inputs]
    produced. A ``None`` in that list stands for an argument whose fragments
    carried no finality -- an empty list argument writes nothing -- and the
    close below is what ends the port.

    Autofilled inputs are written, drained and closed by the native run flow,
    so only the inputs fed here are closed. Closed, not finalized: the
    forwarded fragments carry whatever finality the caller's arguments had.
    """
    for fragment in fragments:
        if fragment is None:
            continue
        await action[fragment.id].put_fragment(fragment)
    for input_name, port in action.get_schema().inputs.items():
        if port.autofills:
            continue
        await action[input_name].close()


async def collect_action_outputs(
    action: a11.Action, deadline: a11.Time
) -> tuple[list[NodeFragment], Status | None]:
    """Everything one finished action wrote, as fragments a caller can report.

    Drains every declared output port, then applies the schema's
    ``output_to_json_field``: each fragment's id becomes the field it maps to,
    a port the mapping does not name is left out, and a whole-JSON port becomes
    [WHOLE_JSON_FRAGMENT_ID][a11.sdk.llm.WHOLE_JSON_FRAGMENT_ID] so that
    [decode_action_output_fragments][a11.sdk.llm.decode_action_output_fragments]
    reads the payload as the result rather than as a field of one.

    Returns:
        The fragments, and the status that ended a port early. An aborted
        output carries the failure that aborted it and a timed-out one the
        deadline; whatever arrived before it is kept.
    """
    failure: Status | None = None
    per_port: dict[str, list[NodeFragment]] = {}
    for output_name in action.get_schema().outputs.keys():
        per_port[output_name] = []
        try:
            node = action[output_name]
            async for fragment in node.iter_fragments(
                timeout=_drain_timeout(deadline)
            ):
                fragment.id = output_name
                fragment.continued = True
                per_port[output_name].append(fragment)
        except StatusException as error:
            if failure is None:
                failure = error.status
        if per_port[output_name]:
            per_port[output_name][-1].continued = False

    mapping = dict(action.get_schema().output_to_json_field)
    fragments: list[NodeFragment] = []
    whole = whole_json_port(mapping)
    if whole is not None:
        for fragment in per_port.get(whole, []):
            fragment.id = WHOLE_JSON_FRAGMENT_ID
            fragments.append(fragment)
        return fragments, failure

    for output_name, port_fragments in per_port.items():
        map_to = mapping.get(output_name) if mapping else output_name
        if map_to is None:
            continue
        for fragment in port_fragments:
            fragment.id = map_to
            fragments.append(fragment)
    return fragments, failure


def _status_of(error: BaseException) -> Status:
    """The status an action failed with, however the failure reached us."""
    if isinstance(error, StatusException):
        return error.status
    if isinstance(error, asyncio.TimeoutError):
        return Status(
            code=StatusCode.DEADLINE_EXCEEDED, message="The action timed out."
        )
    return Status(code=StatusCode.UNKNOWN, message=str(error))


async def execute_actions_from_interaction(
    interaction: Interaction,
    action: a11.Action,
    registry: a11.ActionRegistry | None = None,
    rejected: Mapping[str, Status] | None = None,
) -> ExecutedActions:
    """Run an interaction's tool calls and collect a result for each of them.

    One call failing does not sink the others: every call is run, every call
    is waited for, and a failure is recorded against that call alone. The
    model asked for each of these independently and can act on a partial
    answer.
    """
    deadline = a11.get_deadline(action)

    registry = registry or action.get_registry()
    if registry is None:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="Cannot execute actions against an empty registry.",
        ).to_exception()

    allowed_patterns = get_allowed_llm_action_patterns(action)
    nested_actions: list[a11.Action] = []
    log_nodes: dict[str, a11.AsyncNode] = {}
    errors: dict[str, Status] = dict(rejected or {})
    for call in interaction.action_calls:
        # Setting a call up can fail on its own — an action the model may not
        # call, a name no longer in the registry, inputs that will not go onto
        # their ports. That is this call's failure, not the round's.
        try:
            if not action_name_matches_allowed(call.name, allowed_patterns):
                raise Status(
                    code=StatusCode.PERMISSION_DENIED,
                    message=f"Action {call.name} is not allowed",
                ).to_exception()

            nested_action = (
                action.make_nested(registry.get_schema(call.name))
                .set_id(call.id)
                .bind_stream(None)
                .bind_handler(registry.get_handler(call.name))
            )
            a11.set_deadline_header(nested_action, deadline)
            # Claimed before the run, because the log the handler writes on its
            # first line has to arrive here rather than on the process sink:
            # this runner is the consumer that shows it to a person.
            log_nodes[call.id] = nested_action.get_log_node()
            nested_action.run()
            await feed_action_inputs(
                nested_action, interaction.action_inputs.get(call.id, [])
            )
            nested_actions.append(nested_action)
        except Exception as error:
            errors[call.id] = _status_of(error)

    # `return_exceptions` is the point: a raising `wait` must not abandon the
    # other calls mid-flight, leaving results the model is never told about.
    finished = await asyncio.gather(
        *[
            nested_action.wait(max(deadline - a11.now(), a11.zero_duration()))
            for nested_action in nested_actions
        ],
        return_exceptions=True,
    )
    for nested_action, outcome in zip(nested_actions, finished):
        if isinstance(outcome, BaseException):
            errors[nested_action.id] = _status_of(outcome)

    all_outputs: dict[str, list[NodeFragment]] = defaultdict(list)
    logs: dict[str, str] = {}
    # Every call the model made gets an entry, including one that failed before
    # it could run. Backends pair exactly one tool result with each tool call,
    # so a call missing here is a protocol error on the next request rather than
    # an empty result — and the turn then dies after the tools have run, which
    # reads as the model falling silent.
    for call in interaction.action_calls:
        all_outputs.setdefault(call.id, [])
    for call_id in errors:
        all_outputs.setdefault(call_id, [])

    for nested_action in nested_actions:
        fragments, failure = await collect_action_outputs(
            nested_action, deadline
        )
        all_outputs[nested_action.id] = fragments
        if failure is not None:
            errors.setdefault(nested_action.id, failure)

        # Read separately from the outputs above, because the log port is not
        # one of them: it is not in the schema, so the model's tool result is
        # built from what the action declared and the narration cannot leak into
        # it by accident. Nothing has to close the port either -- the action
        # does, with its other outputs -- so an action that never logs costs one
        # ended read.
        node = log_nodes.get(nested_action.id)
        if node is not None:
            try:
                if log := await user_facing_log(node, _drain_timeout(deadline)):
                    logs[nested_action.id] = log
            except StatusException as error:
                errors.setdefault(nested_action.id, error.status)

    return ExecutedActions(outputs=dict(all_outputs), errors=errors, logs=logs)
