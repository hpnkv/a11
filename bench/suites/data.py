# Copyright 2026 The A11 Authors.

"""The codec floor: what a byte costs before any of A11 has touched it.

Everything else in this suite sits on top of these numbers. A session moving
20k actions/s is moving at least 40k chunk encodes and 40k decodes with it, so
if `to_chunk` costs 20us nothing above it can be fast, and no amount of work on
the transport will show up. Measured here:

* **Chunk codec** -- `to_chunk`/`from_chunk` per representation (JSON,
  MessagePack, raw bytes, text) and per payload size, in ops/s and MiB/s.
* **Model codec** -- the same for a registered pydantic model, which is the
  path a typed port actually takes.
* **WireMessage codec** -- `to_msgpack`/`from_msgpack` for the envelope, at a
  range of fragments per message. This is the per-message tax the transport
  pays, and its shape against batch size is the argument for or against
  coalescing.
* **Value memory** -- resident bytes per live `Chunk` and per `NodeFragment`,
  which is what bounds how much a store can hold.
"""

from __future__ import annotations

from typing import ClassVar

import pydantic

import a11
from a11.data import types
from bench.harness import (
    Result,
    benchmark,
    memory_slope,
    throughput_sync,
)

SUITE = "data"

#: Small / medium / large, chosen to straddle the interesting boundaries: a
#: control message, a chat token batch, a screenshot, a model response blob.
_SIZES = (64, 1024, 64 * 1024, 1024 * 1024)


class _Reading(pydantic.BaseModel):
    """A typical small typed port payload: a few scalars and a list."""

    A11_SERIAL_TAG: ClassVar[str] = "bench.Reading"

    name: str
    seq: int
    ok: bool
    values: list[float]


def _payload(size: int) -> dict:
    """A dict whose JSON encoding is roughly `size` bytes."""
    return {"id": "bench", "body": "x" * max(size - 24, 1)}


def _scaled(count: int, scale: float) -> int:
    return max(int(count * scale), 1)


@benchmark(SUITE, "chunk_codec")
async def chunk_codec(scale: float) -> list[Result]:
    """to_chunk/from_chunk per representation and payload size."""
    results: list[Result] = []
    cases = [
        ("json", "application/json", _payload),
        ("msgpack", "application/x-msgpack", _payload),
        # Two spellings of the same intent, because they cost wildly different
        # things: with no mimetype a `bytes` payload takes the JSON
        # registration and goes out base64-encoded, 4/3 the size and a text
        # encode; naming MessagePack puts it on the wire as a `bin` and copies
        # it. See `binary_representation`.
        ("bytes-default", "", lambda size: b"x" * size),
        ("bytes-msgpack", "application/x-msgpack", lambda size: b"x" * size),
        ("text", "", lambda size: "x" * size),
    ]
    for label, mimetype, build in cases:
        for size in _SIZES:
            value = build(size)
            encoded = a11.to_chunk(value, mimetype=mimetype)
            wire_bytes = len(encoded.data)
            iterations = _scaled(_iterations_for(size), scale)

            results.append(
                Result(
                    SUITE,
                    "to_chunk",
                    throughput_sync(
                        lambda _i, v=value, m=mimetype: a11.to_chunk(
                            v, mimetype=m
                        ),
                        iterations=iterations,
                        warmup=iterations // 10,
                        per_op_bytes=wire_bytes,
                    ),
                    {"repr": label, "size": _human(size)},
                )
            )
            results.append(
                Result(
                    SUITE,
                    "from_chunk",
                    throughput_sync(
                        lambda _i, c=encoded: a11.from_chunk(c),
                        iterations=iterations,
                        warmup=iterations // 10,
                        per_op_bytes=wire_bytes,
                    ),
                    {"repr": label, "size": _human(size)},
                )
            )
    return results


@benchmark(SUITE, "codec_dispatch_tax")
async def codec_dispatch_tax(scale: float) -> list[Result]:
    """What `to_chunk` costs beyond the encoder it eventually calls.

    The interesting number is not either rate on its own but the gap. A11's
    chunk codec resolves a registration on every call -- an `isinstance` per
    registered type, then a sort -- and that resolution is the same work
    whether the payload is 8 bytes or 8 megabytes. Measuring the bare encoder
    beside it separates the fixed tax from the per-byte cost, and the fixed tax
    is what caps small-message rates everywhere above this layer.
    """
    import json

    import ormsgpack

    value = {"id": "bench", "seq": 1, "ok": True}
    iterations = _scaled(50_000, scale)
    results = []

    json_chunk = a11.to_chunk(value, mimetype="application/json")
    msgpack_chunk = a11.to_chunk(value, mimetype="application/x-msgpack")

    for direction, label, wrapped, bare in (
        (
            "to_chunk",
            "json",
            lambda _i: a11.to_chunk(value, mimetype="application/json"),
            lambda _i: json.dumps(value).encode(),
        ),
        (
            "to_chunk",
            "msgpack",
            lambda _i: a11.to_chunk(value, mimetype="application/x-msgpack"),
            lambda _i: ormsgpack.packb(value),
        ),
        (
            "from_chunk",
            "json",
            lambda _i: a11.from_chunk(json_chunk),
            lambda _i: json.loads(json_chunk.data),
        ),
        (
            "from_chunk",
            "msgpack",
            lambda _i: a11.from_chunk(msgpack_chunk),
            lambda _i: ormsgpack.unpackb(msgpack_chunk.data),
        ),
    ):
        with_registry = throughput_sync(
            wrapped, iterations=iterations, warmup=iterations // 10
        )
        without = throughput_sync(
            bare, iterations=iterations, warmup=iterations // 10
        )
        tax_ns = with_registry["ns_per_op"] - without["ns_per_op"]
        results.append(
            Result(
                SUITE,
                f"{direction}_dispatch",
                {
                    **with_registry,
                    "p50_us": with_registry["ns_per_op"] / 1000,
                    "encoder_ops_per_s": without["ops_per_s"],
                    "dispatch_tax_us": tax_ns / 1000,
                },
                {"repr": label},
                note=(
                    f"bare encoder {without['ns_per_op']:.0f}ns, "
                    f"registry adds {tax_ns:.0f}ns"
                ),
            )
        )
    return results


@benchmark(SUITE, "binary_representation")
async def binary_representation(scale: float) -> list[Result]:
    """Wire inflation of a bytes payload, per representation.

    A `bytes` value with no mimetype named has exactly one registration to
    fall into that is not MessagePack, and JSON has no way to hold a byte
    string -- so it is base64. This measures the cost of not saying which.
    """
    payload = b"\xd3" * (256 * 1024)
    results = []
    for label, mimetype in (
        ("default", ""),
        ("msgpack", "application/x-msgpack"),
        ("json", "application/json"),
    ):
        chunk = a11.to_chunk(payload, mimetype=mimetype)
        iterations = _scaled(400, scale)
        metrics = throughput_sync(
            lambda _i, m=mimetype: a11.to_chunk(payload, mimetype=m),
            iterations=iterations,
            warmup=max(iterations // 10, 1),
            per_op_bytes=len(payload),
        )
        metrics["wire_bytes"] = float(len(chunk.data))
        metrics["inflation"] = len(chunk.data) / len(payload)
        results.append(
            Result(
                SUITE,
                "bytes_payload",
                metrics,
                {"asked_for": label},
                note=(
                    f"{chunk.metadata.mimetype} -> "
                    f"{len(chunk.data) / len(payload):.2f}x the payload"
                ),
            )
        )
    return results


@benchmark(SUITE, "model_codec")
async def model_codec(scale: float) -> list[Result]:
    """A registered pydantic model through the chunk codec, both ways."""
    registry = a11.get_global_serialization_registry()
    registry.set_type_tag(_Reading, _Reading.A11_SERIAL_TAG)
    results: list[Result] = []
    for width in (4, 64, 1024):
        model = _Reading(
            name="sensor",
            seq=1,
            ok=True,
            values=[float(i) for i in range(width)],
        )
        for mimetype, label in (
            ("application/json", "json"),
            ("application/x-msgpack", "msgpack"),
        ):
            chunk = a11.to_chunk(model, mimetype=mimetype)
            iterations = _scaled(20_000 // max(width // 16, 1), scale)
            results.append(
                Result(
                    SUITE,
                    "model_to_chunk",
                    throughput_sync(
                        lambda _i, m=model, t=mimetype: a11.to_chunk(
                            m, mimetype=t
                        ),
                        iterations=iterations,
                        warmup=iterations // 10,
                        per_op_bytes=len(chunk.data),
                    ),
                    {"repr": label, "floats": width},
                )
            )
            results.append(
                Result(
                    SUITE,
                    "model_from_chunk",
                    throughput_sync(
                        lambda _i, c=chunk: a11.from_chunk(
                            c, obj_type=_Reading
                        ),
                        iterations=iterations,
                        warmup=iterations // 10,
                        per_op_bytes=len(chunk.data),
                    ),
                    {"repr": label, "floats": width},
                )
            )
    return results


@benchmark(SUITE, "wire_message_codec")
async def wire_message_codec(scale: float) -> list[Result]:
    """WireMessage envelope encode/decode against fragments per message."""
    results: list[Result] = []
    for fragments in (1, 8, 64):
        for size in (64, 4096):
            message = types.WireMessage(
                node_fragments=[
                    types.NodeFragment(
                        data=types.Chunk(data=b"x" * size),
                        seq=index,
                        continued=True,
                    )
                    for index in range(fragments)
                ]
            )
            encoded = message.to_msgpack()
            iterations = _scaled(
                max(200_000 // (fragments * max(size // 64, 1)), 200), scale
            )
            results.append(
                Result(
                    SUITE,
                    "wire_to_msgpack",
                    throughput_sync(
                        lambda _i, m=message: m.to_msgpack(),
                        iterations=iterations,
                        warmup=iterations // 10,
                        per_op_items=fragments,
                        per_op_bytes=len(encoded),
                    ),
                    {"frags": fragments, "size": _human(size)},
                )
            )
            results.append(
                Result(
                    SUITE,
                    "wire_from_msgpack",
                    throughput_sync(
                        lambda _i, b=encoded: types.WireMessage.from_msgpack(b),
                        iterations=iterations,
                        warmup=iterations // 10,
                        per_op_items=fragments,
                        per_op_bytes=len(encoded),
                    ),
                    {"frags": fragments, "size": _human(size)},
                )
            )
    return results


@benchmark(SUITE, "value_memory")
async def value_memory(scale: float) -> list[Result]:
    """Resident bytes per live Chunk and per live NodeFragment."""
    stages = [_scaled(50_000, scale)] * 6
    results: list[Result] = []

    slope, trail = await memory_slope(
        lambda count: [types.Chunk(data=b"x" * 64) for _ in range(count)],
        counts=stages,
    )
    results.append(
        Result(
            SUITE,
            "chunk_resident",
            {"bytes_each": slope},
            {"payload": "64B"},
            note=f"64 payload bytes; the rest is the value's own. {trail}",
        )
    )

    seq = iter(range(1 << 30))
    slope, trail = await memory_slope(
        lambda count: [
            types.NodeFragment(
                data=types.Chunk(data=b"x" * 64),
                seq=next(seq),
                continued=True,
            )
            for _ in range(count)
        ],
        counts=stages,
    )
    results.append(
        Result(
            SUITE,
            "fragment_resident",
            {"bytes_each": slope},
            {"payload": "64B"},
            note=trail,
        )
    )
    return results


def _iterations_for(size: int) -> int:
    """Fewer iterations for bigger payloads; the byte rate stays comparable."""
    if size <= 1024:
        return 100_000
    if size <= 64 * 1024:
        return 10_000
    return 500


def _human(size: int) -> str:
    if size >= 1024 * 1024:
        return f"{size // 1024 // 1024}M"
    if size >= 1024:
        return f"{size // 1024}K"
    return f"{size}B"
