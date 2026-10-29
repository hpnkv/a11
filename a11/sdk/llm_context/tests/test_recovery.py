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

"""Fence distributed workers and recover durable view operations."""

import asyncio
import sqlite3
import sys

import pytest

from a11.sdk.llm_context import (
    LlmContextConfig,
    LlmContextScope,
    LlmContextServices,
    project_view,
)
from a11.sdk.llm_context.retention import RetainedContentStore
from a11.status import StatusCode, StatusException

SCOPE = {"owner": "host", "conversation": "session", "branch": "main"}


@pytest.mark.asyncio
async def test_reconciliation_fences_late_worker_and_recovers_after_restart(
    tmp_path,
):
    store = RetainedContentStore(tmp_path, owner="host")
    await store.claim_operation(
        SCOPE,
        operation_id="op",
        source_digest="digest",
        expected_revision=0,
        claim_token="worker-one",
    )
    reopened = RetainedContentStore(tmp_path, owner="host")
    observed = await reopened.inspect_operation(SCOPE, "op")
    assert observed["state"] == "pending"
    await reopened.reconcile_operation(
        SCOPE,
        "op",
        expected_token="worker-one",
        disposition="retry",
        evidence="Owner confirmed the summarizer has no external effects",
    )
    await reopened.claim_operation(
        SCOPE,
        operation_id="op",
        source_digest="digest",
        expected_revision=0,
        claim_token="worker-two",
    )
    with pytest.raises(StatusException) as error:
        await store.save_view(
            SCOPE,
            {"history": []},
            source_digest="digest",
            expected_revision=0,
            operation_id="op",
            claim_token="worker-one",
        )
    assert error.value.status.code == StatusCode.ABORTED
    accepted = await reopened.save_view(
        SCOPE,
        {"history": []},
        source_digest="digest",
        expected_revision=0,
        operation_id="op",
        claim_token="worker-two",
    )
    replay = await store.claim_operation(
        SCOPE,
        operation_id="op",
        source_digest="digest",
        expected_revision=0,
        claim_token="worker-three",
    )
    assert replay["reference"] == accepted["reference"]
    assert replay["view"] == {"history": []}
    result = await store.collect_garbage()
    assert result["payloads"] >= 0
    with sqlite3.connect(store.index) as db:
        assert (
            db.execute(
                "SELECT COUNT(*) FROM content WHERE ready=-1"
            ).fetchone()[0]
            == 0
        )


@pytest.mark.asyncio
async def test_expired_heads_reject_stale_revision_and_reads(
    tmp_path,
):
    store = RetainedContentStore(tmp_path, owner="host")
    accepted = await store.save_view(
        SCOPE,
        {"history": []},
        source_digest="one",
        expected_revision=0,
        operation_id="one",
    )
    with sqlite3.connect(store.index) as db:
        db.execute("UPDATE content SET expires=0")
    await store.collect_garbage()
    with pytest.raises(StatusException) as error:
        await store.save_view(
            SCOPE,
            {},
            source_digest="two",
            expected_revision=0,
            operation_id="two",
        )
    assert error.value.status.code == StatusCode.ABORTED
    ref = await store.retain("raw", SCOPE)
    await store._orphan(ref["reference"])
    with pytest.raises(StatusException):
        await store.retrieve(ref, SCOPE)
    assert accepted["revision"] == 1


@pytest.mark.asyncio
async def test_view_branch_copies_raw_source_into_destination_scope(tmp_path):
    store = RetainedContentStore(tmp_path, owner="host")
    source = await store.retain("source", SCOPE)
    await store.save_view(
        SCOPE,
        {"source": source, "history": []},
        source_digest="digest",
        expected_revision=0,
        operation_id="source",
    )
    destination = {**SCOPE, "branch": "fork"}
    await store.fork_view(SCOPE, destination, operation_id="fork")
    branch = await store.load_view(destination)
    fork_source = branch["view"]["source"]
    assert (await store.retrieve(fork_source, destination))["text"] == "source"
    with pytest.raises(StatusException):
        await store.retrieve(fork_source, SCOPE)


@pytest.mark.asyncio
async def test_projection_reuses_committed_view_after_restart(
    tmp_path,
):
    scope = {**SCOPE, "expected_revision": 0, "operation_id": "request"}
    messages = [{"role": "user", "content": "{ " + " " * 1000 + '"x":1 }'}]
    policy = LlmContextConfig(retention=True, input_content="lossless")
    first = await project_view(
        messages,
        policy,
        scope=scope,
        services=LlmContextServices(
            retention=RetainedContentStore(tmp_path, owner="host")
        ),
    )
    second = await project_view(
        messages,
        policy,
        scope=scope,
        services=LlmContextServices(
            retention=RetainedContentStore(tmp_path, owner="host")
        ),
    )
    assert first["messages"] == second["messages"]
    assert first["scope"] == second["scope"]


@pytest.mark.asyncio
async def test_concurrent_claims_admit_one_worker(tmp_path):
    stores = [RetainedContentStore(tmp_path, owner="host") for _ in range(2)]
    results = await asyncio.gather(
        *[
            s.claim_operation(
                SCOPE,
                operation_id="same",
                source_digest="digest",
                expected_revision=0,
                claim_token=str(i),
            )
            for i, s in enumerate(stores)
        ],
        return_exceptions=True,
    )
    assert sum(r is None for r in results) == 1
    assert sum(isinstance(r, StatusException) for r in results) == 1


@pytest.mark.asyncio
async def test_operation_claims_serialize_across_processes(tmp_path):
    RetainedContentStore(tmp_path, owner="host")
    program = """
import asyncio, sys
from a11.sdk.llm_context import RetainedContentStore
from a11.status import StatusException
async def main():
    store = RetainedContentStore(sys.argv[1], owner="host")
    try:
        await store.claim_operation(
            {"owner":"host","conversation":"session","branch":"main"},
            operation_id="race", source_digest="digest", expected_revision=0,
            claim_token=sys.argv[2],
        )
        print("claimed")
    except StatusException:
        print("rejected")
asyncio.run(main())
"""
    processes = await asyncio.gather(
        *[
            asyncio.create_subprocess_exec(
                sys.executable,
                "-c",
                program,
                str(tmp_path),
                str(i),
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            for i in range(2)
        ]
    )
    outputs = await asyncio.gather(*(p.communicate() for p in processes))
    assert all(p.returncode == 0 for p in processes), outputs
    assert sorted(out.strip() for out, error in outputs) == [
        b"claimed",
        b"rejected",
    ]


@pytest.mark.asyncio
async def test_derived_operation_identity_fits_wire_scope(
    tmp_path,
):
    store = RetainedContentStore(tmp_path, owner="host")
    services = LlmContextServices(retention=store)
    policy = LlmContextConfig(retention=True)
    identities = []
    for branch in ("one", "two"):
        scope = {
            **SCOPE,
            "branch": branch,
            "expected_revision": 0,
            "operation_id": "x" * 256,
        }
        await project_view(
            [{"role": "user", "content": "hello"}],
            policy,
            scope=scope,
            services=services,
        )
        saved = await store.load_view(scope)
        LlmContextScope.model_validate(
            {**scope, "operation_id": saved["operation_id"]}
        )
        identities.append(saved["operation_id"])
    assert identities[0] != identities[1]
