# Copyright 2026 The A11 Authors.

"""``a11 chat`` wiring for the shell tools (offline).

Checks that the CLI turns the shell tools on by default and that
``--no-shell-tools`` turns them off -- without touching the network.
"""

import argparse

from a11.cli.backends import PROVIDERS
from a11.cli.chat_ui import ChatUI
from a11.cli.commands.chat import CHAT_COMMAND


def _provider():
    return PROVIDERS["ollama"]


def test_shell_tools_are_on_by_default():
    ui = ChatUI(_provider(), "some-model", shell_tools=True)
    assert ui._registry is not None
    assert [t["name"] for t in ui._tool_definitions] == [
        "shell_start",
        "shell_execute",
        "shell_list",
        "shell_exit",
    ]
    assert ui._allowed_actions == "shell_.*"
    assert "shell_execute" in ui._system_prompt


def test_shell_tools_can_be_disabled():
    ui = ChatUI(_provider(), "some-model", shell_tools=False)
    assert ui._registry is None
    assert ui._tool_definitions == []
    assert ui._allowed_actions == ""
    assert ui._system_prompt == ""


def test_command_defines_the_no_shell_tools_flag():
    parser = argparse.ArgumentParser()
    CHAT_COMMAND.configure(parser)

    assert parser.parse_args([]).no_shell_tools is False
    assert parser.parse_args(["--no-shell-tools"]).no_shell_tools is True


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
        ["claude", "old-model", "--provider", "ollama", "--model", "new-model"]
    )
    assert (args.provider or args.backend) == "ollama"
    assert (args.model_flag or args.model) == "new-model"


def test_extra_headers_are_stored_on_the_ui():
    ui = ChatUI(
        _provider(),
        "some-model",
        shell_tools=False,
        extra_headers=[("x-a11-llm-base-url", "http://host:11434")],
    )
    assert ui._extra_headers == [("x-a11-llm-base-url", "http://host:11434")]
