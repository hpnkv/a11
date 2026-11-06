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

"""Local object chunks cross Flow and action ports without codec calls."""

import json
from dataclasses import dataclass, field

import pytest

from a11 import flow
from a11.actions import ActionRegistry, ActionSchema
from a11.data.serialization import (
    SerializationRegistry,
    get_global_serialization_registry,
    set_global_serialization_registry,
)
from a11.nodes.async_node import AsyncNode
from a11.status import StatusException
from a11.stores.local_chunk_store import LocalChunkStore


@dataclass
class LocalValue:
    items: list[int] = field(default_factory=list)


@pytest.fixture
def local_registry():
    old = get_global_serialization_registry()
    registry = SerializationRegistry(register_defaults=True)
    registry.set_type_tag(LocalValue, "test.LocalValue")
    calls = {"encode": 0, "decode": 0}

    def encode(value: LocalValue) -> bytes:
        calls["encode"] += 1
        if value.items == [-1]:
            raise ValueError("cannot encode this value")
        return json.dumps(value.items).encode()

    def decode(data: bytes, obj_type: type[LocalValue]) -> LocalValue:
        calls["decode"] += 1
        return obj_type(json.loads(data))

    registry.register(
        LocalValue,
        "application/x-local-value",
        encode,
        decode,
        local_passthrough=True,
    )
    set_global_serialization_registry(registry)
    try:
        yield registry, calls
    finally:
        set_global_serialization_registry(old)


@pytest.mark.asyncio
async def test_typed_flow_branch_replay_and_action_input_skip_codecs(
    local_registry,
):
    _, calls = local_registry
    received: list[LocalValue] = []

    async def inspect(action):
        value = await action["value"].consume(LocalValue)
        received.append(value)
        await action["count"].finalize(len(value.items))

    actions = ActionRegistry()
    actions.register(
        "inspect-local",
        ActionSchema.model_validate(
            {
                "name": "inspect-local",
                "inputs": {"value": {"type": LocalValue, "unary": True}},
                "outputs": {"count": {"type": int, "unary": True}},
            }
        ),
        inspect,
    )
    program = flow.loads("""flow main {
          in value: test::LocalValue required
          out counts: int stream
          for i in [1, 2] | flatten {
            if true {
              seen = run inspect-local(value: value)
              seen.count -> counts
            }
          }
        }""")
    program.register_all(actions)
    assert await program.main.invoke(
        value=LocalValue([2, 3]), registry=actions
    ) == {"counts": [2, 2]}
    assert received == [LocalValue([2, 3]), LocalValue([2, 3])]
    assert received[0] is not received[1]
    assert calls == {"encode": 0, "decode": 0}


@pytest.mark.asyncio
async def test_mutable_python_value_is_snapshotted_for_replay(local_registry):
    registry, calls = local_registry
    node = AsyncNode(
        LocalChunkStore("local-snapshot"), serialization_registry=registry
    )
    original = LocalValue([4])
    assert await (await node.put(original, final=True)) == 0
    original.items.append(5)
    first = await node.next_object(LocalValue)
    assert first == LocalValue([4])
    first.items.append(6)
    node.reset_reader()
    second = await node.next_object(LocalValue)
    assert second == LocalValue([4])
    assert calls == {"encode": 0, "decode": 0}


@pytest.mark.asyncio
async def test_incompatible_reader_materializes_and_conversion_failure_surfaces(
    local_registry,
):
    registry, calls = local_registry
    node = AsyncNode(
        LocalChunkStore("local-incompatible"), serialization_registry=registry
    )
    assert await (await node.put(LocalValue([8]), final=True)) == 0
    with pytest.raises(StatusException):
        await node.next_object(dict)
    assert calls == {"encode": 1, "decode": 0}

    failing = AsyncNode(
        LocalChunkStore("local-failing"), serialization_registry=registry
    )
    assert await (await failing.put(LocalValue([-1]), final=True)) == 0
    with pytest.raises(StatusException, match="cannot encode this value"):
        await failing.next_object(dict)
    assert calls["encode"] == 2


@pytest.mark.asyncio
async def test_raw_chunk_reader_gets_bytes(local_registry):
    registry, calls = local_registry
    node = AsyncNode(
        LocalChunkStore("local-raw"), serialization_registry=registry
    )
    assert await (await node.put(LocalValue([9]), final=True)) == 0
    chunk = await node.next_chunk()
    assert chunk is not None
    assert chunk.data == b"[9]"
    assert calls == {"encode": 1, "decode": 0}


@pytest.mark.asyncio
async def test_incompatible_action_input_reports_failure_to_try(local_registry):
    _, calls = local_registry

    async def expect_number(action):
        await action["number"].consume(int)

    actions = ActionRegistry()
    actions.register(
        "expect-number",
        ActionSchema.model_validate(
            {
                "name": "expect-number",
                "inputs": {"number": {"type": int, "unary": True}},
                "outputs": {},
            }
        ),
        expect_number,
    )
    program = flow.loads("""flow main {
          in value: test::LocalValue required
          out ok: bool
          bad = try run expect-number(number: value)
          bad.status.ok -> ok
        }""")
    program.register_all(actions)
    assert await program.main.invoke(
        value=LocalValue([1]), registry=actions
    ) == {"ok": False}
    assert calls == {"encode": 1, "decode": 0}
