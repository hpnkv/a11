# Sessions and Services

A [`Session`][a11.service.session.Session] manages connection-scoped state:
multiplexing [wire streams](net.md), dispatching inbound [actions](actions.md), and
tracking streaming node fragments.

A [`Service`][a11.service.service.Service] manages an action registry across multiple
sessions, allowing one service instance to power multiple listeners and protocols.

## Using Session and Service

```python
import a11
from a11.actions import ActionRegistry
from a11.service.serving import serving, websocket

# 1. Register application actions
registry = ActionRegistry()

@registry.action(name="greet")
async def greet(name: str) -> str:
    return f"Hello, {name}!"

# 2. Create the service with the registry
service = a11.Service(action_registry=registry)

# 3. Serve over listeners with automatic graceful shutdown
async def run_server():
    async with serving(service, websocket(port=8080)):
        print("Server running on ws://localhost:8080")
        await asyncio.Event().wait()
```

::: a11.service.session.Session

## SessionWithRecv

[`SessionWithRecv`][a11.service.session.SessionWithRecv] provides explicit pull-based
message reception for applications managing custom event loops or multiplexed transports:

::: a11.service.session.SessionWithRecv

::: a11.service.session.SessionOptions

## Service

::: a11.service.service.Service

::: a11.service.service.ServiceOptions

## Serving

[`serving`][a11.service.serving.serving] binds a service to multiple transport listeners,
yielding the active listeners and performing ordered teardown upon exit:

```python
async with serving(service, websocket(ws_options), http_sse("0.0.0.0", 8012)):
    await shutdown_event.wait()
```

::: a11.service.serving.serving

::: a11.service.serving.websocket

::: a11.service.serving.http_sse

::: a11.service.serving.webrtc

## CLI: `a11 serve`

The CLI command serves an action module over configured transports:

```sh
a11 serve mypkg.actions                       # REGISTRY over WebSocket
a11 serve mypkg.actions:TOOLS --ws --sse      # a named registry, two endpoints
a11 serve ./examples/demo/main.py             # a file, nothing installed
a11 serve mypkg.actions --webrtc \
    --webrtc-signalling-server wss://a11.services/ice \
    --webrtc-signalling-identity demoserver
a11 serve mypkg.app:SERVICE --ws --hosted demoserver   # a Service, on the exchange
```

The symbol may be a `Service` as well as an `ActionRegistry`, which is how a
backend that specialises each connection -- a registry copy per caller, a
reverse-dispatch bridge bound to the session -- is served by this command rather
than by a loop of its own.
The
[`a11.demos.web_demos_server`](https://github.com/hpnkv/a11/blob/main/a11/demos/web_demos_server.py)
module is a complete example. With `--hosted`, its actions are also available
through the exchange at `a11.to/ui`, without an inbound port.

::: a11.cli.commands.serve
