# Copyright 2026 The A11 Authors.

"""Talking to an A11 exchange over its HTTP API.

The exchange is a separate service with a documented HTTP contract. This client
supports `a11 register`, `a11 login`, and `a11 serve --hosted` through that
protocol without importing the exchange implementation.

Errors are returned as `Status` documents and raised as `StatusException` with
the service's code, allowing callers to handle claim renewal and invalid keys
without parsing messages.
"""

from __future__ import annotations

import dataclasses
from typing import Any

import httpx

from a11.status import Status, StatusCode, StatusException

#: How long any single exchange call is given.
DEFAULT_TIMEOUT = 30.0


@dataclasses.dataclass
class Claim:
    """A hosting claim, and everything needed to act on it."""

    identity: str
    token: str
    expires_at: str
    renew_after: str
    signalling_url: str
    ice_servers: list[dict]
    relay_ws_url: str = ""
    relay_sse_url: str = ""

    @staticmethod
    def from_response(payload: dict) -> "Claim":
        claim = payload.get("claim", {})
        return Claim(
            identity=claim.get("identity", ""),
            token=payload.get("token", ""),
            expires_at=claim.get("expires_at", ""),
            renew_after=claim.get("renew_after", ""),
            signalling_url=payload.get("signalling_url", ""),
            ice_servers=list(payload.get("ice_servers", [])),
            relay_ws_url=payload.get("relay_ws_url", ""),
            relay_sse_url=payload.get("relay_sse_url", ""),
        )


class ExchangeClient:
    """One authenticated conversation with one exchange."""

    def __init__(
        self,
        base_url: str,
        *,
        api_key: str = "",
        client: httpx.AsyncClient | None = None,
        timeout: float = DEFAULT_TIMEOUT,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self._owned = client is None
        self._client = client or httpx.AsyncClient(
            base_url=self.base_url, timeout=timeout
        )

    async def __aenter__(self) -> "ExchangeClient":
        return self

    async def __aexit__(self, exc_type, exc, traceback) -> None:
        await self.aclose()

    async def aclose(self) -> None:
        """Close the underlying HTTP client, if this object made it."""
        if self._owned:
            await self._client.aclose()

    # --- plumbing -------------------------------------------------------

    def _headers(self) -> dict[str, str]:
        return (
            {"Authorization": f"Bearer {self.api_key}"} if self.api_key else {}
        )

    async def _request(self, method: str, path: str, **kwargs: Any) -> Any:
        headers = {**self._headers(), **kwargs.pop("headers", {})}
        try:
            response = await self._client.request(
                method, f"{self.base_url}{path}", headers=headers, **kwargs
            )
        except httpx.HTTPError as exc:
            raise Status(
                code=StatusCode.UNAVAILABLE,
                message=(
                    f"Could not reach the exchange at {self.base_url}: {exc}"
                ),
            ).to_exception() from exc

        if response.status_code >= 400:
            raise _status_from(response).to_exception()
        if response.status_code == 204 or not response.content:
            return None
        return response.json()

    # --- accounts -------------------------------------------------------

    async def register(
        self,
        *,
        username: str,
        email: str,
        password: str,
        display_name: str = "",
        client_name: str = "cli",
    ) -> dict:
        """Create an account and come away logged in.

        The key is issued as part of registration rather than by a second
        call, so `a11 register` leaves the user able to host immediately.
        """
        return await self._request(
            "POST",
            "/v1/auth/register",
            json={
                "username": username,
                "email": email,
                "password": password,
                "display_name": display_name,
                "issue_key_named": client_name,
            },
        )

    async def login(
        self,
        *,
        username: str,
        password: str,
        client_name: str = "cli",
    ) -> dict:
        """Exchange a password for this client's API key."""
        return await self._request(
            "POST",
            "/v1/auth/login",
            json={
                "username": username,
                "password": password,
                "client_name": client_name,
            },
        )

    async def whoami(self) -> dict:
        """Who this credential is, and what it may do."""
        return await self._request("GET", "/v1/auth/whoami")

    # --- identities and claims ------------------------------------------

    async def list_identities(self) -> list[dict]:
        """The identities the caller's organization owns."""
        return await self._request("GET", "/v1/identities")

    async def get_identity(self, name: str) -> dict:
        """One identity, including whether the exchange can currently reach it.

        The ``online`` field is presence, which the exchange writes when a
        signalling connection is admitted and withdraws when it departs. A host
        can therefore ask whether the exchange agrees that it is hosting --
        which is not the same question as whether its own socket is open. See
        `a11.client.hosting.HostedEndpoint._verify_registration`.
        """
        return await self._request("GET", f"/v1/identities/{name}")

    async def register_identity(
        self,
        name: str,
        *,
        display_name: str = "",
        description: str = "",
        visibility: str = "private",
        organization: str = "",
        scoped: bool = False,
    ) -> dict:
        """Register a name to host under.

        ``scoped`` prefixes ``name`` with the organization and registers a
        disposable identity. Register it explicitly when metadata such as
        public visibility is required; claiming a scoped name otherwise creates
        it automatically.
        """
        return await self._request(
            "POST",
            "/v1/identities",
            json={
                "name": name,
                "display_name": display_name,
                "description": description,
                "visibility": visibility,
                "organization": organization,
                "scoped": scoped,
            },
        )

    async def claim(
        self, identity: str, *, holder: str = "", ttl_seconds: int | None = None
    ) -> Claim:
        """Take the hosting claim on ``identity``.

        A *scoped* identity -- `<organization>--<name>` -- need not have been
        registered: the exchange creates it here, which is what makes hosting
        one a single call.
        """
        payload = await self._request(
            "POST",
            f"/v1/identities/{identity}/claim",
            json={"holder": holder, "ttl_seconds": ttl_seconds},
        )
        return Claim.from_response(payload)

    async def claim_scoped(
        self,
        *,
        name: str = "",
        organization: str = "",
        holder: str = "",
        ttl_seconds: int | None = None,
        visibility: str = "",
    ) -> Claim:
        """Get a scoped identity and the claim on it, in one call.

        For a host that wants somewhere to be rather than a particular name:
        without ``name`` the exchange picks one, and the granted identity comes
        back on the claim. Scoped identities are disposable -- reclaimed once
        nothing has hosted them for a while -- so this is the cheap way to put a
        process on the exchange and the wrong way to publish an agent.
        """
        request: dict[str, Any] = {
            "name": name,
            "organization": organization,
            "holder": holder,
            "ttl_seconds": ttl_seconds,
        }
        if visibility:
            request["visibility"] = visibility
        payload = await self._request(
            "POST", "/v1/identities/claim", json=request
        )
        return Claim.from_response(payload)

    async def renew(
        self, identity: str, token: str, *, ttl_seconds: int | None = None
    ) -> Claim:
        """Extend a live claim, keeping its token."""
        payload = await self._request(
            "POST",
            f"/v1/identities/{identity}/claim/renew",
            json={"token": token, "ttl_seconds": ttl_seconds},
        )
        return Claim.from_response(payload)

    async def release(self, identity: str, token: str) -> None:
        """Give a claim up, ending signalling and relay access with it."""
        await self._request(
            "POST",
            f"/v1/identities/{identity}/claim/release",
            json={"token": token},
        )

    async def ice_servers(
        self, *, identity: str = "", claim: str = ""
    ) -> list[dict]:
        """Fresh ICE servers, with TURN credentials when a claim is named."""
        params = {}
        if identity and claim:
            params = {"identity": identity, "claim": claim}
        payload = await self._request("GET", "/v1/ice-servers", params=params)
        return list(payload.get("ice_servers", []))

    # --- keys -----------------------------------------------------------

    async def list_keys(self) -> list[dict]:
        """The caller's API keys, without their secrets."""
        return await self._request("GET", "/v1/api-keys")

    async def revoke_key(self, prefix: str) -> dict:
        """Revoke a key by its prefix."""
        return await self._request("DELETE", f"/v1/api-keys/{prefix}")


def _status_from(response: httpx.Response) -> Status:
    """The `Status` an error response carries, or one describing why not."""
    try:
        payload = response.json()
    except ValueError:
        payload = None
    if isinstance(payload, dict) and "code" in payload:
        try:
            return Status.model_validate(payload)
        except Exception:  # noqa: BLE001 - fall through to the generic form
            pass
    return Status(
        code=StatusCode.from_http_code(response.status_code),
        message=(
            response.text.strip()
            or f"The exchange answered {response.status_code}."
        ),
    )


__all__ = ["DEFAULT_TIMEOUT", "Claim", "ExchangeClient", "StatusException"]
