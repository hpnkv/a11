# Copyright 2026 The A11 Authors.

"""Provider-agnostic entry point that routes to a concrete `interact_with_*`.

`interact_with_llm` is a thin dispatcher: it inspects the action's headers to
decide which backend should serve the turn, imports that provider lazily, and
then runs the provider's handler **inline on the same action**. Because this
action's schema is the union of the provider schemas, the downstream handler
reads the very same input nodes and writes the very same output nodes — no new
action object is created, and no data is copied between node sets.

Running inline (rather than spawning a nested action) is what keeps a
backend's inline tool-calling working: the provider handler calls
`action.make_nested(...)`, which requires the real, running `Action` it was
handed.

Failures are surfaced the ordinary A11 way. An unknown/absent provider raises
`INVALID_ARGUMENT`; a provider whose SDK is not installed raises
`FAILED_PRECONDITION` with an install hint. In both cases the raised status is
propagated by the runtime onto every output node, so a caller reading e.g.
`new_interactions` observes the error instead of hanging. A caller that would
rather learn about a missing SDK before the first turn can call
`load_provider` at start-up.
"""

from __future__ import annotations

import dataclasses
import importlib
from typing import Awaitable, Callable

import a11
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
    "gemini": _Provider(
        module="a11.sdk.gemini.interact_with_gemini",
        handler="interact_with_gemini",
        extra="gemini",
    ),
    "ollama": _Provider(
        module="a11.sdk.ollama.interact_with_ollama",
        handler="interact_with_ollama",
        extra="ollama",
    ),
}

# Fallbacks used when no explicit provider header is set: the model id's prefix
# usually names its family. Ollama serves many open-weight families, so a few of
# the common ones route to it — set the provider header explicitly to override.
_MODEL_PREFIXES: tuple[tuple[str, str], ...] = (
    ("claude", "claude"),
    ("gemini", "gemini"),
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
        provider = provider.strip().casefold()
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
        # An SDK that is installed but unimportable (a version conflict, say)
        # would otherwise surface as whatever it happened to raise, from a
        # frame the caller cannot place. Name the provider instead.
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                f"The {provider!r} backend's SDK failed to import:"
                f" {type(exc).__name__}: {exc}"
            ),
        ).to_exception() from exc
    return getattr(module, spec.handler)


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
    """Route the interaction to the header-selected backend, run it inline."""
    provider = _resolve_provider(action)
    handler = _load_handler(provider)
    await handler(action)


__all__ = [
    "INTERACT_WITH_LLM_SCHEMA",
    "install_hint",
    "interact_with_llm",
    "load_provider",
]
