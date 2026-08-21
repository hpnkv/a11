# Sessions

A [`Session`][a11.service.session.Session] is the connection-scoped runtime:
it multiplexes [wire streams](net.md), dispatches inbound
[action](actions.md) calls, and manages their lifetimes. It is the object you
build a server or client agent around.

## Session

Attach a transport with [`add_stream`][a11.service.session.Session.add_stream]
and route outbound messages with [`send`][a11.service.session.Session.send].
[`half_close`][a11.service.session.Session.half_close] begins an orderly
shutdown; await [`done`][a11.service.session.Session.done] when every stream and
dispatched action must have released its state. Use
[`abort`][a11.service.session.Session.abort] for a failed connection so the
peer receives a structured reason.

::: a11.service.session.Session

## SessionWithRecv

[`receive`][a11.service.session.SessionWithRecv.receive] is the pull-style flow
for one transport, while
[`receive_with_stream_id`][a11.service.session.SessionWithRecv.receive_with_stream_id]
retains the source when a gateway multiplexes clients.

::: a11.service.session.SessionWithRecv

## SessionOptions

::: a11.service.session.SessionOptions

## Service

A [`Service`][a11.service.service.Service] is the action registry plus the
sessions serving it. [`accept`][a11.service.service.Service.accept] is shaped to
be a transport's on-stream callback, which is what lets one service stand behind
several listeners — or behind none, when the stream is in-process.
[`aclose`][a11.service.service.Service.aclose] is the graceful shutdown: stop
accepting, then wait for what is in flight.

::: a11.service.service.Service

::: a11.service.service.ServiceOptions

## Serving

A service owns no sockets, so binding it to
listeners is a separate sentence:
[`serving`][a11.service.serving.serving] takes a service and any number of
listener factories, yields the live listeners, and on the way out stops them in
reverse before draining the service — so nothing new arrives while it is
finishing what it has.

```python
async with serving(service, websocket(ws_options), http_sse("0.0.0.0", 8012)):
    await stop.wait()
```

One service behind every listener is the point: a caller's session, the registry
it dispatches against and the concurrency it shares are the same whichever
endpoint it arrived on.

::: a11.service.serving.serving

::: a11.service.serving.websocket

::: a11.service.serving.http_sse

::: a11.service.serving.webrtc

## `a11 serve`

The command form of the above: name a module holding an
[`ActionRegistry`][a11.actions.registry.ActionRegistry] and it is served on the
transports you ask for.

```sh
a11 serve mypkg.actions                       # REGISTRY over WebSocket
a11 serve mypkg.actions:TOOLS --ws --sse      # a named registry, two endpoints
a11 serve ./examples/demo/main.py             # a file, nothing installed
a11 serve mypkg.actions --webrtc \
    --webrtc-signalling-server wss://a11.services/ice \
    --webrtc-signalling-identity demoserver
```

The target names a module either as an import path (`pkg.subpkg.module`) or as a
path to a `.py` file, with an optional `:SYMBOL` defaulting to `REGISTRY`. A
`.py` suffix, a path separator, a leading `.`/`~`, or a file that is simply
there means a path; anything else is imported the ordinary way. A file is loaded
under its own stem rather than as `__main__`, so its `if __name__ ==
"__main__":` block stays asleep, and its directory joins `sys.path` so sibling
imports resolve. Each transport has its own flag group (`--ws-host`, `--sse-port`,
`--webrtc-signalling-server`, …); `--h11`/`--h2c`/`--h2` and `--cert`/`--privkey`
are shared by every HTTP-based endpoint. HTTP/1.1 is the default, because that
is what an RFC 6455 WebSocket client speaks; SSE runs over it too, spending a
second connection on its outbound direction because an HTTP/1.1 connection
carries one request and the event stream has it. `SIGINT` and `SIGTERM` are
clean shutdowns.

::: a11.cli.commands.serve
