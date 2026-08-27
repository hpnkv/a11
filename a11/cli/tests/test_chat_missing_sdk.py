# Copyright 2026 The A11 Authors.

"""``a11 chat`` against a backend whose provider SDK is not installed.

Installing without the ``[llm]`` extra is a supported state, so choosing such a
backend has to fail as a message rather than as a failed turn: the CLI says so
at start-up and refuses to send, instead of handing the gateway a turn that can
only end in `FAILED_PRECONDITION`.
"""

import pytest

import a11
from a11.cli.backends import PROVIDERS
from a11.cli.chat_ui import ChatUI
from a11.client.connection import GatewayConnection
from a11.sdk import interact_with_llm as illm


def _connection(*, embedded: bool) -> GatewayConnection:
    """A connection object with no transport behind it.

    Nothing here runs a turn -- and that is the point of the refusal tests: a
    turn that reached the transport would fail on this ``None``.
    """

    class _Session:
        action_registry = a11.ActionRegistry()
        node_map = None

    return GatewayConnection(
        _Session(),
        None,
        url="" if embedded else "ws://gateway.example:8011/a11",
        embedded=embedded,
    )


def _ui(connection: GatewayConnection) -> ChatUI:
    return ChatUI(
        PROVIDERS["ollama"], "some-model", connection, shell_tools=False
    )


@pytest.fixture
def uninstalled_sdk(monkeypatch):
    """Make the ollama backend look like a provider whose SDK is absent."""
    monkeypatch.setitem(
        illm._PROVIDERS,
        "ollama",
        illm._Provider("a11.sdk._does_not_exist", "x", "ollama"),
    )


def test_missing_sdk_is_reported(uninstalled_sdk, capsys):
    ui = _ui(_connection(embedded=True))
    assert ui._report_missing_sdk() is True
    printed = capsys.readouterr().out
    assert "needs its provider SDK" in printed
    assert "pip install 'a11-kit[ollama]'" in printed


@pytest.mark.asyncio
async def test_turn_is_refused_without_the_sdk(uninstalled_sdk, capsys):
    ui = _ui(_connection(embedded=True))
    await ui._turn("hello")
    assert "needs its provider SDK" in capsys.readouterr().out
    # Refused before the turn: nothing was recorded in the conversation.
    assert ui._history == []


def test_a_remote_gateway_is_not_judged_by_this_environment(uninstalled_sdk):
    # A remote gateway has its own environment, so local imports do not
    # determine which backends it can serve.
    ui = _ui(_connection(embedded=False))
    assert ui._report_missing_sdk() is False


def test_an_installed_sdk_is_not_reported(monkeypatch):
    monkeypatch.setattr(illm, "_load_handler", lambda provider: None)
    ui = _ui(_connection(embedded=True))
    assert ui._report_missing_sdk() is False
