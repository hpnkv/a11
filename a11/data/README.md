# Data serialization

`a11.data.serialization` converts Python objects to and from
`a11.data.types.Chunk`. A registry maps a Python type and a representation
MIME type to serializer and deserializer callbacks.

A chunk's metadata is the only thing that says how to read its bytes. When the
value is one the format already describes, the media type is the whole story:

```text
application/json
application/x-msgpack
```

When it is not, a stable `type` parameter names it:

```text
application/x-msgpack;type=Reading
application/json;type=a11.sdk.Interaction
```

The seven JSON-native shapes (`object`, `array`, `integer`, `number`, `string`,
`boolean`, `null`) never carry a parameter — writing `;type=object` on an object
says nothing a parser did not already know. Nothing inside the payload names a
type either: a serialized value is ordinary JSON or MessagePack.

## Quick start

The global registry already contains the standard JSON and MessagePack
handlers:

```python
from a11.data.serialization import get_global_serialization_registry

registry = get_global_serialization_registry()

chunk = registry.to_chunk(
    {"sensor": "outside", "temperature": 18.5},
    "application/json",
)

print(chunk.get_mimetype())
# application/json

reading = registry.from_chunk(chunk)
assert reading == {"sensor": "outside", "temperature": 18.5}
assert type(reading) is dict
```

`Chunk.data` is bytes. `Chunk.metadata.mimetype` records both the selected
representation and the serialized, language-neutral type where possible.

If no output MIME type is supplied, registration order selects the preferred
representation. The standard registry registers JSON first:

```python
chunk = registry.to_chunk([1, 2, 3])
assert chunk.get_mimetype() == "application/json"
```

At the wire boundary, a top-level Python `tuple` is the same JSON `array` as a
`list`, so default decoding returns a `list`. Pass `obj_type=tuple` when a
Python consumer specifically needs a tuple.

## Standard codecs

The standard registry supports these representations:

| Representation | MIME type |
| --- | --- |
| JSON | `application/json` |
| MessagePack | `application/x-msgpack` |

Both representations support:

- `dict`, `list`, `tuple`, `set`, and `frozenset`
- `int`, `float`, `bool`, `str`, `bytes`, `bytearray`, and `None`
- `datetime.datetime`, `datetime.date`, `datetime.time`, and
  `datetime.timedelta`
- `uuid.UUID`
- `pydantic.BaseModel` subclasses
- `a11.timing.Time` and `a11.timing.Duration`, including infinities
- native types from `a11.data.types`, with Pydantic-compatible protocols

A value at the top of a chunk keeps its Python type, because the chunk's `type`
parameter names it: `bytes` come back as `bytes`, a `set` as a `set`, a
`datetime` as a `datetime`.

Values *nested inside* schemaless data do not. A dictionary is serialized as a
JSON object, so `{"payload": b"\xff"}` reads back as `{"payload": "/w=="}` —
what JSON can say about it, and nothing more. Nesting keeps its fidelity when a
schema names it: a Pydantic model's `bytes` field, or a `list[Chunk]`, is
rebuilt from the annotation. Declare a model for structured payloads rather
than relying on a bare dictionary to carry Python types.

## Selecting and matching MIME types

Pass an exact MIME type when a particular representation is required:

```python
json_chunk = registry.to_chunk({"answer": 42}, "application/json")
msgpack_chunk = registry.to_chunk(
    {"answer": 42},
    "application/x-msgpack",
)
```

`to_chunk()` also accepts wildcard selectors. The closest registered Python
type wins, followed by registration order:

```python
chunk = registry.to_chunk({"answer": 42}, "application/*")
assert chunk.get_mimetype() == "application/json"
```

A selector chooses the representation only. The object's own type decides the
tag, so a `type` parameter in the selector is ignored.

Normally `from_chunk()` reads the exact MIME type from the chunk:

```python
value = registry.from_chunk(chunk)
```

The optional `mimetype_patterns` argument can be one selector or an ordered
sequence. Explicit selectors are authoritative, so they can also be used when
metadata is missing or stale:

```python
from a11.data.types import Chunk

metadata_free = Chunk(data=b'{"answer":42}')
value = registry.from_chunk(
    metadata_free,
    mimetype_patterns="application/json",
    obj_type=dict,
)
assert value == {"answer": 42}
```

When a sequence is supplied, registry matches are considered in that order:

```python
value = registry.from_chunk(
    json_chunk,
    mimetype_patterns=["application/json", "application/*"],
)
```

Selectors route to a codec; they are not decode-error recovery. Once a handler
matches, its deserialization error is returned rather than trying a later
selector.

Without an `obj_type`, the chunk's `type` parameter decides what comes back; if
it names nothing — because it is absent, or because the module that would
define it was never imported — the payload decodes to whatever its format
describes, a `dict`, a `list` or a scalar.

Pass `obj_type` when the caller requires a particular result type. The registry
makes a best effort to produce it and reports the deserializer's own error when
the data will not fit:

```python
value = registry.from_chunk(
    metadata_free,
    "application/json",
    obj_type=dict,
)
```

If an exact deserializer is unavailable, the registry searches the requested
type's superclasses. A deserializer that accepts a second argument receives
the resolved target type and can construct the subclass.

## Global and private registries

`get_global_serialization_registry()` returns the process-wide registry with
all standard codecs installed:

```python
from a11.data.serialization import get_global_serialization_registry

registry = get_global_serialization_registry()
```

A new registry is empty unless `register_defaults=True` is passed:

```python
from a11.data.serialization import SerializationRegistry

empty_registry = SerializationRegistry()
standard_registry = SerializationRegistry(register_defaults=True)
```

Defaults can also be installed later:

```python
from a11.data.serialization import register_default_serializers

registry = SerializationRegistry()
registry.register_defaults()

# Equivalent on an empty registry:
another_registry = SerializationRegistry()
register_default_serializers(another_registry)
```

Applications can replace the global registry:

```python
from a11.data.serialization import set_global_serialization_registry

set_global_serialization_registry(standard_registry)
```

Prefer a private registry when handlers are local to one component or tests
must not affect process-wide behavior.

## Registering custom types

`register()` installs a serializer/deserializer pair atomically:

```python
from dataclasses import dataclass

from a11.data.serialization import SerializationRegistry


@dataclass(frozen=True)
class Point:
    x: int
    y: int


def serialize_point(point: Point) -> str:
    return f"{point.x},{point.y}"


def deserialize_point(data: bytes, obj_type: type[Point]) -> Point:
    x, y = (int(part) for part in data.decode("utf-8").split(","))
    return obj_type(x=x, y=y)


registry = SerializationRegistry()
registry.register(
    Point,
    "text/x-point",
    serialize_point,
    deserialize_point,
)

chunk = registry.to_chunk(Point(3, 7))
assert chunk.get_mimetype() == "text/x-point;type=Point"
assert registry.from_chunk(chunk) == Point(3, 7)
```

Serializers and deserializers can instead be installed independently with
`register_serializer()` and `register_deserializer()`.

A serializer has this primary contract:

```python
serializer(obj) -> str | bytes | bytearray | memoryview | Chunk
```

Raw data is wrapped in a new `Chunk`. If the callback returns a `Chunk`, its
timestamp and attributes are preserved, but the registry replaces its MIME
type with the exact registered MIME and Python type.

A deserializer can accept either raw data alone or raw data plus the resolved
target type:

```python
deserializer(data)
deserializer(data, obj_type)
```

To inspect attributes or other chunk metadata, receive the complete chunk:

```python
from a11.data.types import Chunk


def deserialize_with_metadata(chunk: Chunk) -> str:
    encoding = chunk.metadata.get_attribute("encoding")
    return chunk.data.decode(encoding.decode() if encoding else "utf-8")


registry.register_deserializer(
    str,
    "text/x-with-metadata",
    deserialize_with_metadata,
    receives_chunk=True,
)
```

The `receives_chunk=True` flag is explicit and recommended. The registry also
recognizes a first parameter named `chunk` or annotated as `Chunk`.

Registering the same Python type and MIME type twice raises an
`ALREADY_EXISTS` status.

## Pydantic and native a11 models

Pydantic model subclasses use the normal Pydantic validation path:

```python
import pydantic

from a11.data.serialization import get_global_serialization_registry


class Reading(pydantic.BaseModel):
    sensor: str
    value: float


registry = get_global_serialization_registry()
chunk = registry.to_chunk(
    Reading(sensor="outside", value=18.5),
    "application/json",
)
assert chunk.get_mimetype() == "application/json;type=Reading"

reading = registry.from_chunk(chunk)
assert isinstance(reading, Reading)
```

The class name disambiguates subclasses. The receiving process must import the
model class before deserialization so it is loaded and discoverable. Class
names are stable across registries; classes with the same name from different
module paths are intentionally not disambiguated.

Native values such as `Chunk`, `NodeRef`, and `WireMessage` work through the
same registration. Their validation and JSON helpers follow Pydantic's public
protocols without creating a second Pydantic object representation.

## Error handling

Registry methods only expose `StatusException` for ordinary failures:

```python
from a11.status import StatusException

try:
    registry.to_chunk(object())
except StatusException as exc:
    print(exc.status.code)
    print(exc.status.message)
```

Important status codes include:

- `NOT_FOUND` when no matching serializer, deserializer, or type exists
- `INVALID_ARGUMENT` for missing MIME metadata, incompatible target types, or
  malformed serialized data
- `ALREADY_EXISTS` for duplicate registrations

A `StatusException` raised by a callback is propagated with its status intact.
Other ordinary callback exceptions are converted through `Status`.
`KeyboardInterrupt`, `asyncio.CancelledError`, and similar process/task
control-flow exceptions are allowed to propagate.

Referenced chunks must be resolved before calling `from_chunk()`; the registry
does not fetch `Chunk.ref` values.

## Complete example

Run the commented example from the repository root:

```shell
python examples/001-serialization/main.py
```

It demonstrates the default formats, nested special values, Pydantic and
native models, MIME overrides, custom registration, and status handling.
