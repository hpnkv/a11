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

"""Retain exact content in native stores with scoped SDK reference metadata."""

from __future__ import annotations

import asyncio
import fcntl
import hashlib
import json
import sqlite3
import time
import uuid
from contextlib import contextmanager
from pathlib import Path

import a11
from a11.status import Status, StatusCode
from a11.stores.sqlite_chunk_store import SQLiteChunkStoreFactory


class RetainedContentStore:
    """Bind a local durable owner to native chunk data and a revision index.

    Remote callers use utility actions on this owner; node IDs are owner-local.
    Applications authorize scopes before calling these host-side methods.

    Args:
        root: Host-selected directory for native stores and the SDK index.
        owner: Stable identity used by references and scoped lookups.
        max_bytes: Quota across published and pending content.
        ttl_seconds: Lifetime of newly retained content and views.
        max_records: Ceiling for content reservations and operation records.

    Concurrent owner startup uses an OS file lock for native SQLite connection
    initialization. View/claim transactions use SQLite's cross-process locking.
    Hosts initialize the store outside latency-sensitive asynchronous tasks.
    """

    def __init__(
        self,
        root: str | Path,
        *,
        owner: str,
        max_bytes: int = 64 * 1024 * 1024,
        ttl_seconds: float = 3600,
        max_records: int = 10000,
    ):
        if not owner or max_bytes < 1 or ttl_seconds <= 0 or max_records < 1:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Retention needs owner and positive limits",
            ).to_exception()
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.owner = owner
        self.max_bytes = max_bytes
        self.ttl_seconds = ttl_seconds
        self.max_records = max_records
        with (self.root / ".initialize.lock").open("a+b") as initialization:
            fcntl.flock(initialization, fcntl.LOCK_EX)
            try:
                self.factory = SQLiteChunkStoreFactory(self.root / "nodes")
            finally:
                fcntl.flock(initialization, fcntl.LOCK_UN)
        self.index = self.root / "context-index.sqlite"
        with self._connect() as db:
            db.execute("BEGIN IMMEDIATE")
            db.execute(
                "CREATE TABLE IF NOT EXISTS content (ref TEXT PRIMARY KEY,"
                " scope TEXT NOT NULL, bytes INTEGER NOT NULL, digest TEXT NOT"
                " NULL, expires REAL NOT NULL, ready INTEGER NOT NULL)"
            )
            db.execute(
                "CREATE TABLE IF NOT EXISTS views (scope TEXT PRIMARY KEY,"
                " revision INTEGER NOT NULL, ref TEXT NOT NULL, digest TEXT NOT"
                " NULL, operation TEXT NOT NULL)"
            )
            db.execute(
                "CREATE TABLE IF NOT EXISTS operations (scope TEXT NOT NULL,"
                " operation TEXT NOT NULL, digest TEXT NOT NULL, ref TEXT,"
                " revision INTEGER, expires REAL NOT NULL,"
                " PRIMARY KEY(scope,operation))"
            )

            db.execute(
                "CREATE TABLE IF NOT EXISTS heads (scope TEXT PRIMARY KEY,"
                " revision INTEGER NOT NULL)"
            )
            db.execute(
                "INSERT OR IGNORE INTO heads SELECT scope,revision FROM views"
            )
            columns = {
                row[1] for row in db.execute("PRAGMA table_info(operations)")
            }
            for name, declaration in (
                ("state", "TEXT NOT NULL DEFAULT 'pending'"),
                ("token", "TEXT NOT NULL DEFAULT ''"),
                ("expected_revision", "INTEGER NOT NULL DEFAULT 0"),
                ("evidence", "TEXT NOT NULL DEFAULT ''"),
            ):
                if name not in columns:
                    db.execute(
                        f"ALTER TABLE operations ADD COLUMN {name}"
                        f" {declaration}"
                    )
            db.execute(
                "UPDATE operations SET state='committed' WHERE ref IS NOT NULL"
                " AND state='pending'"
            )
            content_columns = {
                row[1] for row in db.execute("PRAGMA table_info(content)")
            }
            if "created" not in content_columns:
                db.execute(
                    "ALTER TABLE content ADD COLUMN created REAL NOT NULL"
                    " DEFAULT 0"
                )
            db.execute(
                "CREATE TABLE IF NOT EXISTS links (parent TEXT NOT NULL, child"
                " TEXT NOT NULL, PRIMARY KEY(parent,child))"
            )

    @contextmanager
    def _connect(self):
        db = sqlite3.connect(self.index, timeout=5)
        try:
            with db:
                yield db
        finally:
            db.close()

    def _scope(self, scope: dict) -> str:
        if scope.get("owner") != self.owner or not scope.get("conversation"):
            raise Status(
                code=StatusCode.PERMISSION_DENIED,
                message=(
                    "Retained reference belongs to another owner or missing"
                    " scope"
                ),
            ).to_exception()
        return json.dumps(
            {
                "owner": self.owner,
                "conversation": scope["conversation"],
                "branch": scope.get("branch", "main"),
            },
            sort_keys=True,
        )

    async def retain(
        self, text: str, scope: dict, *, mimetype: str = "text/plain"
    ) -> dict:
        """Publish a reference after storing its exact UTF-8 content."""
        key = self._scope(scope)
        await self.purge_expired()
        data = text.encode()
        ref = "ctx_" + uuid.uuid4().hex
        expires = time.time() + self.ttl_seconds
        digest = hashlib.sha256(data).hexdigest()

        def reserve():
            with self._connect() as db:
                db.execute("BEGIN IMMEDIATE")
                total, count = db.execute(
                    "SELECT COALESCE(SUM(bytes),0),COUNT(*) FROM content"
                ).fetchone()
                if (
                    total + len(data) > self.max_bytes
                    or count >= self.max_records
                ):
                    raise Status(
                        code=StatusCode.RESOURCE_EXHAUSTED,
                        message="Context retention quota exhausted",
                    ).to_exception()
                db.execute(
                    "INSERT INTO"
                    " content(ref,scope,bytes,digest,expires,ready,created)"
                    " VALUES (?,?,?,?,?,0,?)",
                    (ref, key, len(data), digest, expires, time.time()),
                )

        await asyncio.to_thread(reserve)
        try:
            node = self.factory.open(ref)
            await node.put(
                a11.NodeFragment(
                    data=a11.Chunk(
                        data=data, metadata=a11.ChunkMetadata(mimetype=mimetype)
                    ),
                    seq=0,
                    continued=False,
                )
            )
            await node.close_writes_with_status(a11.Status())

            def publish():
                with self._connect() as db:
                    updated = db.execute(
                        "UPDATE content SET ready=1 WHERE ref=? AND ready=0",
                        (ref,),
                    ).rowcount
                    if updated != 1:
                        raise Status(
                            code=StatusCode.ABORTED,
                            message=(
                                "Retention reservation is no longer current"
                            ),
                        ).to_exception()

            await asyncio.to_thread(publish)
        except BaseException:
            await asyncio.shield(self._orphan(ref))
            raise
        return {
            "owner": self.owner,
            "reference": ref,
            "scope": json.loads(key),
            "source_digest": digest,
            "expires_at": expires,
            "availability": "owner-local",
        }

    async def retrieve(
        self,
        reference: dict,
        scope: dict,
        *,
        cursor: int = 0,
        max_bytes: int = 65536,
    ) -> dict:
        """Read an exact UTF-8 page under a host-authorized scope.

        Args:
            reference: Owner/reference descriptor returned by retain.
            scope: Authorized owner, conversation, and branch.
            cursor: UTF-8 byte offset, initially zero.
            max_bytes: Maximum page size, up to 1 MiB.

        Returns:
            Text, current/next cursor, source digest, and expiry. next_cursor
            is None at the end. A multibyte character stays within one page.

        Raises:
            StatusException: Scope/reference is unavailable, expired, corrupt,
                or the requested page cannot start/end on UTF-8 boundaries.
        """
        key = self._scope(scope)
        if (
            type(cursor) is not int
            or type(max_bytes) is not int
            or cursor < 0
            or max_bytes < 1
            or max_bytes > 1024 * 1024
        ):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Invalid context retrieval page",
            ).to_exception()
        ref = reference.get("reference", "")

        def lookup():
            with self._connect() as db:
                return db.execute(
                    "SELECT scope,expires,ready,digest FROM content WHERE"
                    " ref=?",
                    (ref,),
                ).fetchone()

        row = await asyncio.to_thread(lookup)
        if not row or row[0] != key or reference.get("owner") != self.owner:
            raise Status(
                code=StatusCode.NOT_FOUND,
                message="Context reference not found in this scope",
            ).to_exception()
        if row[1] <= time.time() or row[2] != 1:
            raise Status(
                code=StatusCode.NOT_FOUND,
                message="Context reference expired or unavailable",
            ).to_exception()
        fragment = await self.factory.open(ref).get(0)
        raw = bytes(fragment.get_chunk().data)
        if hashlib.sha256(raw).hexdigest() != row[3]:
            raise Status(
                code=StatusCode.DATA_LOSS,
                message="Retained source digest does not match",
            ).to_exception()
        if cursor > len(raw):
            raise Status(
                code=StatusCode.OUT_OF_RANGE,
                message="Context cursor exceeds source size",
            ).to_exception()
        end = min(len(raw), cursor + max_bytes)
        try:
            text = raw[cursor:end].decode()
        except UnicodeDecodeError as error:
            if (
                error.reason != "unexpected end of data"
                or error.end != end - cursor
            ):
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message="Page cursor is not a UTF-8 boundary",
                ).to_exception()
            end = cursor + error.start
            if end == cursor:
                raise Status(
                    code=StatusCode.INVALID_ARGUMENT,
                    message="Page cannot hold the next UTF-8 character",
                ).to_exception()
            text = raw[cursor:end].decode()
        return {
            "text": text,
            "cursor": cursor,
            "next_cursor": end if end < len(raw) else None,
            "source_digest": row[3],
            "expires_at": row[1],
        }

    async def _orphan(self, reference: str) -> None:
        def mark():
            with self._connect() as db:
                db.execute(
                    "UPDATE content SET ready=-1,expires=? WHERE ref=? AND NOT"
                    " EXISTS(SELECT 1 FROM views WHERE ref=?) AND NOT"
                    " EXISTS(SELECT 1 FROM operations WHERE ref=?) AND NOT"
                    " EXISTS(SELECT 1 FROM links WHERE child=?)",
                    (time.time(), reference, reference, reference, reference),
                )

        await asyncio.to_thread(mark)

    async def _document(self, reference: str, scope: dict) -> dict:
        pages = []
        cursor = 0
        while cursor is not None:
            page = await self.retrieve(
                {"owner": self.owner, "reference": reference},
                scope,
                cursor=cursor,
                max_bytes=1024 * 1024,
            )
            pages.append(page["text"])
            cursor = page["next_cursor"]
        return json.loads("".join(pages))

    async def save_view(
        self,
        scope: dict,
        view: dict,
        *,
        source_digest: str,
        expected_revision: int,
        operation_id: str,
        claim_token: str | None = None,
    ) -> dict:
        """Commit a view with revision checks and durable operation fencing.

        Args:
            scope: Host-authorized owner, conversation, and branch.
            view: JSON view document with optional retained source descriptor.
            source_digest: Identity of the canonical input used for this view.
            expected_revision: Current scope revision, or zero for a new scope.
            operation_id: Stable identity for retry reconciliation.
            claim_token: Token supplied when claiming summary work. A replaced
                or abandoned token fences late workers out of publication.

        Returns:
            Owner, scope, committed revision, and owner-local reference.

        Raises:
            StatusException: Revision, source, operation, or claim conflicts.
        """
        key = self._scope(scope)
        if (
            not operation_id
            or type(expected_revision) is not int
            or expected_revision < 0
        ):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="View requires identity and nonnegative revision",
            ).to_exception()
        encoded = json.dumps(view, ensure_ascii=False, separators=(",", ":"))
        ref = await self.retain(encoded, scope, mimetype="application/json")

        def commit():
            with self._connect() as db:
                db.execute("BEGIN IMMEDIATE")
                op = db.execute(
                    "SELECT digest,ref,revision,state,token,expires FROM"
                    " operations WHERE scope=? AND operation=?",
                    (key, operation_id),
                ).fetchone()
                if op is not None:
                    if op[0] != source_digest:
                        raise Status(
                            code=StatusCode.ABORTED,
                            message=(
                                "Operation identity reused with different"
                                " source"
                            ),
                        ).to_exception()
                    if op[3] == "committed" and op[5] > time.time():
                        existing = db.execute(
                            "SELECT digest FROM content WHERE ref=? AND scope=?"
                            " AND ready=1",
                            (op[1], key),
                        ).fetchone()
                        if (
                            existing is None
                            or existing[0] != ref["source_digest"]
                        ):
                            raise Status(
                                code=StatusCode.ABORTED,
                                message=(
                                    "Operation identity reused with different"
                                    " view"
                                ),
                            ).to_exception()
                        return {"revision": op[2], "reference": op[1]}
                    if (
                        op[3] != "pending"
                        or op[5] <= time.time()
                        or op[4] != claim_token
                    ):
                        raise Status(
                            code=StatusCode.ABORTED,
                            message=(
                                "Context operation claim is no longer current"
                            ),
                        ).to_exception()
                elif claim_token is not None:
                    raise Status(
                        code=StatusCode.ABORTED,
                        message="Context operation claim is unavailable",
                    ).to_exception()
                head = db.execute(
                    "SELECT revision FROM heads WHERE scope=?", (key,)
                ).fetchone()
                current = head[0] if head else 0
                if current != expected_revision:
                    raise Status(
                        code=StatusCode.ABORTED,
                        message="Context view revision conflict",
                    ).to_exception()
                db.execute(
                    "INSERT OR REPLACE INTO heads VALUES (?,?)",
                    (key, current + 1),
                )
                db.execute(
                    "INSERT OR REPLACE INTO views VALUES (?,?,?,?,?)",
                    (
                        key,
                        current + 1,
                        ref["reference"],
                        source_digest,
                        operation_id,
                    ),
                )
                if op is None:
                    db.execute(
                        "INSERT INTO"
                        " operations(scope,operation,digest,ref,revision,"
                        "expires,state,token,expected_revision)"
                        " VALUES (?,?,?,?,?,?,'committed','',?)",
                        (
                            key,
                            operation_id,
                            source_digest,
                            ref["reference"],
                            current + 1,
                            ref["expires_at"],
                            current,
                        ),
                    )
                else:
                    db.execute(
                        "UPDATE operations SET"
                        " ref=?,revision=?,state='committed',expires=? WHERE"
                        " scope=? AND operation=?",
                        (
                            ref["reference"],
                            current + 1,
                            ref["expires_at"],
                            key,
                            operation_id,
                        ),
                    )
                source = view.get("source")
                if (
                    isinstance(source, dict)
                    and source.get("owner") == self.owner
                ):
                    source_ref = source.get("reference")
                    found = db.execute(
                        "SELECT scope,ready FROM content WHERE ref=?",
                        (source_ref,),
                    ).fetchone()
                    if found is None or found[0] != key or found[1] != 1:
                        raise Status(
                            code=StatusCode.NOT_FOUND,
                            message="View source is unavailable in this scope",
                        ).to_exception()
                    db.execute(
                        "INSERT OR IGNORE INTO links VALUES (?,?)",
                        (ref["reference"], source_ref),
                    )
                    db.execute(
                        "UPDATE content SET expires=MAX(expires,?) WHERE ref=?",
                        (ref["expires_at"], source_ref),
                    )
                return {"revision": current + 1, "reference": ref["reference"]}

        work = asyncio.create_task(asyncio.to_thread(commit))
        try:
            accepted = await asyncio.shield(work)
        except asyncio.CancelledError:
            try:
                await work
            finally:
                await asyncio.shield(self._orphan(ref["reference"]))
            raise
        except BaseException:
            await self._orphan(ref["reference"])
            raise
        if accepted["reference"] != ref["reference"]:
            await self._orphan(ref["reference"])
        return {"owner": self.owner, "scope": scope, **accepted}

    async def collect_garbage(
        self, *, pending_grace_seconds: float = 300
    ) -> dict:
        """Clear expired and abandoned payloads, retaining revision heads.

        Args:
            pending_grace_seconds: Age after which unpublished reservations are
                abandoned. The host accounts for live writers before collection.

        Returns:
            Counts of reclaimed payloads and expired operation records.
        """
        if pending_grace_seconds < 0:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Garbage collection grace must be nonnegative",
            ).to_exception()
        now = time.time()

        def select():
            with self._connect() as db:
                db.execute("BEGIN IMMEDIATE")
                db.execute(
                    "UPDATE operations SET state='expired',token=? WHERE"
                    " expires<=? AND state='pending'",
                    (uuid.uuid4().hex, now),
                )
                refs = [
                    r[0]
                    for r in db.execute(
                        "SELECT ref FROM content WHERE expires<=? OR ready=-1"
                        " OR (ready=0 AND created<=?)",
                        (now, now - pending_grace_seconds),
                    )
                ]
                for ref in refs:
                    db.execute(
                        "UPDATE content SET ready=-1 WHERE ref=?", (ref,)
                    )
                return refs

        refs = await asyncio.to_thread(select)
        for ref in refs:
            await self.factory.open(ref).clear_data(0)

        def remove():
            with self._connect() as db:
                db.execute("BEGIN IMMEDIATE")
                for ref in refs:
                    db.execute("DELETE FROM views WHERE ref=?", (ref,))
                    db.execute(
                        "DELETE FROM links WHERE parent=? OR child=?",
                        (ref, ref),
                    )
                    db.execute(
                        "DELETE FROM content WHERE ref=? AND ready=-1", (ref,)
                    )
                count = db.execute(
                    "DELETE FROM operations WHERE expires<=? AND"
                    " state!='pending'",
                    (now - pending_grace_seconds,),
                ).rowcount
                return count

        return {
            "payloads": len(refs),
            "operations": await asyncio.to_thread(remove),
        }

    async def purge_expired(self) -> int:
        """Clear expired data and retain revision tombstones."""
        result = await self.collect_garbage()
        return result["payloads"]

    async def load_view(self, scope: dict) -> dict | None:
        """Read the current view and revision under a host-authorized scope."""
        key = self._scope(scope)

        def lookup():
            with self._connect() as db:
                return db.execute(
                    "SELECT revision,ref,digest,operation FROM views WHERE"
                    " scope=?",
                    (key,),
                ).fetchone()

        row = await asyncio.to_thread(lookup)
        if row is None:
            return None
        return {
            "owner": self.owner,
            "scope": scope,
            "revision": row[0],
            "reference": row[1],
            "source_digest": row[2],
            "operation_id": row[3],
            "view": await self._document(row[1], scope),
        }

    async def inspect_operation(self, scope: dict, operation_id: str) -> dict:
        """Read durable operation state for receiving-host reconciliation."""
        key = self._scope(scope)

        def lookup():
            with self._connect() as db:
                return db.execute(
                    "SELECT"
                    " digest,ref,revision,expires,state,token,"
                    "expected_revision,evidence"
                    " FROM operations WHERE scope=? AND operation=?",
                    (key, operation_id),
                ).fetchone()

        row = await asyncio.to_thread(lookup)
        if row is None:
            raise Status(
                code=StatusCode.NOT_FOUND, message="Context operation not found"
            ).to_exception()
        return {
            "owner": self.owner,
            "scope": scope,
            "operation_id": operation_id,
            "source_digest": row[0],
            "reference": row[1],
            "revision": row[2],
            "expires_at": row[3],
            "state": row[4],
            "claim_token": row[5],
            "expected_revision": row[6],
            "evidence": row[7],
        }

    async def claim_operation(
        self,
        scope: dict,
        *,
        operation_id: str,
        source_digest: str,
        expected_revision: int,
        claim_token: str | None = None,
    ) -> dict | None:
        """Claim summary work or recover its accepted view without rerunning it.

        Pass a unique claim_token through save_view to fence late workers after
        reconciliation. Pending and abandoned operations require host decisions.
        """
        key = self._scope(scope)
        if (
            not operation_id
            or type(expected_revision) is not int
            or expected_revision < 0
        ):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=(
                    "Operation identity and nonnegative revision are required"
                ),
            ).to_exception()

        def claim():
            with self._connect() as db:
                db.execute("BEGIN IMMEDIATE")
                row = db.execute(
                    "SELECT digest,ref,revision,expires,state,token FROM"
                    " operations WHERE scope=? AND operation=?",
                    (key, operation_id),
                ).fetchone()
                if row:
                    if row[0] != source_digest:
                        raise Status(
                            code=StatusCode.ABORTED,
                            message=(
                                "Operation identity reused with different"
                                " source"
                            ),
                        ).to_exception()
                    if row[3] <= time.time():
                        raise Status(
                            code=StatusCode.NOT_FOUND,
                            message="Context operation expired",
                        ).to_exception()
                    if row[4] == "committed":
                        return {"reference": row[1], "revision": row[2]}
                    if row[4] != "released":
                        raise Status(
                            code=StatusCode.FAILED_PRECONDITION,
                            message=(
                                "Context operation needs owner reconciliation"
                                " before retry"
                            ),
                        ).to_exception()
                head = db.execute(
                    "SELECT revision FROM heads WHERE scope=?", (key,)
                ).fetchone()
                if (head[0] if head else 0) != expected_revision:
                    raise Status(
                        code=StatusCode.ABORTED,
                        message="Context view revision conflict",
                    ).to_exception()
                if (
                    db.execute("SELECT COUNT(*) FROM operations").fetchone()[0]
                    >= self.max_records
                    and row is None
                ):
                    raise Status(
                        code=StatusCode.RESOURCE_EXHAUSTED,
                        message="Context operation quota exhausted",
                    ).to_exception()
                db.execute(
                    "INSERT OR REPLACE INTO"
                    " operations(scope,operation,digest,expires,state,token,"
                    "expected_revision)"
                    " VALUES (?,?,?,?,'pending',?,?)",
                    (
                        key,
                        operation_id,
                        source_digest,
                        time.time() + self.ttl_seconds,
                        claim_token or uuid.uuid4().hex,
                        expected_revision,
                    ),
                )
                return None

        accepted = await asyncio.to_thread(claim)
        if accepted is None:
            return None
        return {
            **accepted,
            "owner": self.owner,
            "scope": scope,
            "view": await self._document(accepted["reference"], scope),
        }

    async def reconcile_operation(
        self,
        scope: dict,
        operation_id: str,
        *,
        expected_token: str,
        disposition: str,
        evidence: str,
    ) -> dict:
        """Record an explicit owner decision and fence the previous worker.

        Args:
            scope: Host-authorized scope for the operation.
            operation_id: Durable operation identity to inspect or resolve.
            expected_token: Claim observed by the authorized recovery caller.
            disposition: abandon, retry, or inspect. Retry is an explicit owner
                decision after reconciling external effects, never an automatic
                rerun after a lost response.
            evidence: Host audit note describing that reconciliation.

        Returns:
            Current operation metadata, including its replacement claim token.
        """
        key = self._scope(scope)
        if (
            disposition not in ("abandon", "retry", "inspect")
            or not evidence.strip()
        ):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Reconciliation requires a disposition and evidence",
            ).to_exception()

        def reconcile():
            with self._connect() as db:
                db.execute("BEGIN IMMEDIATE")
                row = db.execute(
                    "SELECT state,token FROM operations WHERE scope=? AND"
                    " operation=?",
                    (key, operation_id),
                ).fetchone()
                if row is None:
                    raise Status(
                        code=StatusCode.NOT_FOUND,
                        message="Context operation not found",
                    ).to_exception()
                if row[1] != expected_token:
                    raise Status(
                        code=StatusCode.ABORTED,
                        message=(
                            "Context operation claim changed during"
                            " reconciliation"
                        ),
                    ).to_exception()
                if row[0] == "committed" or disposition == "inspect":
                    return
                db.execute(
                    "UPDATE operations SET state=?,token=?,evidence=? WHERE"
                    " scope=? AND operation=?",
                    (
                        "released" if disposition == "retry" else "abandoned",
                        uuid.uuid4().hex,
                        evidence,
                        key,
                        operation_id,
                    ),
                )

        await asyncio.to_thread(reconcile)
        return await self.inspect_operation(scope, operation_id)

    async def fork_view(
        self, scope: dict, destination: dict, *, operation_id: str
    ) -> dict:
        """Copy an accepted view into a new authorized branch."""
        current = await self.load_view(scope)
        if current is None:
            raise Status(
                code=StatusCode.NOT_FOUND,
                message="Source context view not found",
            ).to_exception()
        document = dict(current["view"])
        original = document.get("source")
        if isinstance(original, dict):
            cursor, pages = 0, []
            while cursor is not None:
                page = await self.retrieve(
                    original, scope, cursor=cursor, max_bytes=1024 * 1024
                )
                pages.append(page["text"])
                cursor = page["next_cursor"]
            document["source"] = await self.retain(
                "".join(pages), destination, mimetype="application/json"
            )
        document["parent_view"] = {
            k: current[k] for k in ("owner", "scope", "reference", "revision")
        }
        return await self.save_view(
            destination,
            document,
            source_digest=current["source_digest"],
            expected_revision=0,
            operation_id=operation_id,
        )
