# Copyright 2026 The A11 Authors.

"""``a11 serve``: the target it resolves, the protocol it speaks, what it binds.

The command is mostly two decisions -- which registry, and which listeners --
so most of this needs no socket at all. The two that do check the thing worth
checking: that a caller arriving on either endpoint reaches the *same* service.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import os
import signal
import subprocess
import sys
import textwrap
import time
from pathlib import Path

import pytest

import a11
from a11 import net, timing
from a11.cli.commands.serve import (
    SERVE_COMMAND,
    ServeError,
    http2_options,
    is_path_target,
    resolve_registry,
    serve,
    split_target,
)


def _args(target: str = "unused", **overrides) -> argparse.Namespace:
    """Parsed flags as the command's own parser produces them."""
    parser = argparse.ArgumentParser()
    SERVE_COMMAND.configure(parser)
    parsed = parser.parse_args([target])
    for name, value in overrides.items():
        setattr(parsed, name, value)
    return parsed


def _own(registry) -> list[str]:
    """The actions a served module registered, without A11's own builtins.

    Every registry answers for `__list_actions__` and friends whether or not
    anybody installed them, so they show up in a listing and are not what these
    tests are about.
    """
    return [
        name
        for name in registry.list_registered_actions()
        if not name.startswith("__")
    ]


def _module(tmp_path: Path, name: str, source: str) -> None:
    """Write an importable module and put its directory on the path."""
    (tmp_path / f"{name}.py").write_text(textwrap.dedent(source))
    if str(tmp_path) not in sys.path:
        sys.path.insert(0, str(tmp_path))


_REGISTRY_SOURCE = """
import a11

REGISTRY = a11.ActionRegistry()
TOOLS = a11.ActionRegistry()

@REGISTRY.action
async def shout(text: str) -> str:
    \"\"\"Shout a line.\"\"\"
    return text.upper()

@TOOLS.action
async def whisper(text: str) -> str:
    \"\"\"Whisper a line.\"\"\"
    return text.lower()
"""


# --- The target --------------------------------------------------------------


def test_the_default_symbol_is_registry(tmp_path: Path) -> None:
    _module(tmp_path, "serve_default", _REGISTRY_SOURCE)

    registry, module_path, symbol = resolve_registry("serve_default")
    assert module_path == "serve_default"
    assert symbol == "REGISTRY"
    assert _own(registry) == ["shout"]


def test_a_named_symbol_is_read_after_the_colon(tmp_path: Path) -> None:
    _module(tmp_path, "serve_named", _REGISTRY_SOURCE)

    registry, _, symbol = resolve_registry("serve_named:TOOLS")
    assert symbol == "TOOLS"
    assert _own(registry) == ["whisper"]


def test_an_unimportable_module_says_so() -> None:
    with pytest.raises(ServeError, match="cannot import 'nope.not.here'"):
        resolve_registry("nope.not.here")


def test_a_missing_symbol_names_the_one_registry_it_found(
    tmp_path: Path,
) -> None:
    """The common near-miss: a module whose registry is spelled lowercase."""
    _module(
        tmp_path,
        "serve_lowercase",
        """
        import a11
        registry = a11.ActionRegistry()
        """,
    )

    with pytest.raises(ServeError) as raised:
        resolve_registry("serve_lowercase")
    assert "has no 'REGISTRY'" in str(raised.value)
    assert "serve_lowercase:registry" in str(raised.value)


def test_a_missing_symbol_lists_several_candidates(tmp_path: Path) -> None:
    _module(tmp_path, "serve_many", _REGISTRY_SOURCE)

    with pytest.raises(ServeError) as raised:
        resolve_registry("serve_many:ABSENT")
    assert "REGISTRY, TOOLS" in str(raised.value)


def test_a_symbol_of_the_wrong_type_is_refused(tmp_path: Path) -> None:
    _module(
        tmp_path,
        "serve_wrong",
        """
        REGISTRY = "not a registry"
        """,
    )

    with pytest.raises(ServeError, match="is a str, not an ActionRegistry"):
        resolve_registry("serve_wrong")


def test_an_empty_target_is_refused() -> None:
    with pytest.raises(ServeError, match="give a module to serve"):
        resolve_registry(":REGISTRY")


# --- A file path instead of an import path ------------------------------------


def _file(tmp_path: Path, name: str, source: str) -> Path:
    """Write a module file without putting its directory on the path."""
    path = tmp_path / name
    path.write_text(textwrap.dedent(source))
    return path


@pytest.mark.parametrize(
    ("target", "module", "symbol"),
    [
        ("pkg.sub.mod", "pkg.sub.mod", "REGISTRY"),
        ("pkg.sub.mod:TOOLS", "pkg.sub.mod", "TOOLS"),
        ("mod", "mod", "REGISTRY"),
        ("a/b/mod.py", "a/b/mod.py", "REGISTRY"),
        ("a/b/mod.py:TOOLS", "a/b/mod.py", "TOOLS"),
        # A drive letter is not a symbol separator: the tail is not an
        # identifier, so the whole thing stays the module.
        (r"C:\src\mod.py", r"C:\src\mod.py", "REGISTRY"),
        (r"C:\src\mod.py:TOOLS", r"C:\src\mod.py", "TOOLS"),
    ],
)
def test_the_symbol_is_split_off_from_the_right(
    target: str, module: str, symbol: str
) -> None:
    assert split_target(target) == (module, symbol)


@pytest.mark.parametrize(
    "target",
    ["main.py", "./main", "../pkg/main.py", "~/actions.py", "a/b/mod"],
)
def test_these_targets_read_as_paths(target: str) -> None:
    assert is_path_target(target)


@pytest.mark.parametrize("target", ["mypkg.actions", "mod", "a11.sdk.bash"])
def test_these_targets_read_as_import_paths(target: str) -> None:
    assert not is_path_target(target)


def test_a_bare_name_that_is_a_file_reads_as_a_path(
    tmp_path: Path, monkeypatch
) -> None:
    """No suffix, no separator -- but it is right there, so it is meant."""
    (tmp_path / "actions").write_text("REGISTRY = None\n")
    monkeypatch.chdir(tmp_path)

    assert is_path_target("actions")


def test_a_file_path_is_loaded_and_its_registry_read(tmp_path: Path) -> None:
    path = _file(tmp_path, "from_file.py", _REGISTRY_SOURCE)

    registry, label, symbol = resolve_registry(str(path))
    assert label == str(path)
    assert symbol == "REGISTRY"
    assert _own(registry) == ["shout"]


def test_a_file_path_takes_a_symbol_too(tmp_path: Path) -> None:
    path = _file(tmp_path, "from_file_named.py", _REGISTRY_SOURCE)

    registry, _, symbol = resolve_registry(f"{path}:TOOLS")
    assert symbol == "TOOLS"
    assert _own(registry) == ["whisper"]


def test_a_served_file_does_not_run_its_own_entry_point(tmp_path: Path) -> None:
    """Loaded under its stem, not as __main__, so the guard stays shut.

    Every runnable example ends in one of these; serving one must not start it.
    """
    path = _file(
        tmp_path,
        "guarded.py",
        """
        import a11

        REGISTRY = a11.ActionRegistry()
        RAN = []

        if __name__ == "__main__":
            RAN.append("entry point")
        """,
    )

    resolve_registry(str(path))

    import guarded  # noqa: PLC0415 - written just above

    assert guarded.__name__ == "guarded"
    assert guarded.RAN == []


def test_a_served_file_can_import_its_siblings(tmp_path: Path) -> None:
    """Its directory joins sys.path, as it would for `python thatfile.py`."""
    _file(tmp_path, "sibling_helper.py", "SHOUTED = 'FROM A SIBLING'\n")
    path = _file(
        tmp_path,
        "with_sibling.py",
        """
        import a11
        from sibling_helper import SHOUTED

        REGISTRY = a11.ActionRegistry()

        @REGISTRY.action
        async def shout(text: str) -> str:
            \"\"\"Shout.\"\"\"
            return SHOUTED
        """,
    )

    registry, _, _ = resolve_registry(str(path))
    assert _own(registry) == ["shout"]


def test_a_missing_file_says_so(tmp_path: Path) -> None:
    with pytest.raises(ServeError, match="no such file"):
        resolve_registry(str(tmp_path / "absent.py"))


def test_a_directory_is_refused_with_the_file_to_use(tmp_path: Path) -> None:
    (tmp_path / "apkg").mkdir()

    with pytest.raises(ServeError, match="is a directory"):
        resolve_registry(str(tmp_path / "apkg"))


def test_a_file_that_is_not_python_is_refused(tmp_path: Path) -> None:
    path = tmp_path / "actions.txt"
    path.write_text("REGISTRY = 1")

    with pytest.raises(ServeError, match="is not a .py file"):
        resolve_registry(str(path))


def test_a_file_that_raises_on_import_reports_the_cause(tmp_path: Path) -> None:
    path = _file(tmp_path, "explodes.py", "raise ValueError('no thanks')\n")

    with pytest.raises(ServeError, match="failed to load.*no thanks"):
        resolve_registry(str(path))
    # And it is not left half-executed under a name something else may want.
    assert "explodes" not in sys.modules


def test_a_file_missing_the_symbol_still_names_the_alternatives(
    tmp_path: Path,
) -> None:
    path = _file(
        tmp_path,
        "lowercase_file.py",
        """
        import a11
        registry = a11.ActionRegistry()
        """,
    )

    with pytest.raises(ServeError) as raised:
        resolve_registry(str(path))
    assert f"{path}:registry" in str(raised.value)


# --- HTTP protocol and TLS ---------------------------------------------------


def test_http11_is_the_default_and_the_only_thing_enabled() -> None:
    options = http2_options(_args())
    assert options.enable_http1
    assert not options.enable_h2
    assert not options.enable_h2c
    assert not options.tls.enabled


def test_h11_explicitly_is_the_same_as_the_default() -> None:
    assert http2_options(_args(h11=True)).enable_http1
    assert not http2_options(_args(h11=True)).enable_h2c


def test_one_protocol_choice_covers_every_http_endpoint() -> None:
    """SSE and WebSocket get the same options; the flags are not per group."""
    assert http2_options(_args(h2c=True)).enable_h2c
    assert not http2_options(_args(h2c=True)).enable_http1


def test_h2c_is_cleartext_http2_only() -> None:
    options = http2_options(_args(h2c=True))
    assert options.enable_h2c
    assert not options.enable_http1
    assert not options.enable_h2
    assert not options.tls.enabled


def test_h2_needs_tls_and_gets_it(tmp_path: Path) -> None:
    cert = tmp_path / "cert.pem"
    key = tmp_path / "key.pem"
    cert.write_text("-----BEGIN CERTIFICATE-----\n")
    key.write_text("-----BEGIN PRIVATE KEY-----\n")

    options = http2_options(_args(h2=True, cert=cert, privkey=key))
    assert options.enable_h2
    assert not options.enable_http1
    assert not options.enable_h2c
    assert options.tls.enabled
    assert options.tls.certificate_pem_file == str(cert)
    assert options.tls.key_pem_file == str(key)


def test_a_certificate_alone_enables_tls_over_http11(tmp_path: Path) -> None:
    cert = tmp_path / "c.pem"
    key = tmp_path / "k.pem"
    cert.write_text("x")
    key.write_text("y")

    options = http2_options(_args(cert=cert, privkey=key))
    assert options.tls.enabled
    assert options.enable_http1
    assert not options.enable_h2


def test_h2_without_a_certificate_is_refused() -> None:
    with pytest.raises(ServeError, match="--h2 is HTTP/2 over TLS"):
        http2_options(_args(h2=True))


def test_h2c_with_a_certificate_is_refused(tmp_path: Path) -> None:
    cert = tmp_path / "c.pem"
    key = tmp_path / "k.pem"
    cert.write_text("x")
    key.write_text("y")

    with pytest.raises(ServeError, match="--h2c is cleartext"):
        http2_options(_args(h2c=True, cert=cert, privkey=key))


def test_half_a_tls_identity_is_refused(tmp_path: Path) -> None:
    cert = tmp_path / "c.pem"
    cert.write_text("x")

    with pytest.raises(ServeError, match="go together"):
        http2_options(_args(cert=cert))


def test_a_certificate_that_is_not_there_is_refused(tmp_path: Path) -> None:
    with pytest.raises(ServeError, match="does not exist"):
        http2_options(
            _args(cert=tmp_path / "absent.pem", privkey=tmp_path / "absent.key")
        )


def test_the_protocol_flags_are_mutually_exclusive() -> None:
    parser = argparse.ArgumentParser()
    SERVE_COMMAND.configure(parser)
    with pytest.raises(SystemExit):
        parser.parse_args(["mod", "--h2c", "--h2"])


# --- WebRTC flag validation --------------------------------------------------


@pytest.mark.asyncio
async def test_webrtc_without_a_signalling_server_is_refused(
    tmp_path: Path,
) -> None:
    _module(tmp_path, "serve_rtc_a", _REGISTRY_SOURCE)
    with pytest.raises(ServeError, match="--webrtc-signalling-server"):
        await serve(_args("serve_rtc_a", webrtc=True))


@pytest.mark.asyncio
async def test_webrtc_without_an_identity_is_refused(tmp_path: Path) -> None:
    _module(tmp_path, "serve_rtc_b", _REGISTRY_SOURCE)
    with pytest.raises(ServeError, match="--webrtc-signalling-identity"):
        await serve(
            _args(
                "serve_rtc_b",
                webrtc=True,
                webrtc_signalling_server="ws://127.0.0.1:1",
            )
        )


@pytest.mark.asyncio
async def test_a_signalling_credential_travels_on_the_handshake(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A credential given must reach the server, not be quietly dropped.

    It used to be refused outright, because A11's signalling client could not
    send headers. It can, so the flag is honoured -- and this asserts the
    honouring rather than the refusal, because "silently unauthenticated" is
    still the one outcome worse than an error.
    """
    from a11.cli.commands.serve import _signalling_client

    captured: dict[str, object] = {}

    async def fake_connect(url, identity, on_message, options):
        captured["url"] = url
        captured["identity"] = identity
        captured["headers"] = dict(options.headers)
        return object()

    monkeypatch.setattr(
        net.WebSocketSignallingClient, "connect", fake_connect
    )
    await _signalling_client(
        _args(
            "unused",
            webrtc=True,
            webrtc_signalling_server="ws://127.0.0.1:1",
            webrtc_signalling_identity="server",
            webrtc_signalling_authorization="Bearer token",
        )
    )

    assert captured["identity"] == "server"
    assert captured["headers"]["authorization"] == "Bearer token"


# --- Serving, for real -------------------------------------------------------


def _client_options() -> net.WebSocketClientOptions:
    options = net.WebSocketClientOptions()
    options.http2_options.enable_h2 = False
    options.http2_options.enable_h2c = False
    return options


async def _call_shout(session, stream, schema, text: str) -> str:
    call = (
        a11.Action(schema)
        .bind_node_map(session.node_map)
        .bind_session(session)
        .bind_stream(stream)
    )
    await call.call()
    await call["text"].finalize(text)
    result = await call["output"].consume(str)
    await call.wait(timing.Duration.seconds(10))
    return result


@contextlib.asynccontextmanager
async def _serving(args: argparse.Namespace):
    """Run the command until the body is done, then signal it to stop."""
    task = asyncio.ensure_future(serve(args))
    # The command binds its listeners before awaiting the stop event; give it
    # the turns to get there rather than guessing at a sleep.
    for _ in range(200):
        await asyncio.sleep(0.01)
        if _listening(args):
            break
    try:
        yield
    finally:
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError, Exception):
            await asyncio.wait_for(task, timeout=10)


def _listening(args: argparse.Namespace) -> bool:
    """Whether the requested WebSocket port accepts a connection."""
    import socket

    with socket.socket() as probe:
        probe.settimeout(0.2)
        return probe.connect_ex((args.ws_host, args.ws_port)) == 0


@pytest.mark.asyncio
async def test_serving_a_module_registry_over_websocket(tmp_path: Path) -> None:
    _module(tmp_path, "serve_ws", _REGISTRY_SOURCE)
    import serve_ws  # noqa: PLC0415 - written by the fixture above

    schema = serve_ws.REGISTRY.get_schema("shout")
    args = _args("serve_ws", ws=True, ws_port=8099, ws_path="/a11")

    async with _serving(args):
        stream = net.WebSocketWireStream.connect(
            f"ws://127.0.0.1:{args.ws_port}{args.ws_path}",
            websocket_options=_client_options(),
        )
        client = a11.Session(action_registry=a11.ActionRegistry())
        await asyncio.wait_for(client.add_stream(stream, mode="start"), 10)
        assert await _call_shout(client, stream, schema, "hi") == "HI"
        stream.half_close()
        await stream.drain_outgoing_messages()


@pytest.mark.asyncio
async def test_no_transport_flag_serves_websocket(tmp_path: Path) -> None:
    """The bare command still listens; the default is announced, not silent."""
    _module(tmp_path, "serve_implied", _REGISTRY_SOURCE)
    import serve_implied  # noqa: PLC0415 - written by the fixture above

    schema = serve_implied.REGISTRY.get_schema("shout")
    args = _args("serve_implied", ws_port=8098)
    assert not args.ws and not args.sse and not args.webrtc

    async with _serving(args):
        stream = net.WebSocketWireStream.connect(
            f"ws://127.0.0.1:{args.ws_port}/a11",
            websocket_options=_client_options(),
        )
        client = a11.Session(action_registry=a11.ActionRegistry())
        await asyncio.wait_for(client.add_stream(stream, mode="start"), 10)
        assert await _call_shout(client, stream, schema, "yes") == "YES"
        stream.half_close()
        await stream.drain_outgoing_messages()


@pytest.mark.asyncio
async def test_two_endpoints_reach_one_service(tmp_path: Path) -> None:
    """The requirement: --ws and --sse are two doors into the same service.

    Checked through the service rather than by inspection -- both endpoints
    answer, and the service counts both connections as its own sessions.
    """
    _module(tmp_path, "serve_both", _REGISTRY_SOURCE)
    import serve_both  # noqa: PLC0415 - written by the fixture above

    schema = serve_both.REGISTRY.get_schema("shout")
    args = _args("serve_both", ws=True, ws_port=8097, sse=True, sse_port=8096)

    async with _serving(args):
        ws_stream = net.WebSocketWireStream.connect(
            f"ws://127.0.0.1:{args.ws_port}/a11",
            websocket_options=_client_options(),
        )
        ws_client = a11.Session(action_registry=a11.ActionRegistry())
        await asyncio.wait_for(
            ws_client.add_stream(ws_stream, mode="start"), 10
        )
        assert await _call_shout(ws_client, ws_stream, schema, "ws") == "WS"

        # The client mirrors the endpoint: HTTP/1.1, which SSE reaches by
        # giving its outbound direction a connection of its own.
        sse_options = net.HttpSseOptions()
        sse_options.http2_options.enable_http1 = True
        sse_options.http2_options.enable_h2 = False
        sse_options.http2_options.enable_h2c = False
        sse_stream = net.HttpSseClientWireStream.create(
            f"http://127.0.0.1:{args.sse_port}", sse_options
        )
        sse_client = a11.Session(action_registry=a11.ActionRegistry())
        await asyncio.wait_for(
            sse_client.add_stream(sse_stream, mode="start"), 10
        )
        assert await _call_shout(sse_client, sse_stream, schema, "sse") == "SSE"

        for stream in (ws_stream, sse_stream):
            stream.half_close()
            await stream.drain_outgoing_messages()


# --- Shutdown ----------------------------------------------------------------


def _spawn_serve(tmp_path: Path, port: int) -> subprocess.Popen:
    """`a11 serve` as a real process, so a real signal can be sent to it."""
    environment = dict(os.environ)
    environment["PYTHONPATH"] = (
        f"{tmp_path}{os.pathsep}{environment.get('PYTHONPATH', '')}"
    )
    return subprocess.Popen(
        [
            sys.executable,
            "-m",
            "a11.cli",
            "serve",
            "serve_signal",
            "--ws",
            "--ws-port",
            str(port),
            "--plain",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=environment,
    )


def _accepts(port: int) -> bool:
    """Whether anything is listening on ``port``."""
    import socket

    with socket.socket() as probe:
        probe.settimeout(0.2)
        return probe.connect_ex(("127.0.0.1", port)) == 0


def _await_listening(process: subprocess.Popen, port: int) -> None:
    for _ in range(400):
        if process.poll() is not None:
            raise AssertionError(
                f"exited early ({process.returncode}): {process.stdout.read()}"
            )
        if _accepts(port):
            return
        time.sleep(0.05)
    raise AssertionError("never started listening")


@pytest.mark.parametrize(
    "number", [signal.SIGTERM, signal.SIGINT], ids=["sigterm", "sigint"]
)
def test_a_termination_signal_is_a_clean_exit(
    tmp_path: Path, number: signal.Signals
) -> None:
    """Exit 0 and no crash dump.

    The dump is the point: the native runtime installs Abseil's failure-signal
    handler, which treats an unhandled SIGTERM as a crash and prints a stack
    trace while the socket is still open. Handling it on the loop is what makes
    a signal mean "stop", and this is what would notice that regressing.
    """
    _module(tmp_path, "serve_signal", _REGISTRY_SOURCE)
    port = 8095 if number is signal.SIGTERM else 8094
    process = _spawn_serve(tmp_path, port)
    try:
        _await_listening(process, port)
        process.send_signal(number)
        output = process.communicate(timeout=30)[0]
    except BaseException:
        process.kill()
        raise

    assert process.returncode == 0, output
    # Abseil's failure-signal handler prints one of these before dying. The
    # command's own `[serve] stopping` is not asserted on: `a11.cli` leaves absl
    # logging off, so the exit code and the freed port are what is observable.
    assert "*** SIGTERM received" not in output
    assert "*** Signal" not in output
    assert "Aborted" not in output
    assert not _accepts(port), "still listening after exit"


@pytest.mark.asyncio
async def test_serving_over_webrtc_through_a_signalling_server(
    tmp_path: Path,
) -> None:
    """`--webrtc` with a signalling server it dials out to, as specified.

    The `WebSocketSignallingServer` here stands in for the
    ``wss://a11.services/ice`` of the flag's own example; the command registers
    with it as a client and peers dial the identity it registered under.
    """
    _module(tmp_path, "serve_rtc", _REGISTRY_SOURCE)
    import serve_rtc  # noqa: PLC0415 - written by the fixture above

    schema = serve_rtc.REGISTRY.get_schema("shout")
    rendezvous = net.SignallingService.create()
    signalling_server = net.WebSocketSignallingServer.create(rendezvous)
    url = f"ws://127.0.0.1:{signalling_server.port}"
    args = _args(
        "serve_rtc",
        webrtc=True,
        webrtc_signalling_server=url,
        webrtc_signalling_identity="demoserver",
    )

    task = asyncio.ensure_future(serve(args))
    client_signalling = None
    try:
        # No port to probe, so wait for the identity to appear on the
        # rendezvous: that is exactly "the server is reachable".
        for _ in range(400):
            if "demoserver" in rendezvous.identities():
                break
            await asyncio.sleep(0.02)
        assert "demoserver" in rendezvous.identities()

        client_signalling = await asyncio.wait_for(
            net.WebSocketSignallingClient.connect(url, "peer"), timeout=10
        )
        stream = net.WebRtcWireStream.create_client(
            "demoserver", client_signalling
        )
        client = a11.Session(action_registry=a11.ActionRegistry())
        await asyncio.wait_for(client.add_stream(stream, mode="start"), 30)
        assert await _call_shout(client, stream, schema, "rtc") == "RTC"
        stream.half_close()
        await stream.drain_outgoing_messages()
    finally:
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError, Exception):
            await asyncio.wait_for(task, timeout=15)
        if client_signalling is not None:
            client_signalling.close()
        signalling_server.stop()
        rendezvous.stop()
