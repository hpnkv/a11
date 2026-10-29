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

"""Install Interaction views through revisioned host ownership."""

from __future__ import annotations

import copy
import inspect
from collections.abc import Awaitable, Callable
from typing import TYPE_CHECKING, Any

from a11.sdk.llm_context.config import LlmContextConfig
from a11.sdk.llm_context.interactions import (
    _digest,
    _validate_ack,
    encode_interactions,
    interaction_view_sources,
    project_interactions,
    validate_interaction_view,
)
from a11.status import Status, StatusCode

if TYPE_CHECKING:
    from a11.sdk.llm_context.retention import RetainedContentStore


async def accept_interaction_view(
    proposal: dict,
    read_history: Callable[[], dict | Awaitable[dict]],
    install_view: Callable[..., dict | Awaitable[dict]],
) -> dict:
    """Accept a proposal through an explicit history-owner installation.

    Args:
        proposal: Unaccepted JSON view returned by project_interactions.
        read_history: Host callback returning interactions and their revision.
        install_view: Atomic owner callback receiving the proposal plus
            expected_revision and source_digest. It installs a distinct view
            while retaining canonical history. Its acknowledgement contains
            installed, owner, view_id, source_digest, source_revision, revision.

    Returns:
        An accepted JSON view after the owner acknowledges installation.
        No implicit retry follows a failed or uncertain acknowledgement.

    Raises:
        StatusException: Sources/revisions changed, shadow mode is selected,
            or the owner cannot acknowledge this exact installation.
    """
    validate_interaction_view(proposal)
    if proposal["state"] != "proposal" or proposal["shadow"]:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="Only non-shadow proposals can be installed",
        ).to_exception()
    snapshot = read_history()
    if inspect.isawaitable(snapshot):
        snapshot = await snapshot
    if (
        not isinstance(snapshot, dict)
        or snapshot.get("revision") != proposal["source_revision"]
    ):
        raise Status(
            code=StatusCode.ABORTED,
            message="Interaction source revision changed before installation",
        ).to_exception()
    sources = snapshot.get("interactions")
    if (
        not isinstance(sources, list)
        or _digest(encode_interactions(sources)) != proposal["source_digest"]
    ):
        raise Status(
            code=StatusCode.ABORTED,
            message="Interaction sources changed before installation",
        ).to_exception()
    candidate = copy.deepcopy(proposal)
    acknowledgement = install_view(
        candidate,
        expected_revision=proposal["source_revision"],
        source_digest=proposal["source_digest"],
    )
    if inspect.isawaitable(acknowledgement):
        acknowledgement = await acknowledgement
    if not isinstance(acknowledgement, dict):
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=(
                "History installation is indeterminate; reconcile before"
                " retrying"
            ),
        ).to_exception()
    _validate_ack(proposal, acknowledgement)
    accepted = {
        **copy.deepcopy(proposal),
        "state": "accepted",
        "installation": copy.deepcopy(acknowledgement),
    }
    validate_interaction_view(accepted)
    return accepted


async def persist_interaction_view(
    proposal: dict,
    store: RetainedContentStore,
    scope: dict,
    *,
    claim_token: str | None = None,
) -> dict:
    """Install a declared source snapshot through an authorized retention owner.

    Args:
        proposal: Unaccepted Interaction view with its declared source revision.
        store: Existing RetainedContentStore owned by the receiving host.
        scope: Host-authorized owner/conversation/branch, expected_revision,
            and stable operation_id. The owner revision serializes updates.
        claim_token: Optional durable operation token fencing summary workers.

    Returns:
        An accepted view after the owner's durable compare-and-swap commit.
        Canonical source chunks and the projection share the saved manifest.

    Raises:
        StatusException: Scope/revision conflicts or the commit is uncertain.
    """
    validate_interaction_view(proposal)
    if proposal["source_revision"] != scope.get("expected_revision"):
        raise Status(
            code=StatusCode.ABORTED,
            message="Source and owner view revisions disagree",
        ).to_exception()
    sources = interaction_view_sources(proposal)

    async def install(view, *, expected_revision, source_digest):
        result = await store.save_view(
            scope,
            view,
            source_digest=source_digest,
            expected_revision=expected_revision,
            operation_id=scope.get("operation_id") or view["view_id"],
            claim_token=claim_token,
        )
        return {
            **result,
            "installed": True,
            "view_id": view["view_id"],
            "source_digest": source_digest,
            "source_revision": expected_revision,
        }

    return await accept_interaction_view(
        proposal,
        lambda: {
            "revision": proposal["source_revision"],
            "interactions": sources,
        },
        install,
    )


async def load_interaction_view(
    store: RetainedContentStore, scope: dict
) -> dict | None:
    """Read an accepted view from a host-authorized retention scope.

    Args:
        store: Receiving host's RetainedContentStore.
        scope: Authorized owner/conversation/branch identity.

    Returns:
        An accepted JSON view attested by the owner's stored commit, or None.
        Retrieval uses the owner's ordinary manifest and revision contract.
    """
    record = await store.load_view(scope)
    if record is None:
        return None
    return _accepted_record(record)


def _accepted_record(record: dict) -> dict:
    proposal = record["view"]
    acknowledgement = {
        k: record[k] for k in ("owner", "scope", "reference", "revision")
    }
    accepted = {
        **proposal,
        "state": "accepted",
        "installation": {
            **acknowledgement,
            "installed": True,
            "view_id": proposal["view_id"],
            "source_digest": record.get(
                "source_digest", proposal["source_digest"]
            ),
            "source_revision": proposal["source_revision"],
        },
    }
    validate_interaction_view(accepted)
    return accepted


async def compact_host_interactions(
    read_history: Callable[[], dict | Awaitable[dict]],
    install_view: Callable[..., dict | Awaitable[dict]],
    config: LlmContextConfig,
    **options: Any,
) -> dict:
    """Build and install a view while the host retains its inference client.

    Args:
        read_history: Host snapshot callback with interactions and revision.
        install_view: Atomic owner installer using accept_interaction_view's
            acknowledgement contract; it retains the canonical sources.
        config: Explicit projection/compaction policy.
        **options: Summarizer, checkpoint, counter, or reporter options accepted
            by project_interactions. Source revision comes from the snapshot.

    Returns:
        Accepted view, or an uninstalled proposal for disabled/shadow policy.
    """
    snapshot = read_history()
    if inspect.isawaitable(snapshot):
        snapshot = await snapshot
    if (
        not isinstance(snapshot, dict)
        or "interactions" not in snapshot
        or "revision" not in snapshot
    ):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Host requires an Interaction snapshot and revision",
        ).to_exception()
    proposal = await project_interactions(
        snapshot["interactions"],
        config,
        source_revision=snapshot["revision"],
        **options,
    )
    if not config.enabled or proposal["shadow"]:
        return proposal
    return await accept_interaction_view(proposal, read_history, install_view)
