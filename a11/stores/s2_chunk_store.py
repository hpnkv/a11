# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Persist chunk streams in S2.

`S2ChunkStore` uses a data stream and an auxiliary state stream for one node.
Each fragment occupies one data record. S2 sequence numbers represent arrival
order and ordinary implicit A11 sequence numbers directly. Atomic append
batches and optimistic tail checks serialize mutations across processes.

Install the optional dependency with ``a11-kit[s2]``. The module remains
importable without it; constructing a store reports an `UNIMPLEMENTED` status
with the installation command.
"""

from __future__ import annotations

import asyncio
import dataclasses
import importlib
import os
import uuid
from collections.abc import Callable, Sequence
from typing import Any, Awaitable, Protocol, TypeVar

import msgpack

from a11 import timing
from a11.data import types
from a11.status import Status, StatusCode, StatusException
from a11.stores.chunk_store import ChunkStore

_FORMAT = "a11.chunk-store/v1"
_MAX_UINT32 = (1 << 32) - 1
_MAX_UINT64 = (1 << 64) - 1
_MAX_S2_BATCH_BYTES = 1024 * 1024
_MAX_S2_BATCH_RECORDS = 1000
_POLL_SECONDS = 0.05

_HEADER_FORMAT = b"a11-format"
_HEADER_TRANSACTION = b"a11-transaction"
_HEADER_SEQUENCE = b"a11-sequence"
_HEADER_FINAL = b"a11-final"
_HEADER_CHUNK = b"a11-chunk"
_HEADER_NODE_REF = b"a11-node-ref"
_HEADER_PREVIOUS = b"a11-previous"
_HEADER_CURSOR = b"a11-cursor"

_CHUNK_FORMAT = b"chunk/v2"
_CLEAR_FORMAT = b"clear/v2"
_ADVANCE_FORMAT = b"advance/v2"
_CLOSE_FORMAT = b"close/v2"

T = TypeVar("T")


class _Conflict(Exception):
    pass


@dataclasses.dataclass(frozen=True)
class _Record:
    body: bytes
    headers: tuple[tuple[bytes, bytes], ...] = ()


class _Backend(Protocol):
    async def ensure(self, state: bool = False) -> None: ...

    async def tail(self, state: bool = False) -> int: ...

    async def read(
        self, start: int, count: int, state: bool = False
    ) -> list[tuple[int, _Record]]: ...

    async def append(
        self,
        records: Sequence[_Record],
        expected_tail: int,
        state: bool = False,
    ) -> int: ...

    async def close(self) -> None: ...


def _status(code: StatusCode, message: str) -> StatusException:
    return Status(code=code, message=message).to_exception()


def _unsigned(value: object, name: str, maximum: int) -> int:
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or value < 0
        or value > maximum
    ):
        raise _status(
            StatusCode.INVALID_ARGUMENT,
            f"{name} must be an integer between 0 and {maximum}.",
        )
    return value


async def _before_deadline(
    operation: Awaitable[T], deadline: timing.Time | None
) -> T:
    if deadline is None or deadline == timing.infinite_future():
        return await operation
    remaining = deadline - timing.now()
    if remaining <= timing.zero_duration():
        if hasattr(operation, "close"):
            operation.close()  # type: ignore[union-attr]
        raise _status(
            StatusCode.DEADLINE_EXCEEDED,
            "Chunk store operation exceeded its deadline.",
        )
    seconds = remaining.float_seconds(0.0)
    assert seconds is not None
    try:
        return await asyncio.wait_for(operation, seconds)
    except TimeoutError:
        raise _status(
            StatusCode.DEADLINE_EXCEEDED,
            "Chunk store operation exceeded its deadline.",
        ) from None


def _load_sdk():
    try:
        return importlib.import_module("s2_sdk")
    except ModuleNotFoundError as error:
        if error.name != "s2_sdk":
            raise
        raise _status(
            StatusCode.UNIMPLEMENTED,
            "S2ChunkStore requires the optional S2 SDK. Install a11-kit[s2].",
        ) from None


def _sdk_status(error: Exception) -> StatusException:
    if isinstance(error, StatusException):
        return error
    status_code = getattr(error, "status_code", None)
    s2_code = getattr(error, "code", None)
    mapped_codes = {
        "bad_header": StatusCode.INVALID_ARGUMENT,
        "bad_path": StatusCode.INVALID_ARGUMENT,
        "bad_query": StatusCode.INVALID_ARGUMENT,
        "bad_json": StatusCode.INVALID_ARGUMENT,
        "bad_proto": StatusCode.INVALID_ARGUMENT,
        "bad_frame": StatusCode.INVALID_ARGUMENT,
        "invalid": StatusCode.INVALID_ARGUMENT,
        "permission_denied": StatusCode.PERMISSION_DENIED,
        "quota_exhausted": StatusCode.RESOURCE_EXHAUSTED,
        "basin_not_found": StatusCode.NOT_FOUND,
        "stream_not_found": StatusCode.NOT_FOUND,
        "access_token_not_found": StatusCode.UNAUTHENTICATED,
        "request_timeout": StatusCode.DEADLINE_EXCEEDED,
        "resource_already_exists": StatusCode.ALREADY_EXISTS,
        "basin_deletion_pending": StatusCode.FAILED_PRECONDITION,
        "stream_deletion_pending": StatusCode.FAILED_PRECONDITION,
        "transaction_conflict": StatusCode.ABORTED,
        "rate_limited": StatusCode.RESOURCE_EXHAUSTED,
        "other": StatusCode.UNAVAILABLE,
        "storage": StatusCode.UNAVAILABLE,
        "hot_server": StatusCode.UNAVAILABLE,
        "unavailable": StatusCode.UNAVAILABLE,
        "server_draining": StatusCode.UNAVAILABLE,
        "upstream_timeout": StatusCode.DEADLINE_EXCEEDED,
    }
    if s2_code in mapped_codes:
        code = mapped_codes[s2_code]
    elif isinstance(status_code, int):
        code = StatusCode.from_http_code(status_code)
    elif isinstance(error, (TimeoutError, asyncio.TimeoutError)):
        code = StatusCode.DEADLINE_EXCEEDED
    elif type(error).__name__ == "S2ClientError" and error.__cause__ is None:
        code = StatusCode.INVALID_ARGUMENT
    else:
        code = StatusCode.UNAVAILABLE
    details = []
    if s2_code is not None or status_code is not None:
        details.append(
            {
                "s2_code": str(s2_code or "unknown"),
                "http_status": status_code,
            }
        )
    return Status(
        code=code,
        message=f"S2 operation failed: {error}",
        details=details,
    ).to_exception()


class _SdkBackend:
    def __init__(
        self,
        basin: str,
        stream: str,
        access_token: str | None,
        client: object | None,
    ) -> None:
        sdk = _load_sdk()
        self._sdk = sdk
        self._owns_client = client is None
        try:
            if client is None:
                token = access_token or os.environ.get("S2_ACCESS_TOKEN")
                if not token:
                    raise _status(
                        StatusCode.UNAUTHENTICATED,
                        "S2 access token is required; pass access_token or "
                        "set S2_ACCESS_TOKEN.",
                    )
                retry = sdk.Retry(
                    append_retry_policy=sdk.AppendRetryPolicy.NO_SIDE_EFFECTS
                )
                client = sdk.S2(token, retry=retry)
            self._client = client
            self._basin = client.basin(basin)
            self._stream_name = stream
            self._stream = self._basin.stream(stream)
            self._state_stream_name = f"{stream}.state"
            self._state_stream = self._basin.stream(self._state_stream_name)
        except StatusException:
            raise
        except Exception as error:
            raise _sdk_status(error) from None

    async def ensure(self, state: bool = False) -> None:
        try:
            await self._basin.ensure_stream(
                self._state_stream_name if state else self._stream_name
            )
        except asyncio.CancelledError:
            raise
        except Exception as error:
            raise _sdk_status(error) from None

    async def tail(self, state: bool = False) -> int:
        try:
            stream = self._state_stream if state else self._stream
            return (await stream.check_tail()).seq_num
        except asyncio.CancelledError:
            raise
        except Exception as error:
            raise _sdk_status(error) from None

    async def read(
        self, start: int, count: int, state: bool = False
    ) -> list[tuple[int, _Record]]:
        try:
            stream = self._state_stream if state else self._stream
            batch = await stream.read(
                start=self._sdk.SeqNum(start),
                limit=self._sdk.ReadLimit(count=count),
            )
            return [
                (
                    record.seq_num,
                    _Record(record.body, tuple(record.headers)),
                )
                for record in batch.records
            ]
        except asyncio.CancelledError:
            raise
        except Exception as error:
            raise _sdk_status(error) from None

    async def append(
        self,
        records: Sequence[_Record],
        expected_tail: int,
        state: bool = False,
    ) -> int:
        try:
            stream = self._state_stream if state else self._stream
            ack = await stream.append(
                self._sdk.AppendInput(
                    records=[
                        self._sdk.Record(
                            body=record.body,
                            headers=list(record.headers),
                        )
                        for record in records
                    ],
                    match_seq_num=expected_tail,
                )
            )
            return ack.end.seq_num
        except asyncio.CancelledError:
            raise
        except self._sdk.SeqNumMismatchError as error:
            raise _Conflict from error
        except Exception as error:
            raise _sdk_status(error) from None

    async def close(self) -> None:
        if not self._owns_client:
            return
        try:
            await self._client.close()
        except asyncio.CancelledError:
            raise
        except Exception as error:
            raise _sdk_status(error) from None


class S2ChunkStore(ChunkStore):
    """A durable, multi-process ChunkStore backed by S2 streams.

    Args:
        id: Node identifier exposed by `get_id`.
        basin: S2 basin containing the store stream.
        access_token: S2 access token. Omit it to read ``S2_ACCESS_TOKEN``.
        stream_prefix: Prefix placed before the node id in the stream name.
        client: Shared ``s2_sdk.S2`` client. The caller retains ownership.
        ensure_stream: Create the data and state streams when needed. Set this
            to `False` when both exist and the token is data-plane-only.

    The S2 stream must retain its complete history. A retention policy or a
    manual trim removes transaction-log state and is reported as `DATA_LOSS`.
    One transaction must fit within S2's 1 MiB atomic append limit.
    """

    def __init__(
        self,
        id: str,
        basin: str,
        access_token: str | None = None,
        *,
        stream_prefix: str = "a11/chunks/",
        client: object | None = None,
        ensure_stream: bool = True,
        _backend: _Backend | None = None,
    ) -> None:
        if not isinstance(id, str) or not id or "\0" in id:
            raise _status(
                StatusCode.INVALID_ARGUMENT,
                "id must be a non-empty string without NUL bytes.",
            )
        if not isinstance(basin, str) or not basin:
            raise _status(
                StatusCode.INVALID_ARGUMENT,
                "basin must be a non-empty string.",
            )
        if not isinstance(stream_prefix, str):
            raise _status(
                StatusCode.INVALID_ARGUMENT,
                "stream_prefix must be a string.",
            )
        if not isinstance(ensure_stream, bool):
            raise _status(
                StatusCode.INVALID_ARGUMENT,
                "ensure_stream must be a boolean.",
            )
        stream = f"{stream_prefix}{id}"
        if len(f"{stream}.state".encode("utf-8")) > 512:
            raise _status(
                StatusCode.INVALID_ARGUMENT,
                "The S2 data or state stream name exceeds 512 UTF-8 bytes.",
            )
        super().__init__()
        self._id = id
        self._basin = basin
        self._stream_name = stream
        self._backend = _backend or _SdkBackend(
            basin, stream, access_token, client
        )
        self._lock = asyncio.Lock()
        self._data_ensured = not ensure_stream
        self._state_ensured = not ensure_stream
        self._log_tail = 0
        self._state_tail = 0
        self._chunks: dict[int, types.Chunk | types.NodeRef] = {}
        self._arrivals: list[int] = []
        self._cleared: set[int] = set()
        self._cursor = 0
        self._final_seq: int | None = None
        self._closed_status: Status | None = None
        self._transactions: set[str] = set()

    @staticmethod
    def create(
        id: str,
        basin: str,
        access_token: str | None = None,
        *,
        stream_prefix: str = "a11/chunks/",
        client: object | None = None,
        ensure_stream: bool = True,
    ) -> "S2ChunkStore":
        """Create an S2-backed fragment log for one node id."""
        return S2ChunkStore(
            id,
            basin,
            access_token,
            stream_prefix=stream_prefix,
            client=client,
            ensure_stream=ensure_stream,
        )

    def get_id(self) -> str:
        return self._id

    @property
    def basin(self) -> str:
        """The S2 basin containing this store."""
        return self._basin

    @property
    def stream_name(self) -> str:
        """The S2 stream containing this store's transaction log."""
        return self._stream_name

    async def aclose(self) -> None:
        """Close the S2 client created by this store.

        A client passed to the constructor remains owned by its caller.
        """
        await self._backend.close()

    async def __aenter__(self) -> "S2ChunkStore":
        return self

    async def __aexit__(self, *_: object) -> None:
        await self.aclose()

    async def _ensure(
        self, state: bool, deadline: timing.Time | None = None
    ) -> None:
        ensured = self._state_ensured if state else self._data_ensured
        if ensured:
            return
        await _before_deadline(self._backend.ensure(state), deadline)
        if state:
            self._state_ensured = True
        else:
            self._data_ensured = True

    def _apply_legacy(self, body: bytes, physical_seq: int) -> None:
        try:
            event = msgpack.unpackb(body, raw=False, strict_map_key=False)
        except (TypeError, ValueError, msgpack.UnpackException) as error:
            raise _status(
                StatusCode.DATA_LOSS,
                f"S2 record {physical_seq} is not valid MessagePack: {error}",
            ) from None
        if not isinstance(event, dict) or event.get("format") != _FORMAT:
            raise _status(
                StatusCode.DATA_LOSS,
                f"S2 record {physical_seq} is not an A11 chunk-store event.",
            )
        transaction = event.get("transaction")
        operation = event.get("operation")
        if (
            not isinstance(transaction, str)
            or transaction in self._transactions
        ):
            raise _status(
                StatusCode.DATA_LOSS,
                f"S2 record {physical_seq} has an invalid transaction id.",
            )
        try:
            if operation == "put":
                for encoded in event["fragments"]:
                    fragment = types.NodeFragment.from_msgpack(encoded)
                    seq = fragment.seq
                    if seq is None or seq in self._chunks:
                        raise ValueError("duplicate or missing sequence")
                    self._chunks[seq] = fragment.data
                    self._arrivals.append(seq)
                    if not fragment.continued:
                        if self._final_seq not in (None, seq):
                            raise ValueError("conflicting final sequence")
                        self._final_seq = seq
            elif operation == "clear":
                seq = event["seq"]
                if seq not in self._chunks:
                    raise ValueError("clear refers to a missing sequence")
                self._cleared.add(seq)
            elif operation == "advance":
                previous = event["previous"]
                cursor = event["cursor"]
                if previous != self._cursor or cursor < previous:
                    raise ValueError("invalid shared cursor transition")
                self._cursor = cursor
            elif operation == "close":
                if self._closed_status is not None:
                    raise ValueError("duplicate write closure")
                self._closed_status = Status.model_validate(event["status"])
            else:
                raise ValueError(f"unknown operation {operation!r}")
        except (KeyError, TypeError, ValueError, StatusException) as error:
            raise _status(
                StatusCode.DATA_LOSS,
                f"S2 record {physical_seq} has invalid chunk-store state: "
                f"{error}",
            ) from None
        self._transactions.add(transaction)

    @staticmethod
    def _headers(record: _Record, physical_seq: int) -> dict[bytes, bytes]:
        result: dict[bytes, bytes] = {}
        for name, value in record.headers:
            if name in result:
                raise _status(
                    StatusCode.DATA_LOSS,
                    f"S2 record {physical_seq} repeats header {name!r}.",
                )
            result[name] = value
        return result

    @staticmethod
    def _header_int(
        headers: dict[bytes, bytes], name: bytes, physical_seq: int
    ) -> int | None:
        value = headers.get(name)
        if value is None:
            return None
        try:
            return int(value.decode("ascii"))
        except (UnicodeDecodeError, ValueError):
            raise _status(
                StatusCode.DATA_LOSS,
                f"S2 record {physical_seq} has invalid header {name!r}.",
            ) from None

    def _decode_chunk(
        self, record: _Record, physical_seq: int
    ) -> types.NodeFragment | None:
        headers = self._headers(record, physical_seq)
        if headers.get(_HEADER_FORMAT) != _CHUNK_FORMAT:
            return None
        seq = self._header_int(headers, _HEADER_SEQUENCE, physical_seq)
        seq = physical_seq if seq is None else seq
        if seq < 0 or seq > _MAX_UINT32:
            raise _status(
                StatusCode.DATA_LOSS,
                f"S2 record {physical_seq} has an invalid logical sequence.",
            )
        try:
            if _HEADER_NODE_REF in headers:
                if record.body or _HEADER_CHUNK in headers:
                    raise ValueError("invalid node-reference record")
                data: types.Chunk | types.NodeRef = types.NodeRef.from_msgpack(
                    headers[_HEADER_NODE_REF]
                )
            elif _HEADER_CHUNK in headers:
                data = types.Chunk.from_msgpack(headers[_HEADER_CHUNK])
                data.data = record.body
            else:
                data = types.Chunk(data=record.body)
        except (TypeError, ValueError, StatusException) as error:
            raise _status(
                StatusCode.DATA_LOSS,
                f"S2 record {physical_seq} has invalid chunk metadata: {error}",
            ) from None
        return types.NodeFragment(
            id=self._id,
            data=data,
            seq=seq,
            continued=_HEADER_FINAL not in headers,
        )

    def _apply_record(
        self, record: _Record, physical_seq: int, state: bool
    ) -> None:
        headers = self._headers(record, physical_seq)
        wire_format = headers.get(_HEADER_FORMAT)
        if wire_format is None:
            if state:
                raise _status(
                    StatusCode.DATA_LOSS,
                    f"S2 state record {physical_seq} has no format header.",
                )
            self._apply_legacy(record.body, physical_seq)
            return
        transaction_bytes = headers.get(_HEADER_TRANSACTION)
        transaction = None
        if transaction_bytes is not None:
            try:
                transaction = transaction_bytes.decode("ascii")
            except UnicodeDecodeError:
                transaction = ""
            if not transaction or transaction in self._transactions:
                raise _status(
                    StatusCode.DATA_LOSS,
                    f"S2 record {physical_seq} has an invalid transaction id.",
                )
        try:
            if wire_format == _CHUNK_FORMAT and not state:
                fragment = self._decode_chunk(record, physical_seq)
                assert fragment is not None and fragment.seq is not None
                if fragment.seq in self._chunks:
                    raise ValueError("duplicate sequence")
                self._chunks[fragment.seq] = fragment.data
                self._arrivals.append(fragment.seq)
                if not fragment.continued:
                    if self._final_seq not in (None, fragment.seq):
                        raise ValueError("conflicting final sequence")
                    self._final_seq = fragment.seq
            elif wire_format == _CLOSE_FORMAT and not state:
                if self._closed_status is not None:
                    raise ValueError("duplicate write closure")
                value = msgpack.unpackb(
                    record.body, raw=False, strict_map_key=False
                )
                self._closed_status = Status.model_validate(value)
            elif wire_format == _CLEAR_FORMAT and state:
                seq = self._header_int(headers, _HEADER_SEQUENCE, physical_seq)
                if seq is None or seq < 0 or seq > _MAX_UINT32:
                    raise ValueError("invalid clear sequence")
                self._cleared.add(seq)
            elif wire_format == _ADVANCE_FORMAT and state:
                previous = self._header_int(
                    headers, _HEADER_PREVIOUS, physical_seq
                )
                cursor = self._header_int(headers, _HEADER_CURSOR, physical_seq)
                if (
                    previous is None
                    or cursor is None
                    or previous != self._cursor
                    or cursor < previous
                ):
                    raise ValueError("invalid shared cursor transition")
                self._cursor = cursor
            else:
                raise ValueError(f"unexpected record format {wire_format!r}")
        except (
            KeyError,
            TypeError,
            ValueError,
            msgpack.UnpackException,
        ) as error:
            raise _status(
                StatusCode.DATA_LOSS,
                f"S2 record {physical_seq} has invalid chunk-store state: "
                f"{error}",
            ) from None
        if transaction is not None:
            self._transactions.add(transaction)

    async def _sync_stream(
        self, state: bool, deadline: timing.Time | None = None
    ) -> None:
        await self._ensure(state, deadline)
        current = self._state_tail if state else self._log_tail
        tail = await _before_deadline(self._backend.tail(state), deadline)
        if tail < current:
            raise _status(
                StatusCode.DATA_LOSS,
                "The S2 chunk-store stream was trimmed or replaced.",
            )
        while current < tail:
            records = await _before_deadline(
                self._backend.read(current, min(1000, tail - current), state),
                deadline,
            )
            if not records:
                raise _status(
                    StatusCode.DATA_LOSS,
                    f"S2 record {current} is missing.",
                )
            for physical_seq, record in records:
                if physical_seq != current:
                    raise _status(
                        StatusCode.DATA_LOSS,
                        f"Expected S2 record {current}, got {physical_seq}.",
                    )
                self._apply_record(record, physical_seq, state)
                current += 1
        if state:
            self._state_tail = current
        else:
            self._log_tail = current

    async def _sync(self, deadline: timing.Time | None = None) -> None:
        await self._sync_stream(False, deadline)
        await self._sync_stream(True, deadline)

    def _encode_records(
        self,
        event: dict[str, Any],
        transaction: str,
        physical_start: int,
    ) -> tuple[list[_Record], bool]:
        operation = event["operation"]
        records: list[_Record] = []
        state = operation in ("clear", "advance")
        if operation == "put":
            for index, fragment in enumerate(event["fragments"]):
                assert isinstance(fragment, types.NodeFragment)
                assert fragment.seq is not None
                headers: list[tuple[bytes, bytes]] = [
                    (_HEADER_FORMAT, _CHUNK_FORMAT)
                ]
                if index == 0:
                    headers.append(
                        (_HEADER_TRANSACTION, transaction.encode("ascii"))
                    )
                if fragment.seq != physical_start + index:
                    headers.append(
                        (_HEADER_SEQUENCE, str(fragment.seq).encode("ascii"))
                    )
                if not fragment.continued:
                    headers.append((_HEADER_FINAL, b""))
                if isinstance(fragment.data, types.NodeRef):
                    body = b""
                    headers.append(
                        (_HEADER_NODE_REF, fragment.data.to_msgpack())
                    )
                else:
                    chunk = fragment.data
                    body = bytes(chunk.data)
                    if chunk.metadata is not None or chunk.ref is not None:
                        descriptor = types.Chunk(
                            data=b"", metadata=chunk.metadata, ref=chunk.ref
                        )
                        headers.append((_HEADER_CHUNK, descriptor.to_msgpack()))
                records.append(_Record(body, tuple(headers)))
        elif operation == "clear":
            records = [
                _Record(
                    b"",
                    (
                        (_HEADER_FORMAT, _CLEAR_FORMAT),
                        (_HEADER_TRANSACTION, transaction.encode("ascii")),
                        (_HEADER_SEQUENCE, str(event["seq"]).encode("ascii")),
                    ),
                )
            ]
        elif operation == "advance":
            records = [
                _Record(
                    b"",
                    (
                        (_HEADER_FORMAT, _ADVANCE_FORMAT),
                        (_HEADER_TRANSACTION, transaction.encode("ascii")),
                        (
                            _HEADER_PREVIOUS,
                            str(event["previous"]).encode("ascii"),
                        ),
                        (_HEADER_CURSOR, str(event["cursor"]).encode("ascii")),
                    ),
                )
            ]
        elif operation == "close":
            records = [
                _Record(
                    msgpack.packb(event["status"], use_bin_type=True),
                    (
                        (_HEADER_FORMAT, _CLOSE_FORMAT),
                        (_HEADER_TRANSACTION, transaction.encode("ascii")),
                    ),
                )
            ]
        else:
            raise AssertionError(operation)
        if len(records) > _MAX_S2_BATCH_RECORDS:
            raise _status(
                StatusCode.RESOURCE_EXHAUSTED,
                "Chunk-store transaction exceeds S2's 1000-record atomic "
                "append limit.",
            )
        metered = sum(
            len(record.body)
            + 8
            + sum(len(name) + len(value) + 4 for name, value in record.headers)
            for record in records
        )
        if metered > _MAX_S2_BATCH_BYTES:
            raise _status(
                StatusCode.RESOURCE_EXHAUSTED,
                "Chunk-store transaction exceeds S2's 1 MiB atomic append "
                "limit.",
            )
        return records, state

    async def _mutate(
        self,
        build: Callable[[str], tuple[dict[str, Any] | None, T]],
        deadline: timing.Time | None = None,
    ) -> T:
        transaction = uuid.uuid4().hex
        async with self._lock:
            await self._sync(deadline)
            while True:
                event, result = build(transaction)
                if event is None:
                    return result
                records, state = self._encode_records(
                    event,
                    transaction,
                    (
                        self._state_tail
                        if event["operation"] in ("clear", "advance")
                        else self._log_tail
                    ),
                )
                expected_tail = self._state_tail if state else self._log_tail
                try:
                    end = await _before_deadline(
                        self._backend.append(records, expected_tail, state),
                        deadline,
                    )
                except _Conflict:
                    await self._sync_stream(state)
                    if transaction in self._transactions:
                        return result
                    continue
                except StatusException:
                    # A timed-out append may be durable even without its ack.
                    await self._sync_stream(state)
                    if transaction in self._transactions:
                        return result
                    raise
                if end != expected_tail + len(records):
                    raise _status(
                        StatusCode.DATA_LOSS,
                        "S2 returned an invalid append acknowledgement.",
                    )
                for index, record in enumerate(records):
                    self._apply_record(record, expected_tail + index, state)
                if state:
                    self._state_tail = end
                else:
                    self._log_tail = end
                return result

    def _fragment(self, seq: int) -> types.NodeFragment:
        data = self._chunks[seq]
        if seq in self._cleared:
            data = types.Chunk(
                data=b"",
                metadata=(
                    data.metadata if isinstance(data, types.Chunk) else None
                ),
                ref="__tombstone__",
            )
        return types.NodeFragment(
            id=self._id,
            data=data,
            seq=seq,
            continued=self._final_seq is None or seq < self._final_seq,
        )

    def _visible(self, fragment: types.NodeFragment) -> types.NodeFragment:
        assert fragment.seq is not None
        if fragment.seq not in self._cleared:
            return fragment
        data = fragment.data
        return types.NodeFragment(
            id=self._id,
            data=types.Chunk(
                data=b"",
                metadata=(
                    data.metadata if isinstance(data, types.Chunk) else None
                ),
                ref="__tombstone__",
            ),
            seq=fragment.seq,
            continued=fragment.continued,
        )

    async def _native_fragment(
        self, position: int, deadline: timing.Time | None
    ) -> types.NodeFragment | None:
        await self._sync_stream(True, deadline)
        await self._ensure(False, deadline)
        tail = await _before_deadline(self._backend.tail(False), deadline)
        if position >= tail:
            return None
        records = await _before_deadline(
            self._backend.read(position, 1, False), deadline
        )
        if not records or records[0][0] != position:
            raise _status(
                StatusCode.DATA_LOSS,
                f"S2 record {position} is missing.",
            )
        fragment = self._decode_chunk(records[0][1], position)
        return self._visible(fragment) if fragment is not None else None

    async def _try_native_claim(
        self, limit: int, deadline: timing.Time | None
    ) -> tuple[list[types.NodeFragment], bool] | None:
        async with self._lock:
            await self._sync_stream(True, deadline)
            await self._ensure(False, deadline)
            previous = self._cursor
            tail = await _before_deadline(self._backend.tail(False), deadline)
            if previous >= tail:
                return None
            records = await _before_deadline(
                self._backend.read(
                    previous, min(limit, tail - previous), False
                ),
                deadline,
            )
            fragments: list[types.NodeFragment] = []
            ended = False
            for physical_seq, record in records:
                if physical_seq != previous + len(fragments):
                    raise _status(
                        StatusCode.DATA_LOSS,
                        f"Expected S2 record {previous + len(fragments)}, got "
                        f"{physical_seq}.",
                    )
                fragment = self._decode_chunk(record, physical_seq)
                if fragment is None:
                    break
                if fragment.seq != physical_seq:
                    return None
                fragments.append(self._visible(fragment))
                if not fragment.continued:
                    ended = True
                    break
            if not fragments:
                return None
            transaction = uuid.uuid4().hex
            event = {
                "operation": "advance",
                "previous": previous,
                "cursor": previous + len(fragments),
            }
            encoded, state = self._encode_records(
                event, transaction, self._state_tail
            )
            assert state
            try:
                end = await _before_deadline(
                    self._backend.append(encoded, self._state_tail, True),
                    deadline,
                )
            except _Conflict:
                await self._sync_stream(True, deadline)
                return [], False
            except StatusException:
                await self._sync_stream(True, deadline)
                if transaction not in self._transactions:
                    raise
                return fragments, ended
            if end != self._state_tail + 1:
                raise _status(
                    StatusCode.DATA_LOSS,
                    "S2 returned an invalid append acknowledgement.",
                )
            self._apply_record(encoded[0], self._state_tail, True)
            self._state_tail = end
            return fragments, ended

    async def _wait_for(
        self,
        lookup: Callable[[], T | None],
        deadline: timing.Time | None,
        missing_message: str,
    ) -> T:
        while True:
            async with self._lock:
                await self._sync(deadline)
                found = lookup()
                if found is not None:
                    return found
                if self._closed_status is not None:
                    if not self._closed_status.is_ok():
                        raise self._closed_status.to_exception()
                    raise _status(StatusCode.NOT_FOUND, missing_message)
            await self._pause(deadline)

    async def _pause(self, deadline: timing.Time | None) -> None:
        if deadline is None or deadline == timing.infinite_future():
            await asyncio.sleep(_POLL_SECONDS)
            return
        remaining_duration = deadline - timing.now()
        if remaining_duration <= timing.zero_duration():
            raise _status(
                StatusCode.DEADLINE_EXCEEDED,
                "Chunk store fragment was not available before the deadline.",
            )
        remaining = remaining_duration.float_seconds(0.0)
        assert remaining is not None
        await asyncio.sleep(min(_POLL_SECONDS, remaining))

    async def get(
        self, seq: int, deadline: timing.Time | None = None
    ) -> types.NodeFragment:
        """Wait for and return a fragment by logical sequence number."""
        value = _unsigned(seq, "seq", _MAX_UINT32)
        async with self._lock:
            fragment = await self._native_fragment(value, deadline)
            if fragment is not None and fragment.seq == value:
                return fragment
        return await self._wait_for(
            lambda: self._fragment(value) if value in self._chunks else None,
            deadline,
            f"Chunk store closed without seq {value}.",
        )

    async def get_by_arrival_order(
        self,
        arrival_order: int,
        deadline: timing.Time | None = None,
    ) -> types.NodeFragment:
        """Wait for a fragment by its zero-based ingestion order."""
        value = _unsigned(arrival_order, "arrival_order", _MAX_UINT64)
        async with self._lock:
            fragment = await self._native_fragment(value, deadline)
            if fragment is not None:
                return fragment
        return await self._wait_for(
            lambda: (
                self._fragment(self._arrivals[value])
                if value < len(self._arrivals)
                else None
            ),
            deadline,
            f"Chunk store closed without arrival order {value}.",
        )

    async def next(
        self, deadline: timing.Time | None = None, limit: int = 1
    ) -> list[types.NodeFragment | None]:
        """Read from and atomically advance the shared logical cursor."""
        maximum = _unsigned(limit, "limit", _MAX_UINT64)
        if maximum == 0:
            raise _status(
                StatusCode.INVALID_ARGUMENT, "limit must be positive."
            )
        collected: list[types.NodeFragment | None] = []
        while True:
            try:
                native = await self._try_native_claim(
                    maximum - len(collected), deadline
                )
            except StatusException as error:
                if (
                    collected
                    and error.status.code is StatusCode.DEADLINE_EXCEEDED
                ):
                    return collected
                raise
            if native is not None:
                fragments, ended = native
                collected.extend(fragments)
                if ended:
                    collected.append(None)
                    return collected
                if len(collected) == maximum:
                    return collected
                if fragments:
                    continue

            def build(transaction: str):
                del transaction
                previous = self._cursor
                sequences = []
                while len(collected) + len(sequences) < maximum:
                    candidate = previous + len(sequences)
                    if candidate > _MAX_UINT32 or candidate not in self._chunks:
                        break
                    sequences.append(candidate)
                fragments = [self._fragment(seq) for seq in sequences]
                event = (
                    {
                        "operation": "advance",
                        "previous": previous,
                        "cursor": previous + len(sequences),
                    }
                    if sequences
                    else None
                )
                ended = previous + len(sequences) > _MAX_UINT32 or (
                    self._final_seq is not None
                    and previous + len(sequences) > self._final_seq
                )
                closed = (
                    self._closed_status is not None
                    and previous + len(sequences) not in self._chunks
                )
                return event, (
                    fragments,
                    ended,
                    closed,
                    self._closed_status,
                )

            try:
                fragments, ended, closed, closed_status = await self._mutate(
                    build, deadline
                )
            except StatusException as error:
                if (
                    collected
                    and error.status.code is StatusCode.DEADLINE_EXCEEDED
                ):
                    return collected
                raise
            collected.extend(fragments)
            if ended or closed:
                if closed_status is not None and not closed_status.is_ok():
                    if collected:
                        return collected
                    raise closed_status.to_exception()
                collected.append(None)
                return collected
            if len(collected) == maximum:
                return collected
            if deadline is not None and deadline != timing.infinite_future():
                remaining_duration = deadline - timing.now()
                if remaining_duration <= timing.zero_duration():
                    if collected:
                        return collected
                    raise _status(
                        StatusCode.DEADLINE_EXCEEDED,
                        "Expected seq was not available before the deadline.",
                    )
            await self._pause(deadline)

    async def put(self, fragment: types.NodeFragment) -> int:
        """Atomically append one fragment and return its sequence number."""
        return (await self.put_many([fragment]))[0]

    async def put_many(
        self, fragments: Sequence[types.NodeFragment]
    ) -> list[int]:
        """Atomically append a batch and return its assigned sequences."""
        try:
            values = list(fragments)
        except Exception as error:
            raise _status(
                StatusCode.INVALID_ARGUMENT,
                f"fragments must be a sequence: {error}",
            ) from None
        any_explicit = False
        all_explicit = True
        explicit: set[int] = set()
        for index, fragment in enumerate(values):
            if not isinstance(fragment, types.NodeFragment):
                raise _status(
                    StatusCode.INVALID_ARGUMENT,
                    f"fragments[{index}] must be a NodeFragment.",
                )
            fragment.validate()
            any_explicit = any_explicit or fragment.seq is not None
            all_explicit = all_explicit and fragment.seq is not None
            if fragment.seq is not None:
                if fragment.seq in explicit:
                    raise _status(
                        StatusCode.INVALID_ARGUMENT,
                        f"Explicit seq {fragment.seq} occurs more than once.",
                    )
                explicit.add(fragment.seq)
        if any_explicit != all_explicit:
            raise _status(
                StatusCode.INVALID_ARGUMENT,
                "Sequence numbers must be set on every fragment or none.",
            )

        def build(transaction: str):
            del transaction
            if self._closed_status is not None:
                raise _status(
                    StatusCode.FAILED_PRECONDITION,
                    f"Chunk store {self._id} is closed for writes.",
                )
            if not values:
                return None, []
            if all_explicit:
                assigned = [fragment.seq for fragment in values]
            else:
                assigned = []
                candidate = len(self._arrivals)
                for _ in values:
                    while (
                        candidate <= _MAX_UINT32 and candidate in self._chunks
                    ):
                        candidate += 1
                    if candidate > _MAX_UINT32:
                        raise _status(
                            StatusCode.RESOURCE_EXHAUSTED,
                            "Maximum implicit sequence number exceeded.",
                        )
                    assigned.append(candidate)
                    candidate += 1
            sequences = [int(seq) for seq in assigned]
            for seq in sequences:
                if seq in self._chunks:
                    raise _status(
                        StatusCode.ALREADY_EXISTS,
                        f"A fragment with seq {seq} already exists.",
                    )
            batch_final = None
            saw_final = False
            for index, fragment in enumerate(values):
                if fragment.continued:
                    if saw_final and not all_explicit:
                        raise _status(
                            StatusCode.INVALID_ARGUMENT,
                            "The final implicit fragment must be last.",
                        )
                    continue
                if saw_final:
                    raise _status(
                        StatusCode.INVALID_ARGUMENT,
                        "More than one fragment in the batch is marked final.",
                    )
                saw_final = True
                batch_final = sequences[index]
            if (
                batch_final is not None
                and self._final_seq is not None
                and batch_final != self._final_seq
            ):
                raise _status(
                    StatusCode.FAILED_PRECONDITION,
                    "The chunk store already has a different final sequence.",
                )
            pending_final = (
                batch_final if batch_final is not None else self._final_seq
            )
            if pending_final is not None:
                if any(seq > pending_final for seq in sequences) or any(
                    seq > pending_final for seq in self._chunks
                ):
                    raise _status(
                        StatusCode.INVALID_ARGUMENT,
                        "A fragment sequence exceeds the final sequence.",
                    )
            stored = [
                types.NodeFragment(
                    data=fragment.data,
                    seq=seq,
                    continued=fragment.continued,
                )
                for fragment, seq in zip(values, sequences, strict=True)
            ]
            return {"operation": "put", "fragments": stored}, sequences

        return await self._mutate(build)

    async def clear_data(self, seq: int) -> types.NodeFragment:
        """Tombstone one payload while retaining ordering metadata."""
        value = _unsigned(seq, "seq", _MAX_UINT32)

        def build(transaction: str):
            del transaction
            if value not in self._chunks:
                raise _status(
                    StatusCode.NOT_FOUND,
                    f"No fragment with seq {value} exists.",
                )
            original = self._fragment(value)
            return {"operation": "clear", "seq": value}, original

        return await self._mutate(build)

    async def get_seq_for_arrival_order(self, arrival_order: int) -> int:
        """Translate a zero-based ingestion position to its sequence number."""
        value = _unsigned(arrival_order, "arrival_order", _MAX_UINT64)
        async with self._lock:
            fragment = await self._native_fragment(value, None)
            if fragment is not None:
                assert fragment.seq is not None
                return fragment.seq
            await self._sync()
            if value >= len(self._arrivals):
                raise _status(
                    StatusCode.NOT_FOUND,
                    f"No fragment has arrival order {value}.",
                )
            return self._arrivals[value]

    async def get_final_seq(self) -> int | None:
        """Return the logical final sequence, if one has been written."""
        async with self._lock:
            await self._sync()
            return self._final_seq

    async def close_writes_with_status(
        self,
        status: Status,
        return_status_if_already_closed: bool = False,
    ) -> Status:
        """Atomically seal writes with a terminal status."""
        if not isinstance(status, Status):
            raise _status(
                StatusCode.INVALID_ARGUMENT,
                "status must be an a11.status.Status.",
            )
        if not isinstance(return_status_if_already_closed, bool):
            raise _status(
                StatusCode.INVALID_ARGUMENT,
                "return_status_if_already_closed must be a boolean.",
            )

        def build(transaction: str):
            del transaction
            if self._closed_status is not None:
                if return_status_if_already_closed:
                    return None, self._closed_status
                raise _status(
                    StatusCode.FAILED_PRECONDITION,
                    "Chunk store is already closed for writes.",
                )
            return {
                "operation": "close",
                "status": status.model_dump(mode="python"),
            }, status

        return await self._mutate(build)

    async def size(self) -> int:
        """Return the number of fragments currently in the store."""
        async with self._lock:
            await self._sync()
            return len(self._chunks)


class S2ChunkStoreFactory:
    """Create S2ChunkStores that share one SDK client and stream prefix."""

    def __init__(
        self,
        basin: str,
        access_token: str | None = None,
        *,
        stream_prefix: str = "a11/chunks/",
        client: object | None = None,
        ensure_stream: bool = True,
    ) -> None:
        if not isinstance(basin, str) or not basin:
            raise _status(
                StatusCode.INVALID_ARGUMENT,
                "basin must be a non-empty string.",
            )
        if not isinstance(stream_prefix, str):
            raise _status(
                StatusCode.INVALID_ARGUMENT,
                "stream_prefix must be a string.",
            )
        if not isinstance(ensure_stream, bool):
            raise _status(
                StatusCode.INVALID_ARGUMENT,
                "ensure_stream must be a boolean.",
            )
        sdk = _load_sdk()
        self._owns_client = client is None
        try:
            if client is None:
                token = access_token or os.environ.get("S2_ACCESS_TOKEN")
                if not token:
                    raise _status(
                        StatusCode.UNAUTHENTICATED,
                        "S2 access token is required; pass access_token or "
                        "set S2_ACCESS_TOKEN.",
                    )
                retry = sdk.Retry(
                    append_retry_policy=sdk.AppendRetryPolicy.NO_SIDE_EFFECTS
                )
                client = sdk.S2(token, retry=retry)
        except StatusException:
            raise
        except Exception as error:
            raise _sdk_status(error) from None
        self._basin = basin
        self._stream_prefix = stream_prefix
        self._client = client
        self._ensure_stream = ensure_stream

    def open(self, node_id: str) -> S2ChunkStore:
        """Open one node's S2-backed fragment log."""
        return S2ChunkStore(
            node_id,
            self._basin,
            stream_prefix=self._stream_prefix,
            client=self._client,
            ensure_stream=self._ensure_stream,
        )

    def __call__(self, node_id: str) -> S2ChunkStore:
        return self.open(node_id)

    async def aclose(self) -> None:
        """Close the SDK client created by this factory."""
        if self._owns_client:
            try:
                await self._client.close()
            except asyncio.CancelledError:
                raise
            except Exception as error:
                raise _sdk_status(error) from None

    async def __aenter__(self) -> "S2ChunkStoreFactory":
        return self

    async def __aexit__(self, *_: object) -> None:
        await self.aclose()


__all__ = ["S2ChunkStore", "S2ChunkStoreFactory"]
