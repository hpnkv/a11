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

"""Register context utilities as ordinary local, remote, and Flow actions."""

from __future__ import annotations

import asyncio
import json

import a11
from a11.sdk.llm_context.config import (
    LLM_CONTEXT_HEADERS,
    LlmContextServices,
    _validate_host_limits,
    authorized_scope,
    bind_services,
    config_for,
    services_for,
    services_for_registry,
)
from a11.sdk.llm_context.content import (
    compress_content,
    count_context,
    expand_content,
)
from a11.sdk.llm_context.history import compact_history
from a11.sdk.llm_context.runtime import (
    _summarize,
    emit_report,
    llm_context_handler,
)
from a11.sdk.llm_context.shell import intercept_shell_result
from a11.status import Status, StatusCode

_NAMES = (
    "llm_context_project_interactions",
    "llm_context_compact_interactions",
    "llm_context_accept_interaction_view",
    "llm_context_load_interaction_view",
    "llm_context_use_interaction_view",
    "llm_context_compress",
    "llm_context_count",
    "llm_context_compact",
    "llm_context_retrieve",
    "llm_context_expand",
    "llm_context_retain",
    "llm_context_load_view",
    "llm_context_save_view",
    "llm_context_shell",
    "llm_context_inspect_operation",
    "llm_context_reconcile_operation",
    "llm_context_fork_view",
)


def schema(name: str) -> a11.ActionSchema:
    """Describe one utility with a unary JSON request and result."""
    return a11.ActionSchema(
        name=name,
        description=(
            "Transform or inspect context with explicit policy and provenance."
        ),
        inputs={
            "request": a11.ActionPortSchema(
                "request",
                "application/json",
                typeinfo=dict,
                unary=True,
                required=True,
            )
        },
        outputs={
            "result": a11.ActionPortSchema(
                "result",
                "application/json",
                typeinfo=dict,
                unary=True,
                required=True,
            )
        },
        headers=a11.DEFAULT_ACTION_HEADERS | LLM_CONTEXT_HEADERS,
    )


@llm_context_handler
async def handle(action: a11.Action) -> None:
    """Dispatch a unary utility request using the action's effective policy."""
    config = config_for(action)
    services = services_for(action)
    _validate_host_limits(config, services)
    request = await action["request"].consume(dict)
    name = action.schema.name
    required = {
        "llm_context_project_interactions": ("interactions", list),
        "llm_context_compact_interactions": ("interactions", list),
        "llm_context_accept_interaction_view": ("view", dict),
        "llm_context_use_interaction_view": ("view", dict),
        "llm_context_load_interaction_view": ("scope", dict),
        "llm_context_compact": ("history", list),
        "llm_context_retrieve": ("reference", dict),
        "llm_context_save_view": ("view", dict),
        "llm_context_load_view": ("scope", dict),
        "llm_context_inspect_operation": ("scope", dict),
        "llm_context_reconcile_operation": ("scope", dict),
        "llm_context_fork_view": ("destination", dict),
        "llm_context_shell": ("result", dict),
    }.get(name, ("text", str))
    if not isinstance(request, dict) or not isinstance(
        request.get(required[0]), required[1]
    ):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                f"Context request requires {required[0]} of type"
                f" {required[1].__name__}"
            ),
        ).to_exception()
    if "text" in request and len(request["text"].encode()) > config.max_bytes:
        raise Status(
            code=StatusCode.RESOURCE_EXHAUSTED,
            message="Context exceeds byte limit",
        ).to_exception()
    if name in (
        "llm_context_project_interactions",
        "llm_context_compact_interactions",
        "llm_context_accept_interaction_view",
        "llm_context_use_interaction_view",
        "llm_context_load_interaction_view",
    ):
        if (
            len(json.dumps(request, ensure_ascii=False).encode())
            > config.max_bytes
        ):
            raise Status(
                code=StatusCode.RESOURCE_EXHAUSTED,
                message=(
                    "Interaction utility request exceeds the host byte limit"
                ),
            ).to_exception()
        from a11.sdk.llm_context.history import compact_interactions
        from a11.sdk.llm_context.interaction_owner import (
            load_interaction_view,
            persist_interaction_view,
        )
        from a11.sdk.llm_context.interactions import (
            decode_interactions,
            encode_interactions,
            project_interactions,
            use_interaction_view,
        )

        if name in (
            "llm_context_project_interactions",
            "llm_context_compact_interactions",
        ):
            function = (
                compact_interactions
                if name == "llm_context_compact_interactions"
                else project_interactions
            )
            result = await function(
                decode_interactions(request["interactions"]),
                config,
                source_revision=request.get("source_revision", 0),
                checkpoint=request.get("checkpoint"),
                counter=services.token_counter,
                summarizer=lambda prefix: _summarize(action, prefix),
                reporter=lambda report: emit_report(action, report),
            )
        elif name == "llm_context_use_interaction_view":
            result = {
                "interactions": encode_interactions(
                    use_interaction_view(request["view"])
                )
            }
        else:
            if services.retention is None:
                raise Status(
                    code=StatusCode.FAILED_PRECONDITION,
                    message="No Interaction view owner is configured",
                ).to_exception()
            scope = await authorized_scope(action, request.get("scope"))
            if name == "llm_context_load_interaction_view":
                result = {
                    "view": await load_interaction_view(
                        services.retention, scope
                    )
                }
            else:
                result = await persist_interaction_view(
                    request["view"], services.retention, scope
                )
    elif name == "llm_context_compress":
        result = await asyncio.to_thread(
            compress_content,
            request["text"],
            config,
            counter=services.token_counter,
        )
    elif name == "llm_context_count":
        result = count_context(request["text"], services.token_counter)
    elif name == "llm_context_expand":
        result = {
            "text": await asyncio.to_thread(
                expand_content, request["text"], max_bytes=config.max_bytes
            )
        }
    elif name == "llm_context_compact":
        result = await compact_history(
            request["history"],
            config,
            summarizer=lambda prefix: _summarize(action, prefix),
            counter=services.token_counter,
            checkpoint=request.get("checkpoint"),
        )
    elif name == "llm_context_shell":
        result = await asyncio.to_thread(
            intercept_shell_result,
            request["result"],
            config,
            counter=services.token_counter,
            bypass=request.get("bypass", False),
            source=request.get("source"),
        )
        for report in result["reports"]:
            await emit_report(action, report)
    elif name in (
        "llm_context_retrieve",
        "llm_context_retain",
        "llm_context_save_view",
        "llm_context_load_view",
        "llm_context_inspect_operation",
        "llm_context_reconcile_operation",
        "llm_context_fork_view",
    ):
        if services.retention is None:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="No retention owner is configured on this host",
            ).to_exception()
        scope = await authorized_scope(action, request.get("scope"))
        if name == "llm_context_inspect_operation":
            result = await services.retention.inspect_operation(
                scope, scope["operation_id"]
            )
        elif name == "llm_context_reconcile_operation":
            if not all(
                isinstance(request.get(k), str)
                for k in ("expected_token", "disposition", "evidence")
            ):
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message=(
                        "Recovery requires token, disposition, and evidence"
                    ),
                ).to_exception()
            result = await services.retention.reconcile_operation(
                scope,
                scope["operation_id"],
                expected_token=request["expected_token"],
                disposition=request["disposition"],
                evidence=request["evidence"],
            )
        elif name == "llm_context_fork_view":
            destination = await authorized_scope(action, request["destination"])
            result = await services.retention.fork_view(
                scope, destination, operation_id=destination["operation_id"]
            )
        elif name == "llm_context_retain":
            result = await services.retention.retain(request["text"], scope)
        elif name == "llm_context_load_view":
            result = {"view": await services.retention.load_view(scope)}
        elif name == "llm_context_save_view":
            if not isinstance(request.get("source_digest"), str):
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message="View requires source_digest",
                ).to_exception()
            result = await services.retention.save_view(
                scope,
                request["view"],
                source_digest=request["source_digest"],
                expected_revision=scope["expected_revision"],
                operation_id=scope["operation_id"],
                claim_token=request.get("claim_token"),
            )
        else:
            result = await services.retention.retrieve(
                request["reference"],
                scope,
                cursor=request.get("cursor", 0),
                max_bytes=request.get("max_bytes", 65536),
            )
    else:
        raise Status(
            code=StatusCode.UNIMPLEMENTED, message="Unknown context operation"
        ).to_exception()
    if "report" in result:
        await emit_report(action, result["report"])
    await action["result"].finalize(result)


def register(
    registry: a11.ActionRegistry,
    *,
    services: LlmContextServices | None = None,
    names: list[str] | tuple[str, ...] | None = None,
) -> None:
    """Install selectable LLM context handlers with host-owned dependencies.

    Args:
        registry: Receiving host registry, shared by direct and Flow calls.
        services: Optional tokenizer, summarizers, retention owner, authorizer,
            and recorder. SDK handler bindings survive native registry copies.
        names: Selected utility names; omit to install all context utilities.

    Raises:
        StatusException: A selected utility name is unknown.
    """
    selected_services = services or services_for_registry(registry)
    for name in names if names is not None else _NAMES:
        if name not in _NAMES:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Unknown context utility name",
            ).to_exception()
        registry.register(name, schema(name), handle)
    bind_services(registry, selected_services)


REGISTRY = a11.ActionRegistry()
register(REGISTRY)
