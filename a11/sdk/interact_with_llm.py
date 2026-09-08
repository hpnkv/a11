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

"""Provider-agnostic entry point that routes to a concrete `interact_with_*`.

`interact_with_llm` inspects the action's headers, imports the selected provider
lazily, and runs its handler as a nested action. Fragment pumps connect the
router's ports to the backend's matching ports. The child retains the session,
registry, headers, and nested tool-call context.

The router claims the backend action's log and relays each record with its
level, channel, source location, and structured value intact. A caller therefore
sees provider narration on the `interact_with_llm` log alongside the router's
own selection and completion entries.

Failures are surfaced the ordinary A11 way. An unknown/absent provider raises
`INVALID_ARGUMENT`; a provider whose SDK is not installed raises
`FAILED_PRECONDITION` with an install hint. In both cases the raised status is
propagated by the runtime onto every output node, so a caller reading e.g.
`new_interactions` observes the error instead of hanging. A caller that would
rather learn about a missing SDK before the first turn can call
`load_provider` at start-up.
"""

from __future__ import annotations

import asyncio
import dataclasses
import importlib
from typing import Awaitable, Callable

from absl import logging

import a11
from a11 import _native
from a11.sdk.llm import Interaction, LlmHeaders
from a11.status import Status, StatusCode

Handler = Callable[[a11.Action], Awaitable[None]]


@dataclasses.dataclass(frozen=True)
class _Provider:
    """How to reach one backend's `interact_with_*` handler lazily."""

    module: str
    handler: str
    extra: str


_PROVIDERS: dict[str, _Provider] = {
    "claude": _Provider(
        module="a11.sdk.anthropic.interact_with_claude",
        handler="interact_with_claude",
        extra="claude",
    ),
    "claude_code": _Provider(
        module="a11.sdk.anthropic.interact_with_claude_code",
        handler="interact_with_claude_code",
        extra="claude-code",
    ),
    "gemini": _Provider(
        module="a11.sdk.gemini.interact_with_gemini",
        handler="interact_with_gemini",
        extra="gemini",
    ),
    "gpt": _Provider(
        module="a11.sdk.openai.interact_with_gpt",
        handler="interact_with_gpt",
        extra="openai",
    ),
    "openai": _Provider(
        module="a11.sdk.openai.interact_with_gpt",
        handler="interact_with_gpt",
        extra="openai",
    ),
    "codex": _Provider(
        module="a11.sdk.openai.interact_with_codex",
        handler="interact_with_codex",
        extra="codex",
    ),
    "ollama": _Provider(
        module="a11.sdk.ollama.interact_with_ollama",
        handler="interact_with_ollama",
        extra="ollama",
    ),
    "vllm": _Provider(
        module="a11.sdk.vllm.interact_with_vllm",
        handler="interact_with_vllm",
        extra="vllm",
    ),
}

# When no provider header is set, model-family prefixes select a provider.
# Common open-weight model families use Ollama. An explicit provider header
# overrides this mapping.
_MODEL_PREFIXES: tuple[tuple[str, str], ...] = (
    ("claude", "claude"),
    ("gemini", "gemini"),
    ("gpt", "gpt"),
    ("o1", "gpt"),
    ("o3", "gpt"),
    ("o4", "gpt"),
    ("llama", "ollama"),
    ("qwen", "ollama"),
    ("mistral", "ollama"),
    ("gemma", "ollama"),
    ("phi", "ollama"),
    ("deepseek", "ollama"),
)


INTERACT_WITH_LLM_SCHEMA = a11.ActionSchema(
    name="interact_with_llm",
    description=(
        "Route an LLM interaction to a concrete backend chosen by the"
        f" {LlmHeaders.PROVIDER.value} header."
    ),
    inputs={
        "interactions": a11.ActionPortSchema(
            "interactions",
            "application/json",
            typeinfo=Interaction,
            required=True,
        ),
        "tools": a11.ActionPortSchema(
            "tools",
            "application/json",
            typeinfo=dict,
            required=False,
        ),
        "config": a11.ActionPortSchema(
            "config",
            "application/json",
            typeinfo=dict,
            unary=True,
            required=False,
        ),
    },
    outputs={
        "event_stream": a11.ActionPortSchema(
            "event_stream",
            "application/json",
            typeinfo=dict,
            required=False,
        ),
        "thoughts": a11.ActionPortSchema(
            "thoughts",
            "text/plain",
            required=False,
        ),
        "text_output": a11.ActionPortSchema(
            "text_output",
            "text/plain",
            required=False,
        ),
        "new_interactions": a11.ActionPortSchema(
            "new_interactions",
            "application/json",
            typeinfo=Interaction,
            required=True,
        ),
    },
    headers=a11.DEFAULT_ACTION_HEADERS
    | {
        LlmHeaders.API_KEY: a11.ActionHeaderSchema(
            LlmHeaders.API_KEY, "The backend API key."
        ),
        LlmHeaders.MODEL: a11.ActionHeaderSchema(
            LlmHeaders.MODEL, "The backend model."
        ),
        LlmHeaders.PROVIDER: a11.ActionHeaderSchema(
            LlmHeaders.PROVIDER,
            f"Which backend to route to, one of: {', '.join(_PROVIDERS)}.",
        ),
        LlmHeaders.ALLOWED_LLM_ACTIONS: a11.ActionHeaderSchema(
            LlmHeaders.ALLOWED_LLM_ACTIONS,
            "The allowed action (tool) name patterns, comma-separated.",
        ),
    },
)


def _resolve_provider(action: a11.Action) -> str:
    """Pick the backend from the provider header, or infer it from the model."""
    provider = action.get_header(LlmHeaders.PROVIDER.value, decode=True)
    if provider:
        # A name is written either way round on a command line, so `-` and `_`
        # name the same provider.
        provider = provider.strip().casefold().replace("-", "_")
        if provider not in _PROVIDERS:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    f"Unknown LLM provider {provider!r}; expected one of"
                    f" {', '.join(_PROVIDERS)}."
                ),
            ).to_exception()
        return provider

    model = action.get_header(LlmHeaders.MODEL.value, decode=True) or ""
    model = model.strip().casefold()
    for prefix, name in _MODEL_PREFIXES:
        if model.startswith(prefix):
            return name

    raise Status(
        code=StatusCode.INVALID_ARGUMENT,
        message=(
            f"No {LlmHeaders.PROVIDER.value} header was set and the provider"
            f" could not be inferred from the model {model!r}. Set one of:"
            f" {', '.join(_PROVIDERS)}."
        ),
    ).to_exception()


def install_hint(provider: str) -> str:
    """The ``pip install`` line that adds ``provider``'s SDK."""
    return f"pip install 'a11-kit[{_PROVIDERS[provider].extra}]'"


def _load_handler(provider: str) -> Handler:
    """Import ``provider``'s module lazily and return its handler coroutine."""
    spec = _PROVIDERS[provider]
    try:
        module = importlib.import_module(spec.module)
    except ImportError as exc:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                f"The {provider!r} backend needs its provider SDK. Install it"
                f" with:  {install_hint(provider)}"
            ),
        ).to_exception() from exc
    except Exception as exc:  # noqa: BLE001 - a broken SDK is a precondition
        # Report an installed but unimportable SDK as a provider precondition,
        # with the underlying exception retained in the message.
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                f"The {provider!r} backend's SDK failed to import:"
                f" {type(exc).__name__}: {exc}"
            ),
        ).to_exception() from exc
    return getattr(module, spec.handler)


def _backend_schema(provider: str) -> a11.ActionSchema:
    """Give the nested backend its implementation name and the router ports."""
    return a11.ActionSchema(
        name=_PROVIDERS[provider].handler,
        description=f"Run one interaction with the {provider} backend.",
        inputs=INTERACT_WITH_LLM_SCHEMA.inputs,
        outputs=INTERACT_WITH_LLM_SCHEMA.outputs,
        headers=INTERACT_WITH_LLM_SCHEMA.headers,
    )


async def _pump_fragments(source: a11.AsyncNode, target: a11.AsyncNode) -> None:
    """Copy one router port to its backend counterpart."""
    while True:
        fragment = await source.next_fragment()
        if fragment is None:
            break
        await target.put_fragment(fragment)
    await target.close()


async def _forward_logs(
    source: a11.AsyncNode, parent: a11.Action, child: a11.Action
) -> None:
    """Relay the claimed backend log through the router action."""
    async for chunk in source.iter_chunks():
        if chunk.is_null() or _native.is_status_chunk(chunk):
            continue
        try:
            record = _native.log_record_from_chunk(chunk)
            await parent.log(
                chunk,
                level=record["level"],
                channel=record["channel"] or None,
                internal=record["internal"],
                file=record["file"],
                lineno=record["lineno"],
                metadata={
                    "a11-child-action": child.schema.name,
                    "a11-child-call-id": child.id,
                },
            )
        except Exception:
            logging.warning("failed to forward a provider log", exc_info=True)


async def _run_backend(
    action: a11.Action, provider: str, handler: Handler
) -> None:
    """Run the selected backend and connect all of its ports to the router."""
    backend = action.make_nested(_backend_schema(provider))
    backend.bind_stream(None)
    backend.bind_handler(handler)
    logs = backend.get_log_node()

    pumps = [
        asyncio.create_task(_pump_fragments(action[name], backend[name]))
        for name in INTERACT_WITH_LLM_SCHEMA.inputs
    ]
    pumps.extend(
        asyncio.create_task(_pump_fragments(backend[name], action[name]))
        for name in INTERACT_WITH_LLM_SCHEMA.outputs
    )
    pumps.append(asyncio.create_task(_forward_logs(logs, action, backend)))

    backend.run()
    waiter = asyncio.ensure_future(backend.wait())
    tasks = [waiter, *pumps]
    try:
        await asyncio.gather(*tasks)
    except BaseException:
        backend.cancel()
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        raise


def load_provider(provider: str) -> None:
    """Import ``provider``'s backend now, so a later turn cannot fail on it.

    `interact_with_llm` imports the provider SDK on the first turn that needs
    it, inside the running action. A caller that knows it will drive a provider
    -- `a11 chat` against an in-process gateway, say -- can call this at
    start-up instead: the import then happens on an ordinary stack, and a
    missing or broken SDK is reported before the user has typed anything.

    Args:
        provider: Provider name, one of the keys of the router's table.

    Raises:
        StatusException: `INVALID_ARGUMENT` when ``provider`` is unknown,
            `FAILED_PRECONDITION` when its SDK is missing or unimportable.
    """
    if provider not in _PROVIDERS:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                f"Unknown LLM provider {provider!r}; expected one of"
                f" {', '.join(_PROVIDERS)}."
            ),
        ).to_exception()
    _load_handler(provider)


async def interact_with_llm(action: a11.Action) -> None:
    """Route the interaction through a nested provider action."""
    provider = _resolve_provider(action)
    handler = _load_handler(provider)
    await action.logf("Selected the %s provider.", provider, channel="routing")
    await _run_backend(action, provider, handler)
    await action.logf(
        "The %s interaction completed.", provider, channel="routing"
    )


__all__ = [
    "INTERACT_WITH_LLM_SCHEMA",
    "install_hint",
    "interact_with_llm",
    "load_provider",
]
