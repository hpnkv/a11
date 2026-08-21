# Copyright 2026 The A11 Authors.

"""``a11 chat`` wiring for the shell tools (offline).

Checks that the CLI turns the shell tools on by default and that
``--no-shell-tools`` turns them off -- without touching the network.
"""

import argparse

import a11
from a11.cli.backends import PROVIDERS
from a11.cli.chat_ui import ChatUI
from a11.cli.commands.chat import CHAT_COMMAND
from a11.client.connection import GatewayConnection


def _provider():
    return PROVIDERS["ollama"]


def _connection() -> GatewayConnection:
    """A connection object with no transport behind it.

    Enough for the wiring these tests check: `ChatUI` only needs somewhere to
    register its shell Actions, and nothing here runs a turn.
    """

    class _Session:
        action_registry = a11.ActionRegistry()
        node_map = None

    return GatewayConnection(_Session(), None, embedded=True)


def test_shell_tools_are_on_by_default():
    connection = _connection()
    ui = ChatUI(_provider(), "some-model", connection, shell_tools=True)
    # Registering them is the whole of the client's part. The Actions go on the
    # *connection's* registry, and the gateway asks that session what it serves
    # and dispatches the model's calls back to this process to run them. There
    # is no schema list and no definition list here any more, because there
    # is nothing to announce -- and so nothing to announce wrongly.
    registry = connection.session.action_registry
    for name in ("shell_start", "shell_execute", "shell_list", "shell_exit"):
        assert registry.is_registered(name), name
    # The header names exactly these: it gates every tool the model may see,
    # discovered ones included.
    assert ui._tool_names == [
        "shell_start",
        "shell_execute",
        "shell_list",
        "shell_exit",
    ]
    assert "shell_execute" in ui._system_prompt


def test_shell_tools_can_be_disabled():
    connection = _connection()
    ui = ChatUI(_provider(), "some-model", connection, shell_tools=False)
    assert ui._tool_names == []
    assert ui._system_prompt == ""
    assert not connection.session.action_registry.is_registered("shell_execute")


def test_command_defines_the_no_shell_tools_flag():
    parser = argparse.ArgumentParser()
    CHAT_COMMAND.configure(parser)

    assert parser.parse_args([]).no_shell_tools is False
    assert parser.parse_args(["--no-shell-tools"]).no_shell_tools is True


def test_command_defines_the_gateway_flag():
    parser = argparse.ArgumentParser()
    CHAT_COMMAND.configure(parser)

    # Absent by default, which is what selects "join a running one, else start
    # one in this process".
    assert parser.parse_args([]).gateway is None
    assert (
        parser.parse_args(["--gateway", "ws://host:8011/a11"]).gateway
        == "ws://host:8011/a11"
    )


def test_command_defines_voice_flags():
    parser = argparse.ArgumentParser()
    CHAT_COMMAND.configure(parser)

    defaults = parser.parse_args([])
    assert defaults.no_voice is False
    assert defaults.voice_model == "tiny.en"
    disabled = parser.parse_args(["--no-voice", "--voice-model", "base"])
    assert disabled.no_voice is True
    assert disabled.voice_model == "base"


def test_repeated_header_flag_collects_key_value_pairs():
    parser = argparse.ArgumentParser()
    CHAT_COMMAND.configure(parser)

    args = parser.parse_args(
        [
            "--header",
            "x-a11-llm-base-url",
            "http://192.168.1.209:11434",
            "--header",
            "x-custom",
            "value",
        ]
    )
    assert args.headers == [
        ["x-a11-llm-base-url", "http://192.168.1.209:11434"],
        ["x-custom", "value"],
    ]


def test_provider_and_model_flags_override_positionals():
    parser = argparse.ArgumentParser()
    CHAT_COMMAND.configure(parser)

    args = parser.parse_args(
        [
            "claude",
            "old-model",
            "--provider",
            "ollama",
            "--model",
            "new-model",
        ]
    )
    assert (args.provider or args.backend) == "ollama"
    assert (args.model_flag or args.model) == "new-model"


def test_extra_headers_are_stored_on_the_ui():
    ui = ChatUI(
        _provider(),
        "some-model",
        _connection(),
        shell_tools=False,
        extra_headers=[("x-a11-llm-base-url", "http://host:11434")],
    )
    assert ui._extra_headers == [("x-a11-llm-base-url", "http://host:11434")]


def test_voice_can_be_disabled_without_initialising_it():
    ui = ChatUI(_provider(), "some-model", _connection(), voice=False)
    assert ui._voice_enabled is False
    assert ui._recognizer is None
