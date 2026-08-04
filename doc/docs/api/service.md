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
