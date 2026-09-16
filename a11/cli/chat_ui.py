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

"""Interactive chat loop for ``a11 chat``.

`ChatUI` is presentation and nothing else. The processing -- the LLM call, tool
dispatch, conversation persistence -- happens on a *gateway*: either one the
user is already running, or one started inside this process and reached over an
in-memory stream pair. Either way the turn is driven by
[run_turn][a11.client.turn.run_turn], the same loop every A11 client uses, and
rendered from the [PresentationBlock][a11.sdk.presentation.PresentationBlock]s
that loop produces.

So what is left here: read a line with `prompt_toolkit` (async-native, no
opinion on how we hold state), draw blocks with `rich`, handle the slash
commands, and splice speech into the prompt. Conversation history is still just
a flat ``list[Interaction]``.

Coding tools, workspace access, policy, and sandboxing live on the gateway.
This module discovers that agent and remains a presentation/input client.
"""

from __future__ import annotations

import asyncio
import datetime
import html
import json
from collections.abc import Callable
from pathlib import Path
from typing import TYPE_CHECKING

import a11
from prompt_toolkit.completion import Completer, Completion
from prompt_toolkit.document import Document
from prompt_toolkit.formatted_text import HTML
from prompt_toolkit.history import FileHistory, InMemoryHistory
from rich.console import Console
from rich.panel import Panel
from rich.syntax import Syntax
from rich.table import Table
from rich.text import Text

from a11 import observability
from a11.cli.backends import (
    PROVIDERS,
    Provider,
    make_user_interaction,
    normalize_provider_name,
)
from a11.cli.coding_agent.session import SessionRecord, SessionStore
from a11.cli.coding_agent.tools import (
    CODING_AGENT_INFO_SCHEMA,
    CONFIGURE_CODING_AGENT_SCHEMA,
    FILE_DIFF_SCHEMA,
    RESPOND_USER_INPUT_SCHEMA,
)
from a11.cli.chat_settings import ChatSettings, ChatSettingsStore
from a11.cli.chat_screen import ChatScreen
from a11.cli.chat_tokens import ChatTokens
from a11.cli.presentation_render import render_blocks
from a11.client.connection import GatewayConnection, open_gateway
from a11.client.turn import TurnConfig, run_turn
from a11.gateway.config import GatewayConfig
from a11.sdk.interact_with_llm import load_provider
from a11.sdk.llm import Interaction
from a11.sdk.presentation import BlockKind, PresentationReducer
from a11.status import Status, StatusCode, StatusException

if TYPE_CHECKING:
    from a11.sdk.audio import SpeechRecognizer

_WELCOME_RING = (
    "      #######      ",
    "   #####   #####   ",
    "  ###         ###  ",
    " ###           ### ",
    "  ###         ###  ",
    "   #####   #####   ",
    "      #######      ",
)
_WELCOME_WORD = (
    "       ###             ###      ###",
    "      #####          #####    #####",
    "     ### ###        ## ###   ## ###",
    "    ###   ###          ###      ###",
    "   ###     ###         ###      ###",
    "  #############        ###      ###",
    " ###         ###       ###      ###",
    "###           ###      ###      ###",
)
_WELCOME_ARROW = {
    6: "        ###",
    7: "        ###",
    8: "        ###",
    9: "        ###",
    10: "        ###         ###",
    11: "         ####         ###",
    12: "           #################",
    13: "             ############",
    14: "                    ###",
}


def _welcome_banner() -> Text:
    """Render two nodes and an elbow arrow beside the centred wordmark."""
    banner = Text()
    for row in range(16):
        node = _WELCOME_RING[row] if row < 7 else ""
        if row >= 9:
            node = " " * 20 + _WELCOME_RING[row - 9]
        arrow = _WELCOME_ARROW.get(row, "").ljust(39)
        for column, char in enumerate(node.ljust(39)):
            if arrow[column] != " ":
                banner.append(arrow[column], style="bold #43c6b5")
            else:
                if (
                    row >= 9
                    and column >= 20
                    and any(
                        arrow[x] != " "
                        for x in range(max(0, column - 1), min(39, column + 2))
                    )
                ):
                    char = " "
                banner.append(char, style="bold #8b83ff")
        if 4 <= row < 12:
            banner.append("     ")
            banner.append(_WELCOME_WORD[row - 4], style="bold #b4e6ed")
        if row < 15:
            banner.append("\n")
    return banner


_WELCOME = _welcome_banner().plain

_HELP = (
    "Commands:\n"
    "  /model <claude|claude_code|codex|gemini|gpt|ollama|vllm> [model]  switch"
    " backend (and optionally model)\n"
    "  /clear                                                  forget the"
    " conversation so far\n"
    "  /status                                                 workspace,"
    " policy, changes, and checks\n"
    "  /diff                                                   show the"
    " current agent diff\n"
    "  /approval <suggest|auto>                                change effect"
    " permissions\n"
    "  /sandbox <read-only|workspace-write|unrestricted>        change"
    " sandbox\n"
    "  /config [key [JSON]|clear]                              inspect or set"
    " provider options\n"
    "  /cancel                                                 stop the active"
    " turn\n"
    "  /help, /?                                               show this"
    " help\n"
    "  /exit, /quit                                            leave\n"
)

_COMMANDS = (
    "/model",
    "/clear",
    "/status",
    "/diff",
    "/approval",
    "/sandbox",
    "/config",
    "/cancel",
    "/help",
    "/exit",
    "/quit",
)


class _ChatCompleter(Completer):
    """Context-aware completion for commands, providers, and models."""

    def __init__(
        self, choices: Callable[[], tuple[str, ...]] | None = None
    ) -> None:
        self._choices = choices or (lambda: ())

    def get_completions(self, document: Document, complete_event):
        text = document.text_before_cursor
        if not text.startswith("/"):
            if self._choices():
                yield from self._matching(text, self._choices())
            return
        parts = text.split()
        trailing_space = text.endswith(" ")
        if len(parts) <= 1 and not trailing_space:
            yield from self._matching(text, _COMMANDS)
            return
        command = parts[0]
        current = "" if trailing_space else parts[-1]
        if command == "/model":
            if len(parts) == 1 or (len(parts) == 2 and not trailing_space):
                yield from self._matching(current, tuple(PROVIDERS))
                return
            provider_name = normalize_provider_name(parts[1])
            provider = PROVIDERS.get(provider_name)
            if provider is not None and provider.default_model:
                yield from self._matching(current, (provider.default_model,))
        elif command == "/approval":
            yield from self._matching(current, ("suggest", "auto"))
        elif command == "/sandbox":
            yield from self._matching(
                current, ("read-only", "workspace-write", "unrestricted")
            )
        elif command == "/config" and len(parts) <= 2:
            yield from self._matching(
                current,
                (
                    "temperature",
                    "max_tokens",
                    "top_p",
                    "reasoning_effort",
                    "seed",
                    "clear",
                ),
            )

    @staticmethod
    def _matching(prefix: str, choices: tuple[str, ...]):
        for choice in choices:
            if choice.startswith(prefix):
                yield Completion(choice, start_position=-len(prefix))


class _Repaint:
    """A `PresentationSink` that calls one function on any change.

    The terminal's answer to incremental rendering: rather than track which
    block moved, redraw the turn and let `rich` diff it.
    """

    def __init__(self, paint, on_block=None) -> None:
        self._paint = paint
        self._on_block = on_block

    def on_block_opened(self, block) -> None:
        if self._on_block is not None:
            self._on_block(block)
        self._paint()

    def on_block_appended(self, block, delta: str) -> None:
        if self._on_block is not None:
            self._on_block(block)
        self._paint()

    def on_block_closed(self, block) -> None:
        if self._on_block is not None:
            self._on_block(block)
        self._paint()


class ChatUI:
    """A single interactive chat session over a swappable LLM backend."""

    def __init__(
        self,
        provider: Provider,
        model: str,
        connection: GatewayConnection,
        *,
        verbose: bool = False,
        shell_tools: bool = True,
        voice: bool = True,
        voice_model: str = "tiny.en",
        extra_headers: list[tuple[str, str]] | None = None,
        coding: bool = False,
        cwd: str = ".",
        add_dirs: tuple[str, ...] = (),
        approval: str = "ask",
        sandbox: str = "workspace-write",
        task: str | None = None,
        resume: str | None = None,
        max_turns: int = 50,
        timeout_seconds: int = 600,
        non_interactive: bool = False,
        no_banner: bool = False,
        settings_store: ChatSettingsStore | None = None,
    ) -> None:
        self._connection = connection
        self._provider = provider
        self._model = model
        self._verbose = verbose
        # Extra headers set on every interact_with_* call, applied last so they
        # override the defaults for the same key (e.g. the base URL).
        self._extra_headers = list(extra_headers or [])
        self._history: list[Interaction] = []
        self._console = Console()
        self._settings_store = settings_store or ChatSettingsStore()
        self._provider_configs = self._settings_store.load().provider_configs
        try:
            self._settings_store.root.mkdir(
                parents=True, exist_ok=True, mode=0o700
            )
            history = FileHistory(str(self._settings_store.history_path))
        except OSError:
            history = InMemoryHistory()
        self._input_history = history
        self._traceparent: str | None = None
        self._chat_span: observability.Span | None = None
        self._voice_enabled = voice
        self._voice_model = voice_model
        self._voice_accepting = False
        self._recognizer: SpeechRecognizer | None = None
        self._coding = coding
        self._coding_status: dict[str, object] = {}
        self._cwd = cwd
        self._add_dirs = add_dirs
        self._approval = approval
        self._sandbox = sandbox
        self._initial_task = task
        self._non_interactive = non_interactive
        self._no_banner = no_banner
        self._max_turns = max_turns
        self._timeout_seconds = timeout_seconds
        self._turn_count = 0
        self._session_store: SessionStore | None = None
        self._session_record: SessionRecord | None = None
        self._message_queue: asyncio.Queue[str] = asyncio.Queue()
        self._turn_worker: asyncio.Task[None] | None = None
        self._active_turn: asyncio.Task[None] | None = None
        self._llm_running = False
        self._current_task = task or ""
        self._interactive_started = False
        self._pending_user_inputs: dict[str, dict[str, object]] = {}
        self._tokens = ChatTokens()

        # Shell tools: their definitions, and the system prompt that teaches the
        # model to use them. The Actions themselves are registered on the
        # connection's session registry, because the gateway reverse-dispatches
        # the model's calls back to *this* process to run them.
        self._tool_names: list[str] = []
        self._system_prompt = ""
        if coding:
            self._session_store = SessionStore()
            if resume is not None:
                self._session_record = self._session_store.load(resume or None)
                if cwd == ".":
                    cwd = self._session_record.workspace
                self._history = list(self._session_record.history)
            if self._session_record is None:
                self._session_record = SessionRecord(
                    workspace=str(Path(cwd).expanduser().resolve()),
                    provider=provider.name,
                    model=model,
                    task=task or "",
                )
        elif shell_tools:
            self._enable_shell_tools()
        for interaction in self._history:
            if interaction.usage_metadata is not None:
                self._tokens.restore_context(interaction.usage_metadata)
        self._screen = ChatScreen(
            history=self._input_history,
            completer=_ChatCompleter(self._input_choices),
            prompt=self._prompt_message,
            toolbar=self._bottom_toolbar,
            submit=self._handle,
            cancel=self._cancel_active_turn,
            question=self._current_question,
            working=lambda: self._llm_running or self._active_turn is not None,
            activity=self._activity_message,
            context_usage=lambda: self._tokens.context_label,
        )
        # Kept as a compatibility alias for speech input and integrations that
        # previously received PromptSession-like buffer/application access.
        self._session = self._screen

    def _enable_shell_tools(self) -> None:
        """Register this side's shell tools, for the gateway to find.

        They are the client's tools, not the gateway's: `a11 chat` exists to run
        commands in the user's own shell and working directory, and a gateway --
        which may be shared, or in a container -- is the wrong place for that.
        So nothing asks the gateway for its `shell_*` actions; these run here.

        The gateway calls ``__list_actions__`` on this session and proxies the
        returned schemas. No separate tool announcement is required.
        """
        from a11.sdk import bash

        registry = self._connection.session.action_registry
        bash.register(registry)
        self._tool_names = [schema.name for schema, _ in bash.SHELL_ACTIONS]
        # Chat now runs inside a Session, so shells are scoped to it and the
        # per-session cap is the one the model should be told about.
        self._system_prompt = bash.get_system_prompt()

    # -- lifecycle ---------------------------------------------------------

    async def run(self) -> int:
        """Run the read-eval-print loop until the user exits. Returns 0."""
        self._interactive_started = not self._non_interactive
        # One "A11 Chat" span for the whole session; each turn's interaction is
        # parented to it (via its traceparent), so turns nest under it.
        self._chat_span = observability.start_span("A11 Chat", kind="server")
        self._traceparent = self._chat_span.traceparent()
        compact_welcome = self._no_banner and not self._non_interactive
        if not self._non_interactive and not self._no_banner:
            self._print(_welcome_banner())
            self._print("")
            self._print(_HELP, style="dim", markup=False)
        if self._coding:
            await self._refresh_coding_agent()
        if compact_welcome:
            self._print(self._compact_welcome())
        else:
            self._print_status()
            self._print(
                f"gateway: [bold]{self._connection.description}[/]",
                style="dim",
                highlight=False,
            )
        if self._provider.api_key_env and not self._provider.api_key():
            self._warn_missing_key()
        self._report_missing_sdk()
        if self._coding and not compact_welcome:
            self._print(
                f"workspace: [bold]{self._coding_status.get('root', '?')}[/] ·"
                f" approval: [bold]"
                f"{self._coding_status.get('approval_mode', '?')}[/] · sandbox:"
                f" [bold]{self._coding_status.get('sandbox', '?')}[/]",
                style="dim",
                highlight=False,
            )
        # Nothing to announce: the gateway asks this session what it serves the
        # first time a turn needs tools, and reverse-dispatches the model's
        # calls back here to run them.
        if self._voice_enabled and not self._non_interactive:
            await self._prepare_voice()

        self._chat_span.set_input(
            f"Interactive chat started at {datetime.datetime.now().isoformat()}"
        )

        try:
            if self._initial_task:
                if self._non_interactive:
                    await self._turn(self._initial_task)
                    return 0
                await self._message_queue.put(self._initial_task)
            self._turn_worker = asyncio.create_task(self._run_turn_queue())
            await self._start_voice_input()
            await self._screen.run()
        except StatusException as exc:
            self._apply_span_error(self._chat_span, exc.status)
            self._chat_span.set_output(exc.status.model_dump())
        except Exception as exc:
            status = Status(code=StatusCode.INTERNAL, message=str(exc))
            self._apply_span_error(self._chat_span, status)
            self._chat_span.set_output(status.model_dump())
            raise status.to_exception() from exc
        else:
            self._chat_span.set_status("ok")
            self._chat_span.set_output(
                "Interactive chat ended at"
                f" {datetime.datetime.now().isoformat()}"
            )
        finally:
            await self._stop_voice_input()
            if self._turn_worker is not None:
                self._turn_worker.cancel()
                await asyncio.gather(self._turn_worker, return_exceptions=True)
            self._chat_span.end()

        self._print("bye", style="dim")
        return 0

    # -- command handling --------------------------------------------------

    def _compact_welcome(self) -> Panel:
        """Return a small startup card with the active model and directory."""
        cwd = Path(str(self._coding_status.get("cwd", self._cwd)))
        cwd = cwd.expanduser().resolve()
        try:
            relative = cwd.relative_to(Path.home())
            directory = "~" if str(relative) == "." else f"~/{relative}"
        except ValueError:
            directory = str(cwd)
        parts = directory.split("/")
        while len(directory) > 60 and len(parts) > 2:
            parts.pop(1)
            directory = parts[0] + "/.../" + "/".join(parts[1:])
        content = Text("A11 Chat\n\n", style="bold")
        content.append("model:     ", style="dim")
        content.append(self._model, style="bold #b4e6ed")
        content.append("   /model to change\n", style="#43c6b5")
        content.append("directory: ", style="dim")
        content.append(directory, style="")
        content.append("\n\n/help for commands", style="dim")
        return Panel.fit(content, border_style="dim", padding=(0, 1))

    def _prompt_message(self) -> HTML:
        """Return a simple composer chevron or pending-answer prompt."""
        return HTML("› ")

    def _current_question(self) -> dict | None:
        if not self._pending_user_inputs:
            return None
        request_id = next(iter(self._pending_user_inputs))
        return self._pending_user_inputs[request_id] | {
            "request_id": request_id
        }

    def _input_choices(self) -> tuple[str, ...]:
        if not self._pending_user_inputs:
            return ()
        request = next(iter(self._pending_user_inputs.values()))
        options = request.get("options", [])
        if not isinstance(options, list):
            return ()
        return tuple(
            str(option.get("label", ""))
            for option in options
            if isinstance(option, dict) and option.get("label")
        )

    def _activity_message(self) -> HTML | None:
        if self._pending_user_inputs:
            return HTML(
                "<ansiyellow>• Waiting for input</ansiyellow>"
                f" <ansigray>· {self._tokens.output_label} output</ansigray>"
            )
        if self._llm_running:
            return HTML(
                "<ansicyan>• Working</ansicyan>"
                f" <ansigray>· {self._tokens.output_label} output</ansigray>"
                " <ansigray>· Ctrl+C to interrupt</ansigray>"
            )
        return None

    def _bottom_toolbar(self) -> HTML:
        """Show model, effort, location, and task on the terminal gutter."""
        config = self._provider_configs.get(self._provider.name, {})
        effort = config.get("reasoning_effort") or config.get("effort")
        cwd = Path(str(self._coding_status.get("cwd", self._cwd))).expanduser()
        try:
            resolved = cwd.resolve()
            relative = resolved.relative_to(Path.home())
            workdir = f"~/{relative}" if str(relative) != "." else "~"
        except (OSError, ValueError):
            workdir = str(cwd)
        task = (
            self._session_record.task
            if self._session_record is not None and self._session_record.task
            else self._current_task
        )
        model = html.escape(self._model or "default model")
        fields = [f"<ansiyellow>{model}</ansiyellow>"]
        if effort:
            fields.append(
                f"<ansiyellow>{html.escape(str(effort))}</ansiyellow>"
            )
        fields.append(f"<ansigreen>{html.escape(workdir)}</ansigreen>")
        if task:
            fields.append(f"<ansicyan>{html.escape(task)}</ansicyan>")
        separator = " <ansigray>·</ansigray> "
        return HTML(separator.join(fields))

    async def _run_turn_queue(self) -> None:
        """Run submitted messages sequentially while input remains available."""
        while True:
            text = await self._message_queue.get()
            self._current_task = text
            turn = asyncio.create_task(self._turn(text))
            self._active_turn = turn
            self._screen.invalidate()
            try:
                await turn
            except asyncio.CancelledError:
                if asyncio.current_task().cancelling():
                    raise
                self._print("(turn cancelled)", style="yellow")
            finally:
                self._active_turn = None
                self._message_queue.task_done()
                self._screen.invalidate()

    def _cancel_active_turn(self) -> None:
        """Cancel the current model turn without disturbing typed input."""
        if self._active_turn is not None:
            self._active_turn.cancel()

    async def _handle(self, text: str) -> bool:
        """Handle one line of input. Returns False to end the session."""
        lowered = text.casefold()
        if self._pending_user_inputs and not text.startswith("/"):
            request_id = next(iter(self._pending_user_inputs))
            try:
                await self._call_gateway_action(
                    RESPOND_USER_INPUT_SCHEMA,
                    {"request_id": request_id, "answer": text},
                )
            except StatusException as error:
                self._print(
                    f"answer not accepted: {error.status.message}",
                    style="red",
                    markup=False,
                )
                return True
            self._pending_user_inputs.pop(request_id, None)
            self._screen.invalidate()
            return True
        if lowered in ("/exit", "/quit"):
            return False
        if lowered in ("/help", "/?"):
            self._print(_HELP, style="dim", markup=False)
            return True
        if lowered == "/clear":
            self._history.clear()
            self._tokens = ChatTokens()
            self._print("(conversation cleared)", style="dim")
            return True
        if lowered == "/status":
            await self._print_agent_status()
            return True
        if lowered == "/diff":
            await self._print_agent_diff()
            return True
        if lowered == "/cancel":
            if self._active_turn is None:
                self._print("(no active turn)", style="dim")
            else:
                self._active_turn.cancel()
            return True
        if text.startswith("/model"):
            self._switch_model(text.split())
            return True
        if text.startswith("/approval"):
            await self._switch_approval(text.split())
            return True
        if text.startswith("/sandbox"):
            await self._switch_sandbox(text.split())
            return True
        if text.startswith("/config"):
            self._configure_provider(text)
            return True
        if text.startswith("/"):
            self._print(
                f"unknown command {text.split()[0]!r} — try /help",
                style="red",
                markup=False,
            )
            return True

        queued = (
            self._active_turn is not None or not self._message_queue.empty()
        )
        await self._message_queue.put(text)
        if queued:
            self._print("(message queued)", style="dim")
        return True

    async def _switch_approval(self, parts: list[str]) -> None:
        """Change effect approval for subsequent coding actions."""
        if not self._coding or len(parts) != 2:
            self._print(
                "usage: /approval <suggest|auto>",
                style="red",
                markup=False,
            )
            return
        if parts[1] not in {"suggest", "auto"}:
            self._print(
                "usage: /approval <suggest|auto>",
                style="red",
                markup=False,
            )
            return
        result = await self._call_gateway_action(
            CONFIGURE_CODING_AGENT_SCHEMA,
            {"approval_mode": parts[1]},
        )
        self._coding_status.update(result)
        self._print(
            f"approval: [bold]{result['approval_mode']}[/] · sandbox:"
            f" [bold]{result['sandbox_ceiling']}[/]",
            style="dim",
            highlight=False,
        )

    async def _switch_sandbox(self, parts: list[str]) -> None:
        """Set the gateway sandbox for subsequent coding actions."""
        modes = {"read-only", "workspace-write", "unrestricted"}
        if not self._coding or len(parts) != 2 or parts[1] not in modes:
            self._print(
                "usage: /sandbox <read-only|workspace-write|unrestricted>",
                style="red",
                markup=False,
            )
            return
        result = await self._call_gateway_action(
            CONFIGURE_CODING_AGENT_SCHEMA, {"sandbox_mode": parts[1]}
        )
        self._sandbox = str(result["sandbox_ceiling"])
        await self._refresh_coding_agent()
        self._print(
            f"Gateway sandbox: {self._sandbox} (subsequent calls)."
            + (
                " Host filesystem access; kernel process confinement is off."
                if self._sandbox == "unrestricted"
                else ""
            ),
            style="yellow",
            markup=False,
        )
        self._screen.invalidate()

    async def _call_gateway_action(
        self, schema: a11.ActionSchema, inputs: dict[str, object]
    ) -> dict[str, object]:
        """Call one gateway-owned coding control or inspection action."""
        call = (
            a11
            .Action(schema)
            .bind_node_map(self._connection.session.node_map)
            .bind_session(self._connection.session)
            .bind_stream(self._connection.stream)
        )
        await call.call()
        for name in schema.inputs:
            await call[name].finalize(inputs.get(name))
        result = await call["result"].consume(dict)
        await call.wait(a11.Duration.seconds(30))
        return result

    async def _refresh_coding_agent(self) -> None:
        """Discover agent instructions and capabilities from the gateway."""
        self._coding_status = await self._call_gateway_action(
            CODING_AGENT_INFO_SCHEMA, {}
        )
        self._tool_names = [
            str(name) for name in self._coding_status.get("tool_names", [])
        ]
        self._system_prompt = str(self._coding_status.get("system_prompt", ""))
        self._pending_user_inputs = {
            request["request_id"]: request
            for request in self._coding_status.get("pending_user_inputs", [])
            if isinstance(request, dict) and request.get("request_id")
        }

    async def _print_agent_status(self) -> None:
        if not self._coding:
            self._print_status()
            return
        await self._refresh_coding_agent()
        status = self._coding_status
        table = Table.grid(padding=(0, 2))
        table.add_column(style="dim", no_wrap=True)
        table.add_column()
        table.add_row("workspace", str(status["root"]))
        table.add_row("cwd", str(status["cwd"]))
        table.add_row("policy", str(status["approval_mode"]))
        table.add_row("sandbox", str(status["sandbox"]))
        table.add_row("changes", str(len(status["agent_changed_files"])))
        table.add_row("checks", str(len(status["checks"])))
        self._print(Panel(table, title="agent", title_align="left"))

    async def _print_agent_diff(self) -> None:
        if not self._coding:
            self._print("No coding workspace is active.", style="dim")
            return
        result = await self._call_gateway_action(FILE_DIFF_SCHEMA, {})
        diff = str(result.get("diff", ""))
        if diff:
            self._print(Syntax(diff, "diff", word_wrap=True))
        else:
            self._print("(no agent diff)", style="dim")

    def _switch_model(self, parts: list[str]) -> None:
        name = normalize_provider_name(parts[1]) if len(parts) > 1 else ""
        if name not in PROVIDERS:
            self._print(
                f"usage: /model <{'|'.join(PROVIDERS)}> [model]",
                style="red",
                markup=False,
            )
            return

        provider = PROVIDERS[name]
        self._provider = provider
        self._model = parts[2] if len(parts) > 2 else provider.default_model
        self._tokens = ChatTokens()
        self._remember_model()
        self._print_status()
        if provider.api_key_env and not provider.api_key():
            self._warn_missing_key()
        self._report_missing_sdk()

    def _configure_provider(self, text: str) -> None:
        """Inspect or update saved options for the selected provider."""
        parts = text.split(maxsplit=2)
        config = self._provider_configs.setdefault(self._provider.name, {})
        if len(parts) == 1:
            self._print(
                Panel(
                    Syntax(
                        json.dumps(config, indent=2, ensure_ascii=False),
                        "json",
                        background_color="default",
                    ),
                    title=f"{self._provider.name} config",
                    title_align="left",
                )
            )
            return
        if parts[1] == "clear" and len(parts) == 2:
            config.clear()
            self._remember_model()
            self._print("(provider config cleared)", style="dim")
            return
        if len(parts) == 2:
            if parts[1] not in config:
                self._print(
                    "usage: /config <key> <JSON value>",
                    style="red",
                    markup=False,
                )
                return
            self._print(
                f"{parts[1]} = {json.dumps(config[parts[1]])}",
                markup=False,
            )
            return
        try:
            value = json.loads(parts[2])
        except json.JSONDecodeError:
            value = parts[2]
        config[parts[1]] = value
        self._remember_model()
        self._print(
            f"{parts[1]} = {json.dumps(value, ensure_ascii=False)}",
            style="dim",
            markup=False,
        )

    # -- one conversational turn ------------------------------------------

    async def _turn(self, text: str) -> None:
        """Run one turn on the gateway and draw what it produces."""
        # Nothing to send if the backend cannot run here: the gateway would
        # raise the same precondition, only after a round-trip.
        if self._report_missing_sdk():
            return
        if self._turn_count >= self._max_turns:
            self._print("turn limit reached", style="red")
            return
        self._turn_count += 1
        self._tokens.begin_turn()

        user_interaction = make_user_interaction(text)
        # The tool system prompt rides on the first interaction of the
        # conversation (every backend reads system instructions only there).
        if self._system_prompt and not self._history:
            user_interaction.system_instructions = [
                a11.to_chunk(self._system_prompt)
            ]

        def paint() -> None:
            """Update state; prompt-toolkit diffs and blits the next frame."""
            self._screen.set_active(
                render_blocks(reducer.blocks, verbose=self._verbose)
            )

        def observe(block) -> None:
            self._tokens.observe(block)
            if (
                block.kind == BlockKind.TOOL_RUN
                and block.tool_name == "request_user_input"
            ):
                if block.id and block.tool_arguments and block.partial:
                    self._pending_user_inputs[block.id] = dict(
                        block.tool_arguments
                    )
                elif block.id and not block.partial:
                    self._pending_user_inputs.pop(block.id, None)
                self._screen.invalidate()

        reducer = PresentationReducer(_Repaint(paint, observe))

        config = TurnConfig(
            provider=self._provider.name,
            model=self._model,
            api_key=self._provider.api_key(),
            base_url=self._provider.base_url,
            # The names this client announced, and only those. The header gates
            # every tool the model may see, bridged ones included, so announcing
            # is not enough on its own -- the same two steps the IDE plugin
            # takes. Because a peer's tool shadows a gateway tool of the same
            # name on this connection, these names resolve to *this* process's
            # shells rather than the gateway's.
            allowed_actions=",".join(self._tool_names),
            extra_headers=self._extra_headers,
            traceparent=self._traceparent,
            timeout=a11.Duration.seconds(self._timeout_seconds),
            provider_config=self._provider_config(),
            # The raw provider events, shown only under -v. They are *not* used
            # to reconstruct text or thoughts -- those come from their own
            # ports. `run_turn` drains the port either way.
            on_event=(
                (lambda event: self._print(repr(event), style="dim"))
                if self._verbose
                else None
            ),
        )

        self._print(f"[bold green]{self._provider.name}[/]", highlight=False)
        self._llm_running = True
        self._screen.invalidate()
        complete = False
        try:
            new_interactions = await run_turn(
                self._connection,
                self._history,
                user_interaction,
                # Nothing pushed: `allowed_actions` names this side's tools, and
                # the gateway discovers their schemas by asking this session. A
                # Tool definitions come from the session registry; sending them
                # here would duplicate that description.
                (),
                config,
                reducer,
            )
            # History grows only on success, matching what the gateway recorded.
            self._history.append(user_interaction)
            self._history.extend(new_interactions)
            complete = True
            self._save_session(text)
        except StatusException as exc:
            reducer.on_error(exc.status)
            paint()
        except Exception as exc:  # pragma: no cover - defensive
            reducer.on_error(Status(code=StatusCode.INTERNAL, message=str(exc)))
            paint()
        finally:
            self._tokens.finish(complete=complete)
            self._llm_running = False
            self._pending_user_inputs.clear()
            self._screen.invalidate()
            paint()
            if self._screen.is_running:
                self._screen.commit_active()
            else:
                self._console.print(
                    render_blocks(reducer.blocks, verbose=self._verbose)
                )
            self._print(
                f"  {self._tokens.output_label} output", style="dim"
            )

    def _save_session(self, task: str) -> None:
        self._remember_model()
        if self._session_store is None or self._session_record is None:
            return
        record = self._session_record
        record.provider = self._provider.name
        record.model = self._model
        record.task = record.task or task
        record.history = list(self._history)
        if self._coding:
            record.agent_state = dict(self._coding_status)
        self._session_store.save(record)

    def _remember_model(self) -> None:
        """Persist the selected provider and model for the next invocation."""
        self._settings_store.save(
            ChatSettings(
                provider=self._provider.name,
                model=self._model,
                provider_configs=self._provider_configs,
            )
        )

    # -- small helpers -----------------------------------------------------

    def _provider_config(self) -> dict[str, object] | None:
        """Constrain provider-native agents to the gateway-owned tool path."""
        configured = self._normalise_provider_config(
            self._provider.name,
            self._provider_configs.get(self._provider.name, {}),
        )
        if not self._coding:
            return configured
        cwd = str(self._coding_status.get("cwd", self._cwd))
        if self._provider.name == "codex":
            return configured | {
                "cwd": cwd,
                "add_dirs": [],
                "sandbox": "read-only",
            }
        if self._provider.name == "claude_code":
            return configured | {
                "builtin_tools": False,
                "permission_mode": "plan",
                "cwd": cwd,
                "add_dirs": [],
            }
        return configured or None

    @staticmethod
    def _normalise_provider_config(
        provider: str, configured: dict[str, object]
    ) -> dict[str, object]:
        """Translate Chat's portable knobs to each provider's real schema."""
        result = dict(configured)
        if "max_tokens" not in result:
            return result
        native_names = {
            "gpt": "max_completion_tokens",
            "gemini": "max_output_tokens",
            "ollama": "num_predict",
        }
        native_name = native_names.get(provider, "max_tokens")
        if native_name != "max_tokens":
            result[native_name] = result.pop("max_tokens")
        return result

    async def _prepare_voice(self) -> None:
        """Download/load the selected model without blocking asyncio."""
        try:
            from a11.cli.voice import ensure_vad_model, ensure_voice_model
            from a11.sdk.audio import (
                SpeechRecognizer,
                SpeechRecognizerOptions,
            )

            # Both resolve on A11's fiber pool and are awaited directly; the
            # download does not need a worker thread of its own.
            model: Path = await ensure_voice_model(
                self._voice_model, self._console
            )
            # Silero VAD gates the decoder on genuine speech, so brief noise
            # while the user gathers their thoughts does not spawn
            # transcription.
            vad_model: Path = await ensure_vad_model(self._console)
            language = "en" if self._voice_model.endswith(".en") else "auto"
            options = SpeechRecognizerOptions(
                language=language, vad_model=str(vad_model)
            )
            self._recognizer = await asyncio.to_thread(
                SpeechRecognizer, model, None, options
            )
            self._print(
                f"voice input: [bold]{self._voice_model}[/] · microphone on "
                "during your turns",
                style="dim",
            )
        except StatusException as exc:
            self._disable_voice(exc.status.message)
        except Exception as exc:  # pragma: no cover - network/device specific
            self._disable_voice(str(exc))

    async def _start_voice_input(self) -> None:
        """Start ASR for one prompt and splice pieces into its edit buffer."""
        if self._recognizer is None:
            return

        self._voice_accepting = True

        async def on_transcription(piece: str | None) -> None:
            if piece is None or not self._voice_accepting:
                return
            piece = piece.strip()
            if not piece:
                return
            buffer = self._session.default_buffer
            document = buffer.document
            before = document.text_before_cursor
            after = document.text_after_cursor
            leading = "" if not before or before[-1].isspace() else " "
            trailing = "" if not after or after[0].isspace() else " "
            buffer.insert_text(leading + piece + trailing)
            self._session.app.invalidate()

        async def on_done() -> None:
            return None

        try:
            await self._recognizer.start(on_transcription, on_done)
        except StatusException as exc:
            self._voice_accepting = False
            self._disable_voice(exc.status.message)
        except Exception as exc:  # pragma: no cover - defensive
            self._voice_accepting = False
            self._disable_voice(str(exc))

    async def _stop_voice_input(self) -> None:
        """Stop capture before the LLM turn; late callbacks are ignored."""
        self._voice_accepting = False
        if self._recognizer is None or not self._recognizer.running:
            return
        try:
            await self._recognizer.stop()
        except StatusException as exc:
            self._disable_voice(exc.status.message)
        except Exception as exc:  # pragma: no cover - defensive
            self._disable_voice(str(exc))

    def _disable_voice(self, reason: str) -> None:
        self._voice_enabled = False
        self._voice_accepting = False
        self._recognizer = None
        self._print(
            f"voice input unavailable: {reason} (typed input remains active)",
            style="yellow",
            markup=False,
        )

    @staticmethod
    def _apply_span_error(span: observability.Span, status: Status) -> None:
        """Mirror an A11 Status onto a span: error status + error.type, plus
        error.details when the status carries any."""
        span.set_status("error", status.message)
        span.set_attribute("error.type", status.code.name)
        if status.details:
            span.set_attribute(
                "error.details", json.dumps(status.details, default=str)
            )

    def _print_status(self) -> None:
        self._print(
            f"backend: [bold]{self._provider.name}[/] · model:"
            f" [bold]{self._model}[/]",
            style="dim",
            highlight=False,
        )

    def _report_missing_sdk(self) -> bool:
        """Print why the current backend cannot run here; return that it can't.

        Only asked of an in-process gateway. A gateway reached over the network
        runs in its own environment, so local installation state cannot
        determine whether it can serve the backend. Remote failures are reported
        by the turn's `FAILED_PRECONDITION` status.
        """
        if not self._connection.embedded:
            return False
        try:
            # Import the backend before dispatch so its SDK setup stays outside
            # the action's call stack.
            load_provider(self._provider.name)
        except StatusException as exc:
            # Soft-wrapped: the message ends in a command to run, and a wrap
            # rich inserted mid-command would not survive a copy-paste.
            self._print(
                f"error: {exc.status.message}",
                style="red",
                markup=False,
                soft_wrap=True,
            )
            self._print(
                f"or switch backends with /model <{'|'.join(PROVIDERS)}>.",
                style="dim",
                markup=False,
            )
            return True
        return False

    def _warn_missing_key(self) -> None:
        envs = ", ".join(self._provider.api_key_env)
        self._print(
            f"warning: no API key for {self._provider.name}"
            f" (set one of {envs})",
            style="yellow",
        )

    def _print(self, *objects, **kwargs) -> None:
        """Write through the sole interactive renderer when it is active."""
        if self._non_interactive or not self._interactive_started:
            self._console.print(*objects, **kwargs)
        else:
            self._screen.print(*objects, **kwargs)


async def run_chat(
    provider_name: str,
    model: str | None,
    *,
    provider_explicit: bool = True,
    gateway: str | None = None,
    verbose: bool = False,
    shell_tools: bool = True,
    voice: bool = True,
    voice_model: str = "tiny.en",
    extra_headers: list[tuple[str, str]] | None = None,
    coding: bool = False,
    cwd: str = ".",
    add_dirs: tuple[str, ...] = (),
    approval: str = "ask",
    sandbox: str = "workspace-write",
    task: str | None = None,
    resume: str | None = None,
    max_turns: int = 50,
    timeout_seconds: int = 600,
    non_interactive: bool = False,
    no_banner: bool = False,
) -> int:
    """Run the interactive chat loop against ``provider_name``.

    Args:
        provider_name: Which LLM backend to use.
        model: Model id, or None for the provider's default.
        gateway: An explicit gateway URL. When given it must be reachable, and
            the command fails if it is not -- silently running a local gateway
            instead would execute the user's tools somewhere they did not
            choose. When omitted, an already-running gateway at the default
            endpoint is used, and otherwise one is started in this process.
        verbose: Show thoughts and token usage.
        shell_tools: Offer this side's shell tools to the model.
        voice: Enable speech input.
        voice_model: Transcription model shorthand or path.
        no_banner: Show a compact welcome card instead of the ASCII logo.
        extra_headers: Headers set on every turn, overriding the defaults.

    Returns:
        A process exit code. 2 for an unknown backend or an unreachable
        explicit gateway; provider-SDK and API-key problems surface in the loop.
    """
    console = Console()
    settings_store = ChatSettingsStore()
    settings = settings_store.load()
    if not provider_explicit and settings.provider in PROVIDERS:
        provider_name = settings.provider
    if model is None and settings.provider == provider_name and settings.model:
        model = settings.model
    provider = PROVIDERS.get(provider_name)
    if provider is None:
        console.print(
            f"unknown backend {provider_name!r};"
            f" choose from {', '.join(PROVIDERS)}",
            style="red",
        )
        return 2

    try:
        cwd = str(Path(cwd).expanduser().resolve())
        local_config = GatewayConfig(
            coding_cwd=cwd,
            coding_add_dirs=add_dirs,
            coding_approval="auto" if approval == "auto" else "suggest",
            coding_sandbox=sandbox,
            shell_tools=shell_tools,
        )
        async with open_gateway(
            gateway, local_config=local_config
        ) as connection:
            return await ChatUI(
                provider,
                model or provider.default_model,
                connection,
                verbose=verbose,
                shell_tools=shell_tools,
                voice=voice,
                voice_model=voice_model,
                extra_headers=extra_headers,
                coding=coding,
                cwd=cwd,
                add_dirs=add_dirs,
                approval=approval,
                sandbox=sandbox,
                task=task,
                resume=resume,
                max_turns=max_turns,
                timeout_seconds=timeout_seconds,
                non_interactive=non_interactive,
                no_banner=no_banner,
                settings_store=settings_store,
            ).run()
    except StatusException as exc:
        # Only reached when the *connection* failed; a failure inside a turn is
        # drawn in the transcript and does not end the session.
        target = gateway or "the default gateway endpoint"
        console.print(
            f"could not reach {target}: {exc.status.message}",
            style="red",
            markup=False,
        )
        return 2
