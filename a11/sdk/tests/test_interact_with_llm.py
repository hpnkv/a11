"""Offline tests for the `interact_with_llm` routing action.

These exercise routing and stream-node forwarding without any provider SDK or
network: a fake provider handler is registered in place of a real backend.
"""

import sys
import types

import pytest

import a11
from a11.sdk import interact_with_llm as illm
from a11.sdk.interact_with_llm import (
    INTERACT_WITH_LLM_SCHEMA,
    interact_with_llm,
)
from a11.sdk.llm import Interaction, LlmHeaders
from a11.status import StatusCode, StatusException


def _user_message() -> a11.Chunk:
    return a11.to_chunk(
        {"role": "user", "content": [{"type": "text", "text": "hi"}]}
    )


async def _run(headers: dict[str, str], read: str = "new_interactions"):
    """Run one turn and return the values read from ``read`` (or raise)."""
    action = a11.Action(INTERACT_WITH_LLM_SCHEMA).bind_handler(
        interact_with_llm
    )
    for key, value in headers.items():
        action.set_header(key, value.encode())
    action = action.run()

    await action["interactions"].finalize(_user_message())
    await action["config"].finalize()
    await action["tools"].finalize()

    collected = []
    async for value in action[read]:
        collected.append(value)
    return collected


@pytest.mark.asyncio
async def test_unknown_provider_errors_on_outputs():
    with pytest.raises(StatusException) as excinfo:
        await _run({LlmHeaders.PROVIDER.value: "bogus"})
    assert excinfo.value.status.code == StatusCode.INVALID_ARGUMENT


@pytest.mark.asyncio
async def test_missing_provider_and_model_errors():
    with pytest.raises(StatusException) as excinfo:
        await _run({})
    assert excinfo.value.status.code == StatusCode.INVALID_ARGUMENT


@pytest.mark.asyncio
async def test_provider_inferred_from_model_prefix(monkeypatch):
    seen = {}

    async def fake_handler(action):
        seen["ran"] = True
        await action["new_interactions"].put(Interaction(model="fake"))
        for name in ("event_stream", "text_output", "thoughts"):
            await action[name].finalize()
        await action["new_interactions"].finalize()

    module = types.ModuleType("a11.sdk._fake_infer")
    module.fake_handler = fake_handler
    monkeypatch.setitem(sys.modules, "a11.sdk._fake_infer", module)
    monkeypatch.setitem(
        illm._PROVIDERS,
        "claude",
        illm._Provider("a11.sdk._fake_infer", "fake_handler", "claude"),
    )

    out = await _run({LlmHeaders.MODEL.value: "claude-sonnet-4-6"})
    assert seen.get("ran") is True
    assert len(out) == 1


@pytest.mark.parametrize("model", ["gpt-6-astra", "o3", "o4-mini"])
@pytest.mark.asyncio
async def test_openai_model_families_infer_gpt(monkeypatch, model):
    seen = {}

    async def fake_handler(action):
        seen["ran"] = True
        await action["new_interactions"].put(Interaction(model="fake"))
        for name in ("event_stream", "text_output", "thoughts"):
            await action[name].finalize()
        await action["new_interactions"].finalize()

    module = types.ModuleType("a11.sdk._fake_gpt")
    module.fake_handler = fake_handler
    monkeypatch.setitem(sys.modules, "a11.sdk._fake_gpt", module)
    monkeypatch.setitem(
        illm._PROVIDERS,
        "gpt",
        illm._Provider("a11.sdk._fake_gpt", "fake_handler", "openai"),
    )

    out = await _run({LlmHeaders.MODEL.value: model})
    assert seen.get("ran") is True
    assert len(out) == 1


@pytest.mark.asyncio
async def test_openai_is_an_explicit_gpt_provider_alias(monkeypatch):
    seen = {}

    async def fake_handler(action):
        seen["ran"] = True
        await action["new_interactions"].put(Interaction(model="fake"))
        for name in ("event_stream", "text_output", "thoughts"):
            await action[name].finalize()
        await action["new_interactions"].finalize()

    module = types.ModuleType("a11.sdk._fake_openai")
    module.fake_handler = fake_handler
    monkeypatch.setitem(sys.modules, "a11.sdk._fake_openai", module)
    monkeypatch.setitem(
        illm._PROVIDERS,
        "openai",
        illm._Provider("a11.sdk._fake_openai", "fake_handler", "openai"),
    )

    await _run({LlmHeaders.PROVIDER.value: "OpenAI"})
    assert seen.get("ran") is True


@pytest.mark.parametrize(
    "provider", ["claude_code", "claude-code", "Claude-Code"]
)
@pytest.mark.asyncio
async def test_claude_code_provider_names(monkeypatch, provider):
    """Either separator, in either case, names the same backend."""
    seen = {}

    async def fake_handler(action):
        seen["ran"] = True
        await action["new_interactions"].put(Interaction(model="fake"))
        for name in ("event_stream", "text_output", "thoughts"):
            await action[name].finalize()
        await action["new_interactions"].finalize()

    module = types.ModuleType("a11.sdk._fake_claude_code")
    module.fake_handler = fake_handler
    monkeypatch.setitem(sys.modules, "a11.sdk._fake_claude_code", module)
    monkeypatch.setitem(
        illm._PROVIDERS,
        "claude_code",
        illm._Provider(
            "a11.sdk._fake_claude_code", "fake_handler", "claude-code"
        ),
    )

    out = await _run({LlmHeaders.PROVIDER.value: provider})
    assert seen.get("ran") is True
    assert len(out) == 1


def test_claude_code_install_hint():
    assert (
        illm.install_hint("claude_code") == "pip install 'a11-kit[claude-code]'"
    )


@pytest.mark.asyncio
async def test_import_failure_is_graceful(monkeypatch):
    monkeypatch.setitem(
        illm._PROVIDERS,
        "claude",
        illm._Provider("a11.sdk._does_not_exist", "x", "claude"),
    )
    with pytest.raises(StatusException) as excinfo:
        await _run({LlmHeaders.PROVIDER.value: "claude"})
    assert excinfo.value.status.code == StatusCode.FAILED_PRECONDITION


@pytest.mark.asyncio
async def test_unimportable_sdk_is_a_precondition(monkeypatch):
    """An SDK that is installed but broken is still a precondition failure."""
    module = types.ModuleType("a11.sdk._raises_on_use")

    def bad_import(name):
        raise RuntimeError("incompatible pydantic")

    monkeypatch.setitem(sys.modules, "a11.sdk._raises_on_use", module)
    monkeypatch.setattr(illm.importlib, "import_module", bad_import)
    monkeypatch.setitem(
        illm._PROVIDERS,
        "claude",
        illm._Provider("a11.sdk._raises_on_use", "x", "claude"),
    )
    with pytest.raises(StatusException) as excinfo:
        await _run({LlmHeaders.PROVIDER.value: "claude"})
    assert excinfo.value.status.code == StatusCode.FAILED_PRECONDITION
    assert "incompatible pydantic" in excinfo.value.status.message


def test_load_provider_rejects_an_unknown_name():
    with pytest.raises(StatusException) as excinfo:
        illm.load_provider("bogus")
    assert excinfo.value.status.code == StatusCode.INVALID_ARGUMENT


def test_load_provider_reports_a_missing_sdk(monkeypatch):
    monkeypatch.setitem(
        illm._PROVIDERS,
        "claude",
        illm._Provider("a11.sdk._does_not_exist", "x", "claude"),
    )
    with pytest.raises(StatusException) as excinfo:
        illm.load_provider("claude")
    assert excinfo.value.status.code == StatusCode.FAILED_PRECONDITION
    assert "pip install 'a11-kit[claude]'" in excinfo.value.status.message


def test_load_provider_imports_an_available_backend(monkeypatch):
    async def fake_handler(action):
        pass

    module = types.ModuleType("a11.sdk._fake_preload")
    module.fake_handler = fake_handler
    monkeypatch.setitem(sys.modules, "a11.sdk._fake_preload", module)
    monkeypatch.setitem(
        illm._PROVIDERS,
        "claude",
        illm._Provider("a11.sdk._fake_preload", "fake_handler", "claude"),
    )
    illm.load_provider("claude")


@pytest.mark.asyncio
async def test_text_output_and_thoughts_stream_through_router(monkeypatch):
    async def fake_handler(action):
        for token in ["Hel", "lo ", "world"]:
            await action["text_output"].put(token)
        await action["thoughts"].put("pondering")
        await action["new_interactions"].put(Interaction(model="fake"))
        for name in ("event_stream", "text_output", "thoughts"):
            await action[name].finalize()
        await action["new_interactions"].finalize()

    module = types.ModuleType("a11.sdk._fake_stream")
    module.fake_handler = fake_handler
    monkeypatch.setitem(sys.modules, "a11.sdk._fake_stream", module)
    monkeypatch.setitem(
        illm._PROVIDERS,
        "gemini",
        illm._Provider("a11.sdk._fake_stream", "fake_handler", "gemini"),
    )

    text = await _run({LlmHeaders.PROVIDER.value: "gemini"}, read="text_output")
    thoughts = await _run(
        {LlmHeaders.PROVIDER.value: "gemini"}, read="thoughts"
    )
    assert "".join(text) == "Hello world"
    assert "".join(thoughts) == "pondering"
