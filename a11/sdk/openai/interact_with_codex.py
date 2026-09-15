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

"""Drive one conversational turn through the Codex app-server protocol."""

import asyncio
import base64
import binascii
import contextlib
import hashlib
import json
import mimetypes
import os
import re
import tempfile
import traceback
from collections.abc import Awaitable, Callable
from pathlib import Path
from typing import Any

from absl import logging

import a11
from a11.sdk import llm
from a11.sdk.llm_tools import runner
from a11.sdk.openai.interact_with_codex_schema import (
    CreateCodexSessionConfig,
    DEFAULT_MODEL,
    THREAD_ID_METADATA_KEY,
)
from a11.sdk.vllm import interact_with_vllm as chat
from a11.status import Status, StatusCode, StatusException


CODEX_TRANSPORT_METADATA_KEY = "codex_transport"
CODEX_TRANSPORT = b"app-server-dynamic-tools/v1"
CODEX_TOOLS_METADATA_KEY = "codex_tools_sha256"


def _codex_to_normalized(interaction: llm.Interaction) -> llm.NormalizedMessage:
    return chat._vllm_to_normalized(interaction)


llm.register_interaction_normalizer(llm.Backend.CODEX, _codex_to_normalized)


def _latest_thread(
    interactions: list[llm.Interaction], tools_digest: bytes
) -> tuple[str | None, int]:
    for index in range(len(interactions) - 1, -1, -1):
        metadata = interactions[index].backend_specific_metadata
        if metadata.get(llm.BACKEND_METADATA_KEY) != b"codex":
            continue
        if metadata.get(CODEX_TRANSPORT_METADATA_KEY) != CODEX_TRANSPORT:
            continue
        if metadata.get(CODEX_TOOLS_METADATA_KEY, b"") != tools_digest:
            continue
        if value := metadata.get(THREAD_ID_METADATA_KEY):
            return value.decode(), index
    return None, -1


def _message_text(interaction: llm.Interaction) -> str:
    message = llm.normalize_interaction(interaction)
    pieces: list[str] = []
    for part in message.parts:
        if part.type == llm.NormalizedContentType.TEXT and part.text:
            pieces.append(part.text)
        elif part.type == llm.NormalizedContentType.TOOL_RESULT:
            pieces.append(
                f"Tool result ({part.call_id or ''}): {part.content or ''}"
            )
    return "\n".join(pieces)


def _system_instructions(interactions: list[llm.Interaction]) -> str | None:
    if not interactions:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="At least one interaction is required.",
        ).to_exception()
    values: list[str] = []
    for chunk in interactions[0].system_instructions:
        value = a11.from_chunk(chunk)
        if not isinstance(value, str):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Only text system instructions are allowed.",
            ).to_exception()
        values.append(value)
    return "\n\n".join(values) or None


def _build_prompt(
    interactions: list[llm.Interaction], resume: str | None
) -> str:
    if not interactions:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="At least one interaction is required.",
        ).to_exception()
    if resume or len(interactions) == 1:
        return "\n\n".join(_message_text(item) for item in interactions)
    turns = []
    for interaction in interactions:
        role = "Assistant" if interaction.role == llm.Role.ASSISTANT else "User"
        turns.append(f"{role}: {_message_text(interaction)}")
    return "\n\n".join(turns)


def _toml_value(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return str(value)
    return json.dumps(value, separators=(",", ":"))


def _app_server_command(config: CreateCodexSessionConfig) -> list[str]:
    command = [config.cli_path]
    if config.profile:
        command.extend(["--profile", config.profile])
    command.append("app-server")
    for key, value in config.config_overrides.items():
        command.extend(["-c", f"{key}={_toml_value(value)}"])
    return command


def _tool_name(name: str, used: set[str]) -> str:
    candidate = re.sub(r"[^A-Za-z0-9_-]", "_", name)[:128] or "a11_tool"
    if candidate == "mcp" or candidate.startswith("mcp__"):
        candidate = f"a11_{candidate}"
    if candidate not in used:
        used.add(candidate)
        return candidate
    suffix = hashlib.sha256(name.encode()).hexdigest()[:10]
    candidate = f"{candidate[:117]}_{suffix}"
    while candidate in used:
        suffix = hashlib.sha256((name + suffix).encode()).hexdigest()[:10]
        candidate = f"{candidate[:117]}_{suffix}"
    used.add(candidate)
    return candidate


def _dynamic_tools(
    definitions: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], dict[str, str], bytes]:
    used: set[str] = set()
    result: list[dict[str, Any]] = []
    names: dict[str, str] = {}
    for definition in definitions:
        original = definition["name"]
        exposed = _tool_name(original, used)
        names[exposed] = original
        result.append(
            {
                "type": "function",
                "name": exposed,
                "description": definition.get("description") or "",
                "inputSchema": definition.get("input_schema")
                or {"type": "object", "properties": {}},
            }
        )
    encoded = json.dumps(result, sort_keys=True, separators=(",", ":")).encode()
    return result, names, hashlib.sha256(encoded).hexdigest().encode()


def _write_prompt_images(
    interactions: list[llm.Interaction], directory: str
) -> list[str]:
    """Materialize inline prompt images for Codex local-image inputs."""
    parts: list[llm.NormalizedPart] = []
    for interaction in interactions:
        parts.extend(llm.normalize_interaction(interaction).parts)
    paths: list[str] = []
    for part in parts:
        if part.type != llm.NormalizedContentType.IMAGE or not part.data:
            continue
        try:
            data = base64.b64decode(part.data, validate=True)
        except (ValueError, binascii.Error) as exc:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="A Codex image is not valid base64.",
            ).to_exception() from exc
        suffix = mimetypes.guess_extension(part.mime_type or "") or ".img"
        path = Path(directory) / f"prompt-image-{len(paths)}{suffix}"
        path.write_bytes(data)
        paths.append(str(path))
    return paths


async def _read_stderr(stream: asyncio.StreamReader) -> str:
    return (await stream.read()).decode(errors="replace")


async def _read_jsonl_line(stream: asyncio.StreamReader) -> bytes:
    """Read one app-server message without asyncio's 64 KiB line bound."""
    chunks: list[bytes] = []
    while True:
        try:
            chunks.append(await stream.readuntil(b"\n"))
            return b"".join(chunks)
        except asyncio.LimitOverrunError as error:
            chunks.append(await stream.readexactly(error.consumed))
        except asyncio.IncompleteReadError as error:
            chunks.append(error.partial)
            return b"".join(chunks)


def _rpc_error(message: dict[str, Any]) -> StatusException:
    error = message.get("error") or {}
    return Status(
        code=StatusCode.INTERNAL,
        message=error.get("message") or "Codex app-server request failed.",
        details=[error] if error else [],
    ).to_exception()


ToolHandler = Callable[[dict[str, Any], str], Awaitable[dict[str, Any]]]


async def _run_codex(
    action: a11.Action,
    config: CreateCodexSessionConfig,
    model: str,
    prompt: str,
    resume: str | None,
    developer_instructions: str | None,
    dynamic_tools: list[dict[str, Any]],
    tool_handler: ToolHandler,
    env: dict[str, str],
    image_paths: list[str] | None = None,
) -> tuple[str, str, llm.UsageMetadata | None]:
    """Run one turn over Codex's JSONL stdio app-server protocol."""
    try:
        process = await asyncio.create_subprocess_exec(
            *_app_server_command(config),
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            env=env,
        )
    except FileNotFoundError as exc:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                f"Codex CLI {config.cli_path!r} was not found. Install it with"
                " `npm install -g @openai/codex`."
            ),
        ).to_exception() from exc

    assert process.stdin is not None
    assert process.stdout is not None
    assert process.stderr is not None
    write_lock = asyncio.Lock()
    next_id = 1

    async def send(message: dict[str, Any]) -> None:
        async with write_lock:
            process.stdin.write(
                json.dumps(message, separators=(",", ":")).encode() + b"\n"
            )
            await process.stdin.drain()

    async def request(method: str, params: dict[str, Any]) -> int:
        nonlocal next_id
        request_id = next_id
        next_id += 1
        await send({"method": method, "id": request_id, "params": params})
        return request_id

    stderr_task = asyncio.create_task(_read_stderr(process.stderr))
    pending_calls: set[asyncio.Task[None]] = set()
    responses: dict[int, dict[str, Any]] = {}
    final_text = ""
    streamed_text = False
    usage = None
    turn_done = False

    async def answer_tool(message: dict[str, Any], thread_id: str) -> None:
        try:
            result = await tool_handler(message.get("params") or {}, thread_id)
            await send({"id": message["id"], "result": result})
        except Exception as exc:
            logging.debug("Codex dynamic tool call failed", exc_info=True)
            await send(
                {
                    "id": message["id"],
                    "result": {
                        "contentItems": [
                            {"type": "inputText", "text": str(exc)}
                        ],
                        "success": False,
                    },
                }
            )

    async def read_one(thread_id: str = "") -> None:
        nonlocal final_text, streamed_text, usage, turn_done
        line = await _read_jsonl_line(process.stdout)
        if not line:
            raise Status(
                code=StatusCode.INTERNAL,
                message="Codex app-server ended before the turn completed.",
            ).to_exception()
        try:
            message = json.loads(line)
        except json.JSONDecodeError as exc:
            raise Status(
                code=StatusCode.INTERNAL,
                message=f"Codex emitted invalid JSONL: {exc}",
            ).to_exception() from exc
        await action["event_stream"].put(message)
        if "id" in message and "method" not in message:
            responses[message["id"]] = message
        method = message.get("method")
        params = message.get("params") or {}
        if method == "item/tool/call":
            task = asyncio.create_task(answer_tool(message, thread_id))
            pending_calls.add(task)
            task.add_done_callback(pending_calls.discard)
        elif method == "item/agentMessage/delta":
            if delta := params.get("delta"):
                streamed_text = True
                await action["text_output"].put(delta)
        elif method in (
            "item/reasoning/summaryTextDelta",
            "item/reasoning/textDelta",
        ):
            if delta := params.get("delta"):
                await action["thoughts"].put(delta)
        elif method == "item/completed":
            item = params.get("item") or {}
            if item.get("type") == "agentMessage":
                final_text = item.get("text") or final_text
        elif method == "thread/tokenUsage/updated":
            values = (params.get("tokenUsage") or {}).get("last") or {}
            usage = llm.UsageMetadata(
                input_tokens=values.get("inputTokens"),
                output_tokens=values.get("outputTokens"),
                cached_input_tokens=values.get("cachedInputTokens"),
                total_tokens=values.get("totalTokens"),
            )
        elif method == "turn/completed":
            turn = params.get("turn") or {}
            if turn.get("status") != "completed":
                error = turn.get("error") or {}
                raise Status(
                    code=StatusCode.INTERNAL,
                    message=(
                        error.get("message")
                        or f"Codex turn ended as {turn.get('status')}."
                    ),
                ).to_exception()
            turn_done = True

    async def wait_response(
        request_id: int, thread_id: str = ""
    ) -> dict[str, Any]:
        while request_id not in responses:
            await read_one(thread_id)
        response = responses.pop(request_id)
        if "error" in response:
            raise _rpc_error(response)
        return response.get("result") or {}

    try:
        initialize_id = await request(
            "initialize",
            {
                "clientInfo": {"name": "a11", "title": "A11", "version": "1"},
                "capabilities": {"experimentalApi": True},
            },
        )
        await wait_response(initialize_id)
        await send({"method": "initialized", "params": {}})
        workspace_roots = [
            str(Path(path).resolve())
            for path in ([config.cwd] if config.cwd else []) + config.add_dirs
        ]
        common: dict[str, Any] = {
            "model": model or None,
            "cwd": config.cwd,
            "sandbox": config.sandbox,
            "approvalPolicy": "never",
        }
        if workspace_roots:
            common["runtimeWorkspaceRoots"] = workspace_roots
        if resume:
            thread_request = await request(
                "thread/resume", {"threadId": resume, **common}
            )
        else:
            thread_request = await request(
                "thread/start",
                {
                    **common,
                    "developerInstructions": developer_instructions,
                    "ephemeral": config.ephemeral,
                    "dynamicTools": dynamic_tools,
                },
            )
        thread_result = await wait_response(thread_request)
        thread_id = (thread_result.get("thread") or {}).get("id") or resume
        if not thread_id:
            raise Status(
                code=StatusCode.INTERNAL,
                message="Codex did not return a thread id.",
            ).to_exception()
        turn_input = [{"type": "text", "text": prompt}]
        turn_input.extend(
            {"type": "localImage", "path": path}
            for path in image_paths or []
        )
        turn_request = await request(
            "turn/start",
            {
                "threadId": thread_id,
                "input": turn_input,
                "model": model or None,
                "effort": config.reasoning_effort,
                "outputSchema": config.output_schema,
            },
        )
        await wait_response(turn_request, thread_id)
        while not turn_done:
            await read_one(thread_id)
        if pending_calls:
            await asyncio.gather(*pending_calls)
        if final_text and not streamed_text:
            await action["text_output"].put(final_text)
        return final_text, thread_id, usage
    finally:
        for task in pending_calls:
            task.cancel()
        if pending_calls:
            await asyncio.gather(*pending_calls, return_exceptions=True)
        if process.returncode is None:
            process.terminate()
            await process.wait()
        if not stderr_task.done():
            stderr_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await stderr_task


def _metadata(thread_id: str, tools_digest: bytes) -> dict[str, bytes]:
    return {
        llm.BACKEND_METADATA_KEY: b"codex",
        THREAD_ID_METADATA_KEY: thread_id.encode(),
        CODEX_TRANSPORT_METADATA_KEY: CODEX_TRANSPORT,
        CODEX_TOOLS_METADATA_KEY: tools_digest,
    }


async def interact_with_codex(action: a11.Action) -> None:
    """Run a Codex turn with A11 actions exposed as native dynamic tools."""
    deadline = a11.get_deadline(action)
    config = await action["config"].consume(
        CreateCodexSessionConfig,
        timeout=max(deadline - a11.now(), a11.zero_duration()),
        allow_none=True,
    )
    config = config or CreateCodexSessionConfig()
    if config.ignore_user_config or config.ignore_rules:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                "ignore_user_config and ignore_rules are not supported by"
                " Codex app-server. Use config_overrides for Codex settings."
            ),
        ).to_exception()
    model = (
        action.get_header(llm.LlmHeaders.MODEL.value, decode=True)
        or DEFAULT_MODEL
    )
    interactions = [interaction async for interaction in action["interactions"]]
    definitions = await runner.collect_tools(action, deadline)
    dynamic_tools, tool_names, tools_digest = _dynamic_tools(definitions)
    recorded_thread, recorded_at = _latest_thread(interactions, tools_digest)
    resume = config.resume or recorded_thread
    if config.resume:
        prompt_interactions = interactions[-1:]
    elif recorded_thread:
        prompt_interactions = interactions[recorded_at + 1 :]
        if not prompt_interactions:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="A resumed Codex thread needs a new interaction.",
            ).to_exception()
    else:
        prompt_interactions = interactions
    prompt = _build_prompt(prompt_interactions, resume)
    developer_instructions = (
        None if resume else _system_instructions(interactions)
    )
    env = dict(os.environ)
    if api_key := action.get_header(llm.LlmHeaders.API_KEY.value, decode=True):
        env["CODEX_API_KEY"] = api_key
    if action.trace_id:
        try:
            action.set_span_name("Codex interaction")
            action.set_span_attribute("gen_ai.system", "codex")
            if model:
                action.set_span_attribute("gen_ai.request.model", model)
            action.set_span_input(prompt)
        except Exception:
            logging.debug("failed to record LLM span input", exc_info=True)

    previous_id = interactions[-1].id if interactions else ""
    failed_rounds = llm.FailedToolRounds()
    tool_lock = asyncio.Lock()
    current_thread = resume or ""

    async def handle_tool(
        params: dict[str, Any], thread_id: str
    ) -> dict[str, Any]:
        nonlocal previous_id, current_thread
        current_thread = thread_id
        exposed_name = params.get("tool") or ""
        call = llm.ToolCall(
            name=tool_names.get(exposed_name, exposed_name),
            id=params.get("callId") or f"call_{os.urandom(8).hex()}",
            params=params.get("arguments") or {},
        )
        async with tool_lock:
            interaction = llm.Interaction(
                previous_interaction_id=previous_id,
                role=llm.Role.ASSISTANT,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                model=model,
                content=[
                    a11.to_chunk(
                        {
                            "role": "assistant",
                            "content": "",
                            "tool_calls": [
                                {
                                    "id": call.id,
                                    "type": "function",
                                    "function": {
                                        "name": call.name,
                                        "arguments": json.dumps(call.params),
                                    },
                                }
                            ],
                        }
                    )
                ],
                backend_specific_metadata=_metadata(thread_id, tools_digest),
            )
            rejected = await llm.add_tool_calls_to_interaction(
                [call], interaction, action.get_registry()
            )
            previous_id = interaction.id
            await action["new_interactions"].put(interaction)
        executed = await runner.execute_actions_from_interaction(
            interaction,
            action,
            action.get_registry(),
            rejected=rejected,
            max_output_bytes=None,
        )
        failure = executed.error_message(call.id)
        text = failure or ""
        images: list[llm.NormalizedPart] = []
        if failure is None:
            text, images = await llm.decoded_output_content(
                executed.outputs.get(call.id, [])
            )
        result_content = [{"type": "inputText", "text": text}]
        result_content.extend(
            {
                "type": "inputImage",
                "imageUrl": f"data:{image.mime_type};base64,{image.data}",
            }
            for image in images
            if image.data and image.mime_type
        )
        async with tool_lock:
            result = llm.Interaction(
                previous_interaction_id=previous_id,
                role=llm.Role.USER,
                created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
                action_outputs=executed.outputs,
                content=[a11.to_chunk({"role": "user", "content": text})],
                backend_specific_metadata={
                    **_metadata(thread_id, tools_digest),
                    **executed.log_metadata(),
                },
            )
            previous_id = result.id
            await action["new_interactions"].put(result)
            if not failed_rounds.record(executed):
                return {
                    "contentItems": [
                        {
                            "type": "inputText",
                            "text": (
                                "A11 stopped after repeated failed tool calls."
                            ),
                        }
                    ],
                    "success": False,
                }
        return {"contentItems": result_content, "success": failure is None}

    try:
        with tempfile.TemporaryDirectory(prefix="a11-codex-") as directory:
            image_paths = _write_prompt_images(prompt_interactions, directory)
            answer, current_thread, usage = await _run_codex(
                action,
                config,
                model,
                prompt,
                resume,
                developer_instructions,
                dynamic_tools,
                handle_tool,
                env,
                image_paths,
            )
        interaction = llm.Interaction(
            previous_interaction_id=previous_id,
            role=llm.Role.ASSISTANT,
            created_at_millis=a11.now().nanoseconds_since_epoch // 1000000,
            model=model,
            content=[a11.to_chunk({"role": "assistant", "content": answer})],
            backend_specific_metadata=_metadata(current_thread, tools_digest),
            usage_metadata=usage,
        )
        await action["new_interactions"].put(interaction)
        if action.trace_id:
            try:
                action.set_span_output(answer)
            except Exception:
                logging.debug("failed to record LLM span output", exc_info=True)
    except StatusException:
        raise
    except Exception as exc:
        logging.debug("Codex interaction failed", exc_info=True)
        raise Status(
            code=StatusCode.INTERNAL, message=traceback.format_exc()
        ).to_exception() from exc
    else:
        await action["event_stream"].finalize()
        await action["thoughts"].finalize()
        await action["text_output"].finalize()
        await action["new_interactions"].finalize()
