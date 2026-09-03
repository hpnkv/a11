import asyncio
import datetime
import uuid

import pydantic
import pytest

from a11 import timing
from a11.data import types
from a11.data.serialization import (
    CORE_TYPE_TAGS,
    JSON_MIMETYPE,
    MSGPACK_MIMETYPE,
    SerializationRegistry,
)
from a11.status import Status, StatusCode, StatusException

#: Values a JSON or MessagePack payload already describes. They travel under a
#: bare media type, and a reader that only has the bytes still gets them right.
_GENERIC_VALUES = [
    {"key": [1, "two", None]},
    [1, 2.5, True],
    (1, "two"),
    1.25,
    "héllo",
    True,
    None,
]

#: Values the formats have no shape for. The chunk's ``;type=`` is what makes
#: them recoverable, so each must name itself.
_TAGGED_VALUES = [
    {1, 2},
    frozenset({1, 2}),
    b"\x00\xff",
    bytearray(b"\x00\xff"),
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
]


@pytest.mark.parametrize("mimetype", [JSON_MIMETYPE, MSGPACK_MIMETYPE])
@pytest.mark.parametrize("value", _GENERIC_VALUES + _TAGGED_VALUES)
def test_default_codecs_round_trip_required_types(mimetype, value):
    registry = SerializationRegistry(register_defaults=True)

    chunk = registry.to_chunk(value, mimetype)

    canonical_tags = {
        dict: "object",
        list: "array",
        tuple: "array",
        int: "integer",
        float: "number",
        str: "string",
        bool: "boolean",
        type(None): "null",
    }
    tag = canonical_tags.get(
        type(value), CORE_TYPE_TAGS.get(type(value), type(value).__name__)
    )
    # A tag the format already implies is left off: the media type alone is a
    # complete description of an object, an array, a string or a number.
    expected = (
        mimetype if tag in canonical_tags.values() else f"{mimetype};type={tag}"
    )
    assert chunk.get_mimetype() == expected

    result = registry.from_chunk(chunk)
    if type(value) is tuple:
        # A JSON array decodes to each language's native array representation
        # unless the caller explicitly requests tuple.
        assert type(result) is list
        assert result == list(value)
        assert registry.from_chunk(chunk, obj_type=tuple) == value
    else:
        assert type(result) is type(value)
        assert result == value


def test_a_bare_mimetype_is_a_complete_description():
    """Bytes plus a media type are enough; no type parameter is required."""
    registry = SerializationRegistry(register_defaults=True)
    chunk = types.Chunk(
        metadata=types.ChunkMetadata(mimetype=JSON_MIMETYPE),
        data=b'{"answer":42}',
    )

    assert registry.from_chunk(chunk) == {"answer": 42}
    # Naming a type asks for a best effort, not a different parse.
    assert registry.from_chunk(chunk, obj_type=dict) == {"answer": 42}
    # Asking for something the data cannot fill is a real error.
    with pytest.raises(StatusException) as raised:
        registry.from_chunk(chunk, obj_type=list)
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT


def test_an_unloadable_type_tag_still_yields_the_payload():
    """A peer that never imported the naming module can still read the data."""
    registry = SerializationRegistry(register_defaults=True)
    chunk = types.Chunk(
        metadata=types.ChunkMetadata(
            mimetype=f"{JSON_MIMETYPE};type=never.imported.Model"
        ),
        data=b'{"answer":42}',
    )

    assert registry.from_chunk(chunk) == {"answer": 42}


def test_messagepack_reports_integers_it_cannot_represent():
    """JSON carries arbitrary precision; MessagePack says so when it cannot."""
    registry = SerializationRegistry(register_defaults=True)

    json_chunk = registry.to_chunk(2**100, JSON_MIMETYPE)
    assert registry.from_chunk(json_chunk) == 2**100

    with pytest.raises(StatusException) as raised:
        registry.to_chunk(2**100, MSGPACK_MIMETYPE)
    assert raised.value.status.code == StatusCode.INVALID_ARGUMENT


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

    # User-defined pydantic subclasses use their fully-qualified name so that
    # like-named classes from different modules cannot collide on the wire.
    assert (
        model_chunk.get_mimetype()
        == f"{mimetype};type={_Model.__module__}.{_Model.__qualname__}"
    )
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


def test_deserializer_registration_invalidates_cached_selection():
    class Base:
        def __init__(self, source):
            self.source = source

    class Child(Base):
        pass

    registry = SerializationRegistry()
    registry.register_deserializer(
        Base,
        "application/x-cache-invalidation",
        lambda _data, obj_type: obj_type("base"),
    )
    chunk = types.Chunk(
        metadata=types.ChunkMetadata(
            mimetype="application/x-cache-invalidation"
        ),
        data=b"value",
    )

    assert registry.from_chunk(chunk, obj_type=Child).source == "base"
    registry.register_deserializer(
        Child,
        "application/x-cache-invalidation",
        lambda _data, obj_type: obj_type("child"),
    )
    assert registry.from_chunk(chunk, obj_type=Child).source == "child"


def test_cached_chunk_metadata_is_copied_into_each_result():
    registry = SerializationRegistry(register_defaults=True)

    first = registry.to_chunk("first")
    second = registry.to_chunk("second")
    first.metadata.mimetype = "text/x-mutated"

    assert first.get_mimetype() == "text/x-mutated"
    assert second.get_mimetype() == "text/plain"


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

    # Use a representation with no registered decoder. `text/plain` is the
    # standard `str` representation and therefore cannot exercise this case.
    with pytest.raises(StatusException) as raised:
        registry.from_chunk(chunk, "audio/*")
    assert raised.value.status.code == StatusCode.NOT_FOUND


@pytest.mark.parametrize("mimetype", ["image/png", "image/jpeg", "image/webp"])
def test_image_bytes_use_self_describing_media_types(mimetype):
    registry = SerializationRegistry(register_defaults=True)

    chunk = registry.to_chunk(b"image data", mimetype)

    assert chunk.get_mimetype() == mimetype
    assert registry.from_chunk(chunk) == b"image data"
    assert registry.from_chunk(chunk, "image/*") == b"image data"


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
