# Copyright 2026 The A11 Authors

import asyncio
import time

import pytest

import a11
from a11.client.connection import GatewayConnection
from a11.net.in_process_wire_stream import InProcessWireStream
from a11.service.session import Session
from a11.status import StatusException


def _envelope() -> a11.AuthorizationEnvelope:
    return a11.AuthorizationEnvelope(("header.payload.signature",))


def test_authorization_envelope_has_one_canonical_binary_encoding():
    envelope = _envelope()
    encoded = a11.encode_authorization(envelope)

    assert a11.decode_authorization(encoded) == envelope
    assert (
        a11.authorization_from_text(a11.authorization_to_text(envelope))
        == envelope
    )
    assert envelope.fingerprint

    noncanonical = b"\x92\xcc\x01" + encoded[2:]
    with pytest.raises(StatusException):
        a11.decode_authorization(noncanonical)
    with pytest.raises(StatusException):
        a11.AuthorizationEnvelope(())


@pytest.mark.asyncio
async def test_same_stream_nested_action_retains_the_verified_context():
    stream, _ = InProcessWireStream.create_pair()
    session = Session()
    parent = (
        a11.Action(a11.ActionSchema(name="parent"))
        .bind_session(session)
        .bind_stream(stream)
    )
    verified = a11.VerifiedAuthorization(
        envelope=_envelope(),
        subject="user-1",
        subject_kind="user",
        actors=("user-1",),
        audience="agent",
        expires_at=time.time() + 60,
    )
    parent.bind_verified_authorization(verified)

    child = parent.make_nested(a11.ActionSchema(name="child"))
    detached = parent.make_nested(
        a11.ActionSchema(name="detached"), propagate_io=False
    )

    assert a11.get_verified_authorization(child) is verified
    assert a11.get_verified_authorization(detached) is None


@pytest.mark.asyncio
async def test_authorizing_once_installs_a_zero_byte_default_context():
    schema = a11.ActionSchema(
        name="who",
        outputs={
            "output": a11.ActionPortSchema(
                name="output", type="text/plain", required=True, unary=True
            )
        },
    )
    envelope = _envelope()
    verified = a11.VerifiedAuthorization(
        envelope=envelope,
        subject="user-1",
        subject_kind="user",
        actors=("user-1",),
        audience="agent",
        expires_at=time.time() + 60,
    )
    restricted_envelope = a11.AuthorizationEnvelope(
        ("other.payload.signature",)
    )
    restricted = a11.VerifiedAuthorization(
        envelope=restricted_envelope,
        subject="user-1",
        subject_kind="user",
        actors=("user-1",),
        audience="agent",
        expires_at=time.time() + 60,
        restrictions={"actions": ["different"]},
    )
    registry = a11.ActionRegistry()

    async def verifier(value: bytes) -> a11.VerifiedAuthorization:
        decoded = a11.decode_authorization(value)
        assert decoded in (envelope, restricted_envelope)
        return restricted if decoded == restricted_envelope else verified

    contexts = a11.install_authorizer(registry, verifier)
    received_headers = None

    async def who(action: a11.Action) -> None:
        nonlocal received_headers
        received_headers = dict(action.headers)
        authorization = a11.get_verified_authorization(action)
        assert authorization is not None
        await action["output"].put(authorization.subject, final=True)

    registry.register("who", schema, who)
    client_stream, server_stream = InProcessWireStream.create_pair()
    client = Session()
    server = Session(action_registry=registry)
    await asyncio.gather(
        client.add_stream(client_stream),
        server.add_stream(server_stream, mode="accept"),
    )
    connection = GatewayConnection(client, client_stream)

    try:
        rejected = connection.action("who", schema)
        await rejected.call()
        with pytest.raises(StatusException):
            await rejected.wait()

        established = await a11.establish_authorization(connection, envelope)
        assert established.is_default
        assert established.fingerprint == envelope.fingerprint

        action = connection.action("who", schema)
        await action.call()
        assert await action["output"].consume(str) == "user-1"
        await action.wait()
        assert a11.AUTH_HEADER not in received_headers
        assert a11.AUTH_REF_HEADER not in received_headers

        alternate = await a11.establish_authorization(
            connection, envelope, make_default=False
        )
        referenced = connection.action("who", schema)
        a11.set_authorization_reference(referenced, alternate.context_id)
        await referenced.call()
        assert await referenced["output"].consume(str) == "user-1"
        await referenced.wait()
        assert received_headers[a11.AUTH_REF_HEADER]

        denied = await a11.establish_authorization(
            connection, restricted_envelope, make_default=False
        )
        disallowed = connection.action("who", schema)
        a11.set_authorization_reference(disallowed, denied.context_id)
        await disallowed.call()
        with pytest.raises(StatusException):
            await disallowed.wait()
    finally:
        client.half_close()
        server.half_close()
        await asyncio.gather(
            client_stream.drain_outgoing_messages(),
            server_stream.drain_outgoing_messages(),
        )


@pytest.mark.asyncio
async def test_context_reference_is_validated_before_it_is_written():
    action = a11.Action(a11.ActionSchema(name="test"))
    with pytest.raises(StatusException):
        a11.set_authorization_reference(action, "short")

    a11.set_authorization_header(action, _envelope())
    assert a11.get_authorization(action) == _envelope()
    a11.set_authorization_header(action, None)
    assert a11.get_authorization(action) is None
