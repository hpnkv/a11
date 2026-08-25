# Copyright 2026 The A11 Authors.

"""A Python A11 echo peer, for checking that a folded TypeScript frame is read.

The TypeScript sender folds queued wire messages together, so a Python peer now
receives frames carrying several fragments where it used to receive one frame
each. Nothing in the format changed -- a WireMessage has always been a batch --
but "has always been allowed" and "is actually handled" are different claims and
only one of them is testable.

Echoes one fragment per arriving fragment, which is the credit contract
`bench/peer.py` settled on for exactly this reason: counting messages through a
fold reports a stall that is not one.
"""

import asyncio
import json
import sys

import a11
from a11 import net
from a11.data import types
from a11.net import WireStreamWithRecv


async def main() -> None:
    transport = sys.argv[1]
    loop = asyncio.get_running_loop()
    held: list[object] = []
    tasks: set[asyncio.Task] = set()
    seen = {"frames": 0, "fragments": 0}

    async def echo_loop(endpoint) -> None:
        try:
            while True:
                message = await endpoint.receive()
                if message is None:
                    break
                fragments = list(message.node_fragments or ())
                seen["frames"] += 1
                seen["fragments"] += len(fragments)
                endpoint.send(
                    types.WireMessage(
                        node_fragments=[
                            types.NodeFragment(
                                id="echo",
                                data=types.Chunk(
                                    data=bytes(fragment.data.data),
                                    metadata=fragment.data.metadata,
                                ),
                            )
                            for fragment in fragments
                        ]
                    )
                )
        except Exception:  # noqa: BLE001 - a closed peer ends the loop
            pass

    async def on_stream(stream) -> None:
        endpoint = WireStreamWithRecv(stream)
        await endpoint.accept()
        held.append(endpoint)
        task = asyncio.ensure_future(echo_loop(endpoint))
        tasks.add(task)
        task.add_done_callback(tasks.discard)

    if transport == "websocket":
        options = net.WebSocketServerOptions()
        options.path = "/xlang"
        options.bind_address = "127.0.0.1"
        options.port = 0
        # The gateway client mirrors this; HTTP/1.1 both ends or the handshake
        # negotiates past each other.
        options.http2_options.enable_h2 = False
        options.http2_options.enable_h2c = False
        server = net.WebSocketWireServer.create(on_stream, options)
    else:
        server = net.HttpSseServer.create(
            "127.0.0.1", 0, on_stream, net.HttpSseOptions()
        )

    print(json.dumps({"port": server.port}), flush=True)
    # Held open until the harness closes stdin.
    await loop.run_in_executor(None, sys.stdin.readline)
    print(json.dumps({"observed": seen}), flush=True)


if __name__ == "__main__":
    asyncio.run(main())
