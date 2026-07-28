"""Native Action types with Python validation and protocol conveniences."""

from __future__ import annotations

import base64
import json
from collections.abc import Awaitable, Callable, Mapping
from typing import Any

from pydantic_core import core_schema

from a11 import _native
from a11.actions._native_action import Action
from a11.data import types
from a11.status import Status, StatusCode, StatusException

ACTION_STATUS_MIMETYPE = _native.ACTION_STATUS_MIMETYPE
ACTION_STATUS_OUTPUT = _native.ACTION_STATUS_OUTPUT
ACTION_DISPATCH_STATUS_OUTPUT = _native.ACTION_DISPATCH_STATUS_OUTPUT
CANCEL_ACTION_NAME = getattr(_native, "CANCEL_ACTION_NAME", "__cancel__")
CANCEL_ACTION_HEADER = getattr(_native, "CANCEL_ACTION_HEADER", "__action")
ACTION_HEADER_PREFIX = getattr(_native, "ACTION_HEADER_PREFIX", "x-a11-")

DEFAULT_MAX_CONCURRENT_NESTED_ACTIONS = getattr(
    _native, "DEFAULT_MAX_CONCURRENT_NESTED_ACTIONS", 64
)

ActionHandler = Callable[["Action"], Awaitable[None] | None]
OnActionCancelled = Callable[["Action"], Any]


ActionPortSchema = _native.ActionPortSchema
ActionHeaderSchema = _native.ActionHeaderSchema
ActionSchema = _native.ActionSchema
ActionSettings = _native.ActionSettings

for _schema_type in (
    ActionPortSchema,
    ActionHeaderSchema,
    ActionSchema,
    ActionSettings,
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
    return ActionPortSchema(
        name=data["name"],
        type=data["type"],
        description=data.get("description", ""),
        required=data.get("required", False),
        unary=data.get("unary", False),
        autofills=(
            None
            if data.get("autofills") is None
            else [
                (
                    types.Chunk.model_validate_json(json.dumps(item))
                    if json_mode
                    else types.Chunk.model_validate(item)
                )
                for item in data["autofills"]
            ]
        ),
    )


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
            str(name): _validate_action_port(port, json_mode=json_mode)
            for name, port in data.get("inputs", {}).items()
        },
        outputs={
            str(name): _validate_action_port(port, json_mode=json_mode)
            for name, port in data.get("outputs", {}).items()
        },
        headers={
            str(name): _validate_action_header(header, json_mode=json_mode)
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
        return {
            "name": value.name,
            "type": value.type,
            "description": value.description,
            "required": value.required,
            "unary": value.unary,
            "autofills": (
                None
                if value.autofills is None
                else [chunk.model_dump(mode=mode) for chunk in value.autofills]
            ),
        }
    if isinstance(value, ActionHeaderSchema):
        default = value.default
        if mode == "json" and default is not None:
            default = base64.b64encode(default).decode("ascii")
        return {
            "name": value.name,
            "description": value.description,
            "default": default,
        }
    if isinstance(value, ActionSchema):
        return {
            "name": value.name,
            "description": value.description,
            "inputs": {
                name: _dump_action_model(port, mode)
                for name, port in value.inputs.items()
            },
            "outputs": {
                name: _dump_action_model(port, mode)
                for name, port in value.outputs.items()
            },
            "headers": {
                name: _dump_action_model(header, mode)
                for name, header in value.headers.items()
            },
            "output_to_json_field": dict(value.output_to_json_field),
        }
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
        decoded = json.loads(value)
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
    return json.dumps(self.model_dump(mode="json"), **kwargs)


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
        "type": str,
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


def status_to_chunk(status: Status) -> types.Chunk:
    if not isinstance(status, Status):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="status must be a Status instance.",
        ).to_exception()
    return _native.status_to_chunk(status)


def status_from_chunk(chunk: types.Chunk) -> Status:
    if not isinstance(chunk, types.Chunk):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="chunk must be a Chunk instance.",
        ).to_exception()
    return _native.status_from_chunk(chunk)


def is_status_chunk(chunk: types.Chunk) -> bool:
    return isinstance(chunk, types.Chunk) and _native.is_status_chunk(chunk)


__all__ = [
    "ACTION_DISPATCH_STATUS_OUTPUT",
    "ACTION_HEADER_PREFIX",
    "ACTION_STATUS_MIMETYPE",
    "ACTION_STATUS_OUTPUT",
    "CANCEL_ACTION_HEADER",
    "CANCEL_ACTION_NAME",
    "DEFAULT_MAX_CONCURRENT_NESTED_ACTIONS",
    "Action",
    "ActionHandler",
    "ActionHeaderSchema",
    "ActionPortSchema",
    "ActionSchema",
    "ActionSettings",
    "OnActionCancelled",
    "is_status_chunk",
    "status_from_chunk",
    "status_to_chunk",
]
