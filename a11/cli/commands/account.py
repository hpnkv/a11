# Copyright 2026 The A11 Authors.

"""``a11 register`` / ``login`` / ``logout`` / ``whoami``, and identities.

Hosting an agent on an exchange needs an account and a name, and both are
things a person does once from a terminal. These commands are that terminal
half; everything they do afterwards is an API key, stored in
`~/.config/a11/credentials.json` and picked up by `a11 serve --hosted`.

```sh
a11 register --username ada --email ada@example.com
a11 login --username ada
a11 identity create my-agent --public
a11 serve mypkg.actions --hosted my-agent
```
"""

from __future__ import annotations

import argparse
import getpass
import os

from a11.client.credentials import (
    DEFAULT_EXCHANGE,
    Credential,
    CredentialStore,
    normalise,
)
from a11.client.exchange import ExchangeClient
from a11.cli import console as console_module
from a11.cli.app import Command
from a11.status import StatusException


def _out():
    """The console ordinary output goes to."""
    return console_module.console()


def _fail(message: str) -> None:
    """Report a failure on stderr, so a pipeline's stdout stays clean."""
    console_module.console(stderr=True).print(
        message, markup=False, highlight=False
    )


def _exchange_url(args: argparse.Namespace) -> str:
    return normalise(
        getattr(args, "exchange", "")
        or os.environ.get("A11_EXCHANGE")
        or DEFAULT_EXCHANGE
    )


def _client_name() -> str:
    """What this machine's key is called, so a second login replaces it."""
    import socket

    return f"cli@{socket.gethostname()}"


def _ask_password(prompt: str, *, confirm: bool = False) -> str:
    password = getpass.getpass(prompt)
    if confirm:
        again = getpass.getpass("Repeat the password: ")
        if password != again:
            raise ValueError("The passwords do not match.")
    return password


def _store(args: argparse.Namespace) -> CredentialStore:
    del args
    return CredentialStore()


def _remember(
    store: CredentialStore, exchange: str, token: str, whoami: dict
) -> None:
    """Save the key together with what the exchange said about itself."""
    store.put(
        Credential(
            exchange=exchange,
            api_key=token,
            username=whoami.get("display", ""),
            signalling_url=whoami.get("signalling_url", ""),
            relay_ws_url=whoami.get("relay_ws_url", ""),
            relay_sse_url=whoami.get("relay_sse_url", ""),
        )
    )


# --- register ----------------------------------------------------------------


def configure_register(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--exchange", default="", help="Exchange base URL.")
    parser.add_argument("--username", required=True, help="Your username.")
    parser.add_argument("--email", required=True, help="Your email address.")
    parser.add_argument("--name", default="", help="Your display name.")
    parser.add_argument(
        "--password",
        default="",
        help=(
            "Your password. Omit it to be prompted, which is what you want:"
            " a password on a command line ends up in the shell history."
        ),
    )


async def run_register(args: argparse.Namespace) -> int:
    """Create an account and leave this machine logged in to it."""
    console = _out()
    exchange = _exchange_url(args)
    try:
        password = args.password or _ask_password(
            "Choose a password: ", confirm=True
        )
    except ValueError as exc:
        _fail(str(exc))
        return 2

    async with ExchangeClient(exchange) as client:
        try:
            created = await client.register(
                username=args.username,
                email=args.email,
                password=password,
                display_name=args.name,
                client_name=_client_name(),
            )
        except StatusException as exc:
            _fail(exc.status.message)
            return 1

        credential = created.get("credential") or {}
        token = credential.get("token", "")
        if not token:
            _fail(
                "The exchange created the account but issued no key; run"
                " `a11 login`."
            )
            return 1

        client.api_key = token
        whoami = await client.whoami()

    _remember(_store(args), exchange, token, whoami)
    console.print(
        f"Registered {args.username} at {exchange}, and logged in on this"
        " machine."
    )
    console.print(f"Your organization is {whoami.get('organization', '?')}.")
    console.print("Next: `a11 identity create <name>` to reserve a name.")
    return 0


# --- login -------------------------------------------------------------------


def configure_login(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--exchange", default="", help="Exchange base URL.")
    parser.add_argument("--username", required=True, help="Your username.")
    parser.add_argument(
        "--password", default="", help="Omit it to be prompted."
    )


async def run_login(args: argparse.Namespace) -> int:
    """Exchange a password for an API key, and store it."""
    console = _out()
    exchange = _exchange_url(args)
    password = args.password or _ask_password("Password: ")

    async with ExchangeClient(exchange) as client:
        try:
            issued = await client.login(
                username=args.username,
                password=password,
                client_name=_client_name(),
            )
        except StatusException as exc:
            _fail(exc.status.message)
            return 1
        token = issued["token"]
        client.api_key = token
        whoami = await client.whoami()

    _remember(_store(args), exchange, token, whoami)
    console.print(f"Logged in to {exchange} as {args.username}.")
    if whoami.get("relay_ws_url"):
        console.print(f"Hosted agents will answer at {whoami['relay_ws_url']}/<identity>.")
    return 0


# --- logout ------------------------------------------------------------------


def configure_logout(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--exchange", default="", help="Exchange base URL.")
    parser.add_argument(
        "--revoke",
        action="store_true",
        help=(
            "Also revoke the key at the exchange, rather than only forgetting"
            " it here. Do this on a machine you are giving up."
        ),
    )


async def run_logout(args: argparse.Namespace) -> int:
    """Forget this machine's credential, optionally revoking it."""
    console = _out()
    store = _store(args)
    credential = store.get(args.exchange or None)
    if credential is None:
        console.print("Not logged in.")
        return 0

    if args.revoke:
        async with ExchangeClient(
            credential.exchange, api_key=credential.api_key
        ) as client:
            try:
                keys = await client.list_keys()
                mine = next(
                    (key for key in keys if key["name"] == _client_name()), None
                )
                if mine is not None:
                    await client.revoke_key(mine["prefix"])
                    console.print("Revoked this machine's key.")
            except StatusException as exc:
                _fail(f"Could not revoke the key: {exc.status.message}")

    store.remove(credential.exchange)
    console.print(f"Logged out of {credential.exchange}.")
    return 0


# --- whoami ------------------------------------------------------------------


def configure_whoami(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--exchange", default="", help="Exchange base URL.")


async def run_whoami(args: argparse.Namespace) -> int:
    """Report the stored credential and what the exchange says about it."""
    console = _out()
    store = _store(args)
    credential = store.get(args.exchange or None)
    if credential is None:
        console.print("Not logged in. Run `a11 login`.")
        return 1

    async with ExchangeClient(
        credential.exchange, api_key=credential.api_key
    ) as client:
        try:
            whoami = await client.whoami()
        except StatusException as exc:
            _fail(exc.status.message)
            return 1

    console.print(f"exchange      {credential.exchange}")
    console.print(f"account       {whoami.get('display', '?')}")
    console.print(f"kind          {whoami.get('kind', '?')}")
    console.print(f"organization  {whoami.get('organization', '?')}")
    console.print(f"roles         {', '.join(whoami.get('roles', [])) or '-'}")
    console.print(
        f"permissions   {', '.join(whoami.get('permissions', [])) or '-'}"
    )
    console.print(f"signalling    {whoami.get('signalling_url', '-')}")
    console.print(f"relay (ws)    {whoami.get('relay_ws_url', '-')}")
    console.print(f"relay (sse)   {whoami.get('relay_sse_url', '-')}")
    return 0


# --- identities --------------------------------------------------------------


def configure_identity(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--exchange", default="", help="Exchange base URL.")
    subparsers = parser.add_subparsers(dest="identity_command", required=True)

    listing = subparsers.add_parser("list", help="List your identities.")
    listing.set_defaults(identity_command="list")

    create = subparsers.add_parser("create", help="Register a new identity.")
    create.add_argument("name", help="The identity to register.")
    create.add_argument("--description", default="")
    create.add_argument(
        "--public",
        action="store_true",
        help="Let anyone reach it through the relay, with no credential.",
    )
    create.set_defaults(identity_command="create")


async def run_identity(args: argparse.Namespace) -> int:
    """Register and list the names this account can host under."""
    console = _out()
    credential = _store(args).get(args.exchange or None)
    if credential is None:
        console.print("Not logged in. Run `a11 login`.")
        return 1

    async with ExchangeClient(
        credential.exchange, api_key=credential.api_key
    ) as client:
        try:
            if args.identity_command == "create":
                created = await client.register_identity(
                    args.name,
                    description=args.description,
                    visibility="public" if args.public else "private",
                )
                console.print(f"Registered {created['name']}.")
                console.print(f"  websocket  {created['relay_ws_url']}")
                console.print(f"  sse        {created['relay_sse_url']}")
                console.print(f"  actions    {created['actions_url']}")
                return 0

            identities = await client.list_identities()
        except StatusException as exc:
            _fail(exc.status.message)
            return 1

    if not identities:
        console.print("No identities yet. Try `a11 identity create <name>`.")
        return 0
    for identity in identities:
        state = "online" if identity.get("online") else "offline"
        console.print(
            f"{identity['name']:<32} {identity['visibility']:<8} {state}"
        )
    return 0


REGISTER_COMMAND = Command(
    name="register",
    help="Create an account on an A11 exchange.",
    run=run_register,
    configure=configure_register,
    description=(
        "Create an account and store an API key for this machine, so that"
        " `a11 serve --hosted` can host under it."
    ),
)

LOGIN_COMMAND = Command(
    name="login",
    help="Log in to an A11 exchange.",
    run=run_login,
    configure=configure_login,
    description=(
        "Exchange a password for an API key and store it in"
        " ~/.config/a11/credentials.json. Logging in again from the same"
        " machine replaces that key rather than adding one."
    ),
)

LOGOUT_COMMAND = Command(
    name="logout",
    help="Forget this machine's exchange credential.",
    run=run_logout,
    configure=configure_logout,
)

WHOAMI_COMMAND = Command(
    name="whoami",
    help="Show the stored credential and what it may do.",
    run=run_whoami,
    configure=configure_whoami,
)

IDENTITY_COMMAND = Command(
    name="identity",
    help="Register and list hosted identities.",
    run=run_identity,
    configure=configure_identity,
)


__all__ = [
    "IDENTITY_COMMAND",
    "LOGIN_COMMAND",
    "LOGOUT_COMMAND",
    "REGISTER_COMMAND",
    "WHOAMI_COMMAND",
]
