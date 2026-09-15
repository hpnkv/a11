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

"""Stream a two-sentence message through an S2-backed AsyncNode.

Environment variables:
    S2_ACCESS_TOKEN: Required S2 access token.
    S2_BASIN: Destination basin. Defaults to ``test-basin-for-ae``.
    S2_NODE_ID: Node and stream suffix. Defaults to a unique
        ``hello-world-<8 hex characters>`` value.
"""

import asyncio
import os
import uuid

import a11
from pydantic import BaseModel


class User(BaseModel):
    name: str
    age: int


async def main() -> None:
    basin = os.environ.get("S2_BASIN", "test-basin-for-ae")
    node_id = os.environ.get(
        "S2_NODE_ID", f"hello-world-{uuid.uuid4().hex[:8]}"
    )
    store = a11.sdk.S2ChunkStore(node_id, basin)
    node = a11.AsyncNode(store)
    pieces = [
        "Hello ",
        "world. ",
        "A11 is streaming ",
        "this message through S2.",
        " This is a piece of structured data: ",
        User(name="Alice", age=30),
        ".",
    ]

    async def _write():
        for piece in pieces[:-1]:
            await node.put(piece)
        await node.finalize(pieces[-1], wait=True)

    async def _read():
        async for piece in node:
            print(piece, end="", flush=True)
        print()

    try:
        # The writer and reader run simultaneously, so pieces stream through
        # S2 without either side waiting for the whole message.
        _, received = await asyncio.gather(_write(), _read())
        print(f"Stored in s2://{basin}/{store.stream_name}")
    finally:
        await store.aclose()


if __name__ == "__main__":
    asyncio.run(main())
