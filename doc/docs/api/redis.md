# Redis

The reusable Redis layer drives hiredis from A11's libuv loop and presents
binary-safe commands as native futures and Python awaitables. It maintains a
multiplexed command connection and a separate Pub/Sub connection, so waiting
for chunk-store changes does not block ordinary commands.

`RedisChunkStore` maps A11's ordered chunks, cursors, and terminal status onto
Redis Streams, hashes, Lua transactions, and Pub/Sub. Persistence and
multi-process access therefore use a widely deployed database and its existing
operational tools. A11 does not add a separate storage service. Redis
replication and scaling behavior still apply, including the Cluster routing
constraints below.

Compose a client explicitly into a
[`RedisChunkStore`][a11.stores.redis_chunk_store.RedisChunkStore] when the
application owns connection policy:

```python
from a11.redis import RedisClient
from a11.stores.redis_chunk_store import RedisChunkStore

client = RedisClient({"host": "redis.internal", "port": 6379})
await client.ready()
store = RedisChunkStore("agent-output", client=client)
await store.initialize()
```

Omitting `client` uses the process-global client. The global is created lazily
from the environment and can be replaced for dependency injection with
[`set_default_client`][a11.redis.client.set_default_client]. Existing stores
retain the client they were constructed with; replacing the global affects
only subsequently created stores.

## Configuration

`A11_REDIS_URL` takes precedence over all individual connection variables. Its
supported form is `redis://[user:password@]host[:port][/database]`; credentials
and hosts may use percent encoding, and IPv6 hosts use brackets.

| Variable | Default | Meaning |
| --- | --- | --- |
| `A11_REDIS_URL` | unset | Complete connection URL; overrides the variables below. |
| `A11_REDIS_HOST` | `127.0.0.1` | Redis host or routing endpoint. |
| `A11_REDIS_PORT` | `6379` | TCP port. |
| `A11_REDIS_USERNAME` | empty | ACL username. |
| `A11_REDIS_PASSWORD` | empty | Password, with or without an ACL username. |
| `A11_REDIS_DB` | `0` | Logical database selected on both connections. |
| `A11_REDIS_CLIENT_NAME` | `a11` | Base name reported by `CLIENT SETNAME`. |
| `A11_REDIS_CONNECT_TIMEOUT_MS` | `10000` | Finite connection timeout in milliseconds. |
| `A11_REDIS_COMMAND_TIMEOUT_MS` | `10000` | Default command and subscription-ack timeout. |

Chunk-store layout has two additional settings:

| Variable | Default | Meaning |
| --- | --- | --- |
| `A11_REDIS_CHUNK_STORE_KEY_PREFIX` | `a11:` | Prefix placed before each per-node hash tag. Braces are rejected. |
| `A11_REDIS_CHUNK_STORE_INLINE_DATA_THRESHOLD_BYTES` | `262144` | Raw `Chunk.data` size above which the encoded chunk is stored in the blob hash. |

An explicit `RedisClientOptions` or `RedisChunkStoreOptions` object takes the
place of its corresponding environment configuration.

!!! note "TLS and Redis Cluster endpoints"

    Native TLS is not enabled yet: `rediss://` is rejected. Use a trusted
    TLS-terminating Redis proxy or tunnel and configure its plain Redis
    endpoint for A11.

    Every key touched by one chunk-store script shares a Redis Cluster hash
    tag, so atomic Lua operations are cross-slot safe. The client does not yet
    follow `MOVED` or `ASK` redirects. For a sharded deployment, configure a
    cluster-aware proxy/routing endpoint, or connect only to the node that owns
    the relevant slot. Redis Cluster deployments must use database `0`.

## Chunk-store layout and atomicity

For node ID `agent-output`, the default base is
`a11:{chunk-store:agent-output}`. A store owns six names under that base:

| Suffix | Redis role |
| --- | --- |
| `:metadata` | Hash with ID, closure status, final sequence, size, cursors, and revision. |
| `:stream` | Redis Stream containing ordered chunk records and control history. |
| `:sequences` | Sequence-to-stream-entry index. |
| `:arrivals` | Arrival-order-to-sequence index. |
| `:blobs` | Encoded chunks whose raw data exceeds the inline threshold. |
| `:events` | Pub/Sub invalidation channel used by responsive getters. |

`Put`, `PutMany`, closure, and data clearing each run as one Lua state-machine
operation. Validation happens before mutation, so a rejected batch cannot
leave partial indexes, blobs, metadata, or stream entries. Getters perform an
atomic lookup, subscribe only if they must wait, and then recheck after the
subscription acknowledgement; this closes the notification race with a
concurrent write or close.

Stream records carry a storage kind. `inline` embeds the encoded chunk,
`redis` references the separate blob hash, and `tombstone` preserves metadata
after `clear_data`. The reserved `s3` kind makes the format forward-compatible
with an S3-compatible blob backend; reading it currently returns an
`UNIMPLEMENTED` status.

## General-purpose client

Commands are binary safe: Python accepts strings or bytes-like arguments and
returns an owned [`RedisReply`][a11.redis.client.RedisReply]. Avoid blocking
commands such as `XREAD BLOCK` and `BLPOP` on the multiplexed command
connection. For wake-ups, use a subscription and ordinary non-blocking state
queries, as `RedisChunkStore` does. A command's effective deadline is the
earlier of its explicit absolute deadline and `command_timeout`.

::: a11.redis.client.RedisClient

::: a11.redis.client.RedisClientOptions

::: a11.redis.client.RedisReply

::: a11.redis.client.RedisReplyType

::: a11.redis.client.RedisSubscription

::: a11.redis.client.default_client

::: a11.redis.client.set_default_client

::: a11.redis.client.reset_default_client
