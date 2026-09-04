# Copyright 2026 The A11 Authors.

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

The shell tools are announced to the gateway rather than requested from it,
which means the model's commands run in *this* process -- the user's shell, cwd
and environment -- even when the gateway is somewhere else.
"""

from __future__ import annotations

import asyncio
import datetime
import json
from pathlib import Path
from typing import TYPE_CHECKING

import a11
from prompt_toolkit import PromptSession
from prompt_toolkit.formatted_text import HTML
from prompt_toolkit.history import InMemoryHistory
from prompt_toolkit.patch_stdout import patch_stdout
from rich.console import Console
from rich.live import Live

from a11 import observability
from a11.cli.backends import (
    PROVIDERS,
    Provider,
    make_user_interaction,
    normalize_provider_name,
)
from a11.cli.presentation_render import render_blocks
from a11.client.connection import GatewayConnection, open_gateway
from a11.client.turn import TurnConfig, run_turn
from a11.sdk.interact_with_llm import load_provider
from a11.sdk.llm import Interaction
from a11.sdk.presentation import PresentationReducer
from a11.status import Status, StatusCode, StatusException

if TYPE_CHECKING:
    from a11.sdk.audio import SpeechRecognizer

_HELP = (
    "Commands:\n"
    "  /model <claude|claude_code|codex|gemini|gpt|ollama|vllm> [model]  switch"
    " backend (and optionally model)\n"
    "  /clear                                                  forget the"
    " conversation so far\n"
    "  /help, /?                                               show this"
    " help\n"
    "  /exit, /quit                                            leave\n"
)


class _Repaint:
    """A `PresentationSink` that calls one function on any change.

    The terminal's answer to incremental rendering: rather than track which
    block moved, redraw the turn and let `rich` diff it.
    """

    def __init__(self, paint) -> None:
        self._paint = paint

    def on_block_opened(self, block) -> None:
        self._paint()

    def on_block_appended(self, block, delta: str) -> None:
        self._paint()

    def on_block_closed(self, block) -> None:
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
        self._session: PromptSession[str] = PromptSession(
            history=InMemoryHistory()
        )
        self._traceparent: str | None = None
        self._chat_span: observability.Span | None = None
        self._voice_enabled = voice
        self._voice_model = voice_model
        self._voice_accepting = False
        self._recognizer: SpeechRecognizer | None = None

        # Shell tools: their definitions, and the system prompt that teaches the
        # model to use them. The Actions themselves are registered on the
        # connection's session registry, because the gateway reverse-dispatches
        # the model's calls back to *this* process to run them.
        self._tool_names: list[str] = []
        self._system_prompt = ""
        if shell_tools:
            self._enable_shell_tools()

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
        # One "A11 Chat" span for the whole session; each turn's interaction is
        # parented to it (via its traceparent), so turns nest under it.
        self._chat_span = observability.start_span("A11 Chat", kind="server")
        self._traceparent = self._chat_span.traceparent()
        self._console.print(_HELP, style="dim", markup=False)
        self._print_status()
        self._console.print(
            f"gateway: [bold]{self._connection.description}[/]",
            style="dim",
            highlight=False,
        )
        if self._provider.api_key_env and not self._provider.api_key():
            self._warn_missing_key()
        self._report_missing_sdk()
        # Nothing to announce: the gateway asks this session what it serves the
        # first time a turn needs tools, and reverse-dispatches the model's
        # calls back here to run them.
        if self._voice_enabled:
            await self._prepare_voice()

        self._chat_span.set_input(
            f"Interactive chat started at {datetime.datetime.now().isoformat()}"
        )

        try:
            while True:
                try:
                    voice_start: asyncio.Task[None] | None = None

                    def start_voice() -> None:
                        nonlocal voice_start
                        voice_start = asyncio.create_task(
                            self._start_voice_input()
                        )

                    try:
                        with patch_stdout():
                            text = await self._session.prompt_async(
                                HTML(
                                    f"<ansicyan>{self._provider.name}"
                                    "</ansicyan> › "
                                ),
                                # PromptSession resets its edit buffer at the
                                # start of prompt_async(). Starting capture as a
                                # pre-run hook guarantees ASR pieces land in the
                                # active prompt rather than the old buffer.
                                pre_run=start_voice,
                            )
                    finally:
                        if voice_start is not None:
                            await voice_start
                        await self._stop_voice_input()
                except (EOFError, KeyboardInterrupt):
                    break

                text = text.strip()
                if not text:
                    continue
                if not await self._handle(text):
                    break
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
            self._chat_span.end()

        self._console.print("bye", style="dim")
        return 0

    # -- command handling --------------------------------------------------

    async def _handle(self, text: str) -> bool:
        """Handle one line of input. Returns False to end the session."""
        lowered = text.casefold()
        if lowered in ("/exit", "/quit"):
            return False
        if lowered in ("/help", "/?"):
            self._console.print(_HELP, style="dim", markup=False)
            return True
        if lowered == "/clear":
            self._history.clear()
            self._console.print("(conversation cleared)", style="dim")
            return True
        if text.startswith("/model"):
            self._switch_model(text.split())
            return True
        if text.startswith("/"):
            self._console.print(
                f"unknown command {text.split()[0]!r} — try /help",
                style="red",
                markup=False,
            )
            return True

        await self._turn(text)
        return True

    def _switch_model(self, parts: list[str]) -> None:
        name = normalize_provider_name(parts[1]) if len(parts) > 1 else ""
        if name not in PROVIDERS:
            self._console.print(
                f"usage: /model <{'|'.join(PROVIDERS)}> [model]",
                style="red",
                markup=False,
            )
            return

        provider = PROVIDERS[name]
        self._provider = provider
        self._model = parts[2] if len(parts) > 2 else provider.default_model
        self._print_status()
        if provider.api_key_env and not provider.api_key():
            self._warn_missing_key()
        self._report_missing_sdk()

    # -- one conversational turn ------------------------------------------

    async def _turn(self, text: str) -> None:
        """Run one turn on the gateway and draw what it produces."""
        # Nothing to send if the backend cannot run here: the gateway would
        # raise the same precondition, only after a round-trip.
        if self._report_missing_sdk():
            return

        user_interaction = make_user_interaction(text)
        # The tool system prompt rides on the first interaction of the
        # conversation (every backend reads system instructions only there).
        if self._system_prompt and not self._history:
            user_interaction.system_instructions = [
                a11.to_chunk(self._system_prompt)
            ]

        live = Live(
            console=self._console,
            refresh_per_second=16,
            transient=False,
            vertical_overflow="visible",
        )
        reducer = PresentationReducer()

        def paint() -> None:
            """Redraw the whole turn.

            Wholesale rather than incremental: `rich`'s Live already diffs, and
            a block can change after it was first drawn -- a tool run's log
            arrives in the interaction *after* the call that produced it.
            """
            live.update(render_blocks(reducer.blocks, verbose=self._verbose))

        reducer = PresentationReducer(_Repaint(paint))

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
            # The raw provider events, shown only under -v. They are *not* used
            # to reconstruct text or thoughts -- those come from their own
            # ports. `run_turn` drains the port either way.
            on_event=(self._console.log if self._verbose else None),
        )

        self._console.print(
            f"[bold green]{self._provider.name}[/]", highlight=False
        )
        live.start()
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
        except StatusException as exc:
            reducer.on_error(exc.status)
            live.update(render_blocks(reducer.blocks, verbose=self._verbose))
        except Exception as exc:  # pragma: no cover - defensive
            reducer.on_error(Status(code=StatusCode.INTERNAL, message=str(exc)))
            live.update(render_blocks(reducer.blocks, verbose=self._verbose))
        finally:
            if live.is_started:
                live.stop()
            self._console.print()

    # -- small helpers -----------------------------------------------------

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
            self._console.print(
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
        self._console.print(
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
        self._console.print(
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
            self._console.print(
                f"error: {exc.status.message}",
                style="red",
                markup=False,
                soft_wrap=True,
            )
            self._console.print(
                f"or switch backends with /model <{'|'.join(PROVIDERS)}>.",
                style="dim",
                markup=False,
            )
            return True
        return False

    def _warn_missing_key(self) -> None:
        envs = ", ".join(self._provider.api_key_env)
        self._console.print(
            f"warning: no API key for {self._provider.name}"
            f" (set one of {envs})",
            style="yellow",
        )


async def run_chat(
    provider_name: str,
    model: str | None,
    *,
    gateway: str | None = None,
    verbose: bool = False,
    shell_tools: bool = True,
    voice: bool = True,
    voice_model: str = "tiny.en",
    extra_headers: list[tuple[str, str]] | None = None,
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
        extra_headers: Headers set on every turn, overriding the defaults.

    Returns:
        A process exit code. 2 for an unknown backend or an unreachable
        explicit gateway; provider-SDK and API-key problems surface in the loop.
    """
    console = Console()
    provider = PROVIDERS.get(provider_name)
    if provider is None:
        console.print(
            f"unknown backend {provider_name!r};"
            f" choose from {', '.join(PROVIDERS)}",
            style="red",
        )
        return 2

    try:
        async with open_gateway(gateway) as connection:
            return await ChatUI(
                provider,
                model or provider.default_model,
                connection,
                verbose=verbose,
                shell_tools=shell_tools,
                voice=voice,
                voice_model=voice_model,
                extra_headers=extra_headers,
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
