"""A binary-safe, fiber-aware Redis client with asyncio-shaped methods.

`RedisClient` runs hiredis on A11's shared libuv loop. Commands return asyncio
awaitables at the Python boundary while remaining A11 futures in C++, so the
same client can be composed into a
[RedisChunkStore][a11.stores.redis_chunk_store.RedisChunkStore] or reused for
application-specific Redis work. Pub/Sub subscriptions expose broadcast
generations rather than buffering messages, making them suitable for
invalidation and wake-up signals.
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any, Self

from a11 import _native, timing
from a11._native_options import install_native_options
from a11._native_protocol import attach_protocol
from a11.status import Status, StatusCode

from a11._native import RedisClient
from a11._native import RedisClientOptions
from a11._native import RedisReply
from a11._native import RedisReplyType
from a11._native import RedisSubscription

install_native_options(
    RedisClientOptions,
    {
        "host": (str, "127.0.0.1"),
        "port": (int, 6379),
        "username": (str, ""),
        "password": (str, ""),
        "database": (int, 0),
        "client_name": (str, "a11"),
        "connect_timeout": (timing.Duration, timing.Duration.seconds(10)),
        "command_timeout": (timing.Duration, timing.Duration.seconds(10)),
    },
)

_native_default_client = _native.default_redis_client
_native_set_default_client = _native.set_default_redis_client
_native_reset_default_client = _native.reset_default_redis_client

_native_init = RedisClient.__init__
_native_ready = RedisClient.ready
_native_command = RedisClient.command
_native_eval = RedisClient.eval
_native_subscribe = RedisClient.subscribe
_native_wait = RedisSubscription.wait

_MAX_UINT64 = (1 << 64) - 1


def _unsigned(value: object, name: str) -> int:
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or value < 0
        or value > _MAX_UINT64
    ):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"{name} must be an integer between 0 and {_MAX_UINT64}.",
        ).to_exception()
    return value


def default_client() -> RedisClient:
    """Return the process-global environment-configured Redis client."""
    return _native_default_client()


def set_default_client(client: RedisClient) -> None:
    """Replace the process-global client used by new Redis chunk stores."""
    if not isinstance(client, RedisClient):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="client must be a RedisClient.",
        ).to_exception()
    _native_set_default_client(client)


def reset_default_client() -> None:
    """Clear the process-global client so the environment is read again."""
    _native_reset_default_client()


class _RedisClientProtocol:
    """Typed asyncio protocol over the native A11-future methods."""

    def __init__(
        self,
        options: RedisClientOptions | dict[str, Any] | None = None,
    ) -> None:
        """Begin connecting with validated native options.

        A plain mapping is validated into the same bound options object used
        by C++, rather than creating a second Python configuration model.
        """
        if options is None:
            options = RedisClientOptions()
        elif not isinstance(options, RedisClientOptions):
            options = RedisClientOptions.model_validate(options)
        _native_init(self, options)

    @staticmethod
    def create(
        options: RedisClientOptions | dict[str, Any] | None = None,
    ) -> RedisClient:
        """Create a client and begin connecting without blocking."""
        return RedisClient(options)

    async def ready(self) -> None:
        """Wait until command and Pub/Sub connections are initialized."""
        await _native_ready(self)

    async def command(
        self,
        parts: Sequence[str | bytes | bytearray | memoryview],
        deadline: timing.Time | None = None,
    ) -> RedisReply:
        """Execute one binary-safe Redis command."""
        return await _native_command(self, parts, deadline)

    async def eval(
        self,
        script: str | bytes,
        keys: Sequence[str | bytes],
        arguments: Sequence[str | bytes | bytearray | memoryview] = (),
        deadline: timing.Time | None = None,
    ) -> RedisReply:
        """Execute Lua with every cluster-sensitive key declared."""
        return await _native_eval(self, script, keys, arguments, deadline)

    async def subscribe(
        self,
        channel: str,
        deadline: timing.Time | None = None,
    ) -> RedisSubscription:
        """Subscribe and wait for Redis's acknowledgement."""
        return await _native_subscribe(self, channel, deadline)

    async def __aenter__(self) -> Self:
        await self.ready()
        return self

    async def __aexit__(self, exc_type, exc, traceback) -> None:
        del exc_type, exc, traceback
        self.close()


class _RedisSubscriptionProtocol:
    """Typed asyncio protocol for generation waits."""

    async def wait(
        self,
        after: int,
        deadline: timing.Time | None = None,
    ) -> int:
        """Wait until a message advances the generation beyond ``after``."""
        return await _native_wait(self, _unsigned(after, "after"), deadline)


attach_protocol(RedisClient, _RedisClientProtocol)
attach_protocol(RedisSubscription, _RedisSubscriptionProtocol)

for _class in (
    RedisClient,
    RedisClientOptions,
    RedisReply,
    RedisReplyType,
    RedisSubscription,
):
    _class.__module__ = __name__


__all__ = [
    "RedisClient",
    "RedisClientOptions",
    "RedisReply",
    "RedisReplyType",
    "RedisSubscription",
    "default_client",
    "reset_default_client",
    "set_default_client",
]
