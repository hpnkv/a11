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

"""Map public harness callbacks onto shared LLM context utilities."""

from __future__ import annotations

import asyncio
import inspect
import json
import time
from collections.abc import Awaitable, Callable
from typing import Any

from a11.sdk.llm_context.config import LlmContextConfig
from a11.sdk.llm_context.runtime import project_messages
from a11.sdk.llm_context.shell import intercept_shell_result
from a11.status import Status, StatusCode


async def compact_codex_thread(
    request: Callable[[str, dict], Awaitable[dict]],
    next_event: Callable[[], Awaitable[dict]],
    thread_id: str,
    *,
    timeout: float = 120,
    reporter: Callable[[dict], None | Awaitable[None]] | None = None,
) -> dict:
    """Await native Codex compaction before sending the next turn.

    Args:
        request: Existing app-server RPC transport returning decoded results.
        next_event: Dedicated subscription established before the request. It
            yields notifications while the host retains its ordinary reader.
        thread_id: Explicit thread identity to compact.
        timeout: Whole-operation deadline in seconds.
        reporter: Optional sync/async metadata sink.

    Returns:
        Completion evidence from the matching compaction item and turn.

    Raises:
        StatusException: The matching turn fails or evidence is incomplete.
        TimeoutError: Completion is indeterminate; reconcile thread state
            before issuing another compaction request.
    """
    started = time.monotonic()
    item_turn = None
    completed_turns = {}
    async with asyncio.timeout(timeout):
        await request("thread/compact/start", {"threadId": thread_id})
        while True:
            event = await next_event()
            params = event.get("params", {})
            if params.get("threadId") != thread_id:
                continue
            method = event.get("method")
            item = params.get("item", {})
            if (
                method == "item/completed"
                and item.get("type") == "contextCompaction"
            ):
                item_turn = params.get("turnId")
            if method == "turn/completed":
                turn = params.get("turn", {})
                completed_turns[turn.get("id")] = turn
            if item_turn and item_turn in completed_turns:
                turn = completed_turns[item_turn]
                if turn.get("status") != "completed":
                    raise Status(
                        code=StatusCode.FAILED_PRECONDITION,
                        message="Codex compaction turn did not complete",
                    ).to_exception()
                result = {
                    "schema": "a11.llm-context.report/v1",
                    "operation": "native_compaction",
                    "harness": "codex",
                    "session_id": thread_id,
                    "turn_id": item_turn,
                    "installed": True,
                    "elapsed_seconds": time.monotonic() - started,
                }
                if reporter is not None:
                    observed = reporter(result)
                    if inspect.isawaitable(observed):
                        await observed
                return result


async def compact_claude_session(
    client: Any,
    *,
    instructions: str = "",
    timeout: float = 120,
    reporter: Callable[[dict], None | Awaitable[None]] | None = None,
    on_message: Callable[[object], None | Awaitable[None]] | None = None,
) -> dict:
    """Compact a connected Claude Agent SDK client through its public command.

    The host serializes this operation with its receive_response reader. The
    client remains connected and resumes through query after this returns.

    Args:
        client: Connected ClaudeSDKClient with query and receive_response.
        instructions: Optional native summary focus instructions. State which
            facts must survive independently of transcript/file access.
        timeout: Whole-operation deadline in seconds; timeout is indeterminate.
        reporter: Optional sync/async operation metadata sink.
        on_message: Optional observer for every compaction message, including
            provider usage and compact_boundary evidence.

    Returns:
        Native installation evidence after compact_boundary and a successful
        ResultMessage. Explicit insufficient-history status returns
        installed=False. No generated summary is injected into the session.

    Raises:
        StatusException: The command fails or emits no compaction boundary.
    """
    started = time.monotonic()
    boundary = False
    insufficient_history = False
    result = None
    if len(instructions.encode()) > 8192:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Native compaction instructions exceed 8192 bytes",
        ).to_exception()
    async with asyncio.timeout(timeout):
        await client.query(
            "/compact" + (" " + instructions if instructions else "")
        )
        async for message in client.receive_response():
            if on_message is not None:
                observed = on_message(message)
                if inspect.isawaitable(observed):
                    await observed
            if getattr(message, "subtype", None) == "compact_boundary":
                boundary = True
            data = getattr(message, "data", None)
            if (
                getattr(message, "subtype", None) == "status"
                and isinstance(data, dict)
                and data.get("compact_result") == "failed"
                and data.get("compact_error")
                == "Not enough messages to compact."
            ):
                insufficient_history = True
            if type(message).__name__ == "ResultMessage":
                result = message
    if (
        (not boundary and not insufficient_history)
        or result is None
        or getattr(result, "is_error", False)
    ):
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="Claude Code did not acknowledge native compaction",
        ).to_exception()
    report = {
        "schema": "a11.llm-context.report/v1",
        "operation": "native_compaction",
        "harness": "claude_code",
        "session_id": getattr(result, "session_id", ""),
        "installed": boundary,
        "skip_reason": None if boundary else "insufficient_history",
        "elapsed_seconds": time.monotonic() - started,
    }
    if reporter is not None:
        observed = reporter(report)
        if inspect.isawaitable(observed):
            await observed
    return report


def codex_tool_hook(event: dict, config: LlmContextConfig) -> dict:
    """Return a Codex PostToolUse replacement for a proven successful result.

    Args:
        event: Public PostToolUse input. tool_response must contain an integer
            exit_code and captured stdout/stderr. Unknown result shapes pass.
        config: Explicit shell policy; absence of compression returns {}.

    Returns:
        A continue/stopReason hook decision when the model view changes, or {}.
        The original tool result remains with the host. Code-mode promises use
        Codex's normal completion path.
    """
    raw = event.get("tool_response")
    if not isinstance(raw, dict):
        return {}
    projected = intercept_shell_result(raw, config)
    if projected["view"] == raw:
        return {}
    return {
        "continue": False,
        "stopReason": json.dumps(projected["view"], ensure_ascii=False),
    }


def claude_code_hooks(config: LlmContextConfig) -> dict:
    """Build optional Claude Agent SDK hooks for successful Bash presentation.

    Args:
        config: Explicit shell policy. Errors and interrupted results pass.

    Returns:
        ClaudeAgentOptions hooks using the installed optional claude-agent-sdk.
        Only stdout/stderr presentation changes; other output fields survive.
    """
    from claude_agent_sdk import HookMatcher

    async def post_tool_use(event, tool_use_id, context):
        raw = event.get("tool_response")
        if (
            not isinstance(raw, dict)
            or raw.get("interrupted")
            or raw.get("isImage")
        ):
            return {}
        exit_code = raw.get("exit_code", raw.get("exitCode"))
        if type(exit_code) is not int:
            return {}
        projected = intercept_shell_result(
            {**raw, "exit_code": exit_code}, config
        )
        view = {k: projected["view"][k] for k in raw}
        if view == raw:
            return {}
        return {
            "hookSpecificOutput": {
                "hookEventName": "PostToolUse",
                "updatedToolOutput": view,
            }
        }

    return {"PostToolUse": [HookMatcher(matcher="Bash", hooks=[post_tool_use])]}


async def project_acp_prompt(params: dict, config: LlmContextConfig) -> dict:
    """Project editable text in an ACP session/prompt request.

    Args:
        params: Session prompt parameters containing prompt content blocks.
        config: Explicit text compression policy. ACP resource/image blocks
            and session identity retain their original values.

    Returns:
        Parameters with projected user text. Filesystem and terminal RPC
        responses stay under the host's canonical resource contract.
    """
    prompt = params.get("prompt")
    if not isinstance(prompt, list):
        return params
    projected = await project_messages(
        [{"role": "user", "content": prompt}], config
    )
    return {**params, "prompt": projected[0]["content"]}


async def compact_acp_session(
    request: Callable[[str, dict], Awaitable[dict]],
    session_id: str,
    commands: list[dict],
) -> dict:
    """Invoke Junie's advertised native compaction command through ACP.

    Args:
        request: Existing ACP transport. It routes session/update notifications
            through the host and resolves session/prompt on completion.
        session_id: Explicit negotiated session identity.
        commands: Latest advertised command list for this session.

    Returns:
        The acknowledged session/prompt response. Missing /compress support
        raises FAILED_PRECONDITION before any request is sent.
    """
    if not any(c.get("name") in ("compress", "/compress") for c in commands):
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="ACP session does not advertise the compress command",
        ).to_exception()
    return await request(
        "session/prompt",
        {
            "sessionId": session_id,
            "prompt": [{"type": "text", "text": "/compress"}],
        },
    )
