import datetime
from typing import Any, Callable, TypeVar, Sequence

import msgpack

from a11 import status

Status = status.Status
StatusCode = status.StatusCode

Packer = msgpack.Packer
Unpacker = msgpack.Unpacker

T = TypeVar("T")


def ensure_memoryview(data: bytes | bytearray | memoryview) -> memoryview:
    if isinstance(data, memoryview):
        return data

    if not isinstance(data, (bytes, bytearray)):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="argument must be bytes, bytearray, or memoryview",
        ).to_exception()

    return memoryview(data)


def msgpack_exception_to_status(exc: Exception):
    if isinstance(exc, status.StatusException):
        return exc.status

    if isinstance(exc, msgpack.ExtraData):
        return Status(
            code=StatusCode.OUT_OF_RANGE,
            message="msgpack unpack error: extra data",
        )

    if isinstance(exc, msgpack.UnpackException):
        if isinstance(exc, msgpack.BufferFull):
            return Status(
                code=StatusCode.RESOURCE_EXHAUSTED,
                message="msgpack unpack error: buffer full",
            )
        if isinstance(exc, msgpack.OutOfData):
            return Status(
                code=StatusCode.OUT_OF_RANGE,
                message="msgpack unpack error: out of data",
            )
        if isinstance(exc, msgpack.FormatError):
            return Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="msgpack unpack error: format error",
            )
        if isinstance(exc, msgpack.StackError):
            return Status(
                code=StatusCode.RESOURCE_EXHAUSTED,
                message="msgpack unpack error: stack error",
            )
        return Status(
            code=StatusCode.UNKNOWN,
            message=f"msgpack unpack error: {str(exc)}",
        )

    if isinstance(exc, OverflowError):
        return Status(
            code=StatusCode.OUT_OF_RANGE,
            message="msgpack unpack error: overflow error",
        )

    return Status(
        code=StatusCode.UNKNOWN,
        message=f"msgpack unpack error: {str(exc)}",
    )


_EPOCH = datetime.datetime(1970, 1, 1, tzinfo=datetime.timezone.utc)


def datetime_to_micros(dt: datetime.datetime):
    dt = dt.astimezone(datetime.timezone.utc)
    delta = dt - _EPOCH
    return (
        delta.days * 86_400_000_000
        + delta.seconds * 1_000_000
        + delta.microseconds
    )


def datetime_from_micros(
    micros: int,
) -> datetime.datetime:
    return _EPOCH + datetime.timedelta(microseconds=micros)


def unpack(
    unpacker: msgpack.Unpacker,
    cls: type[T] | Sequence[type],
    *,
    allow_none: bool = False,
) -> T:
    try:
        unpacked = unpacker.unpack()
    except Exception as exc:
        raise msgpack_exception_to_status(exc).to_exception() from exc

    if unpacked is None:
        if allow_none:
            return None
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                "MessagePack data contained a nullopt, allow_none is False."
            ),
        ).to_exception()

    if isinstance(cls, type):
        cls = [cls]

    cls: Sequence[type]
    for c in cls:
        if isinstance(unpacked, c):
            return unpacked

    raise Status(
        code=StatusCode.INVALID_ARGUMENT,
        message=(
            "MessagePack data did not contain a valid instance of"
            f" {', '.join(c.__name__ for c in cls)}."
        ),
    ).to_exception()


def unpackb(
    data: bytes | bytearray | memoryview,
    cls: type[T] | Sequence[type[T]],
    unpack_fn: Callable[[bytes], Any] | None = None,
    *,
    allow_none: bool = False,
) -> T:
    data = ensure_memoryview(data)

    if data[0] == b"\xc0" and allow_none:
        if len(data) == 1:
            return None
        else:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "MessagePack data contained a nullopt, but had extra"
                    " following bytes."
                ),
            )

    unpack_fn = unpack_fn or msgpack.unpackb

    try:
        unpacked = unpack_fn(data)
    except Exception as exc:
        raise msgpack_exception_to_status(exc).to_exception() from exc

    if isinstance(cls, type):
        cls = [cls]

    cls: Sequence[type]
    for c in cls:
        if isinstance(unpacked, c):
            return unpacked

    raise Status(
        code=StatusCode.INVALID_ARGUMENT,
        message=(
            "MessagePack data did not contain a valid instance of"
            f" {', '.join(c.__name__ for c in cls)}."
        ),
    ).to_exception()


def make_unpacker(
    data: bytes | bytearray | memoryview | None = None,
) -> msgpack.Unpacker:
    unpacker = msgpack.Unpacker()
    if data is None:
        return unpacker

    try:
        unpacker.feed(ensure_memoryview(data))
    except Exception as exc:
        raise msgpack_exception_to_status(exc).to_exception() from exc
    return unpacker


def ensure_data_consumed(
    unpacker: msgpack.Unpacker,
    data: bytes | bytearray | memoryview,
) -> None:
    if unpacker.tell() != len(ensure_memoryview(data)):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Extra data after deserialization.",
        ).to_exception()


def unpack_status(data: bytes | bytearray | memoryview) -> Status:
    unpacker = make_unpacker(data)

    code = unpack(unpacker, int)

    try:
        status_code = StatusCode(code)
    except ValueError:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                "MessagePack data contained a status code that is not a"
                f" valid StatusCode: {code}."
            ),
        ).to_exception()

    message: str | bytes = unpack(unpacker, (str, bytes))

    if isinstance(message, bytes):
        try:
            message = message.decode("utf-8")
        except UnicodeDecodeError:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "MessagePack data contained a string message that could"
                    " not be decoded as UTF-8."
                ),
            ).to_exception()

    message: str

    try:
        details = unpacker.unpack()
    except Exception as exc:
        raise msgpack_exception_to_status(exc).to_exception() from exc

    if details is not None and not isinstance(details, list):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="MessagePack data did not contain a valid list of details.",
        ).to_exception()

    details = details or []
    for idx, detail in enumerate(details):
        if not isinstance(detail, dict):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "MessagePack data contained a list of details that did not"
                    f" contain a valid dict at index {idx}."
                ),
            ).to_exception()

    ensure_data_consumed(unpacker, data)

    return Status(code=status_code, message=message, details=details)


def pack_status(s: Status) -> bytes:
    packer = msgpack.Packer(autoreset=False)
    packer.pack(s.code.value)
    packer.pack(s.message)
    packer.pack(s.details)
    return packer.bytes()
