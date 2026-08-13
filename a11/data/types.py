"""Native A11 wire values with Python validation and JSON conveniences.

The classes in this module are the C++ value types themselves.  Pydantic-style
methods are attached as a convenience protocol so native values remain easy to
use in FastAPI models and ordinary Python serialization code without keeping a
second object representation in sync.
"""

from __future__ import annotations

import base64
import copy
import datetime
import json
from collections.abc import Mapping
from typing import Annotated, Any, ClassVar

from pydantic import Field
from pydantic_core import core_schema

from a11 import _native
from a11.status import Status, StatusCode

_NAME_STRING_PATTERN = r"^[a-zA-Z0-9_](?:[a-zA-Z0-9\-_#]*[a-zA-Z0-9_])?$"
NameString = Annotated[
    str, Field(min_length=1, max_length=255, pattern=_NAME_STRING_PATTERN)
]

from a11._native import ChunkMetadata
from a11._native import Chunk
from a11._native import NodeRef
from a11._native import NodeFragment
from a11._native import Port
from a11._native import ActionMessage
from a11._native import WireMessage

_NATIVE_TYPES = (
    ChunkMetadata,
    Chunk,
    NodeRef,
    NodeFragment,
    Port,
    ActionMessage,
    WireMessage,
)

for _type in _NATIVE_TYPES:
    _type.__module__ = __name__


def validate_name_string(name: str) -> NameString:
    """Validate and return an A11 identifier using the native contract."""

    return _native.validate_name_string(name)


def _mapping(value: Any, name: str) -> Mapping[str, Any]:
    if isinstance(value, Mapping):
        return value
    model_dump = getattr(value, "model_dump", None)
    if callable(model_dump):
        dumped = model_dump()
        if isinstance(dumped, Mapping):
            return dumped
    raise Status(
        code=StatusCode.INVALID_ARGUMENT,
        message=f"{name} must be validated from a mapping.",
    ).to_exception()


def _bytes_from_json(value: Any, field: str) -> bytes:
    if isinstance(value, bytes):
        return value
    if not isinstance(value, str):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"{field} must be a base64 string.",
        ).to_exception()
    try:
        return base64.b64decode(value, validate=True)
    except ValueError as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"{field} contains invalid base64 data.",
        ).to_exception() from exc


def _byte_map_from_json(value: Any, field: str) -> dict[str, bytes]:
    if value is None:
        return {}
    if not isinstance(value, Mapping):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"{field} must be a mapping.",
        ).to_exception()
    return {
        str(key): _bytes_from_json(item, f"{field}.{key}")
        for key, item in value.items()
    }


def _timestamp_from_json(value: Any) -> Any:
    if value is None or isinstance(value, datetime.datetime):
        return value
    if not isinstance(value, str):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="ChunkMetadata.timestamp must be an RFC 3339 string.",
        ).to_exception()
    try:
        return datetime.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Invalid ChunkMetadata.timestamp: {value!r}.",
        ).to_exception() from exc


def _validate_metadata(value: Any, *, json_mode: bool = False) -> ChunkMetadata:
    if isinstance(value, ChunkMetadata):
        return value
    data = _mapping(value, "ChunkMetadata")
    attributes = data.get("attributes", {})
    if json_mode:
        attributes = _byte_map_from_json(attributes, "ChunkMetadata.attributes")
    # Accepts a datetime as-is, so this is safe in either mode -- a timestamp
    # reaches us as an RFC 3339 string whenever the sender's field was typed.
    timestamp = _timestamp_from_json(data.get("timestamp"))
    if "mimetype" not in data:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="ChunkMetadata.mimetype is required.",
        ).to_exception()
    return ChunkMetadata(
        mimetype=data["mimetype"],
        timestamp=timestamp,
        attributes=attributes,
    )


def _validate_chunk(value: Any, *, json_mode: bool = False) -> Chunk:
    if isinstance(value, Chunk):
        return value
    data = _mapping(value, "Chunk")
    metadata = data.get("metadata")
    if metadata is not None:
        metadata = _validate_metadata(metadata, json_mode=json_mode)
    raw_data = data.get("data", b"")
    if json_mode:
        raw_data = _bytes_from_json(raw_data, "Chunk.data")
    return Chunk(
        metadata=metadata,
        ref=data.get("ref", ""),
        data=raw_data,
    )


def _validate_node_ref(value: Any, *, json_mode: bool = False) -> NodeRef:
    del json_mode
    if isinstance(value, NodeRef):
        return value
    data = _mapping(value, "NodeRef")
    if "id" not in data:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="NodeRef.id is required.",
        ).to_exception()
    return NodeRef(
        id=data["id"],
        offset=data.get("offset", 0),
        length=data.get("length"),
    )


def _validate_fragment(value: Any, *, json_mode: bool = False) -> NodeFragment:
    if isinstance(value, NodeFragment):
        return value
    data = _mapping(value, "NodeFragment")
    if "data" not in data:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="NodeFragment.data is required.",
        ).to_exception()
    payload = data["data"]
    if not isinstance(payload, (Chunk, NodeRef)):
        payload_data = _mapping(payload, "NodeFragment.data")
        is_node_ref = (
            "id" in payload_data
            and "data" not in payload_data
            and "ref" not in payload_data
            and "metadata" not in payload_data
        )
        payload = (
            _validate_node_ref(payload_data, json_mode=json_mode)
            if is_node_ref
            else _validate_chunk(payload_data, json_mode=json_mode)
        )
    return NodeFragment(
        id=data.get("id", ""),
        data=payload,
        seq=data.get("seq"),
        continued=data.get("continued", False),
    )


def _validate_port(value: Any, *, json_mode: bool = False) -> Port:
    del json_mode
    if isinstance(value, Port):
        return value
    data = _mapping(value, "Port")
    return Port(name=data.get("name", ""), id=data.get("id", ""))


def _validate_action(value: Any, *, json_mode: bool = False) -> ActionMessage:
    if isinstance(value, ActionMessage):
        return value
    data = _mapping(value, "ActionMessage")
    headers = data.get("headers", {})
    if json_mode:
        headers = _byte_map_from_json(headers, "ActionMessage.headers")
    return ActionMessage(
        id=data.get("id", ""),
        name=data.get("name", ""),
        inputs=[
            _validate_port(item, json_mode=json_mode)
            for item in data.get("inputs", [])
        ],
        outputs=[
            _validate_port(item, json_mode=json_mode)
            for item in data.get("outputs", [])
        ],
        headers=headers,
    )


def _validate_wire(value: Any, *, json_mode: bool = False) -> WireMessage:
    if isinstance(value, WireMessage):
        return value
    data = _mapping(value, "WireMessage")
    headers = data.get("headers", {})
    if json_mode:
        headers = _byte_map_from_json(headers, "WireMessage.headers")
    return WireMessage(
        node_fragments=[
            _validate_fragment(item, json_mode=json_mode)
            for item in data.get("node_fragments", [])
        ],
        actions=[
            _validate_action(item, json_mode=json_mode)
            for item in data.get("actions", [])
        ],
        headers=headers,
    )


_VALIDATORS = {
    ChunkMetadata: _validate_metadata,
    Chunk: _validate_chunk,
    NodeRef: _validate_node_ref,
    NodeFragment: _validate_fragment,
    Port: _validate_port,
    ActionMessage: _validate_action,
    WireMessage: _validate_wire,
}


def _json_bytes(value: bytes) -> str:
    return base64.b64encode(value).decode("ascii")


def _dump_metadata(value: ChunkMetadata, mode: str) -> dict[str, Any]:
    result: dict[str, Any] = {"mimetype": value.mimetype}
    if value.timestamp is not None:
        result["timestamp"] = (
            value.timestamp.isoformat() if mode == "json" else value.timestamp
        )
    if value.attributes:
        result["attributes"] = {
            key: _json_bytes(item) if mode == "json" else item
            for key, item in value.attributes.items()
        }
    return result


def _dump_chunk(value: Chunk, mode: str) -> dict[str, Any]:
    result: dict[str, Any] = {
        "data": _json_bytes(value.data) if mode == "json" else value.data
    }
    if value.metadata is not None:
        result["metadata"] = _dump_metadata(value.metadata, mode)
    if value.ref:
        result["ref"] = value.ref
    return result


def _dump_node_ref(value: NodeRef, mode: str) -> dict[str, Any]:
    del mode
    result: dict[str, Any] = {"id": value.id}
    if value.offset:
        result["offset"] = value.offset
    if value.length is not None:
        result["length"] = value.length
    return result


def _dump_fragment(value: NodeFragment, mode: str) -> dict[str, Any]:
    result: dict[str, Any] = {
        "data": (
            _dump_chunk(value.data, mode)
            if isinstance(value.data, Chunk)
            else _dump_node_ref(value.data, mode)
        )
    }
    if value.id:
        result["id"] = value.id
    if value.seq is not None:
        result["seq"] = value.seq
    if value.continued:
        result["continued"] = True
    return result


def _dump_port(value: Port, mode: str) -> dict[str, Any]:
    del mode
    result: dict[str, Any] = {}
    if value.name:
        result["name"] = value.name
    if value.id:
        result["id"] = value.id
    return result


def _dump_action(value: ActionMessage, mode: str) -> dict[str, Any]:
    result: dict[str, Any] = {"id": value.id, "name": value.name}
    if value.inputs:
        result["inputs"] = [_dump_port(item, mode) for item in value.inputs]
    if value.outputs:
        result["outputs"] = [_dump_port(item, mode) for item in value.outputs]
    if value.headers:
        result["headers"] = {
            key: _json_bytes(item) if mode == "json" else item
            for key, item in value.headers.items()
        }
    return result


def _dump_wire(value: WireMessage, mode: str) -> dict[str, Any]:
    result: dict[str, Any] = {}
    if value.node_fragments:
        result["node_fragments"] = [
            _dump_fragment(item, mode) for item in value.node_fragments
        ]
    if value.actions:
        result["actions"] = [_dump_action(item, mode) for item in value.actions]
    if value.headers:
        result["headers"] = {
            key: _json_bytes(item) if mode == "json" else item
            for key, item in value.headers.items()
        }
    return result


_DUMPERS = {
    ChunkMetadata: _dump_metadata,
    Chunk: _dump_chunk,
    NodeRef: _dump_node_ref,
    NodeFragment: _dump_fragment,
    Port: _dump_port,
    ActionMessage: _dump_action,
    WireMessage: _dump_wire,
}


def _model_validate(cls, value: Any, **_: Any):
    return _VALIDATORS[cls](value)


def _model_construct(cls, **values: Any):
    """Construct a native value from trusted field values.

    Native records retain C++ invariants. This validates input rather than
    creating an invalid object.
    """

    return _VALIDATORS[cls](values)


def validate_wire(cls: type, value: Any) -> Any:
    """Build a native record from an already-parsed wire tree.

    Wire form spells bytes as base64 and timestamps as RFC 3339, and the
    validators accept the decoded objects too -- so this reads both a JSON tree
    and a MessagePack one, which carries real bytes.
    """
    return _VALIDATORS[cls](value, json_mode=True)


def _model_validate_json(cls, value: str | bytes | bytearray, **_: Any):
    try:
        decoded = json.loads(value)
    except (TypeError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Invalid {cls.__name__} JSON: {exc}.",
        ).to_exception() from exc
    return _VALIDATORS[cls](decoded, json_mode=True)


def _model_dump(self, *, mode: str = "python", **_: Any) -> dict[str, Any]:
    if mode not in {"python", "json"}:
        raise ValueError("mode must be 'python' or 'json'")
    return _DUMPERS[type(self)](self, mode)


def _model_dump_json(self, **kwargs: Any) -> str:
    return json.dumps(self.model_dump(mode="json"), **kwargs)


def _model_copy(
    self, *, update: dict[str, Any] | None = None, deep: bool = False
):
    result = copy.deepcopy(self) if deep else copy.copy(self)
    for name, value in (update or {}).items():
        if name not in getattr(type(self), "__annotations__", {}):
            raise ValueError(f"Unknown {type(self).__name__} field: {name}")
        setattr(result, name, value)
    result.validate()
    return result


_SCHEMAS: dict[type, dict[str, Any]] = {
    ChunkMetadata: {
        "type": "object",
        "required": ["mimetype"],
        "properties": {
            "mimetype": {"type": "string"},
            "timestamp": {"type": ["string", "null"], "format": "date-time"},
            "attributes": {
                "type": "object",
                "additionalProperties": {"type": "string", "format": "byte"},
            },
        },
    },
    Chunk: {
        "type": "object",
        "properties": {
            "metadata": {
                "anyOf": [{"$ref": "#/$defs/ChunkMetadata"}, {"type": "null"}]
            },
            "ref": {"type": "string"},
            "data": {"type": "string", "format": "byte"},
        },
    },
    NodeRef: {
        "type": "object",
        "required": ["id"],
        "properties": {
            "id": {"type": "string", "pattern": _NAME_STRING_PATTERN},
            "offset": {"type": "integer", "minimum": 0, "maximum": 2**32 - 1},
            "length": {
                "type": ["integer", "null"],
                "minimum": 0,
                "maximum": 2**32,
            },
        },
    },
    Port: {
        "type": "object",
        "properties": {
            "name": {"type": "string"},
            "id": {"type": "string"},
        },
    },
    NodeFragment: {
        "type": "object",
        "required": ["data"],
        "properties": {
            "id": {"type": "string"},
            "data": {
                "anyOf": [
                    {"$ref": "#/$defs/Chunk"},
                    {"$ref": "#/$defs/NodeRef"},
                ]
            },
            "seq": {
                "type": ["integer", "null"],
                "minimum": 0,
                "maximum": 2**32 - 1,
            },
            "continued": {"type": "boolean"},
        },
    },
    ActionMessage: {
        "type": "object",
        "properties": {
            "id": {"type": "string"},
            "name": {"type": "string"},
            "inputs": {"type": "array", "items": {"$ref": "#/$defs/Port"}},
            "outputs": {"type": "array", "items": {"$ref": "#/$defs/Port"}},
            "headers": {
                "type": "object",
                "additionalProperties": {"type": "string", "format": "byte"},
            },
        },
    },
    WireMessage: {
        "type": "object",
        "properties": {
            "node_fragments": {
                "type": "array",
                "items": {"$ref": "#/$defs/NodeFragment"},
            },
            "actions": {
                "type": "array",
                "items": {"$ref": "#/$defs/ActionMessage"},
            },
            "headers": {
                "type": "object",
                "additionalProperties": {"type": "string", "format": "byte"},
            },
        },
    },
}


def _expand_schema(value: Any) -> Any:
    if isinstance(value, list):
        return [_expand_schema(item) for item in value]
    if not isinstance(value, dict):
        return copy.deepcopy(value)
    reference = value.get("$ref")
    if isinstance(reference, str) and reference.startswith("#/$defs/"):
        name = reference.rsplit("/", 1)[-1]
        target = next(item for item in _NATIVE_TYPES if item.__name__ == name)
        return _expand_schema(_SCHEMAS[target])
    return {key: _expand_schema(item) for key, item in value.items()}


def _model_json_schema(cls, **_: Any) -> dict[str, Any]:
    schema = _expand_schema(_SCHEMAS[cls])
    schema["title"] = cls.__name__
    return schema


def _core_schema(cls, _source_type, _handler):
    # How a native record validates and dumps when nested in a model.
    #
    # Validation has to know which mode it is in. A native record spells its
    # byte fields as base64 in JSON and as `bytes` in Python, so a `Chunk`
    # nested in a model validated from JSON must take the base64 path —
    # otherwise it rejects the very payload its own dumper wrote.
    #
    # A comment rather than a docstring: this is attached to every native
    # class, and `scripts/generate_stubs.py` would copy a docstring into each
    # one's entry in `a11/_native/__init__.pyi`, seven times over.
    return core_schema.json_or_python_schema(
        json_schema=core_schema.no_info_plain_validator_function(
            lambda value: _VALIDATORS[cls](value, json_mode=True)
        ),
        python_schema=core_schema.no_info_plain_validator_function(
            cls.model_validate
        ),
        serialization=core_schema.plain_serializer_function_ser_schema(
            lambda value, info: value.model_dump(mode=info.mode),
            info_arg=True,
            when_used="always",
        ),
    )


def _json_schema(cls, _schema, _handler):
    return cls.model_json_schema()


_ANNOTATIONS = {
    ChunkMetadata: {
        "mimetype": str,
        "timestamp": datetime.datetime | None,
        "attributes": dict[NameString, bytes],
    },
    Chunk: {
        "metadata": ChunkMetadata | None,
        "ref": str,
        "data": bytes,
    },
    NodeRef: {"id": NameString, "offset": int, "length": int | None},
    NodeFragment: {
        "id": str,
        "data": Chunk | NodeRef,
        "seq": int | None,
        "continued": bool,
    },
    Port: {"name": NameString, "id": NameString},
    ActionMessage: {
        "id": NameString,
        "name": NameString,
        "inputs": list[Port],
        "outputs": list[Port],
        "headers": dict[NameString, bytes],
    },
    WireMessage: {
        "VERSION": ClassVar[int],
        "node_fragments": list[NodeFragment],
        "actions": list[ActionMessage],
        "headers": dict[NameString, bytes],
    },
}

for _type in _NATIVE_TYPES:
    _type.__annotations__ = _ANNOTATIONS[_type]
    _type.model_validate = classmethod(_model_validate)
    _type.model_construct = classmethod(_model_construct)
    _type.model_validate_json = classmethod(_model_validate_json)
    _type.model_dump = _model_dump
    _type.model_dump_json = _model_dump_json
    _type.model_copy = _model_copy
    _type.model_json_schema = classmethod(_model_json_schema)
    _type.__get_pydantic_core_schema__ = classmethod(_core_schema)
    _type.__get_pydantic_json_schema__ = classmethod(_json_schema)


from a11._native import EMPTY_WIRE_MESSAGE_SIZE


def is_half_close_message(message: WireMessage) -> bool:
    """Return whether ``message`` represents a transport half-close."""

    return _native.is_half_close_message(message)


def make_half_close_message(
    trailers: Mapping[str, bytes] | None = None,
) -> WireMessage:
    """Create a half-close wire message carrying optional trailers."""

    return _native.make_half_close_message(dict(trailers or {}))


__all__ = [
    "ActionMessage",
    "Chunk",
    "ChunkMetadata",
    "EMPTY_WIRE_MESSAGE_SIZE",
    "NameString",
    "NodeFragment",
    "NodeRef",
    "Port",
    "WireMessage",
    "is_half_close_message",
    "make_half_close_message",
    "validate_name_string",
]
