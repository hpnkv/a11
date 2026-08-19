"""Action schemas, lifecycle types, and Python validation conveniences.

An [Action][a11.actions.action.Action] is one schema-described unit of agent
work. Its input and output ports map to AsyncNodes, so a handler can consume and
produce streaming values locally or across a Session. This module keeps the
native runtime objects as the public model while adding Pydantic-style schema
validation and helpers for the structured status chunks used during dispatch
and completion.
"""

from __future__ import annotations

import base64
import enum
from collections.abc import Awaitable, Callable, Mapping
from typing import Any

import pydantic_core
from pydantic_core import core_schema

from a11 import _native
from a11.actions._native_action import Action
from a11.data import types
from a11.status import Status, StatusCode, StatusException

from a11._native import ACTION_STATUS_MIMETYPE
from a11._native import ACTION_STATUS_OUTPUT
from a11._native import ACTION_DISPATCH_STATUS_OUTPUT
from a11._native import NativeActionHandler

#: Reserved remote action name used to request cooperative cancellation.
CANCEL_ACTION_NAME = getattr(_native, "CANCEL_ACTION_NAME", "__cancel__")
#: Header on a cancellation message containing the target action id.
CANCEL_ACTION_HEADER = getattr(_native, "CANCEL_ACTION_HEADER", "__action")
#: Prefix reserved for A11 runtime metadata forwarded between nested actions.
ACTION_HEADER_PREFIX = getattr(_native, "ACTION_HEADER_PREFIX", "x-a11-")

#: Default concurrency ceiling for child actions under one runtime context.
DEFAULT_MAX_CONCURRENT_NESTED_ACTIONS = getattr(
    _native, "DEFAULT_MAX_CONCURRENT_NESTED_ACTIONS", 64
)

#: Async/sync application callable invoked by ``Action.run()``, or an opaque
#: handle to an Action implemented in C++ (see
#: [NativeActionHandler][a11.actions.action.NativeActionHandler]). Both are
#: accepted by ``ActionRegistry.register()`` and ``Action.bind_handler()``.
ActionHandler = (
    Callable[["Action"], Awaitable[None] | None] | NativeActionHandler
)
#: Hook invoked once when cooperative action cancellation is requested.
OnActionCancelled = Callable[["Action"], Any]


from a11._native import ActionPortSchema
from a11._native import ActionHeaderSchema
from a11._native import ActionSchema
from a11._native import ActionSettings

for _schema_type in (
    ActionPortSchema,
    ActionHeaderSchema,
    ActionSchema,
    ActionSettings,
    NativeActionHandler,
):
    _schema_type.__module__ = __name__

ActionSchema.WHOLE_JSON = _native.WHOLE_JSON


def _schema_mapping(value: Any, name: str) -> Mapping[str, Any]:
    if isinstance(value, Mapping):
        return value
    dump = getattr(value, "model_dump", None)
    if callable(dump):
        result = dump()
        if isinstance(result, Mapping):
            return result
    raise Status(
        code=StatusCode.INVALID_ARGUMENT,
        message=f"{name} must be validated from a mapping.",
    ).to_exception()


def _validate_action_port(
    value: Any, *, json_mode: bool = False
) -> ActionPortSchema:
    if isinstance(value, ActionPortSchema):
        return value
    data = _schema_mapping(value, "ActionPortSchema")
    autofills = []
    if "autofills" in data:
        for item in data["autofills"]:
            if item is None:
                autofills.append(
                    types.NodeFragment(
                        continued=False,
                        data=types.Chunk(
                            metadata=types.ChunkMetadata(
                                mimetype="application/octet-stream"
                            )
                        ),
                    )
                )
                continue

            autofills.append(
                types.NodeFragment.model_validate_json(
                    pydantic_core.to_json(item)
                )
                if json_mode
                else types.NodeFragment.model_validate(item)
            )

    port_python_type = None
    port_type_name = data["type"]

    if isinstance(data["type"], type):
        port_python_type = data["type"]
        port_type_name = data["type"].__name__

    schema = ActionPortSchema(
        name=data["name"],
        type=port_type_name,
        description=data.get("description", ""),
        required=data.get("required", False),
        unary=data.get("unary", False),
        autofills=autofills,
        typeinfo=port_python_type,
    )
    return schema


def _validate_action_header(
    value: Any, *, json_mode: bool = False
) -> ActionHeaderSchema:
    if isinstance(value, ActionHeaderSchema):
        return value
    data = _schema_mapping(value, "ActionHeaderSchema")
    default = data.get("default")
    if json_mode and default is not None:
        default = base64.b64decode(default, validate=True)
    return ActionHeaderSchema(
        name=data["name"],
        description=data.get("description", ""),
        default=default,
    )


def _validate_action_schema(
    value: Any, *, json_mode: bool = False
) -> ActionSchema:
    if isinstance(value, ActionSchema):
        return value
    data = _schema_mapping(value, "ActionSchema")
    return ActionSchema(
        name=data["name"],
        description=data.get("description", ""),
        inputs={
            str(name): _validate_action_port(
                port | {"name": port.get("name", str(name))},
                json_mode=json_mode,
            )
            for name, port in data.get("inputs", {}).items()
        },
        outputs={
            str(name): _validate_action_port(
                port | {"name": port.get("name", str(name))},
                json_mode=json_mode,
            )
            for name, port in data.get("outputs", {}).items()
        },
        headers={
            str(name): _validate_action_header(
                header | {"name": header.get("name", str(name))},
                json_mode=json_mode,
            )
            for name, header in data.get("headers", {}).items()
        },
        output_to_json_field=dict(data.get("output_to_json_field", {})),
    )


def _validate_action_settings(value: Any) -> ActionSettings:
    if isinstance(value, ActionSettings):
        return value
    data = _schema_mapping(value, "ActionSettings")
    return ActionSettings(**dict(data))


_ACTION_VALIDATORS = {
    ActionPortSchema: _validate_action_port,
    ActionHeaderSchema: _validate_action_header,
    ActionSchema: _validate_action_schema,
    ActionSettings: _validate_action_settings,
}


def _dump_action_model(value: Any, mode: str = "python") -> dict[str, Any]:
    if isinstance(value, ActionPortSchema):
        out = {
            "name": value.name,
        }
        if value.description:
            out["description"] = value.description
        if value.type:
            out["type"] = value.type
        if value.required:
            out["required"] = value.required
        if value.unary:
            out["unary"] = value.unary
        if value.autofills:
            out["autofills"] = [
                chunk.model_dump(mode=mode) for chunk in value.autofills
            ]
        return out
    if isinstance(value, ActionHeaderSchema):
        default = value.default
        if mode == "json" and default is not None:
            default = base64.b64encode(default).decode("ascii")
        out = {
            "name": value.name,
        }
        if value.description:
            out["description"] = value.description
        if default is not None:
            out["default"] = default
        return out
    if isinstance(value, ActionSchema):
        out = {
            "name": value.name,
        }
        if value.description:
            out["description"] = value.description
        if value.inputs:
            out["inputs"] = {
                name: _dump_action_model(port, mode)
                for name, port in value.inputs.items()
            }
            for key, action_input in out["inputs"].items():
                if action_input.get("name") == key:
                    del action_input["name"]
        if value.outputs:
            out["outputs"] = {
                name: _dump_action_model(port, mode)
                for name, port in value.outputs.items()
            }
            for key, action_output in out["outputs"].items():
                if action_output.get("name") == key:
                    del action_output["name"]
        if value.headers:
            out["headers"] = {
                name: _dump_action_model(header, mode)
                for name, header in value.headers.items()
            }
            for key, action_header in out["headers"].items():
                if action_header.get("name") == key:
                    del action_header["name"]
        if value.output_to_json_field:
            out["output_to_json_field"] = dict(value.output_to_json_field)
        return out
    return {
        "bind_streams_on_inputs_by_default": (
            value.bind_streams_on_inputs_by_default
        ),
        "bind_streams_on_outputs_by_default": (
            value.bind_streams_on_outputs_by_default
        ),
        "clear_inputs_after_run": value.clear_inputs_after_run,
        "clear_outputs_after_run": value.clear_outputs_after_run,
    }


def _action_model_validate(cls, value: Any, **_: Any):
    return _ACTION_VALIDATORS[cls](value)


def _action_model_validate_json(cls, value: str | bytes, **_: Any):
    try:
        decoded = pydantic_core.from_json(value)
        if cls is ActionPortSchema:
            return _validate_action_port(decoded, json_mode=True)
        if cls is ActionHeaderSchema:
            return _validate_action_header(decoded, json_mode=True)
        if cls is ActionSchema:
            return _validate_action_schema(decoded, json_mode=True)
        return _validate_action_settings(decoded)
    except StatusException:
        raise
    except Exception as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Invalid {cls.__name__} JSON: {exc}",
        ).to_exception() from exc


def _action_model_dump(self, *, mode: str = "python", **_: Any):
    if mode not in {"python", "json"}:
        raise ValueError("mode must be 'python' or 'json'")
    return _dump_action_model(self, mode)


def _action_model_copy(
    self, *, update: Mapping[str, Any] | None = None, deep: bool = False
):
    del deep
    values = self.model_dump()
    if update:
        values.update(update)
    return type(self).model_validate(values)


def _action_model_dump_json(self, **kwargs: Any) -> str:
    return pydantic_core.to_json(
        self.model_dump(mode="json"), **kwargs
    ).decode()


def _action_model_json_schema(cls, **_: Any) -> dict[str, Any]:
    return {"title": cls.__name__, "type": "object"}


def _action_core_schema(cls, _source_type, _handler):
    return core_schema.no_info_plain_validator_function(
        cls.model_validate,
        serialization=core_schema.plain_serializer_function_ser_schema(
            lambda value, info: value.model_dump(mode=info.mode),
            info_arg=True,
            when_used="always",
        ),
    )


def _action_json_schema(cls, _schema, _handler):
    return cls.model_json_schema()


_ACTION_ANNOTATIONS = {
    ActionPortSchema: {
        "name": types.NameString,
        "type": str | type,
        "description": str,
        "required": bool,
        "unary": bool,
        "autofills": list[types.Chunk] | None,
    },
    ActionHeaderSchema: {
        "name": types.NameString,
        "description": str,
        "default": bytes | None,
    },
    ActionSchema: {
        "name": types.NameString,
        "description": str,
        "inputs": dict[types.NameString, ActionPortSchema],
        "outputs": dict[types.NameString, ActionPortSchema],
        "headers": dict[types.NameString, ActionHeaderSchema],
        "output_to_json_field": dict[types.NameString, str],
    },
    ActionSettings: {
        "bind_streams_on_inputs_by_default": bool | None,
        "bind_streams_on_outputs_by_default": bool | None,
        "clear_inputs_after_run": bool,
        "clear_outputs_after_run": bool,
    },
}

for _schema_type in _ACTION_VALIDATORS:
    _schema_type.__annotations__ = _ACTION_ANNOTATIONS[_schema_type]
    _schema_type.model_validate = classmethod(_action_model_validate)
    _schema_type.model_validate_json = classmethod(_action_model_validate_json)
    _schema_type.model_dump = _action_model_dump
    _schema_type.model_dump_json = _action_model_dump_json
    _schema_type.model_copy = _action_model_copy
    _schema_type.model_json_schema = classmethod(_action_model_json_schema)
    _schema_type.__get_pydantic_core_schema__ = classmethod(_action_core_schema)
    _schema_type.__get_pydantic_json_schema__ = classmethod(_action_json_schema)


def status_to_chunk(status: Status, closing: bool = False) -> types.Chunk:
    """Encode an action dispatch/completion status as a typed Chunk.

    A11 writes these chunks to its reserved action status output nodes so a
    remote caller can distinguish dispatch acknowledgement from eventual
    completion without losing structured status details.

    With ``closing`` set the chunk is a node lifecycle marker instead of a
    value: it reports that the producer drained the node and closed its write
    half with ``status``. A writer tees one of these to its attached streams
    when it closes, which is how a peer's mirror of the node closes too.
    """
    if not isinstance(status, Status):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="status must be a Status instance.",
        ).to_exception()
    return _native.status_to_chunk(status, closing)


def status_from_chunk(chunk: types.Chunk) -> Status:
    """Decode a reserved action status chunk from a remote runtime."""
    if not isinstance(chunk, types.Chunk):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="chunk must be a Chunk instance.",
        ).to_exception()
    return _native.status_from_chunk(chunk)


def is_status_chunk(chunk: types.Chunk) -> bool:
    """Return whether ``chunk`` carries A11's action-status mimetype."""
    return isinstance(chunk, types.Chunk) and _native.is_status_chunk(chunk)


def is_close_status_chunk(chunk: types.Chunk) -> bool:
    """Return whether ``chunk`` is a node closure marker.

    Such a chunk reports that the producer drained the node and closed its
    write half with that status; it is not a value and is never stored.
    """
    return isinstance(chunk, types.Chunk) and _native.is_close_status_chunk(
        chunk
    )


class DefaultHeaders(enum.StrEnum):
    """Well-known action metadata understood by A11 integrations.

    Headers describe one call and normally flow into nested actions. Use these
    names instead of ad-hoc equivalents so deadlines, tool policy, user logs,
    and tracing remain connected across agent boundaries.
    """

    #: Absolute execution deadline propagated through an action tree.
    DEADLINE = "x-a11-deadline"
    #: Policy describing which actions an LLM may expose as tools.
    ALLOWED_LLM_ACTIONS = "x-a11-allowed-llm-actions"
    #: W3C/OpenTelemetry trace-parent context.
    OTEL_TRACEPARENT = "x-otel-traceparent"
    #: Vendor trace-state accompanying ``OTEL_TRACEPARENT``.
    OTEL_TRACESTATE = "x-otel-tracestate"
    #: OpenTelemetry baggage propagated to nested work.
    OTEL_BAGGAGE = "x-otel-baggage"


#: Header schemas applications can merge into an ``ActionSchema``.
DEFAULT_HEADERS = {
    DefaultHeaders.DEADLINE: ActionHeaderSchema(
        DefaultHeaders.DEADLINE,
        "Deadline for execution in milliseconds since epoch.",
    ),
    DefaultHeaders.ALLOWED_LLM_ACTIONS: ActionHeaderSchema(
        DefaultHeaders.ALLOWED_LLM_ACTIONS,
        "Comma-separated regex patterns of actions the LLM may call as tools.",
    ),
    DefaultHeaders.OTEL_TRACEPARENT: ActionHeaderSchema(
        DefaultHeaders.OTEL_TRACEPARENT,
        "OpenTelemetry traceparent header.",
    ),
    DefaultHeaders.OTEL_TRACESTATE: ActionHeaderSchema(
        DefaultHeaders.OTEL_TRACESTATE,
        "OpenTelemetry tracestate header.",
    ),
    DefaultHeaders.OTEL_BAGGAGE: ActionHeaderSchema(
        DefaultHeaders.OTEL_BAGGAGE,
        "OpenTelemetry baggage header.",
    ),
}


__all__ = [
    "ACTION_DISPATCH_STATUS_OUTPUT",
    "ACTION_HEADER_PREFIX",
    "ACTION_STATUS_MIMETYPE",
    "ACTION_STATUS_OUTPUT",
    "CANCEL_ACTION_HEADER",
    "CANCEL_ACTION_NAME",
    "DEFAULT_HEADERS",
    "DEFAULT_MAX_CONCURRENT_NESTED_ACTIONS",
    "Action",
    "ActionHandler",
    "ActionHeaderSchema",
    "ActionPortSchema",
    "ActionSchema",
    "ActionSettings",
    "NativeActionHandler",
    "OnActionCancelled",
    "is_close_status_chunk",
    "is_status_chunk",
    "status_from_chunk",
    "status_to_chunk",
]
