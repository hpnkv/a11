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

"""``a11 chat`` wiring for the shell tools (offline).

Checks that the CLI turns the shell tools on by default and that
``--no-shell-tools`` turns them off -- without touching the network.
"""

import argparse
import asyncio
import json

import a11
import pytest
from prompt_toolkit.document import Document
from a11.cli.backends import PROVIDERS
from a11.cli.chat_settings import ChatSettings, ChatSettingsStore
from a11.cli.chat_ui import ChatUI, _ChatCompleter
from a11.cli.commands.chat import CHAT_COMMAND
from a11.cli.commands.gateway import GATEWAY_COMMAND
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
    assert "typed `web-fetch` action" in ui._system_prompt
    assert "use it for HTTP retrieval" in (
        connection.session.action_registry.get_schema(
            "shell_execute"
        ).description
    )
    assert "do not use curl, wget" in (
        connection.session.action_registry.get_schema("shell_execute")
        .inputs["command"]
        .description
    )
    assert (
        "1000 output lines" in registry.get_schema("shell_execute").description
    )
    assert "131072" in (
        registry.get_schema("shell_execute").inputs["parameters"].json_schema
    )
    assert "capped at 1000 lines and 131072 UTF-8 bytes" in ui._system_prompt


def test_shell_tools_can_be_disabled():
    connection = _connection()
    ui = ChatUI(_provider(), "some-model", connection, shell_tools=False)
    assert ui._tool_names == []
    assert ui._system_prompt == ""
    assert not connection.session.action_registry.is_registered("shell_execute")


def test_coding_tui_does_not_install_filesystem_tools_on_the_client(tmp_path):
    connection = _connection()
    ChatUI(
        _provider(),
        "some-model",
        connection,
        coding=True,
        voice=False,
        settings_store=ChatSettingsStore(tmp_path),
    )

    assert not connection.session.action_registry.is_registered("read_file")
    assert not connection.session.action_registry.is_registered("run_command")


def test_chat_completes_commands_providers_models_and_permission_levels():
    completer = _ChatCompleter()

    def completions(text):
        return [
            item.text
            for item in completer.get_completions(Document(text), None)
        ]

    assert "/model" in completions("/mod")
    assert "gpt" in completions("/model g")
    assert PROVIDERS["gpt"].default_model in completions("/model gpt ")
    assert completions("/approval a") == ["auto"]
    assert "temperature" in completions("/config temp")


def test_chat_settings_preserve_model_and_provider_options(tmp_path):
    store = ChatSettingsStore(tmp_path)
    store.save(
        ChatSettings(
            provider="gpt",
            model="gpt-test",
            provider_configs={"gpt": {"temperature": 0.2}},
        )
    )

    assert store.load() == ChatSettings(
        provider="gpt",
        model="gpt-test",
        provider_configs={"gpt": {"temperature": 0.2}},
    )
    assert json.loads(store.path.read_text())["model"] == "gpt-test"
    assert store.path.stat().st_mode & 0o777 == 0o600


@pytest.mark.parametrize(
    ("provider", "native_name"),
    [
        ("gpt", "max_completion_tokens"),
        ("gemini", "max_output_tokens"),
        ("ollama", "num_predict"),
        ("claude", "max_tokens"),
        ("vllm", "max_tokens"),
    ],
)
def test_portable_max_tokens_reaches_each_provider_schema(
    tmp_path, provider, native_name
):
    ui = ChatUI(
        PROVIDERS[provider],
        "some-model",
        _connection(),
        coding=False,
        voice=False,
        settings_store=ChatSettingsStore(tmp_path),
    )
    ui._provider_configs[provider] = {"max_tokens": 321}

    assert ui._provider_config() == {native_name: 321}


@pytest.mark.asyncio
async def test_messages_queue_while_a_turn_is_active(tmp_path):
    ui = ChatUI(
        _provider(),
        "some-model",
        _connection(),
        voice=False,
        settings_store=ChatSettingsStore(tmp_path),
    )
    ui._active_turn = asyncio.create_task(asyncio.sleep(10))
    try:
        assert await ui._handle("follow-up")
        assert await ui._message_queue.get() == "follow-up"
    finally:
        ui._active_turn.cancel()
        await asyncio.gather(ui._active_turn, return_exceptions=True)


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


def test_gateway_serving_flags_work_before_and_after_run_subcommand():
    parser = argparse.ArgumentParser()
    GATEWAY_COMMAND.configure(parser)

    before = parser.parse_args(["--a11-port", "49321", "run"])
    after = parser.parse_args(["run", "--a11-port", "49322"])

    assert before.a11_port == 49321
    assert after.a11_port == 49322


def test_command_defines_coding_workspace_and_policy_flags():
    parser = argparse.ArgumentParser()
    CHAT_COMMAND.configure(parser)

    args = parser.parse_args(
        [
            "--task",
            "fix it",
            "--cwd",
            "project",
            "--add-dir",
            "shared",
            "--approval",
            "auto",
            "--sandbox",
            "workspace-write",
            "--non-interactive",
        ]
    )

    assert args.task == "fix it"
    assert args.cwd == "project"
    assert args.add_dir == ["shared"]
    assert args.approval == "auto"
    assert args.sandbox == "workspace-write"
    assert args.non_interactive


def test_command_defines_voice_flags():
    parser = argparse.ArgumentParser()
    CHAT_COMMAND.configure(parser)

    defaults = parser.parse_args([])
    assert defaults.no_voice is False
    assert defaults.voice_model == "tiny.en"
    disabled = parser.parse_args(["--no-voice", "--voice-model", "base"])
    assert disabled.no_voice is True
    assert disabled.voice_model == "base"


def test_chat_runtime_logs_require_an_explicit_level():
    parser = argparse.ArgumentParser()
    CHAT_COMMAND.configure(parser)

    assert parser.parse_args([]).log_level is None
    assert parser.parse_args(["--log-level", "debug"]).log_level == "debug"


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
