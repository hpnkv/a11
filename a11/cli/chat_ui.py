# Copyright 2026 The A11 Authors.

"""Interactive chat loop for ``a11 chat``.

The UI is deliberately state-agnostic: conversation history is just a flat
``list[Interaction]`` that `ChatUI` threads back into each turn. Input is
read with `prompt_toolkit` (async-native, no opinion on how we hold state)
and assistant output is streamed live with `rich`.

Backend selection is not the CLI's job: every turn runs the single
[INTERACT_WITH_LLM_SCHEMA][a11.sdk.interact_with_llm.INTERACT_WITH_LLM_SCHEMA]
action with an
``x-a11-llm-provider`` header, and that action routes to the concrete backend
and imports its SDK lazily. The CLI just reads the ``text_output`` (and, when
verbose, ``thoughts``) stream nodes it produces.
"""

from __future__ import annotations

import asyncio
import datetime
import json

import a11
from prompt_toolkit import PromptSession
from prompt_toolkit.formatted_text import HTML
from prompt_toolkit.history import InMemoryHistory
from prompt_toolkit.patch_stdout import patch_stdout
from rich.console import Console, Group
from rich.live import Live
from rich.markdown import Markdown
from rich.text import Text

from a11 import observability
from a11.cli.backends import PROVIDERS, Provider, make_user_interaction
from a11.sdk.interact_with_llm import (
    INTERACT_WITH_LLM_SCHEMA,
    interact_with_llm,
)
from a11.sdk.llm import Interaction, LlmHeaders
from a11.status import Status, StatusCode, StatusException

_HELP = (
    "Commands:\n"
    "  /model <claude|gemini|ollama> [model]   switch backend (and optionally"
    " model)\n"
    "  /clear                                  forget the conversation so far\n"
    "  /help, /?                               show this help\n"
    "  /exit, /quit                            leave\n"
)


class ChatUI:
    """A single interactive chat session over a swappable LLM backend."""

    def __init__(
        self,
        provider: Provider,
        model: str,
        *,
        verbose: bool = False,
        shell_tools: bool = True,
        extra_headers: list[tuple[str, str]] | None = None,
    ) -> None:
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

        # Shell tools: a registry of the four shell Actions, their tool
        # definitions, the pattern that permits them, and the system prompt
        # that teaches the model to use them. Built once and reused each turn.
        self._registry: a11.ActionRegistry | None = None
        self._tool_definitions: list[dict] = []
        self._allowed_actions = ""
        self._system_prompt = ""
        if shell_tools:
            self._enable_shell_tools()

    def _enable_shell_tools(self) -> None:
        from a11.sdk import bash
        from a11.sdk.llm_tools.runner import get_tool_definitions

        registry = a11.ActionRegistry()
        bash.register(registry)
        names = [schema.name for schema, _ in bash.SHELL_ACTIONS]
        self._registry = registry
        self._tool_definitions = get_tool_definitions(registry, names)
        self._allowed_actions = "shell_.*"
        # Chat runs outside an A11 Session, so shells are globally scoped and
        # the global cap is the one the model should be told about.
        self._system_prompt = bash.get_system_prompt(
            max_shells=bash.MAX_GLOBAL_SHELLS
        )

    # -- lifecycle ---------------------------------------------------------

    async def run(self) -> int:
        """Run the read-eval-print loop until the user exits. Returns 0."""
        # One "A11 Chat" span for the whole session; each turn's interaction is
        # parented to it (via its traceparent), so turns nest under it.
        self._chat_span = observability.start_span("A11 Chat", kind="server")
        self._traceparent = self._chat_span.traceparent()
        self._console.print(_HELP, style="dim", markup=False)
        self._print_status()
        if self._provider.api_key_env and not self._provider.api_key():
            self._warn_missing_key()

        self._chat_span.set_input(
            f"Interactive chat started at {datetime.datetime.now().isoformat()}"
        )

        try:
            while True:
                try:
                    with patch_stdout():
                        text = await self._session.prompt_async(
                            HTML(
                                f"<ansicyan>{self._provider.name}</ansicyan> › "
                            )
                        )
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
        if len(parts) < 2 or parts[1] not in PROVIDERS:
            self._console.print(
                f"usage: /model <{'|'.join(PROVIDERS)}> [model]",
                style="red",
                markup=False,
            )
            return

        provider = PROVIDERS[parts[1]]
        self._provider = provider
        self._model = parts[2] if len(parts) > 2 else provider.default_model
        self._print_status()
        if provider.api_key_env and not provider.api_key():
            self._warn_missing_key()

    # -- one conversational turn ------------------------------------------

    async def _turn(self, text: str) -> None:
        interact = (
            a11.Action(INTERACT_WITH_LLM_SCHEMA)
            .bind_handler(interact_with_llm)
            .set_header(LlmHeaders.PROVIDER.value, self._provider.name)
            .set_header(LlmHeaders.MODEL.value, self._model)
            .set_header(LlmHeaders.API_KEY.value, self._provider.api_key())
            .set_header(LlmHeaders.BASE_URL.value, self._provider.base_url)
            .set_header(
                LlmHeaders.ALLOWED_LLM_ACTIONS.value, self._allowed_actions
            )
        )
        # Tool calls the backend makes are dispatched against this registry.
        if self._registry is not None:
            interact.bind_registry(self._registry)
        # Applied last so a user-supplied header overrides the default above.
        for key, value in self._extra_headers:
            interact.set_header(key, value)
        observability.enable_tracing(
            interact,
            traceparent=self._traceparent,
            baggage={"langfuse.trace.name": "chat_ui"},
        )
        interact.run()

        user_interaction = make_user_interaction(text)
        # The tool system prompt rides on the first interaction of the
        # conversation (every backend reads system instructions only there).
        if self._system_prompt and not self._history:
            user_interaction.system_instructions = [
                a11.to_chunk(self._system_prompt)
            ]

        text_buf: list[str] = []
        thoughts_buf: list[str] = []
        live = Live(
            console=self._console,
            refresh_per_second=16,
            transient=False,
            vertical_overflow="visible",
        )
        live.start()

        def render() -> None:
            parts: list[object] = []
            if self._verbose and thoughts_buf:
                parts.append(
                    Text("".join(thoughts_buf).strip(), style="dim italic")
                )
            if text_buf:
                parts.append(Markdown("".join(text_buf)))
            live.update(Group(*parts))

        async def pump(node_name: str, buf: list[str]) -> None:
            async for chunk in interact[node_name]:
                buf.append(chunk)
                render()

        async def log_events() -> None:
            # The raw provider events, shown only under -v. They are *not* used
            # to reconstruct text/thoughts — those come from their own nodes.
            async for event in interact["event_stream"]:
                self._console.log(event)

        self._console.print(
            f"[bold green]{self._provider.name}[/]", highlight=False
        )
        readers = [asyncio.create_task(pump("text_output", text_buf))]
        if self._verbose:
            readers.append(asyncio.create_task(pump("thoughts", thoughts_buf)))
            readers.append(asyncio.create_task(log_events()))

        try:
            async with (
                interact["interactions"] as interactions,
                interact["config"],
                interact["tools"] as tools,
            ):
                # The config node is left empty (closed on block exit) so the
                # backend applies its own default request config.
                for interaction in self._history:
                    await interactions.put(interaction)
                await interactions.put_final(user_interaction)
                for tool in self._tool_definitions:
                    await tools.put(tool)
                await tools.put_null_final()

            new_interactions: list[Interaction] = []
            async for interaction in interact["new_interactions"]:
                new_interactions.append(interaction)

            await asyncio.gather(*readers)

            self._history.append(user_interaction)
            self._history.extend(new_interactions)

        except StatusException as exc:
            self._console.print(
                f"error: {exc.status.message}", style="red", markup=False
            )
        except Exception as exc:  # pragma: no cover - defensive
            self._console.print(f"error: {exc}", style="red", markup=False)
        finally:
            for reader in readers:
                if not reader.done():
                    reader.cancel()
            await asyncio.gather(*readers, return_exceptions=True)
            if live.is_started:
                live.stop()
            self._console.print()

    # -- small helpers -----------------------------------------------------

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
    verbose: bool = False,
    shell_tools: bool = True,
    extra_headers: list[tuple[str, str]] | None = None,
) -> int:
    """Run the interactive chat loop against ``provider_name``.

    Returns a process exit code. Unknown providers are reported as a short
    message; provider-SDK / API-key problems surface inside the loop.
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

    return await ChatUI(
        provider,
        model or provider.default_model,
        verbose=verbose,
        shell_tools=shell_tools,
        extra_headers=extra_headers,
    ).run()
