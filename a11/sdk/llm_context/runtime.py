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

"""Apply context policy at SDK action boundaries and native Flow calls."""

from __future__ import annotations

import asyncio
import copy
import functools
import hashlib
import inspect
import json
import time
import uuid
from collections.abc import AsyncIterable, AsyncIterator, Awaitable, Callable
from typing import TYPE_CHECKING

from a11.sdk.llm_context.config import (
    LlmContextConfig,
    LlmContextHeaders,
    LlmContextServices,
    _active,
    authorized_scope,
    config_for,
    services_for,
)
from a11.sdk.llm_context.content import compress_content, count_context
from a11.sdk.llm_context.history import compact_history
from a11.sdk.llm_context.shell import intercept_shell_result
from a11.status import Status, StatusCode

if TYPE_CHECKING:
    from a11.sdk.llm import Interaction


async def emit_report(action, report: dict) -> None:
    report = {
        **report,
        "operation_id": report.get("operation_id", str(uuid.uuid4())),
        "action_id": action.id,
    }
    services = services_for(action)
    if services.recorder is not None:
        await asyncio.to_thread(services.recorder.record, report)
    await action.log(report, channel="llm_context", internal=True)


def llm_context_handler(handler):
    """Validate declared action policy without changing the handler's
    signature.
    """

    @functools.wraps(handler)
    async def wrapped(action, *args, **kwargs):
        raw = action.get_header(LlmContextHeaders.CONFIG)
        if raw is None:
            return await handler(action, *args, **kwargs)
        config = config_for(action)
        token = _active.set((bytes(raw), config))
        try:
            return await handler(action, *args, **kwargs)
        finally:
            _active.reset(token)

    return wrapped


async def prepare_tool_result(
    action, content: str, failure: str | None = None, *, source=None
) -> str:
    """Prepare successful text only; original fragments and failures remain
    intact.
    """
    if action is None or failure is not None:
        return content
    config = config_for(action)
    if interaction_stage(action):
        return content
    if (
        source in ("spawn_process", "run_command")
        and config.shell.mode != "off"
    ):
        started = time.monotonic()
        try:
            raw = json.loads(content)
        except ValueError:
            raw = None
        payload = raw.get("result", raw) if isinstance(raw, dict) else None
        if isinstance(payload, dict) and type(payload.get("exit_code")) is int:
            captured = {
                **payload,
                "truncated": payload.get("output_truncated", False),
                "complete": (
                    payload.get("complete", True)
                    and not payload.get("stopped_early", False)
                ),
            }
            if source == "spawn_process":
                captured.pop("stdout", None)
                captured.pop("stderr", None)
            result = await asyncio.to_thread(
                intercept_shell_result,
                captured,
                config,
                counter=services_for(action).token_counter,
            )
            projected = content
            if result["view"] != captured:
                view = {
                    **payload,
                    **{k: v for k, v in result["view"].items() if k in payload},
                }
                outer = {**raw, "result": view} if payload is not raw else view
                if payload is not raw and raw.get(
                    "output_lines"
                ) == payload.get("output_lines"):
                    outer["output_lines"] = view["output_lines"]
                projected = json.dumps(
                    outer, ensure_ascii=False, separators=(",", ":")
                )
            before = count_context(content, services_for(action).token_counter)
            after = count_context(projected, services_for(action).token_counter)
            if before["tokens"] - after["tokens"] < config.min_gain:
                projected, after = content, before
            await emit_report(
                action,
                {
                    "schema": "a11.llm-context.report/v1",
                    "operation": "shell",
                    "shell_mode": config.shell.mode,
                    "changed": projected != content,
                    "tokens_before": before["tokens"],
                    "tokens_after": after["tokens"],
                    "bytes_before": before["bytes"],
                    "bytes_after": after["bytes"],
                    "count_source": before["source"],
                    "fields": result["reports"],
                    "elapsed_seconds": time.monotonic() - started,
                    "run_id": config.measurement.run_id,
                    "arm": config.measurement.arm,
                },
            )
            return projected
    if config.tool_results == "off" and config.measurement.mode == "off":
        return content
    selected = (
        config
        if config.tool_results != "off"
        else config.model_copy(update={"enabled": False})
    )
    result = await asyncio.to_thread(
        compress_content,
        content,
        selected,
        counter=services_for(action).token_counter,
    )
    await emit_report(action, result["report"])
    return result["text"]


async def _summarize(action, history):
    config = config_for(action)
    services = services_for(action)
    callback = services.summarizers.get(config.summarizer)
    if callback is not None:
        result = callback(history)
        return await result if inspect.isawaitable(result) else result
    registry = action.get_registry()
    if not config.summarizer or registry is None:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="No context summarizer is configured on this host",
        ).to_exception()
    schema = registry.get_schema(config.summarizer)
    if "history" not in schema.inputs or "checkpoint" not in schema.outputs:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                "Summarizer action requires history input and checkpoint output"
            ),
        ).to_exception()
    child = action.make_nested(config.summarizer)
    disabled = config.model_copy(update={"enabled": False, "compaction": "off"})
    child.set_header(LlmContextHeaders.CONFIG, disabled.model_dump_json())
    child.set_header(LlmContextHeaders.OPERATION, "summarization")
    child.run()
    for name in schema.inputs:
        (
            await child[name].finalize(history)
            if name == "history"
            else await child[name].finalize()
        )

    async def drain(name):
        return [value async for value in child[name]]

    names = list(schema.outputs)
    try:
        values = await asyncio.gather(*(drain(name) for name in names))
        await child.wait()
    except BaseException:
        child.cancel()
        raise
    checkpoints = values[names.index("checkpoint")]
    if len(checkpoints) != 1:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Summarizer must produce exactly one checkpoint",
        ).to_exception()
    return checkpoints[0]


async def prepare_messages(
    action,
    messages: list[dict],
    *,
    stateful: bool = False,
    native_compaction: bool = False,
) -> list[dict]:
    """Copy editable provider spans, retaining unknown and signed blocks
    verbatim.
    """
    config = config_for(action)
    if interaction_stage(action):
        return messages
    if config.enabled and config.compaction == "native" and native_compaction:
        config = config.model_copy(update={"compaction": "off"})
    scope = None
    if (
        config.enabled
        and config.retention
        and config.measurement.mode != "shadow"
    ):
        scope = await authorized_scope(action)
    prepared = await project_view(
        messages,
        config,
        services=services_for(action),
        scope=scope,
        reporter=lambda report: emit_report(action, report),
        summarizer=lambda prefix: _summarize(action, prefix),
        stateful=stateful,
    )
    if prepared["scope"] is not None:
        action.set_header(
            LlmContextHeaders.SCOPE, json.dumps(prepared["scope"])
        )
    return prepared["messages"]


async def project_view(
    messages: list[dict],
    config: LlmContextConfig,
    *,
    services: LlmContextServices | None = None,
    scope: dict | None = None,
    reporter: Callable[[dict], None | Awaitable[None]] | None = None,
    summarizer: Callable[[list[dict]], str | Awaitable[str]] | None = None,
    stateful: bool = False,
) -> dict:
    """Project and optionally persist a host-authorized request view.

    Args:
        messages: Original provider message dictionaries.
        config: Explicit LLM context policy.
        services: Receiving-host storage, counting, and recording services.
        scope: Authorized owner/conversation/branch and operation/revision.
        reporter: Optional sync/async observer overriding the service recorder.
        summarizer: Optional sync/async summary callback.
        stateful: Whether the provider owns an opaque history continuation.

    Returns:
        Projected messages, explicit scope, and optional view descriptor.
        Durable operation claims recover an accepted view after a lost reply.
    """
    services = services or services_for(None)

    async def report(value):
        if reporter is not None:
            pending = reporter(value)
            if inspect.isawaitable(pending):
                await pending
        elif services.recorder is not None:
            await asyncio.to_thread(services.recorder.record, value)

    retain = (
        config.enabled
        and config.retention
        and config.measurement.mode != "shadow"
    )
    operation_id = claim_token = source_ref = digest = None
    if retain:
        if (
            scope is None
            or not scope.get("operation_id")
            or services.retention is None
        ):
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message=(
                    "Retention requires authorized scope, owner, and operation"
                    " identity"
                ),
            ).to_exception()
        if stateful and config.compaction != "off":
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="Opaque history needs native compaction coordination",
            ).to_exception()
        source = json.dumps(messages, ensure_ascii=False, separators=(",", ":"))
        if len(source.encode()) > config.max_bytes:
            raise Status(
                code=StatusCode.RESOURCE_EXHAUSTED,
                message="History exceeds context byte limit",
            ).to_exception()
        digest = hashlib.sha256(source.encode()).hexdigest()
        policy_digest = hashlib.sha256(
            json.dumps(
                {
                    "policy": config.model_dump(exclude={"measurement"}),
                    "services": services.revision,
                },
                sort_keys=True,
                separators=(",", ":"),
            ).encode()
        ).hexdigest()
        operation_id = hashlib.sha256(
            json.dumps(
                {
                    "scope": {
                        k: scope[k]
                        for k in (
                            "owner",
                            "conversation",
                            "branch",
                            "operation_id",
                        )
                    },
                    "source": digest,
                    "policy": policy_digest,
                },
                sort_keys=True,
            ).encode()
        ).hexdigest()
        claim_token = uuid.uuid4().hex
        previous = await services.retention.claim_operation(
            scope,
            operation_id=operation_id,
            source_digest=digest,
            expected_revision=scope["expected_revision"],
            claim_token=claim_token,
        )
        if previous is not None:
            next_scope = {**scope, "expected_revision": previous["revision"]}
            descriptor = {
                k: previous[k]
                for k in ("owner", "scope", "revision", "reference")
            }
            await report(
                {
                    "schema": "a11.llm-context.report/v1",
                    "operation": "reuse_view",
                    "operation_id": operation_id,
                    "view": descriptor,
                    "source_digest": digest,
                }
            )
            return {
                "messages": previous["view"]["history"],
                "scope": next_scope,
                "view": descriptor,
            }
        source_ref = await services.retention.retain(
            source, scope, mimetype="application/json"
        )
    try:
        projected = await project_messages(
            messages,
            config,
            services=services,
            reporter=report,
            summarizer=summarizer,
            stateful=stateful,
        )
        descriptor = None
        if retain:
            descriptor = await services.retention.save_view(
                scope,
                {"history": projected, "source": source_ref},
                source_digest=digest,
                expected_revision=scope["expected_revision"],
                operation_id=operation_id,
                claim_token=claim_token,
            )
            scope = {**scope, "expected_revision": descriptor["revision"]}
            await report(
                {
                    "schema": "a11.llm-context.report/v1",
                    "operation": "save_view",
                    "operation_id": operation_id,
                    "view": descriptor,
                    "source_digest": digest,
                }
            )
        return {"messages": projected, "scope": scope, "view": descriptor}
    except BaseException:
        if retain:
            await asyncio.shield(
                services.retention._orphan(source_ref["reference"])
            )
        raise


async def project_messages(
    messages: list[dict],
    config: LlmContextConfig,
    *,
    services: LlmContextServices | None = None,
    reporter: Callable[[dict], None | Awaitable[None]] | None = None,
    summarizer: Callable[[list[dict]], str | Awaitable[str]] | None = None,
    stateful: bool = False,
) -> list[dict]:
    """Project provider messages in a host's programmatic request hook.

    Args:
        messages: Provider-shaped JSON message dictionaries. Instructions,
            signed/unknown blocks, and original messages remain intact.
        config: Explicit LLM context policy.
        services: Optional host tokenizer, summarizers, and metadata recorder.
        reporter: Optional sync/async report callback, overriding the recorder.
        summarizer: Prefix-to-text callback for summary compaction.
        stateful: Whether the provider owns history behind an opaque ID.

    Returns:
        A projected message list; unchanged disabled paths reuse the input list.

    Raises:
        StatusException: Limits fail or compaction requires unavailable history
            replacement support. This hook installs no host session history.
    """
    services = services or services_for(None)

    async def report(value):
        if reporter is not None:
            observed = reporter(value)
            if inspect.isawaitable(observed):
                await observed
        elif services.recorder is not None:
            await asyncio.to_thread(services.recorder.record, value)

    if not config.enabled and config.measurement.mode == "off":
        return messages
    if (
        config.input_content == "off"
        and config.tool_results == "off"
        and config.compaction == "off"
    ):
        return messages
    if config.enabled and config.compaction == "native":
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="Native compaction requires a harness coordination adapter",
        ).to_exception()
    if config.enabled and stateful and config.compaction != "off":
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                "Compaction requires full history or a verified host"
                " replacement capability"
            ),
        ).to_exception()
    result = copy.deepcopy(messages)
    for message in result:
        if message.get("role") in ("system", "developer", "assistant", "model"):
            continue
        if message.get("type") in (
            "reasoning",
            "compaction",
            "thought",
            "model_output",
            "function_call",
        ):
            continue
        if message.get("is_error"):
            continue
        is_result = message.get("type") in (
            "function_call_output",
            "function_result",
        )
        content_field = (
            "result"
            if message.get("type") == "function_result"
            else (
                "output"
                if message.get("type") == "function_call_output"
                else "content"
            )
        )
        content = message.get(content_field)
        if isinstance(content, str) and (
            (
                (message.get("role") == "tool" or is_result)
                and config.tool_results != "off"
            )
            or (
                message.get("role") != "tool"
                and not is_result
                and config.input_content != "off"
            )
        ):
            candidate = await asyncio.to_thread(
                compress_content,
                content,
                config,
                counter=services.token_counter,
            )
            message[content_field] = candidate["text"]
            await report(candidate["report"])
        elif isinstance(content, list):
            for part in content:
                if not isinstance(part, dict) or part.get("is_error"):
                    continue
                kind = part.get("type")
                field = "content" if kind == "tool_result" else "text"
                eligible = (
                    kind == "tool_result" and config.tool_results != "off"
                ) or (
                    kind in ("text", "input_text")
                    and (
                        config.tool_results != "off"
                        if is_result
                        else config.input_content != "off"
                    )
                )
                if eligible and isinstance(part.get(field), str):
                    candidate = await asyncio.to_thread(
                        compress_content,
                        part[field],
                        config,
                        counter=services.token_counter,
                    )
                    part[field] = candidate["text"]
                    await report(candidate["report"])
    if config.enabled and config.compaction != "off":
        if config.measurement.mode == "shadow":
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message=(
                    "Generative shadow compaction must be requested through the"
                    " standalone utility"
                ),
            ).to_exception()
        compacted = await compact_history(
            result,
            config,
            summarizer=summarizer,
            summary_factory=(
                (
                    lambda text: {
                        "type": "user_input",
                        "content": [{"type": "text", "text": text}],
                    }
                )
                if any(m.get("type") == "user_input" for m in result)
                else None
            ),
            counter=services.token_counter,
        )
        if compacted["report"]["changed"]:
            result = compacted["history"]
            await report(compacted["report"])
    return result


async def prepare_text(
    action,
    text: str,
    *,
    stateful: bool = False,
    native_compaction: bool = False,
) -> str:
    messages = await prepare_messages(
        action,
        [{"role": "user", "content": text}],
        stateful=stateful,
        native_compaction=native_compaction,
    )
    return messages[0]["content"]


@functools.lru_cache(maxsize=256)
def _stage_owner(raw: bytes) -> str | None:
    if len(raw) > 8192:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="LLM context stage exceeds 8192 bytes",
        ).to_exception()
    try:
        value = json.loads(raw)
        if (
            value.get("schema") == "a11.llm-context.stage/v1"
            and value.get("owner") == "interaction"
        ):
            return value.get("action_id")
        return None
    except (ValueError, TypeError, AttributeError) as error:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Invalid LLM context stage",
        ).to_exception() from error


def interaction_stage(action) -> bool:
    """Check serialized projection ownership for this action identity."""
    raw = action.get_header(LlmContextHeaders.STAGE)
    return raw is not None and _stage_owner(bytes(raw)) == action.id


async def _request_interaction_view(action, sources, config):
    from a11.sdk.llm_context.interaction_owner import (
        _accepted_record,
        accept_interaction_view,
        persist_interaction_view,
    )
    from a11.sdk.llm_context.interactions import (
        _digest,
        _require_portable_entries,
        encode_interactions,
        project_interactions,
        use_interaction_view,
    )

    services = services_for(action)
    scope = None
    base_scope = None
    claim_token = None
    source_revision = 0
    if (
        config.enabled
        and config.retention
        and config.measurement.mode != "shadow"
    ):
        if services.retention is None:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="Interaction retention requires a configured owner",
            ).to_exception()
        scope = await authorized_scope(action)
        base_scope = scope
        source_revision = scope["expected_revision"]
        source_digest = _digest(encode_interactions(sources))
        operation = _digest(
            [
                scope["operation_id"],
                source_digest,
                config.model_dump(exclude={"measurement"}),
                services.revision,
            ]
        )
        scope = {**scope, "operation_id": operation}
        claim_token = uuid.uuid4().hex
        prior = await services.retention.claim_operation(
            scope,
            operation_id=operation,
            source_digest=source_digest,
            expected_revision=source_revision,
            claim_token=claim_token,
        )
        if prior is not None:
            accepted = _accepted_record(prior)
            await emit_report(
                action,
                {
                    "operation": "reuse_interaction_view",
                    "view_id": accepted["view_id"],
                },
            )
            action.set_header(
                LlmContextHeaders.SCOPE,
                json.dumps(
                    {**base_scope, "expected_revision": prior["revision"]}
                ),
            )
            return use_interaction_view(accepted), accepted["view_id"]
    proposal = await project_interactions(
        sources,
        config,
        source_revision=source_revision,
        summarizer=lambda prefix: _summarize(action, prefix),
        counter=services.token_counter,
        reporter=lambda report: emit_report(action, report),
    )
    if proposal["shadow"]:
        return sources, proposal["view_id"]
    _require_portable_entries(proposal)

    def install(view, *, expected_revision, source_digest):
        return {
            "installed": True,
            "owner": "request:" + action.id,
            "revision": 1,
            "source_revision": expected_revision,
            "source_digest": source_digest,
            "view_id": view["view_id"],
        }

    if scope is not None:
        accepted = await persist_interaction_view(
            proposal,
            services.retention,
            scope,
            claim_token=claim_token,
        )
        action.set_header(
            LlmContextHeaders.SCOPE,
            json.dumps(
                {
                    **base_scope,
                    "expected_revision": accepted["installation"]["revision"],
                }
            ),
        )
    else:
        accepted = await accept_interaction_view(
            proposal,
            lambda: {"revision": 0, "interactions": sources},
            install,
        )
    prepared = use_interaction_view(accepted)
    await emit_report(
        action,
        {
            "schema": "a11.llm-context.report/v1",
            "operation": "interaction_view",
            "view_id": accepted["view_id"],
            "source_digest": accepted["source_digest"],
            "installation": accepted["installation"],
            "covered_ids": accepted["covered_ids"],
            "scope": "owner" if scope is not None else "request-local",
            "canonical_history_installed": False,
        },
    )
    return prepared, accepted["view_id"]


def _interaction_policy_selected(config: LlmContextConfig) -> bool:
    return (
        config.enabled
        and config.boundary == "interaction"
        and (
            config.input_content != "off"
            or config.tool_results != "off"
            or config.compaction == "summary"
        )
    )


async def prepare_interaction_stream(
    action,
    interactions: AsyncIterable[Interaction],
    *,
    stateful: bool | Callable[[list[Interaction]], bool] = False,
) -> AsyncIterator[Interaction]:
    """Stream canonical inputs and buffer only opted-in Interaction views.

    Args:
        action: Receiving SDK action with optional LLM context configuration.
        interactions: Original input stream; the default path yields each
            record directly and preserves incremental validation by its backend.
        stateful: Server-history selection, or a selector evaluated against
            buffered sources when an Interaction view is requested.

    Yields:
        Original records, or the explicitly selected model-facing view.
    """
    from a11.sdk.llm import interaction_backend
    from a11.sdk.llm_context.interactions import VIEW_PRODUCER

    iterator = aiter(interactions)
    try:
        first = await anext(iterator)
    except StopAsyncIteration:
        return
    if (
        not _interaction_policy_selected(config_for(action))
        and interaction_backend(first) != VIEW_PRODUCER
    ):
        yield first
        async for item in iterator:
            if interaction_backend(item) == VIEW_PRODUCER:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        "An accepted Interaction view must form the history"
                        " prefix"
                    ),
                ).to_exception()
            yield item
        return
    sources = [first, *[item async for item in iterator]]
    prepared = await prepare_interactions(
        action,
        sources,
        stateful=stateful(sources) if callable(stateful) else stateful,
    )
    for item in prepared:
        yield item


async def prepare_interactions(action, interactions, *, stateful: bool = False):
    """Resolve accepted views or install an opt-in request-local projection.

    Args:
        action: Receiving SDK action with ordinary LLM context headers/services.
        interactions: Ordered canonical records or an accepted view prefix and
            optional new records. The router forwards these inputs unchanged.
        stateful: Backend-owned continuation selected before local projection.

    Returns:
        Model-facing Interaction records, leaving canonical objects unchanged.
        Explicit stage provenance suppresses provider/history and bridge repeats
        only within this action. Child actions validate their own inputs.

    Raises:
        StatusException: A view is invalid, opaque history would be replaced,
            or competing projection owners are selected.
    """
    from a11.sdk.llm_context.interactions import (
        VIEW_METADATA,
        _context_rows,
        _proof,
        validate_applied_interactions,
    )

    config = config_for(action)
    prefix = validate_applied_interactions(interactions)
    selected = _interaction_policy_selected(config)
    if not prefix and not selected:
        return interactions
    if config.measurement.mode == "shadow" and config.compaction == "summary":
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                "Generative shadow views require an explicit standalone call"
            ),
        ).to_exception()
    if stateful:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                "An Interaction view requires fresh replay; use the native"
                " history owner for a continuation"
            ),
        ).to_exception()
    if config.enabled and config.shell.mode == "compress":
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                "Choose Interaction projection or completed-shell compression"
                " as the result owner"
            ),
        ).to_exception()
    if prefix:
        prepared = [item.model_copy() for item in interactions[:prefix]]
        view_id = _proof(
            interactions[0].backend_specific_metadata[VIEW_METADATA]
        ).view_id
        if interactions[prefix:]:
            tail_policy = config.model_copy(
                update={"compaction": "off", "retention": False}
            )
            tail, _ = await _request_interaction_view(
                action, interactions[prefix:], tail_policy
            )
            for item in tail:
                item = item.model_copy(
                    update={"previous_interaction_id": prepared[-1].id}
                )
                prepared.append(item)
        if selected and config.compaction == "summary":
            counted = count_context(
                json.dumps(
                    _context_rows(prepared),
                    ensure_ascii=False,
                    separators=(",", ":"),
                ),
                services_for(action).token_counter,
            )
            if counted["tokens"] > config.max_tokens - config.reserve_tokens:
                raise Status(
                    code=StatusCode.RESOURCE_EXHAUSTED,
                    message=(
                        "Accepted view and new history exceed the budget;"
                        " request a new owner-accepted checkpoint"
                    ),
                ).to_exception()
    else:
        selected_config = (
            config.model_copy(update={"compaction": "off"})
            if config.compaction == "native"
            else config
        )
        prepared, view_id = await _request_interaction_view(
            action, interactions, selected_config
        )
    action.set_header(
        LlmContextHeaders.STAGE,
        json.dumps(
            {
                "schema": "a11.llm-context.stage/v1",
                "owner": "interaction",
                "action_id": action.id,
                "view_id": view_id,
            }
        ),
    )
    return prepared


async def prepare_interaction_result(action, interaction):
    """Project a new tool-result record while retaining its canonical outputs.

    The returned clone feeds only the provider conversation. SDK output ports
    retain the original record and typed action fragments. Completed failures
    retain the native backend representation and status semantics.
    """
    if not interaction_stage(action):
        return interaction
    from a11.sdk import llm

    raw = interaction.backend_specific_metadata.get(
        llm.TOOL_STATUSES_METADATA_KEY
    )
    if raw and any(value.get("code", 0) for value in json.loads(raw).values()):
        return interaction
    config = config_for(action).model_copy(
        update={"compaction": "off", "retention": False}
    )
    if not config.enabled or config.tool_results == "off":
        return interaction
    prepared, _ = await _request_interaction_view(action, [interaction], config)
    return prepared[0].model_copy(
        update={"previous_interaction_id": interaction.previous_interaction_id}
    )
