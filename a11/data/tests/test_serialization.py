import asyncio
import datetime
import uuid

import pydantic
import pytest

from a11 import timing
from a11.data import types
from a11.data.serialization import (
    JSON_MIMETYPE,
    MSGPACK_MIMETYPE,
    SerializationRegistry,
)
from a11.status import Status, StatusCode, StatusException


@pytest.mark.parametrize("mimetype", [JSON_MIMETYPE, MSGPACK_MIMETYPE])
@pytest.mark.parametrize(
    "value",
    [
        {1: ("value", b"\x00\xff")},
        [1, {2, 3}],
        (1, "two"),
        {1, 2},
        frozenset({1, 2}),
        2**100,
        1.25,
        "héllo",
        b"\x00\xff",
        bytearray(b"\x00\xff"),
        True,
        None,
        datetime.datetime(2024, 1, 2, 3, 4, 5, 6, tzinfo=datetime.timezone.utc),
        datetime.date(2024, 1, 2),
        datetime.time(3, 4, 5, 6),
        datetime.timedelta(days=-2, seconds=3, microseconds=4),
        uuid.UUID("b8a06a7a-b3ac-4b10-b634-9b2c8272e2f8"),
        timing.Time.from_nanoseconds_since_epoch(-123),
        timing.infinite_future(),
        timing.infinite_past(),
        timing.Duration(-123),
        timing.infinite_duration(),
        -timing.infinite_duration(),
    ],
)
def test_default_codecs_round_trip_required_types(mimetype, value):
    registry = SerializationRegistry(register_defaults=True)

    chunk = registry.to_chunk(value, mimetype)

    assert chunk.get_mimetype() == f"{mimetype};type={type(value).__name__}"
    result = registry.from_chunk(chunk)
    assert type(result) is type(value)
    assert result == value


class _NestedModel(pydantic.BaseModel):
    payload: bytes
    created_at: datetime.datetime


class _Model(pydantic.BaseModel):
    nested: _NestedModel
    values: tuple[int, ...]


@pytest.mark.parametrize("mimetype", [JSON_MIMETYPE, MSGPACK_MIMETYPE])
def test_pydantic_subclasses_and_native_types_round_trip_across_registries(
    mimetype,
):
    producer = SerializationRegistry(register_defaults=True)
    consumer = SerializationRegistry(register_defaults=True)
    model = _Model(
        nested=_NestedModel(
            payload=b"\xff",
            created_at=datetime.datetime(
                2024, 1, 2, tzinfo=datetime.timezone.utc
            ),
        ),
        values=(1, 2),
    )
    node_ref = types.NodeRef(id="target")
    native = types.WireMessage(
        node_fragments=[
            types.NodeFragment(
                id="node",
                data=node_ref,
                seq=4,
                continued=True,
            ),
            types.NodeFragment(
                data=types.Chunk(
                    metadata=types.ChunkMetadata(
                        mimetype="application/octet-stream",
                        attributes={"attribute": b"\xff"},
                    ),
                    data=b"payload",
                )
            ),
        ],
        actions=[
            types.ActionMessage(
                id="action",
                name="call",
                inputs=[types.Port(name="input", id="port")],
                headers={"header": b"value"},
            )
        ],
        headers={"wire": b"\x00\xff"},
    )

    model_chunk = producer.to_chunk(model, mimetype)
    native_chunk = producer.to_chunk(native, mimetype)

    assert model_chunk.get_mimetype() == f"{mimetype};type=_Model"
    assert consumer.from_chunk(model_chunk) == model
    assert consumer.from_chunk(native_chunk) == native


def test_custom_registration_mro_wildcards_and_match_order():
    class Mapping(dict):
        pass

    registry = SerializationRegistry()
    registry.register(
        dict,
        "application/x-custom",
        lambda value: str(sorted(value.items())),
        lambda data, obj_type: obj_type({"decoded": data.decode()}),
    )

    chunk = registry.to_chunk(Mapping(a=1), "application/*")

    assert chunk.get_mimetype() == "application/x-custom;type=Mapping"
    assert registry.from_chunk(
        chunk,
        ["application/json", "application/*"],
        obj_type=Mapping,
    ) == {"decoded": "[('a', 1)]"}
    assert type(registry.from_chunk(chunk, obj_type=Mapping)) is Mapping


def test_deserializer_can_receive_the_complete_chunk():
    registry = SerializationRegistry()

    def deserialize(chunk: types.Chunk):
        return chunk.metadata.attributes["answer"].decode()

    registry.register(
        str,
        "text/x-custom",
        lambda value: types.Chunk(
            metadata=types.ChunkMetadata(
                mimetype="text/ignored",
                attributes={"answer": value.encode()},
            ),
            data=b"",
        ),
        deserialize,
    )

    chunk = registry.to_chunk("forty-two")

    assert chunk.get_mimetype() == "text/x-custom;type=str"
    assert registry.from_chunk(chunk) == "forty-two"


def test_registry_reports_specified_status_codes():
    empty = SerializationRegistry()
    with pytest.raises(StatusException) as raised:
        empty.to_chunk({})
    assert raised.value.status.code == StatusCode.NOT_FOUND

    with pytest.raises(StatusException) as raised:
        empty.from_chunk(types.Chunk(data=b"{}"))
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT

    registry = SerializationRegistry(register_defaults=True)
    chunk = registry.to_chunk({})
    with pytest.raises(StatusException) as raised:
        registry.from_chunk(chunk, obj_type=list)
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT

    with pytest.raises(StatusException) as raised:
        registry.from_chunk(chunk, "text/*")
    assert raised.value.status.code == StatusCode.NOT_FOUND


def test_explicit_mimetype_can_override_stale_metadata():
    registry = SerializationRegistry(register_defaults=True)
    chunk = types.Chunk(
        metadata=types.ChunkMetadata(
            mimetype="application/x-msgpack;type=dict"
        ),
        data=b'{"value":1}',
    )

    assert registry.from_chunk(chunk, JSON_MIMETYPE) == {"value": 1}


def test_callbacks_only_leak_status_or_control_flow_exceptions():
    registry = SerializationRegistry()

    def fail(_value):
        raise ValueError("callback failed")

    registry.register_serializer(dict, "application/x-fail", fail)
    with pytest.raises(StatusException) as raised:
        registry.to_chunk({})
    assert raised.value.status.code == StatusCode.UNKNOWN

    expected = Status(
        code=StatusCode.DATA_LOSS,
        message="status from callback",
    ).to_exception()

    def fail_with_status(_value):
        raise expected

    status_registry = SerializationRegistry()
    status_registry.register_serializer(
        dict, "application/x-status", fail_with_status
    )
    with pytest.raises(StatusException) as raised:
        status_registry.to_chunk({})
    assert raised.value is expected

    def cancel(_value):
        raise asyncio.CancelledError

    cancellation_registry = SerializationRegistry()
    cancellation_registry.register_serializer(
        dict, "application/x-cancel", cancel
    )
    with pytest.raises(asyncio.CancelledError):
        cancellation_registry.to_chunk({})


def test_internal_registry_methods_also_convert_unexpected_exceptions():
    registry = SerializationRegistry()
    registry._next_order = object()

    with pytest.raises(StatusException) as raised:
        registry._take_order()

    assert raised.value.status.code == StatusCode.UNKNOWN
