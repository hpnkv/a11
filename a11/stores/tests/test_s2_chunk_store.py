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

import asyncio
import builtins

import pytest

from a11.data.types import Chunk, NodeFragment, NodeRef
from a11.status import Status, StatusCode, StatusException
from a11.stores.s2_chunk_store import S2ChunkStore
from a11.stores.s2_chunk_store import _sdk_status
from a11.stores.tests.test_chunk_store import _MemoryS2Backend


def _fragment(value, seq=None):
    return NodeFragment(data=Chunk(data=value), seq=seq, continued=True)


def test_public_surface_is_the_sdk_namespace():
    import a11

    assert a11.sdk.S2ChunkStore is S2ChunkStore
    assert not hasattr(a11, "S2ChunkStore")


@pytest.mark.parametrize(
    ("s2_code", "http_status", "expected"),
    [
        ("request_timeout", 408, StatusCode.DEADLINE_EXCEEDED),
        ("quota_exhausted", 403, StatusCode.RESOURCE_EXHAUSTED),
        ("access_token_not_found", 404, StatusCode.UNAUTHENTICATED),
        ("transaction_conflict", 409, StatusCode.ABORTED),
        ("unavailable", 503, StatusCode.UNAVAILABLE),
    ],
)
def test_s2_errors_map_to_public_statuses(s2_code, http_status, expected):
    class S2Failure(Exception):
        code = s2_code
        status_code = http_status

    assert _sdk_status(S2Failure("failed")).status.code is expected


@pytest.mark.asyncio
async def test_two_instances_serialize_implicit_writes_and_shared_next():
    records = []
    first = S2ChunkStore(
        "shared", "test-basin", _backend=_MemoryS2Backend(records)
    )
    second = S2ChunkStore(
        "shared", "test-basin", _backend=_MemoryS2Backend(records)
    )

    seqs = await asyncio.gather(
        first.put(_fragment("first")), second.put(_fragment("second"))
    )
    assert sorted(seqs) == [0, 1]

    batches = await asyncio.gather(first.next(), second.next())
    assert sorted(batch[0].seq for batch in batches) == [0, 1]


class _LostAckBackend(_MemoryS2Backend):
    def __init__(self, records):
        super().__init__(records)
        self.lose_ack = True

    async def append(self, body, expected_tail):
        end = await super().append(body, expected_tail)
        if self.lose_ack:
            self.lose_ack = False
            raise Status(
                code=StatusCode.UNAVAILABLE,
                message="acknowledgement lost",
            ).to_exception()
        return end


@pytest.mark.asyncio
async def test_lost_append_ack_does_not_duplicate_transaction():
    records = []
    store = S2ChunkStore(
        "lost-ack", "test-basin", _backend=_LostAckBackend(records)
    )

    assert await store.put(_fragment("once")) == 0
    assert await store.size() == 1
    assert len(records) == 1


@pytest.mark.asyncio
async def test_node_ref_round_trips_and_can_be_tombstoned():
    store = S2ChunkStore("refs", "test-basin", _backend=_MemoryS2Backend([]))
    reference = NodeRef(id="target", offset=3, length=4)
    await store.put(NodeFragment(data=reference, seq=0, continued=False))

    assert (await store.get(0)).get_node_ref() == reference
    assert (await store.clear_data(0)).get_node_ref() == reference
    assert (await store.get(0)).get_chunk().ref == "__tombstone__"


@pytest.mark.asyncio
async def test_oversized_atomic_transaction_is_rejected_without_a_record():
    records = []
    store = S2ChunkStore(
        "large", "test-basin", _backend=_MemoryS2Backend(records)
    )

    with pytest.raises(StatusException) as raised:
        await store.put(_fragment(b"x" * (1024 * 1024)))
    assert raised.value.status.code is StatusCode.RESOURCE_EXHAUSTED
    assert records == []


@pytest.mark.asyncio
async def test_backs_native_async_node_reader_and_writer():
    import a11

    streams = {}

    def factory(node_id):
        records = streams.setdefault(node_id, [])
        return S2ChunkStore(
            node_id,
            "test-basin",
            _backend=_MemoryS2Backend(records),
        )

    node = a11.AsyncNode.create("stream", chunk_store_factory=factory)

    async def produce():
        await node.put(b"first")
        await node.finalize(b"last", wait=True)

    received = []

    async def consume():
        async for chunk in node:
            received.append(bytes(chunk))

    await asyncio.gather(produce(), consume())
    assert received == [b"first", b"last"]
    assert await factory("stream").get_final_seq() == 1


def test_missing_sdk_is_reported_only_when_s2_is_constructed(monkeypatch):
    real_import = builtins.__import__

    def missing(name, *args, **kwargs):
        if name == "s2_sdk":
            error = ModuleNotFoundError("No module named 's2_sdk'")
            error.name = "s2_sdk"
            raise error
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr("importlib.import_module", missing)
    with pytest.raises(StatusException) as raised:
        S2ChunkStore("missing", "test-basin", access_token="unused")
    assert raised.value.status.code is StatusCode.UNIMPLEMENTED
    assert "a11-kit[s2]" in raised.value.status.message
