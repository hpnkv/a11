# Copyright 2026 The A11 Authors.

"""Drive one conversational turn through `codex exec --json`."""

import asyncio
import base64
import binascii
import contextlib
import json
import mimetypes
import os
import tempfile
import traceback
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


def _codex_to_normalized(interaction: llm.Interaction) -> llm.NormalizedMessage:
    return chat._vllm_to_normalized(interaction)


llm.register_interaction_normalizer(llm.Backend.CODEX, _codex_to_normalized)


def _latest_thread(
    interactions: list[llm.Interaction],
) -> tuple[str | None, int]:
    for index in range(len(interactions) - 1, -1, -1):
        interaction = interactions[index]
        metadata = interaction.backend_specific_metadata
        if metadata.get(llm.BACKEND_METADATA_KEY) != b"codex":
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


def _build_prompt(
    interactions: list[llm.Interaction], resume: str | None
) -> str:
    if not interactions:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="At least one interaction is required.",
        ).to_exception()
    system: list[str] = []
    if not resume:
        for chunk in interactions[0].system_instructions:
            value = a11.from_chunk(chunk)
            if not isinstance(value, str):
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message="Only text system instructions are allowed.",
                ).to_exception()
            system.append(value)
    if resume:
        body = "\n\n".join(_message_text(item) for item in interactions)
    else:
        turns = []
        for interaction in interactions:
            role = (
                "Assistant"
                if interaction.role == llm.Role.ASSISTANT
                else "User"
            )
            turns.append(f"{role}: {_message_text(interaction)}")
        body = "\n\n".join(turns)
    return "\n\n".join([*system, body])


def _tool_protocol_schema(
    tools: list[dict[str, Any]], final_schema: dict[str, Any] | None
) -> dict[str, Any]:
    calls = []
    for tool in tools:
        calls.append(
            {
                "type": "object",
                "properties": {
                    "type": {"const": "tool_call"},
                    "name": {"const": tool["name"]},
                    "arguments": (
                        tool.get("input_schema")
                        or {"type": "object", "properties": {}}
                    ),
                },
                "required": ["type", "name", "arguments"],
                "additionalProperties": False,
            }
        )
    response_schema = final_schema or {"type": "string"}
    calls.append(
        {
            "type": "object",
            "properties": {
                "type": {"const": "response"},
                "response": response_schema,
            },
            "required": ["type", "response"],
            "additionalProperties": False,
        }
    )
    return {"oneOf": calls}


def _tool_protocol_prompt(tools: list[dict[str, Any]]) -> str:
    definitions = json.dumps(tools, separators=(",", ":"))
    return (
        "A11 actions are available as external tools. Return a tool_call object"
        " when one is needed, then wait for its result. Return a response"
        " object only when the answer is complete. Available actions: "
        + definitions
    )


def _toml_value(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return str(value)
    return json.dumps(value, separators=(",", ":"))


def _options(
    config: CreateCodexSessionConfig,
    model: str,
    schema_path: str | None,
    image_paths: list[str] | None = None,
    *,
    resume: bool = False,
) -> list[str]:
    result = ["--json"]
    if not resume:
        result.extend(["--color", "never", "--sandbox", config.sandbox])
    if model:
        result.extend(["--model", model])
    for image_path in image_paths or []:
        result.extend(["--image", image_path])
    if config.profile and not resume:
        result.extend(["--profile", config.profile])
    if config.cwd and not resume:
        result.extend(["--cd", config.cwd])
    if not resume:
        for directory in config.add_dirs:
            result.extend(["--add-dir", directory])
    for flag, enabled in (
        ("--ephemeral", config.ephemeral),
        ("--skip-git-repo-check", config.skip_git_repo_check),
        ("--ignore-user-config", config.ignore_user_config),
        ("--ignore-rules", config.ignore_rules),
    ):
        if enabled:
            result.append(flag)
    overrides = dict(config.config_overrides)
    if config.reasoning_effort:
        overrides["model_reasoning_effort"] = config.reasoning_effort
    for key, value in overrides.items():
        result.extend(["-c", f"{key}={_toml_value(value)}"])
    if schema_path:
        result.extend(["--output-schema", schema_path])
    return result


async def _read_stderr(stream: asyncio.StreamReader) -> str:
    return (await stream.read()).decode(errors="replace")


async def _run_codex(
    action: a11.Action,
    config: CreateCodexSessionConfig,
    model: str,
    prompt: str,
    resume: str | None,
    schema_path: str | None,
    env: dict[str, str],
    image_paths: list[str] | None = None,
) -> tuple[str, str | None, llm.UsageMetadata | None]:
    options = _options(
        config,
        model,
        schema_path,
        image_paths,
        resume=resume is not None,
    )
    command = [config.cli_path, "exec"]
    if resume:
        command.extend(["resume", *options, resume, "-"])
    else:
        command.extend([*options, "-"])
    try:
        process = await asyncio.create_subprocess_exec(
            *command,
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
    process.stdin.write(prompt.encode())
    await process.stdin.drain()
    process.stdin.close()
    stderr_task = asyncio.create_task(_read_stderr(process.stderr))
    thread_id = resume
    final_text = ""
    usage = None
    try:
        async for line in process.stdout:
            try:
                event = json.loads(line)
            except json.JSONDecodeError as exc:
                raise Status(
                    code=StatusCode.INTERNAL,
                    message=f"Codex emitted invalid JSONL: {exc}",
                ).to_exception() from exc
            await action["event_stream"].put(event)
            event_type = event.get("type")
            if event_type == "thread.started":
                thread_id = event.get("thread_id") or thread_id
            elif event_type == "item.completed":
                item = event.get("item") or {}
                text = item.get("text") or ""
                if item.get("type") == "agent_message":
                    final_text = text
                elif item.get("type") == "reasoning" and text:
                    await action["thoughts"].put(text)
            elif event_type == "turn.completed":
                values = event.get("usage") or {}
                usage = llm.UsageMetadata(
                    input_tokens=values.get("input_tokens"),
                    output_tokens=values.get("output_tokens"),
                    cached_input_tokens=values.get("cached_input_tokens"),
                    total_tokens=(
                        (values.get("input_tokens") or 0)
                        + (values.get("output_tokens") or 0)
                    ),
                )
            elif event_type == "turn.failed":
                error = event.get("error") or {}
                raise Status(
                    code=StatusCode.INTERNAL,
                    message=error.get("message") or "Codex turn failed.",
                ).to_exception()
        return_code = await process.wait()
        stderr = await stderr_task
        if return_code != 0:
            raise Status(
                code=StatusCode.INTERNAL,
                message=stderr.strip()
                or f"Codex exited with status {return_code}.",
            ).to_exception()
        return final_text, thread_id, usage
    finally:
        if process.returncode is None:
            process.terminate()
            await process.wait()
        if not stderr_task.done():
            stderr_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await stderr_task


def _response_text(value: Any) -> str:
    if isinstance(value, str):
        return value
    return json.dumps(value, separators=(",", ":"))


def _write_prompt_images(
    interactions: list[llm.Interaction], directory: str
) -> list[str]:
    """Materialize inline prompt images for Codex's `--image` arguments."""
    parts: list[llm.NormalizedPart] = []
    for interaction in interactions:
        message = llm.normalize_interaction(interaction)
        parts.extend(message.parts)
    return _write_images(parts, directory, "prompt")


def _write_images(
    parts: list[llm.NormalizedPart], directory: str, label: str
) -> list[str]:
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
        path = Path(directory) / f"{label}-image-{len(paths)}{suffix}"
        path.write_bytes(data)
        paths.append(str(path))
    return paths


async def _write_tool_images(
    executed: runner.ExecutedActions, directory: str
) -> list[str]:
    images: list[llm.NormalizedPart] = []
    for call_id, fragments in executed.outputs.items():
        if executed.error_message(call_id) is not None:
            continue
        _, call_images = await llm.decoded_output_content(fragments)
        images.extend(call_images)
    return _write_images(images, directory, "tool-result")


async def interact_with_codex(action: a11.Action) -> None:
    """Run a Codex CLI turn, including schema-guided A11 tool calls."""
    deadline = a11.get_deadline(action)
    config = await action["config"].consume(
        CreateCodexSessionConfig,
        timeout=max(deadline - a11.now(), a11.zero_duration()),
        allow_none=True,
    )
    config = config or CreateCodexSessionConfig()
    model = (
        action.get_header(llm.LlmHeaders.MODEL.value, decode=True)
        or DEFAULT_MODEL
    )
    interactions = [interaction async for interaction in action["interactions"]]
    recorded_thread, recorded_at = _latest_thread(interactions)
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
    definitions = await runner.collect_tools(action, deadline)
    if definitions and config.ephemeral:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Codex A11 tools need a resumable, non-ephemeral thread.",
        ).to_exception()
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
    try:
        with tempfile.TemporaryDirectory(prefix="a11-codex-") as directory:
            image_paths = _write_prompt_images(prompt_interactions, directory)
            schema = (
                _tool_protocol_schema(definitions, config.output_schema)
                if definitions
                else config.output_schema
            )
            schema_path = None
            if schema is not None:
                path = Path(directory) / "output-schema.json"
                path.write_text(json.dumps(schema), encoding="utf-8")
                schema_path = str(path)
            if definitions:
                prompt = f"{_tool_protocol_prompt(definitions)}\n\n{prompt}"

            failed_rounds = llm.FailedToolRounds()
            while True:
                text, thread_id, usage = await _run_codex(
                    action,
                    config,
                    model,
                    prompt,
                    resume,
                    schema_path,
                    env,
                    image_paths,
                )
                image_paths = []
                resume = thread_id
                protocol = None
                if definitions:
                    try:
                        protocol = json.loads(text)
                    except json.JSONDecodeError as exc:
                        raise Status(
                            code=StatusCode.INTERNAL,
                            message=(
                                f"Codex tool response is not valid JSON: {exc}"
                            ),
                        ).to_exception() from exc

                if not definitions or protocol.get("type") == "response":
                    answer = (
                        text
                        if not definitions
                        else _response_text(protocol.get("response"))
                    )
                    await action["text_output"].put(answer)
                    interaction = llm.Interaction(
                        previous_interaction_id=previous_id,
                        role=llm.Role.ASSISTANT,
                        created_at_millis=(
                            a11.now().nanoseconds_since_epoch // 1000000
                        ),
                        model=model,
                        content=[
                            a11.to_chunk(
                                {
                                    "role": "assistant",
                                    "content": answer,
                                }
                            )
                        ],
                        backend_specific_metadata={
                            llm.BACKEND_METADATA_KEY: b"codex",
                            **(
                                {THREAD_ID_METADATA_KEY: thread_id.encode()}
                                if thread_id
                                else {}
                            ),
                        },
                        usage_metadata=usage,
                    )
                    await action["new_interactions"].put(interaction)
                    if action.trace_id:
                        try:
                            action.set_span_output(answer)
                        except Exception:
                            logging.debug(
                                "failed to record LLM span output",
                                exc_info=True,
                            )
                    break

                call = llm.ToolCall(
                    name=protocol.get("name", ""),
                    id=f"call_{os.urandom(8).hex()}",
                    params=protocol.get("arguments") or {},
                )
                interaction = llm.Interaction(
                    previous_interaction_id=previous_id,
                    role=llm.Role.ASSISTANT,
                    created_at_millis=a11.now().nanoseconds_since_epoch
                    // 1000000,
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
                                            "arguments": json.dumps(
                                                call.params
                                            ),
                                        },
                                    }
                                ],
                            }
                        )
                    ],
                    backend_specific_metadata={
                        llm.BACKEND_METADATA_KEY: b"codex",
                        **(
                            {THREAD_ID_METADATA_KEY: thread_id.encode()}
                            if thread_id
                            else {}
                        ),
                    },
                    usage_metadata=usage,
                )
                previous_id = interaction.id
                rejected = await llm.add_tool_calls_to_interaction(
                    [call], interaction, action.get_registry()
                )
                await action["new_interactions"].put(interaction)
                executed = await runner.execute_actions_from_interaction(
                    interaction,
                    action,
                    action.get_registry(),
                    rejected=rejected,
                )
                results = await chat._build_tool_results_from_outputs(executed)
                result_text = llm.stringify_content(results)
                image_paths = await _write_tool_images(executed, directory)
                result = llm.Interaction(
                    previous_interaction_id=previous_id,
                    role=llm.Role.USER,
                    created_at_millis=a11.now().nanoseconds_since_epoch
                    // 1000000,
                    action_outputs=executed.outputs,
                    content=[
                        a11.to_chunk({"role": "user", "content": result_text})
                    ],
                    backend_specific_metadata={
                        llm.BACKEND_METADATA_KEY: b"codex",
                        **executed.log_metadata(),
                    },
                )
                previous_id = result.id
                await action["new_interactions"].put(result)
                if not failed_rounds.record(executed):
                    break
                prompt = (
                    f"A11 tool result for {call.name} ({call.id}):\n"
                    f"{result_text}\nContinue the task using the same protocol."
                )
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
