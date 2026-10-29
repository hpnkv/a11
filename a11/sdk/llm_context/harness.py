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

"""Install checkpoint proposals through a host's own history API."""

from __future__ import annotations

import copy
import hashlib
import inspect
import json
from collections.abc import Awaitable, Callable

from a11.sdk.llm_context.config import LlmContextConfig
from a11.sdk.llm_context.history import compact_history
from a11.status import Status, StatusCode


async def compact_host_history(
    read_history: Callable[[], dict | Awaitable[dict]],
    replace_history: Callable[..., dict | Awaitable[dict]] | None,
    config: LlmContextConfig,
    *,
    summarizer: Callable[[list[dict]], str | Awaitable[str]] | None = None,
    counter: Callable[[str], int] | None = None,
    reporter: Callable[[dict], None | Awaitable[None]] | None = None,
) -> dict:
    """Compact through host callbacks while inference stays with that host.

    Args:
        read_history: Read a snapshot containing history and a revision string
            or integer. The mapped history uses the compact_history contract.
        replace_history: Atomically install a proposal using expected_revision
            and source_digest keyword arguments. Return installed=True and the
            new revision only after acceptance. None reports an unavailable
            replacement capability.
        config: Policy selecting budgets, retained suffix, and shadow mode.
        summarizer: Host-selected sync/async checkpoint callback.
        counter: Optional tokenizer for history budget checks.
        reporter: Optional sync/async metadata observer for the operation and
            installation result.

    Returns:
        A versioned result with the proposal and installation acknowledgement.
        Disabled, under-budget, and shadow calls leave history unchanged.

    Raises:
        StatusException: Snapshots are invalid, history changes during summary
            generation, or replacement support/acknowledgement is unavailable.
            A failed or indeterminate installation is never retried implicitly.
    """
    snapshot = read_history()
    if inspect.isawaitable(snapshot):
        snapshot = await snapshot
    if (
        not isinstance(snapshot, dict)
        or not isinstance(snapshot.get("history"), list)
        or type(snapshot.get("revision")) not in (int, str)
    ):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Host history requires messages and a revision",
        ).to_exception()
    source_revision = snapshot["revision"]
    messages = copy.deepcopy(snapshot["history"])
    if (
        replace_history is None
        and config.enabled
        and config.measurement.mode != "shadow"
    ):
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="Host has no history replacement capability",
        ).to_exception()
    proposal = await compact_history(
        messages, config, summarizer=summarizer, counter=counter
    )
    installation = {"installed": False, "revision": source_revision}
    if proposal["report"]["changed"]:
        current = read_history()
        if inspect.isawaitable(current):
            current = await current
        if not isinstance(current, dict) or not isinstance(
            current.get("history"), list
        ):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Host returned an invalid history snapshot",
            ).to_exception()
        digest = hashlib.sha256(
            json.dumps(
                current["history"], ensure_ascii=False, separators=(",", ":")
            ).encode()
        ).hexdigest()
        if (
            current.get("revision") != source_revision
            or digest != proposal["source_digest"]
        ):
            raise Status(
                code=StatusCode.ABORTED,
                message="Host history changed before checkpoint installation",
            ).to_exception()
        installation = replace_history(
            proposal,
            expected_revision=source_revision,
            source_digest=proposal["source_digest"],
        )
        if inspect.isawaitable(installation):
            installation = await installation
        if (
            not isinstance(installation, dict)
            or installation.get("installed") is not True
            or type(installation.get("revision")) not in (int, str)
        ):
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message=(
                    "Host did not acknowledge checkpoint installation;"
                    " reconcile before retrying"
                ),
            ).to_exception()
    if reporter is not None:
        observed = reporter(
            {**proposal["report"], "installation": installation}
        )
        if inspect.isawaitable(observed):
            await observed
    return {
        "schema": "a11.llm-context.installation/v1",
        "proposal": proposal,
        "installation": installation,
        "source_revision": source_revision,
    }
