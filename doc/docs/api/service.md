# Sessions

A [`Session`][a11.service.session.Session] is the connection-scoped runtime:
it multiplexes [wire streams](net.md), dispatches inbound
[action](actions.md) calls, and manages their lifetimes. It is the object you
build a server or client agent around.

## Session

::: a11.service.session.Session

## SessionWithRecv

::: a11.service.session.SessionWithRecv

## SessionOptions

::: a11.service.session.SessionOptions
