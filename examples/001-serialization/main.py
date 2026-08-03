"""A tour of serializing Python objects to and from a11 Chunk values."""

import datetime
import sys
import uuid
from dataclasses import dataclass
from pathlib import Path

# Make this file runnable directly from a source checkout. This needs to happen
# before importing a11 when the package has not been installed into the active
# Python environment.
PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

import pydantic

from a11 import timing
from a11.data import types
from a11.data.serialization import (
    JSON_MIMETYPE,
    MSGPACK_MIMETYPE,
    SerializationRegistry,
    get_global_serialization_registry,
)
from a11.status import StatusException


class Reading(pydantic.BaseModel):
    """A model used to demonstrate Pydantic subclass recovery."""

    sensor: str
    value: float
    observed_at: datetime.datetime
    raw_sample: bytes


@dataclass(frozen=True)
class Point:
    """A non-Pydantic application type for the custom-registry example."""

    x: int
    y: int


def print_heading(title: str) -> None:
    print(f"\n{'=' * 8} {title} {'=' * 8}")


def describe_chunk(chunk: types.Chunk) -> None:
    """Print just enough of a chunk to see what the registry produced."""

    print(f"MIME: {chunk.get_mimetype()}")
    print(f"payload bytes: {len(chunk.data)}")


def default_json_example(registry: SerializationRegistry) -> None:
    print_heading("Default JSON")

    # The global registry contains the standard codecs. Supplying no MIME type
    # asks it to select the preferred registration; JSON is registered first.
    value = {"sensor": "outside", "temperature": 18.5}
    chunk = registry.to_chunk(value)

    describe_chunk(chunk)
    print(f"JSON payload: {chunk.data.decode('utf-8')}")

    # The language-neutral JSON type is carried by ";type=object", so no
    # obj_type hint is needed when reading a chunk produced by the registry.
    restored = registry.from_chunk(chunk)
    assert type(restored) is dict
    assert restored == value
    print(f"restored: {restored!r}")


def nested_values_example(registry: SerializationRegistry) -> None:
    print_heading("Nested Python values")

    # JSON does not natively distinguish tuples, sets, bytes, UUIDs, or dates.
    # The standard codec tags such nested values so their exact types survive.
    value = {
        "sample_ids": {
            uuid.UUID("1aa70f8e-96f6-4674-b7f9-9d12c2f2ab2c"),
            uuid.UUID("cc75b7df-0034-4e45-acf8-072916ac4f5f"),
        },
        "window": (datetime.date(2026, 7, 24), timing.Duration.seconds(30)),
        "raw": b"\x00\xff",
    }
    chunk = registry.to_chunk(value, JSON_MIMETYPE)
    restored = registry.from_chunk(chunk)

    describe_chunk(chunk)
    assert restored == value
    assert isinstance(restored["window"], tuple)
    assert isinstance(restored["raw"], bytes)
    print("tuple, set, UUID, date, Duration, and bytes all survived")


def msgpack_example(registry: SerializationRegistry) -> None:
    print_heading("MessagePack")

    value = bytearray(b"\x00binary\xff")

    # Select MessagePack explicitly when a compact binary representation is
    # preferable. The resulting MIME remains exact, including the Python type.
    chunk = registry.to_chunk(value, MSGPACK_MIMETYPE)
    describe_chunk(chunk)

    # MIME selectors may be ordered. The first selector matches this chunk;
    # the wildcard is a lower-priority option.
    restored = registry.from_chunk(
        chunk,
        mimetype_patterns=[MSGPACK_MIMETYPE, "application/*"],
    )
    assert type(restored) is bytearray
    assert restored == value
    print(f"restored bytearray: {restored!r}")


def pydantic_and_native_models_example(
    registry: SerializationRegistry,
) -> None:
    print_heading("Pydantic and native a11 models")

    reading = Reading(
        sensor="outside",
        value=18.5,
        observed_at=datetime.datetime(
            2026,
            7,
            24,
            12,
            30,
            tzinfo=datetime.timezone.utc,
        ),
        raw_sample=b"\x10\x20",
    )
    chunk = registry.to_chunk(reading, JSON_MIMETYPE)
    restored = registry.from_chunk(chunk)

    describe_chunk(chunk)
    assert type(restored) is Reading
    assert restored == reading
    print(f"restored model type: {type(restored).__name__}")

    # Native a11 types use the same BaseModel registration and ordinary
    # validated Pydantic construction.
    message = types.WireMessage(
        node_fragments=[
            types.NodeFragment(
                id="fragment",
                data=types.NodeRef(id="source-node"),
            )
        ],
        headers={"example": b"serialization"},
    )
    message_chunk = registry.to_chunk(message, MSGPACK_MIMETYPE)
    restored_message = registry.from_chunk(message_chunk)

    assert type(restored_message) is types.WireMessage
    assert restored_message == message
    print(f"restored native type: {type(restored_message).__name__}")


def explicit_mimetype_example(registry: SerializationRegistry) -> None:
    print_heading("Missing or stale metadata")

    # An explicit MIME and obj_type can decode data with no metadata at all.
    metadata_free = types.Chunk(data=b'{"answer":42}')
    restored = registry.from_chunk(
        metadata_free,
        mimetype_patterns=JSON_MIMETYPE,
        obj_type=dict,
    )
    assert restored == {"answer": 42}
    print(f"metadata-free JSON: {restored!r}")

    # Explicit selectors are authoritative. Here the data is JSON even though
    # stale metadata says MessagePack. The useful type tag is still retained.
    stale = types.Chunk(
        metadata=types.ChunkMetadata(mimetype=f"{MSGPACK_MIMETYPE};type=object"),
        data=b'{"metadata":"overridden"}',
    )
    restored = registry.from_chunk(stale, JSON_MIMETYPE)
    assert restored == {"metadata": "overridden"}
    print(f"stale metadata overridden: {restored!r}")


def custom_registry_example() -> None:
    print_heading("Custom registration")

    # New registries are empty. This one intentionally contains only a Point
    # codec, using a small text representation.
    registry = SerializationRegistry()

    def serialize_point(point: Point) -> types.Chunk:
        # A serializer may return a complete Chunk when it needs to add
        # metadata. The registry preserves attributes but replaces this
        # placeholder MIME with "text/x-point;type=Point".
        return types.Chunk(
            metadata=types.ChunkMetadata(
                mimetype="text/placeholder",
                attributes={"coordinate-system": b"cartesian"},
            ),
            data=f"{point.x},{point.y}",
        )

    def deserialize_point(
        chunk: types.Chunk,
        obj_type: type[Point],
    ) -> Point:
        # receives_chunk=True supplies metadata as well as payload bytes.
        coordinate_system = chunk.metadata.get_attribute(
            "coordinate-system",
            decode=True,
        )
        assert coordinate_system == "cartesian"
        x, y = (int(part) for part in chunk.data.decode("utf-8").split(","))
        return obj_type(x=x, y=y)

    registry.register(
        Point,
        "text/x-point",
        serialize_point,
        deserialize_point,
        receives_chunk=True,
    )

    value = Point(3, 7)
    chunk = registry.to_chunk(value)
    restored = registry.from_chunk(chunk)

    describe_chunk(chunk)
    print(
        "coordinate system:",
        chunk.metadata.get_attribute("coordinate-system", decode=True),
    )
    assert restored == value
    print(f"restored custom type: {restored!r}")

    # Ordinary failures always leave registry methods as StatusException.
    try:
        registry.to_chunk({"there-is": "no dict serializer here"})
    except StatusException as exc:
        print(f"expected error: {exc.status.code}: {exc.status.message}")


def main() -> None:
    registry = get_global_serialization_registry()

    default_json_example(registry)
    nested_values_example(registry)
    msgpack_example(registry)
    pydantic_and_native_models_example(registry)
    explicit_mimetype_example(registry)
    custom_registry_example()

    print_heading("Done")
    print("Every round-trip assertion passed.")


if __name__ == "__main__":
    main()
