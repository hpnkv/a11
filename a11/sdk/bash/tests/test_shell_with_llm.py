# Copyright 2026 The A11 Authors.

"""A real tool-use test: a live LLM drives the shell tool end to end.

Skipped unless the Ollama development box is reachable. It registers the shell
Actions alongside ``interact_with_ollama`` and asks the model to run a command;
the assertion checks that the shell genuinely executed and produced the expected
output (captured by a manager output-processor), which is independent of however
the model phrases its final answer.
"""

import asyncio
import functools
import socket
from collections.abc import AsyncIterator

import pytest

import a11
from a11.actions import ActionRegistry
from a11.sdk import bash
from a11.sdk.bash.manager import ShellManager
from a11.sdk.llm import LlmHeaders

OLLAMA_HOST = "192.168.1.209"
OLLAMA_PORT = 11434
OLLAMA_BASE_URL = f"http://{OLLAMA_HOST}:{OLLAMA_PORT}"
MODEL = "glm-4.7-flash:latest"
TOKEN = "HELLO_FROM_A11"


def _ollama_reachable() -> bool:
    try:
        socket.create_connection((OLLAMA_HOST, OLLAMA_PORT), timeout=2).close()
        return True
    except OSError:
        return False


pytestmark = pytest.mark.skipif(
    not _ollama_reachable(),
    reason=f"Ollama dev box at {OLLAMA_BASE_URL} is not reachable.",
)


@pytest.mark.asyncio
async def test_llm_runs_a_command_through_the_shell_tool():
    from a11.sdk.ollama.interact_with_ollama import interact_with_ollama
    from a11.sdk.ollama.interact_with_ollama_schema import (
        INTERACT_WITH_OLLAMA_SCHEMA,
        CreateChatConfig,
        make_text_message_interaction,
    )
    from a11.sdk.llm import Role
    from a11.sdk.llm_tools.runner import get_tool_definitions

    manager = ShellManager()
    produced: list[str] = []

    async def capture(lines: AsyncIterator[str]) -> AsyncIterator[str]:
        async for line in lines:
            produced.append(line)
            yield line

    manager.output_processors.append(capture)

    registry = ActionRegistry()
    for schema, handler in bash.SHELL_ACTIONS:
        registry.register(
            schema.name, schema, functools.partial(handler, manager=manager)
        )
    registry.register(
        "interact_with_ollama",
        INTERACT_WITH_OLLAMA_SCHEMA,
        interact_with_ollama,
    )

    action = registry.make_action("interact_with_ollama")
    action.set_header(LlmHeaders.BASE_URL.value, OLLAMA_BASE_URL.encode())
    action.set_header(LlmHeaders.MODEL.value, MODEL.encode())
    action.set_header(LlmHeaders.ALLOWED_LLM_ACTIONS.value, b"shell_.*")
    action.run()

    system_prompt = (
        "You are a helpful assistant with access to a shell. When the user"
        " asks you to run something, call the shell_execute tool with the"
        " command and then report the result."
    )
    user_prompt = (
        f"Use the shell to print exactly the text {TOKEN} (run the command"
        f" `echo {TOKEN}`), then tell me what it printed."
    )

    tools = get_tool_definitions(registry, ["shell_execute"])
    await action["interactions"].finalize(
        make_text_message_interaction(
            user_prompt, system_prompt=system_prompt, role=Role.USER
        )
    )
    tool_port = action["tools"]
    for tool in tools:
        await tool_port.put(tool)
    await tool_port.finalize()
    await action["config"].finalize(CreateChatConfig())

    # Drain the streaming outputs (as raw fragments, to avoid imposing a type)
    # so the handler never blocks on a full buffer, and drive to completion.
    async def drain(port: str) -> None:
        async for _ in action[port].iter_fragments():
            pass

    drains = [
        asyncio.create_task(drain(port))
        for port in (
            "new_interactions",
            "text_output",
            "thoughts",
            "event_stream",
        )
    ]
    try:
        await asyncio.wait_for(action.wait(), timeout=180)
    finally:
        for task in drains:
            task.cancel()

    assert produced, "the model never invoked shell_execute"
    assert any(TOKEN in line for line in produced), (
        f"shell output {produced!r} did not contain {TOKEN!r}"
    )
