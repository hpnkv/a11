# Authorization

A11 carries signed delegation statements to an application verifier, then
reuses the verified result within one session stream. The runtime owns the
canonical envelope, bounded context storage, propagation, and request
restrictions. The application owns signature verification, trust policy, and
the claims placed in `VerifiedAuthorization`.

## Authorization envelope

`AuthorizationEnvelope` contains a version and an ordered chain of compact JWS
statements. A11 validates its structural limits:

- version `1`;
- one to eight ASCII compact JWS statements;
- at most 16 KiB after canonical MessagePack encoding.

`encode_authorization()` and `decode_authorization()` use the native binary
form carried by action headers. `authorization_to_text()` and
`authorization_from_text()` use the `a11-auth/1.` base64url form required by an
ASCII HTTP header. The fingerprint is the base64url SHA-256 digest of the
canonical envelope.

Structural decoding does not verify JWS signatures. A verifier returns
`VerifiedAuthorization` only after applying the application's keys, audience,
expiry, and delegation policy.

## Connection authorization

`install_authorizer(registry, verifier)` registers the reserved
`__authorize__` action and applies its context store to the registry. Other
incoming actions must then resolve a valid context before their handler runs.

The verifier accepts the native envelope bytes and returns the effective
identity and authority:

```python
import time

import a11


async def verify(value: bytes) -> a11.VerifiedAuthorization:
    envelope = a11.decode_authorization(value)
    claims = await trust_policy.verify(envelope.chain)
    return a11.VerifiedAuthorization(
        envelope=envelope,
        subject=claims.subject,
        subject_kind=claims.subject_kind,
        actors=claims.actors,
        audience=claims.audience,
        expires_at=claims.expires_at,
        grants=claims.grants,
        restrictions=claims.restrictions,
    )


contexts = a11.install_authorizer(registry, verify)
```

The verifier may be synchronous or asynchronous. An expired result is rejected
when the context is installed.

The client sends the complete proof once:

```python
context = await a11.establish_authorization(connection, envelope)
```

The receiver returns a random 128-bit context ID, expiry, envelope fingerprint,
and default flag. A default context applies to later actions on the same
session stream without another authorization header. A non-default context is
selected explicitly:

```python
context = await a11.establish_authorization(
    connection,
    envelope,
    make_default=False,
)
action = connection.action("restricted-operation", schema)
a11.set_authorization_reference(action, context.context_id)
await action.call()
```

Contexts are scoped to the receiving `Session` and `WireStream`. They do not
cross a new connection or another stream. Expired entries are pruned, each
stream has a bounded context set, and session completion clears its entries.
`replace=` rotates a context while keeping installation and removal in one
receiver operation.

## Request restrictions

The context store enforces supported `restrictions` before dispatching an
incoming action:

| Field | Constraint |
| --- | --- |
| `actions` | Action name must match one of the `*` and `?` glob patterns |
| `identities` | Verified audience must appear in the list |
| `request_id` | Action ID must equal the bound request ID |

The verifier defines the restrictions after validating the delegation chain.
Malformed restrictions fail closed with `PERMISSION_DENIED`.

## Per-action verification

Applications that do not install a connection authorizer can carry and verify
a complete proof on one action:

```python
a11.set_authorization_header(action, envelope)
```

The receiving handler calls
`verify_action_authorization(action, verifier)` before reading the resulting
`get_verified_authorization(action)` value. A complete proof and a context
reference are mutually exclusive headers.

Nested actions with I/O propagation inherit the immutable verified
authorization object. Detached children created with `propagate_io=False` do
not inherit connection authority. Raw context references are scoped to their
original stream and are excluded from generic header forwarding.

## Authorization and model tool policy

Connection authorization establishes caller identity and effective authority
for action dispatch. The LLM allowed-action header selects tools offered to a
model for one turn. Applications using both enforce the connection context at
the registry and apply the model allow-list inside the interaction action.

::: a11.authorization
