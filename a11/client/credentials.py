# Copyright 2026 The A11 Authors.

"""Where `a11 login` puts an API key, and how everything else finds it.

One file, `~/.config/a11/credentials.json`, mode 0600, holding one entry per
exchange the user has logged in to. An entry is the durable half of a
credential -- the API key, the account it belongs to, and the endpoints the
exchange told us about at login. The *temporary* half, a hosting claim, is not
stored: it expires in an hour, it is per-identity, and a stale one on disk is
worse than no one at all because a host would try it before asking for a fresh
one.

The environment wins over the file (`A11_API_KEY`, `A11_EXCHANGE`), which is
what lets a deployment inject a key without a login step.
"""

from __future__ import annotations

import contextlib
import dataclasses
import json
import os
import pathlib
import tempfile

from a11.status import Status, StatusCode

#: The exchange a client talks to when it is not told otherwise.
DEFAULT_EXCHANGE = "https://a11.services"


def config_home() -> pathlib.Path:
    """Where A11 keeps per-user state.

    `A11_CONFIG_HOME` overrides everything, then `XDG_CONFIG_HOME`, then the
    conventional `~/.config`. A test sets the first and touches nothing real.
    """
    override = os.environ.get("A11_CONFIG_HOME")
    if override:
        return pathlib.Path(override)
    xdg = os.environ.get("XDG_CONFIG_HOME")
    if xdg:
        return pathlib.Path(xdg) / "a11"
    return pathlib.Path.home() / ".config" / "a11"


def credentials_path() -> pathlib.Path:
    """The credentials file itself."""
    return config_home() / "credentials.json"


@dataclasses.dataclass
class Credential:
    """What is known about one exchange, after logging in to it."""

    exchange: str
    api_key: str
    username: str = ""
    #: Endpoints the exchange reported at login, so a host need not be
    #: configured with them separately.
    signalling_url: str = ""
    relay_ws_url: str = ""
    relay_sse_url: str = ""

    def to_json(self) -> dict:
        return dataclasses.asdict(self)

    @staticmethod
    def from_json(value: dict) -> "Credential":
        known = {field.name for field in dataclasses.fields(Credential)}
        return Credential(**{k: v for k, v in value.items() if k in known})


class CredentialStore:
    """The credentials file, read and written whole."""

    def __init__(self, path: pathlib.Path | None = None) -> None:
        self.path = path or credentials_path()

    def _read(self) -> dict:
        try:
            raw = self.path.read_text(encoding="utf-8")
        except FileNotFoundError:
            return {}
        except OSError as exc:
            raise Status(
                code=StatusCode.INTERNAL,
                message=f"Could not read {self.path}: {exc}",
            ).to_exception() from exc
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise Status(
                code=StatusCode.DATA_LOSS,
                message=(
                    f"{self.path} is not valid JSON. Move it aside and log in"
                    " again."
                ),
            ).to_exception() from exc
        return parsed if isinstance(parsed, dict) else {}

    def _write(self, document: dict) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        # Written through a temporary file in the same directory so an
        # interrupted write cannot leave a half-file where a credential was,
        # and created 0600 from the start rather than chmod'ed afterwards.
        handle, temporary = tempfile.mkstemp(
            dir=str(self.path.parent), prefix=".credentials-"
        )
        try:
            with os.fdopen(handle, "w", encoding="utf-8") as file:
                json.dump(document, file, indent=2, sort_keys=True)
                file.write("\n")
            os.chmod(temporary, 0o600)
            os.replace(temporary, self.path)
        except BaseException:
            with contextlib.suppress(OSError):
                os.unlink(temporary)
            raise

    def all(self) -> dict[str, Credential]:
        """Every stored credential, by exchange URL."""
        document = self._read()
        entries = document.get("exchanges", {})
        return {
            url: Credential.from_json({**value, "exchange": url})
            for url, value in entries.items()
            if isinstance(value, dict)
        }

    def get(self, exchange: str | None = None) -> Credential | None:
        """The credential for ``exchange``, or the default one.

        The environment is consulted first: `A11_API_KEY` makes a credential
        out of thin air, which is how a container is given one.
        """
        url = normalise(exchange or os.environ.get("A11_EXCHANGE") or "")
        env_key = os.environ.get("A11_API_KEY")
        if env_key:
            return Credential(
                exchange=url or normalise(DEFAULT_EXCHANGE), api_key=env_key
            )

        stored = self.all()
        if url:
            return stored.get(url)
        document = self._read()
        default = document.get("default")
        if default and default in stored:
            return stored[default]
        if len(stored) == 1:
            return next(iter(stored.values()))
        return None

    def put(self, credential: Credential, *, make_default: bool = True) -> None:
        """Store ``credential``, replacing any for the same exchange."""
        document = self._read()
        exchanges = document.setdefault("exchanges", {})
        url = normalise(credential.exchange)
        entry = credential.to_json()
        entry.pop("exchange", None)
        exchanges[url] = entry
        if make_default or "default" not in document:
            document["default"] = url
        self._write(document)

    def remove(self, exchange: str | None = None) -> bool:
        """Forget one exchange's credential. Returns whether there was one."""
        document = self._read()
        exchanges = document.get("exchanges", {})
        url = normalise(exchange or document.get("default", ""))
        if url not in exchanges:
            return False
        del exchanges[url]
        if document.get("default") == url:
            document["default"] = next(iter(exchanges), "")
        self._write(document)
        return True

    def require(self, exchange: str | None = None) -> Credential:
        """The credential, or an error telling the user how to get one.

        Raises:
            StatusException: UNAUTHENTICATED when there is none.
        """
        credential = self.get(exchange)
        if credential is None:
            raise Status(
                code=StatusCode.UNAUTHENTICATED,
                message=(
                    "No A11 exchange credential. Run `a11 login`, or set"
                    " A11_API_KEY."
                ),
            ).to_exception()
        return credential


def normalise(url: str) -> str:
    """One spelling per exchange, so two logins do not make two entries."""
    return url.rstrip("/")


__all__ = [
    "DEFAULT_EXCHANGE",
    "Credential",
    "CredentialStore",
    "config_home",
    "credentials_path",
    "normalise",
]
