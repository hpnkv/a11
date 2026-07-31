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
`new_interactions` observes the error instead of hanging.
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
}

# Fallbacks used when no explicit provider header is set: the model id's prefix
# usually names its family.
_MODEL_PREFIXES: tuple[tuple[str, str], ...] = (
    ("claude", "claude"),
    ("gemini", "gemini"),
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
                f" with:  pip install 'a11-kit[{spec.extra}]'"
            ),
        ).to_exception() from exc
    return getattr(module, spec.handler)


async def interact_with_llm(action: a11.Action) -> None:
    """Route the interaction to the header-selected backend, run it inline."""
    provider = _resolve_provider(action)
    handler = _load_handler(provider)
    await handler(action)


__all__ = ["INTERACT_WITH_LLM_SCHEMA", "interact_with_llm"]
