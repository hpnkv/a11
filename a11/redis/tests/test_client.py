import asyncio
import os
import uuid

import pytest
import pytest_asyncio

from a11 import timing
from a11.redis.client import (
    RedisClient,
    RedisClientOptions,
    RedisReplyType,
    set_default_client,
)
from a11.status import StatusCode, StatusException


@pytest_asyncio.fixture
async def redis_client():
    configured_url = os.environ.get("A11_TEST_REDIS_URL")
    options = (
        RedisClientOptions.from_url(configured_url)
        if configured_url
        else RedisClientOptions()
    )
    options.connect_timeout = timing.Duration.milliseconds(250)
    options.command_timeout = timing.Duration.seconds(2)
    client = RedisClient(options)
    try:
        await client.ready()
    except StatusException as error:
        client.close()
        if configured_url:
            raise
        pytest.skip(f"Redis is unavailable: {error}")
    try:
        yield client
    finally:
        client.close()


@pytest.mark.asyncio
async def test_binary_commands_and_broadcast_subscription(redis_client):
    key = f"a11:test:python-client:{uuid.uuid4().hex}"
    channel = f"{key}:events"
    payload = b"a\x00b\xff"

    set_reply = await redis_client.command(["SET", key, payload])
    assert set_reply.type == RedisReplyType.STRING
    assert bytes(set_reply) == b"OK"
    assert (await redis_client.command(["GET", key])).as_bytes() == payload

    subscription = await redis_client.subscribe(channel)
    generation = subscription.generation
    published = await redis_client.command(["PUBLISH", channel, payload])
    assert published.as_integer() >= 1
    assert (
        await subscription.wait(
            generation, timing.now() + timing.Duration.seconds(2)
        )
        > generation
    )

    await redis_client.command(["DEL", key])


@pytest.mark.asyncio
async def test_invalid_command_parts_raise_a11_status(redis_client):
    with pytest.raises(StatusException):
        await redis_client.command(["GET", object()])


def test_options_accept_mappings_and_invalid_values_use_status_boundary():
    options = RedisClientOptions.model_validate(
        {"host": "localhost", "client_name": "mapping-test"}
    )
    assert options.host == "localhost"
    assert options.client_name == "mapping-test"
    assert options.model_json_schema()["properties"]["host"]["type"] == "string"

    with pytest.raises(StatusException) as raised:
        RedisClientOptions.model_validate({"port": 0})
    assert raised.value.status.code is StatusCode.INVALID_ARGUMENT
    with pytest.raises(StatusException) as raised:
        RedisClientOptions.model_validate({"unknown_option": True})
    assert raised.value.status.code is StatusCode.INVALID_ARGUMENT
    with pytest.raises(StatusException) as raised:
        RedisClient(object())
    assert raised.value.status.code is StatusCode.INVALID_ARGUMENT
    with pytest.raises(StatusException) as raised:
        set_default_client(object())
    assert raised.value.status.code is StatusCode.INVALID_ARGUMENT


@pytest.mark.asyncio
async def test_subscription_cancellation_does_not_break_later_waits(
    redis_client,
):
    channel = f"a11:test:python-client:cancel:{uuid.uuid4().hex}"
    subscription = await redis_client.subscribe(channel)
    generation = subscription.generation
    with pytest.raises(StatusException) as raised:
        await subscription.wait(-1)
    assert raised.value.status.code is StatusCode.INVALID_ARGUMENT
    waiting = asyncio.create_task(subscription.wait(generation))
    await asyncio.sleep(0)
    waiting.cancel()
    with pytest.raises(asyncio.CancelledError):
        await waiting

    later = asyncio.create_task(subscription.wait(generation))
    await redis_client.command(["PUBLISH", channel, "wake"])
    assert await later > generation


@pytest.mark.asyncio
async def test_invalid_subscription_channel_raises_a11_status(redis_client):
    with pytest.raises(StatusException) as raised:
        await redis_client.subscribe(object())
    assert raised.value.status.code is StatusCode.INVALID_ARGUMENT
