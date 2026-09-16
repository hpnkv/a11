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
from contextlib import asynccontextmanager
import json

import a11
import pytest
from prompt_toolkit.document import Document
from a11.cli.backends import PROVIDERS
from a11.cli.chat_settings import ChatSettings, ChatSettingsStore
from a11.cli.chat_ui import ChatUI, _ChatCompleter, _WELCOME, _welcome_banner
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


def test_welcome_banner_is_an_ascii_a11_wordmark():
    assert _WELCOME.isascii()
    lines = _WELCOME.splitlines()
    assert len(lines) == 16
    wordmark = [line[44:].rstrip() for line in lines[4:12]]
    assert [len(line) - len(line.lstrip()) for line in wordmark] == list(
        reversed(range(8))
    )
    assert all(line[23:26] == "###" for line in wordmark)
    assert all(line[32:35] == "###" for line in wordmark)
    banner = _welcome_banner()
    assert banner.plain == _WELCOME
    assert {span.style for span in banner.spans} >= {
        "bold #8b83ff",
        "bold #b4e6ed",
        "bold #43c6b5",
    }


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
        connection.session.action_registry
        .get_schema("shell_execute")
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


def test_footer_shows_colored_model_effort_workdir_and_task(tmp_path):
    ui = ChatUI(
        _provider(),
        "model-name",
        _connection(),
        cwd=str(tmp_path),
        task="Change the footer",
        voice=False,
    )
    ui._provider_configs[_provider().name] = {"reasoning_effort": "medium"}

    fragments = ui._bottom_toolbar().__pt_formatted_text__()
    text = "".join(fragment[1] for fragment in fragments)
    styles = " ".join(fragment[0] for fragment in fragments)

    assert text == f"model-name · medium · {tmp_path} · Change the footer"
    assert "ansiyellow" in styles
    assert "ansigreen" in styles
    assert "ansicyan" in styles


def test_composer_prompt_is_only_a_chevron():
    ui = ChatUI(_provider(), "model-name", _connection(), voice=False)

    fragments = ui._prompt_message().__pt_formatted_text__()

    assert "".join(fragment[1] for fragment in fragments) == "› "


def test_working_and_waiting_states_are_outside_the_footer():
    ui = ChatUI(_provider(), "model", _connection(), voice=False)

    def footer():
        return "".join(
            x[1] for x in ui._bottom_toolbar().__pt_formatted_text__()
        )

    assert "Working" not in footer()
    assert ui._activity_message() is None
    ui._llm_running = True
    assert "Working" not in footer()
    assert "Working" in str(ui._activity_message())
    ui._pending_user_inputs["q1"] = {"question": "Choose"}
    assert "Waiting for input" in str(ui._activity_message())
    ui._pending_user_inputs.clear()
    ui._llm_running = False
    assert "Working" not in footer()
    assert ui._activity_message() is None


@pytest.mark.asyncio
async def test_turn_displays_live_and_final_tokens_and_clear_resets(
    tmp_path, monkeypatch
):
    from a11.sdk.llm import Interaction, UsageMetadata
    from a11.cli import chat_ui

    ui = ChatUI(
        _provider(), "model", _connection(), voice=False, shell_tools=False,
        settings_store=ChatSettingsStore(tmp_path),
    )
    printed = []
    monkeypatch.setattr(
        ui, "_print", lambda text, **kwargs: printed.append(text)
    )
    monkeypatch.setattr(ui, "_report_missing_sdk", lambda: False)

    async def run_turn(connection, history, user, tools, config, reducer):
        assert ui._tokens.output_label == "~0 tok"
        reducer.on_text("A" * 400)
        assert "~100 tok output" in str(ui._activity_message())
        result = Interaction(usage_metadata=UsageMetadata(
            input_tokens=172000, output_tokens=120, total_tokens=172120,
        ))
        reducer.on_interaction(result)
        assert ui._screen._context_usage() == "172.1K tok"
        assert "120 tok output" in str(ui._activity_message())
        reducer.end_turn()
        return [result]

    monkeypatch.setattr(chat_ui, "run_turn", run_turn)
    await ui._turn("hello")
    assert printed[-1] == "  120 tok output"
    assert ui._activity_message() is None
    assert ui._screen._context_usage() == "172.1K tok"
    await ui._handle("/clear")
    assert ui._screen._context_usage() == "— tok"


@pytest.mark.asyncio
async def test_sandbox_command_configures_gateway(tmp_path, monkeypatch):
    ui = ChatUI(
        _provider(),
        "model",
        _connection(),
        coding=True,
        cwd=str(tmp_path),
        voice=False,
        settings_store=ChatSettingsStore(tmp_path),
    )
    calls = []

    async def call(schema, inputs):
        calls.append((schema.name, inputs))
        if schema.name == "configure_coding_agent":
            return {"sandbox_ceiling": "unrestricted"}
        return {
            "sandbox": "unrestricted",
            "tool_names": [],
            "system_prompt": "unrestricted",
        }

    monkeypatch.setattr(ui, "_call_gateway_action", call)
    assert await ui._handle("/sandbox unrestricted")
    assert calls[0] == (
        "configure_coding_agent",
        {
            "sandbox_mode": "unrestricted",
        },
    )
    assert ui._coding_status["sandbox"] == "unrestricted"
    assert ui._system_prompt == "unrestricted"


@pytest.mark.asyncio
async def test_chat_supplies_current_workdir_to_gateway_startup(
    tmp_path, monkeypatch
):
    from a11.cli import chat_ui

    monkeypatch.chdir(tmp_path)
    captured = []

    @asynccontextmanager
    async def connect(url, *, local_config):
        captured.append((url, local_config))
        yield _connection()

    async def run(ui):
        return 0

    monkeypatch.setattr(chat_ui, "open_gateway", connect)
    monkeypatch.setattr(chat_ui.ChatUI, "run", run)
    assert (
        await chat_ui.run_chat("ollama", "model", coding=True, voice=False) == 0
    )
    assert captured[0][0] is None
    assert captured[0][1].coding_cwd == str(tmp_path.resolve())
    assert captured[0][1].coding_tools


@pytest.mark.asyncio
async def test_work_indicator_clears_when_model_call_is_cancelled(
    tmp_path, monkeypatch
):
    from a11.cli import chat_ui

    ui = ChatUI(
        _provider(),
        "model",
        _connection(),
        voice=False,
        settings_store=ChatSettingsStore(tmp_path),
    )
    started = asyncio.Event()

    async def run_turn(*args):
        started.set()
        await asyncio.Future()

    monkeypatch.setattr(ui, "_report_missing_sdk", lambda: False)
    monkeypatch.setattr(chat_ui, "run_turn", run_turn)
    task = asyncio.create_task(ui._turn("hello"))
    await started.wait()
    assert ui._llm_running
    task.cancel()
    await asyncio.gather(task, return_exceptions=True)
    assert not ui._llm_running
    assert not ui._pending_user_inputs


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


@pytest.mark.asyncio
async def test_no_banner_shows_compact_welcome(tmp_path, monkeypatch):
    parser = argparse.ArgumentParser()
    CHAT_COMMAND.configure(parser)
    assert parser.parse_args([]).no_banner is False
    args = parser.parse_args(["--no-banner"])
    ui = ChatUI(
        _provider(),
        "test-model",
        _connection(),
        shell_tools=False,
        voice=False,
        cwd=str(tmp_path),
        no_banner=args.no_banner,
    )

    async def close_screen():
        pass

    monkeypatch.setattr(ui._screen, "run", close_screen)
    await ui.run()
    welcome = ui._screen._plain_transcript()
    assert "A11 Chat" in welcome
    assert "test-model" in welcome
    assert tmp_path.name in welcome
    assert "/help for commands" in welcome
    assert "Commands:" not in welcome
    assert "########" not in welcome


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

    args = parser.parse_args([
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
    ])

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

    args = parser.parse_args([
        "--header",
        "x-a11-llm-base-url",
        "http://192.168.1.209:11434",
        "--header",
        "x-custom",
        "value",
    ])
    assert args.headers == [
        ["x-a11-llm-base-url", "http://192.168.1.209:11434"],
        ["x-custom", "value"],
    ]


def test_provider_and_model_flags_override_positionals():
    parser = argparse.ArgumentParser()
    CHAT_COMMAND.configure(parser)

    args = parser.parse_args([
        "claude",
        "old-model",
        "--provider",
        "ollama",
        "--model",
        "new-model",
    ])
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
