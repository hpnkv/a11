# A WebSocket echo session

A [`Session`][a11.service.session.Session] is A11's connection-scoped runtime:
hand it a [`WireStream`][a11.net.wire_stream.WireStream] transport and it
multiplexes streams and routes messages for you. This page stands up a WebSocket
server that echoes every message, then connects a client to it — all with

```python
import a11
```

A wire stream delivers messages with **no ordering guarantee**, but it is
**synchronised on closure**: a reader sees every delivered message before the
stream reports done. The session takes care of that lifecycle.

## The echo handler

A server session dispatches each inbound message to a callback. Ours echoes the
message straight back, and treats a `None` message — the peer's half-close — as
the signal to wind the stream down:

```python
async def echo(message, stream, session):
    if message is None:            # peer half-closed
        stream.half_close()
        return
    session.send(message)          # bounce it back
```

## Stand up the server

`WebSocketWireServer.create` starts listening. Each accepted connection gets its
own `Session` wired to the echo handler; the accept handler keeps the session
alive until it is done:

```python
async def accept(stream):
    session = a11.Session(on_stream_message=echo)
    await session.add_stream(stream, mode="accept")
    await session.done.wait()


options = a11.WebSocketServerOptions()
options.path = "/ws"
server = a11.WebSocketWireServer.create(accept, options)
```

`server.port` is the port it bound to — handy when you let the OS choose one.

## Connect a client

The client side uses a [`SessionWithRecv`][a11.service.session.SessionWithRecv]
so it can *pull* replies with `receive()`. Connect a stream, then attach it in
`"start"` mode (the client initiates; the server `"accept"`s):

```python
session = a11.SessionWithRecv()
stream = a11.WebSocketWireStream.connect(f"ws://127.0.0.1:{server.port}/ws")
await session.add_stream(stream, mode="start")
```

## Send and receive

Messages are [`WireMessage`][a11.data.types.WireMessage] values carrying one or
more fragments. `a11.to_chunk` turns a Python value into the chunk a fragment
holds:

```python
message = a11.WireMessage(
    node_fragments=[a11.NodeFragment(id="text", data=a11.to_chunk("hello"))]
)
session.send(message)
echoed = await session.receive()
print(echoed.debug_string())
```

## Shut down cleanly

Half-close to say "no more messages from me", wait for the session to drain,
then stop the server:

```python
session.half_close()
await session.done.wait()
server.stop()
```

## Putting it together

```python
import asyncio
import a11


async def echo(message, stream, session):
    if message is None:
        stream.half_close()
        return
    session.send(message)


async def accept(stream):
    session = a11.Session(on_stream_message=echo)
    await session.add_stream(stream, mode="accept")
    await session.done.wait()


async def main() -> None:
    options = a11.WebSocketServerOptions()
    options.path = "/ws"
    server = a11.WebSocketWireServer.create(accept, options)
    try:
        session = a11.SessionWithRecv()
        stream = a11.WebSocketWireStream.connect(
            f"ws://127.0.0.1:{server.port}/ws"
        )
        await session.add_stream(stream, mode="start")

        message = a11.WireMessage(
            node_fragments=[
                a11.NodeFragment(id="text", data=a11.to_chunk("hello"))
            ]
        )
        session.send(message)
        echoed = await session.receive()
        print(echoed.debug_string())

        session.half_close()
        await session.done.wait()
    finally:
        server.stop()


asyncio.run(main())
```

The full runnable version — with an interactive prompt and a server that logs
each step — lives in `examples/000-websocket-echo`. To take the same session
peer-to-peer, swap
[`WebSocketWireStream`][a11.net.websocket_wire_stream.WebSocketWireStream] for
[`WebRtcWireStream`][a11.net.webrtc_wire_stream.WebRtcWireStream]; the session
code does not change.

Next: [talk to a model](llm.md).
