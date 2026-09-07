# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Exchange verified authorization once and reuse it on an A11 connection."""

from __future__ import annotations

import base64
import inspect
import re
from collections.abc import Awaitable, Callable
from typing import Any, Protocol

from pydantic import BaseModel, Field

from a11._native import (
    AuthorizationEnvelope,
    AuthorizationContextStore,
    VerifiedAuthorization,
    _authorization_from_text,
    _authorization_to_text,
    _decode_authorization,
    _encode_authorization,
    _get_authorization,
    _set_authorization,
    _set_authorization_reference,
)
from a11.actions.action import (
    Action,
    ActionHeaderSchema,
    ActionPortSchema,
    ActionSchema,
)
from a11.status import Status, StatusCode

AUTH_HEADER = "x-a11-auth"
AUTH_REF_HEADER = "x-a11-auth-ref"
AUTH_DEFAULT_HEADER = "x-a11-auth-default"
AUTH_REPLACE_HEADER = "x-a11-auth-replace"
AUTHORIZE_ACTION = "__authorize__"
AUTHORIZATION_VERSION = 1
MAX_AUTHORIZATION_BYTES = 16 * 1024
MAX_AUTHORIZATION_HOPS = 8


def _invalid(message: str) -> Exception:
    return Status(
        code=StatusCode.INVALID_ARGUMENT, message=message
    ).to_exception()


def _unauthenticated(message: str) -> Exception:
    return Status(
        code=StatusCode.UNAUTHENTICATED, message=message
    ).to_exception()


def _b64url(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def _b64url_decode(value: str) -> bytes:
    try:
        if not re.fullmatch(r"[A-Za-z0-9_-]*", value):
            raise ValueError("not unpadded base64url")
        padding = "=" * (-len(value) % 4)
        return base64.b64decode(value + padding, altchars=b"-_", validate=True)
    except (ValueError, TypeError) as exc:
        raise _invalid(
            "The textual authorization value is not base64url."
        ) from exc


class AuthorizationVerifier(Protocol):
    """Verify one native `x-a11-auth` value for a configured audience."""

    def __call__(
        self, value: bytes
    ) -> VerifiedAuthorization | Awaitable[VerifiedAuthorization]: ...


class AuthorizationContextResponse(BaseModel):
    """The receiver-issued handle for one connection authorization context."""

    context_id: str
    expires_at: float
    fingerprint: str
    is_default: bool = False


AUTHORIZE_SCHEMA = ActionSchema(
    name=AUTHORIZE_ACTION,
    description="Verify authorization and bind it to this connection.",
    outputs={
        "output": ActionPortSchema(
            name="output",
            type="application/json",
            description="The receiver-issued connection context.",
            required=True,
            unary=True,
            typeinfo=AuthorizationContextResponse,
        )
    },
    headers={
        AUTH_HEADER: ActionHeaderSchema(
            AUTH_HEADER, "A signed A11 authorization chain."
        ),
        AUTH_DEFAULT_HEADER: ActionHeaderSchema(
            AUTH_DEFAULT_HEADER, "Whether this becomes the stream default."
        ),
        AUTH_REPLACE_HEADER: ActionHeaderSchema(
            AUTH_REPLACE_HEADER, "Context ID replaced after verification."
        ),
    },
)


def encode_authorization(envelope: AuthorizationEnvelope) -> bytes:
    """Encode an authorization envelope in its native binary form."""
    return _encode_authorization(envelope)


def decode_authorization(
    value: bytes | bytearray | memoryview,
) -> AuthorizationEnvelope:
    """Decode a canonical native binary authorization value."""
    if not isinstance(value, (bytes, bytearray, memoryview)):
        raise _invalid("An authorization value must be bytes.")
    return _decode_authorization(bytes(value))


def authorization_to_text(envelope: AuthorizationEnvelope) -> str:
    """Encode an envelope for an ASCII-only physical HTTP header."""
    return _authorization_to_text(envelope)


def authorization_from_text(value: str) -> AuthorizationEnvelope:
    """Decode the physical HTTP-header representation of an envelope."""
    return _authorization_from_text(value)


def get_authorization(action: Action) -> AuthorizationEnvelope | None:
    """Decode the action's full authorization value without verifying it."""
    return _get_authorization(action)


def set_authorization_header(
    action: Action, envelope: AuthorizationEnvelope | None
) -> Action:
    """Set or remove the action's complete signed authorization chain."""
    return _set_authorization(action, envelope)


def set_authorization_reference(
    action: Action, context_id: str | None
) -> Action:
    """Select a non-default context previously issued on this stream."""
    if context_id is None:
        return _set_authorization_reference(action, None)
    try:
        raw = _b64url_decode(context_id)
    except Exception as exc:
        raise _invalid(
            "context_id must be an A11 authorization context ID."
        ) from exc
    if len(raw) != 16:
        raise _invalid("An authorization context ID must contain 128 bits.")
    return _set_authorization_reference(action, raw)


async def _verify(
    verifier: AuthorizationVerifier, value: bytes
) -> VerifiedAuthorization:
    result = verifier(value)
    if inspect.isawaitable(result):
        result = await result
    if not isinstance(result, VerifiedAuthorization):
        raise TypeError(
            "An authorization verifier must return VerifiedAuthorization."
        )
    return result


async def verify_action_authorization(
    action: Action, verifier: AuthorizationVerifier
) -> VerifiedAuthorization:
    """Verify and bind a complete value carried by one action."""
    raw = action.get_header(AUTH_HEADER)
    if raw is None:
        raise _unauthenticated("This action has no complete authorization.")
    if action.get_header(AUTH_REF_HEADER) is not None:
        raise _invalid("An action cannot carry both authorization forms.")
    verified = await _verify(verifier, raw)
    action.bind_verified_authorization(verified)
    return verified


def get_verified_authorization(action: Action) -> VerifiedAuthorization | None:
    """Return the immutable context already bound to an incoming action."""
    return action.get_verified_authorization()


def install_authorizer(
    registry: Any,
    verifier: AuthorizationVerifier,
    *,
    contexts: AuthorizationContextStore | None = None,
) -> AuthorizationContextStore:
    """Register `__authorize__` and return its connection context store."""
    store = contexts or AuthorizationContextStore()
    registered_sessions: set[str] = set()

    async def authorize(action: Action) -> None:
        raw = action.get_header(AUTH_HEADER)
        if raw is None:
            raise _unauthenticated("The __authorize__ action needs x-a11-auth.")
        if action.get_header(AUTH_REF_HEADER) is not None:
            raise _invalid("__authorize__ cannot use x-a11-auth-ref.")
        make_default = action.get_header(AUTH_DEFAULT_HEADER) != b"0"
        replace = action.get_header(AUTH_REPLACE_HEADER)
        if replace is not None and len(replace) != 16:
            raise _invalid("x-a11-auth-replace must contain a context ID.")
        verified = await _verify(verifier, raw)
        context = store.install(
            action,
            verified,
            make_default=make_default,
            replace=replace,
        )
        session = action.get_session()
        if session is not None and session.get_id() not in registered_sessions:
            session_id = session.get_id()
            registered_sessions.add(session_id)

            def clear(_: Any) -> None:
                store.clear_session(session_id)
                registered_sessions.discard(session_id)

            session.add_done_callback(clear)
        response = AuthorizationContextResponse(
            context_id=_b64url(context.context_id),
            expires_at=verified.expires_at,
            fingerprint=verified.fingerprint,
            is_default=make_default,
        )
        await action["output"].put(response, final=True)

    registry.register(AUTHORIZE_ACTION, AUTHORIZE_SCHEMA, authorize)
    registry._set_authorization_contexts(store)
    return store


async def establish_authorization(
    connection: Any,
    envelope: AuthorizationEnvelope,
    *,
    make_default: bool = True,
    replace: str | None = None,
) -> AuthorizationContextResponse:
    """Authorize a connection and return its receiver-issued context."""
    action = connection.action(AUTHORIZE_ACTION, AUTHORIZE_SCHEMA)
    action.set_header(AUTH_HEADER, encode_authorization(envelope))
    action.set_header(AUTH_DEFAULT_HEADER, b"1" if make_default else b"0")
    if replace is not None:
        raw = _b64url_decode(replace)
        if len(raw) != 16:
            raise _invalid("replace must be an A11 authorization context ID.")
        action.set_header(AUTH_REPLACE_HEADER, raw)
    await action.call()
    response = await action["output"].consume(AuthorizationContextResponse)
    await action.wait()
    return response


__all__ = [
    "AUTHORIZATION_VERSION",
    "AUTHORIZE_ACTION",
    "AUTHORIZE_SCHEMA",
    "AUTH_DEFAULT_HEADER",
    "AUTH_HEADER",
    "AUTH_REF_HEADER",
    "AUTH_REPLACE_HEADER",
    "AuthorizationContextResponse",
    "AuthorizationContextStore",
    "AuthorizationEnvelope",
    "AuthorizationVerifier",
    "MAX_AUTHORIZATION_BYTES",
    "MAX_AUTHORIZATION_HOPS",
    "VerifiedAuthorization",
    "authorization_from_text",
    "authorization_to_text",
    "decode_authorization",
    "encode_authorization",
    "establish_authorization",
    "get_authorization",
    "get_verified_authorization",
    "install_authorizer",
    "set_authorization_header",
    "set_authorization_reference",
    "verify_action_authorization",
]
