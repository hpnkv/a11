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

"""Drive the native A11 process action as one-shot and persistent shells."""

from __future__ import annotations

import asyncio
import json
import os
import pathlib
import uuid
from collections.abc import AsyncIterator, Awaitable, Callable, Sequence
from enum import StrEnum

import a11
from a11 import flow
from a11.sdk.bash.shell import BashShell
from a11.status import Status, StatusCode, StatusException

# Workspace discovery can legitimately enumerate large monorepos before its
# caller applies a result limit. Keep a finite native ceiling, but do not make
# an ordinary repository look like a failed process at 128 KiB.
MAX_PROCESS_OUTPUT = 8 * 1024 * 1024

_ENCODING = {"type": "string", "enum": ["json", "msgpack", "packb"]}


def _object(properties: dict[str, object]) -> dict[str, object]:
    return {
        "type": "object",
        "properties": properties,
        "additionalProperties": False,
    }


def _array(items: dict[str, object]) -> dict[str, object]:
    return {"type": "array", "items": items}


def _install_native_json_schemas(registry: a11.ActionRegistry) -> None:
    """Give Studio and model adapters structured native action inputs."""

    def integer(description: str) -> dict[str, object]:
        return {"type": "integer", "minimum": 0, "description": description}

    def boolean(description: str) -> dict[str, object]:
        return {"type": "boolean", "description": description}

    def string(description: str) -> dict[str, object]:
        return {"type": "string", "description": description}

    plain_string = {"type": "string"}
    omit = _array(plain_string)
    omit["description"] = "Output port names to close without producing."
    encoding = dict(_ENCODING)
    encoding["description"] = (
        "Structured-output encoding; msgpack preserves non-UTF-8 paths."
    )
    duration = {
        "description": "Duration as seconds or a string such as 250ms or 1h.",
        "oneOf": [
            {"type": "number", "minimum": 0},
            {"type": "string", "minLength": 1},
        ],
    }
    stop_event = _object({
        "command": {
            "const": "stop",
            "description": "Request graceful early completion.",
        }
    })
    stop_event["required"] = ["command"]

    schemas = {
        "read_file": {
            "options": _object({
                "chunk_bytes": integer(
                    "Bytes emitted in each bytes chunk; default 65536."
                ),
                "offset": integer(
                    "Zero-based byte offset at which reading begins."
                ),
                "length": integer(
                    "Maximum bytes to read after offset; omit for EOF."
                ),
                "max_bytes": integer(
                    "Safety ceiling for total bytes read from the file."
                ),
                "omit": omit,
                "encoding": encoding,
            })
        },
        "list_directory": {
            "options": _object({
                "recursive": boolean("Walk descendants; default false."),
                "max_depth": integer("Deepest descendant level to visit."),
                "hidden": boolean("Include dot files; default false."),
                "match": {
                    "description": "Name glob, or list of globs, to include.",
                    "oneOf": [plain_string, _array(plain_string)],
                },
                "kinds": {
                    "description": "Entry kind, or kinds, to include.",
                    "oneOf": [plain_string, _array(plain_string)],
                },
                "max_entries": integer("Maximum entries to emit."),
                "omit": omit,
                "encoding": encoding,
            })
        },
        "stat_path": {
            "options": _object({
                "omit": omit,
                "encoding": encoding,
            })
        },
        "write_file": {
            "options": _object({
                "append": boolean("Append instead of replacing."),
                "atomic": boolean("Replace atomically when not appending."),
                "create_parents": boolean("Create missing parent directories."),
                "sync": boolean("Flush file data to durable storage."),
                "mode": integer("POSIX permission mode; default 0644."),
                "max_bytes": integer("Hard ceiling on bytes written."),
                "omit": omit,
                "encoding": encoding,
            })
        },
        "make_directory": {
            "options": _object({
                "parents": boolean("Create missing parents; default true.")
            })
        },
        "remove_path": {
            "options": _object({
                "recursive": boolean("Remove a directory tree."),
                "missing_ok": boolean("Succeed if the path is absent."),
            })
        },
        "move_path": {
            "options": _object({
                "overwrite": boolean("Replace an existing destination.")
            })
        },
        "copy_path": {
            "options": _object({
                "recursive": boolean("Copy a directory tree."),
                "overwrite": boolean("Replace an existing destination."),
            })
        },
        "make_temp": {
            "options": _object({
                "directory": boolean("Create a directory rather than a file."),
                "prefix": string("Unique-name prefix; default 'a11-'."),
                "suffix": string("Suffix appended to the unique name."),
                "in": string("Workspace directory in which to create it."),
            })
        },
        "random_bytes": {
            "options": _object({
                "count": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": 1024 * 1024,
                    "description": "Number of random bytes; default 32.",
                },
                "format": {
                    "type": "string",
                    "enum": ["hex", "base64", "base64url", "raw"],
                    "description": (
                        "Text encoding; raw writes only the bytes output."
                    ),
                },
            })
        },
        "new_uuid": {
            "options": _object({
                "count": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": 100000,
                    "description": "Number of UUIDs to produce; default 1.",
                }
            })
        },
        "ticker": {
            "options": _object({
                "every": duration,
                "count": {
                    "type": "integer",
                    "minimum": 0,
                    "description": (
                        "Ticks to produce; zero or omitted is unbounded."
                    ),
                },
                "for": duration,
                "immediate": boolean("Emit once before the first interval."),
                "catch_up": boolean(
                    "Deliver delayed ticks instead of counting them skipped."
                ),
                "omit": omit,
            }),
            "control_events": stop_event,
        },
        "sleep_for": {"control_events": stop_event},
        "spawn_process": {
            "arguments": {
                "description": "Ordered arguments; values are not shell-split.",
                "oneOf": [plain_string, _array(plain_string)],
            },
            "options": _object({
                "cwd": string("Workspace working directory."),
                "environment": {
                    "type": "object",
                    "description": "Explicit child environment variables.",
                    "additionalProperties": {"type": "string"},
                },
                "clear_environment": boolean("Discard inherited variables."),
                "grace": {
                    "description": "Delay from SIGTERM to SIGKILL; default 5s.",
                    "oneOf": [
                        {"type": "number", "minimum": 0},
                        plain_string,
                    ],
                },
                "max_output_bytes": integer("Combined output byte ceiling."),
                "max_output_lines": integer(
                    "Stop after this many combined stdout/stderr lines."
                ),
                "omit": omit,
            }),
            "control_events": {
                "description": (
                    "Events sent while the process runs; close the stream "
                    "when no more control is needed."
                ),
                "oneOf": [
                    {
                        "type": "object",
                        "properties": {
                            "command": {
                                "const": "stop",
                                "description": (
                                    "Request graceful process termination."
                                ),
                            }
                        },
                        "required": ["command"],
                        "additionalProperties": False,
                    },
                    {
                        "type": "object",
                        "properties": {
                            "command": {
                                "const": "signal",
                                "description": "Send a POSIX signal.",
                            },
                            "signal": {
                                "type": "string",
                                "description": (
                                    "POSIX signal name to send to the process."
                                ),
                                "enum": [
                                    "TERM",
                                    "KILL",
                                    "INT",
                                    "HUP",
                                    "QUIT",
                                    "USR1",
                                    "USR2",
                                    "CONT",
                                ],
                            },
                        },
                        "required": ["command", "signal"],
                        "additionalProperties": False,
                    },
                ],
            },
        },
    }
    descriptions = {
        "read_file": {"options": "Optional read bounds and representation."},
        "list_directory": {"options": "Traversal filters and result bounds."},
        "stat_path": {"options": "Optional result representation."},
        "write_file": {"options": "Write semantics and resource bounds."},
        "make_directory": {"options": "Directory creation semantics."},
        "remove_path": {"options": "Removal semantics."},
        "move_path": {"options": "Move semantics."},
        "copy_path": {"options": "Copy semantics."},
        "make_temp": {"options": "Temporary path shape and location."},
        "random_bytes": {
            "options": "Random byte count and output representation."
        },
        "new_uuid": {"options": "UUID result count."},
        "ticker": {
            "options": "Tick schedule, bounds, and output selection.",
            "control_events": "Cooperative stop events.",
        },
        "sleep_for": {"control_events": "Cooperative stop events."},
        "spawn_process": {
            "arguments": "Arguments passed directly to the program.",
            "options": "Process environment, limits, and output selection.",
            "control_events": "Cooperative stop and signal events.",
        },
    }
    for action_name, ports in schemas.items():
        if not registry.is_registered(action_name):
            continue
        schema = registry.get_schema(action_name)
        for port_name, json_schema in ports.items():
            port = schema.inputs[port_name]
            port.json_schema = json.dumps(json_schema)
            port.description = descriptions[action_name][port_name]
            schema.inputs[port_name] = port
        registry.register(
            action_name, schema, registry.get_handler(action_name)
        )


class SandboxMode(StrEnum):
    """Filesystem rights captured by native action handlers."""

    READ_ONLY = "read-only"
    WORKSPACE_WRITE = "workspace-write"


def safe_environment() -> dict[str, str]:
    """Return the non-credential environment supplied to child processes."""
    allowed = {"LANG", "LC_ALL", "LC_CTYPE", "PATH", "TERM", "TZ"}
    return {key: value for key, value in os.environ.items() if key in allowed}


def register_native_actions(
    registry: a11.ActionRegistry,
    roots: Sequence[pathlib.Path],
    *,
    cwd: pathlib.Path,
    mode: SandboxMode,
    allow_run: bool = True,
) -> None:
    """Register native file and process actions with required confinement."""
    sandbox_roots = [path.resolve() for path in roots]
    temporary_root = pathlib.Path("/tmp").resolve()
    if temporary_root not in sandbox_roots:
        sandbox_roots.append(temporary_root)
    flow.register_standard_actions(
        registry,
        [str(path) for path in sandbox_roots],
        allow_write=mode == SandboxMode.WORKSPACE_WRITE,
        allow_run=allow_run,
        require_sandbox=True,
        inherit_environment=False,
        max_seconds=600,
        current_directory=str(cwd),
    )
    _install_native_json_schemas(registry)


async def _finalize_process_inputs(
    action: a11.Action,
    *,
    program: str,
    arguments: Sequence[str],
    cwd: pathlib.Path,
    keep_stdin: bool,
    max_output_bytes: int,
    max_output_lines: int | None = None,
    keep_control: bool = False,
    environment: dict[str, str] | None = None,
) -> None:
    await action["program"].finalize(program)
    await action["arguments"].finalize(list(arguments))
    options: dict[str, object] = {
        "cwd": str(cwd),
        "clear_environment": True,
        "environment": safe_environment() | dict(environment or {}),
        "max_output_bytes": max_output_bytes,
        "omit": ["stdout", "stderr"],
    }
    if max_output_lines is not None:
        options["max_output_lines"] = max_output_lines
        options["grace"] = "100ms"
    if keep_control:
        options["grace"] = "100ms"
    await action["options"].finalize(options)
    if not keep_stdin:
        await action["stdin"].close()
    if not keep_control:
        await action["control_events"].close()


def make_process_action(registry: a11.ActionRegistry) -> a11.Action:
    """Create one local call to the registered native process handler."""
    return (
        a11
        .Action(registry.get_schema("spawn_process"))
        .bind_registry(registry)
        .bind_handler(registry.get_handler("spawn_process"))
    )


async def run_native_process(
    registry: a11.ActionRegistry,
    *,
    program: str,
    arguments: Sequence[str],
    cwd: pathlib.Path,
    timeout_seconds: int,
    output: a11.AsyncNode,
    stdin_data: bytes | None = None,
    environment: dict[str, str] | None = None,
    on_line: Callable[[str], Awaitable[None]] | None = None,
    max_output_lines: int | None = None,
) -> dict[str, object]:
    """Run a confined native process and relay a bounded line stream."""
    process = make_process_action(registry)
    a11.set_deadline_header(
        process, a11.now() + a11.Duration.seconds(timeout_seconds)
    )
    process.run()
    lines = 0

    async def relay(name: str, prefix: str) -> None:
        nonlocal lines
        async for line in process[name]:
            lines += 1
            rendered = f"{prefix}{line}"
            await output.put(rendered)
            if on_line is not None:
                await on_line(rendered)

    async def unary(name: str, default=None):
        value = await process[name].consume(allow_none=True)
        return default if value is None else value

    stdout = asyncio.create_task(relay("stdout_lines", ""))
    stderr = asyncio.create_task(relay("stderr_lines", "stderr: "))
    values = {
        name: asyncio.create_task(unary(name))
        for name in (
            "exit_code",
            "signal",
            "usage",
            "sandbox",
            "output_truncated",
        )
    }
    pid = asyncio.create_task(unary("pid"))
    try:
        # Attach every output reader before the last input can make the native
        # handler runnable. The native line bound stops fast producers; early
        # readers keep their bounded output streaming without a startup race.
        await _finalize_process_inputs(
            process,
            program=program,
            arguments=arguments,
            cwd=cwd,
            keep_stdin=stdin_data is not None,
            max_output_bytes=MAX_PROCESS_OUTPUT,
            max_output_lines=max_output_lines,
            environment=environment,
        )
        if stdin_data is not None:
            await process["stdin"].put(
                stdin_data, mimetype="application/octet-stream"
            )
            await process["stdin"].close()
        await process.wait(a11.Duration.seconds(timeout_seconds + 6))
        await asyncio.gather(stdout, stderr, pid, *values.values())
    except BaseException:
        process.cancel()
        await asyncio.gather(
            stdout, stderr, pid, *values.values(), return_exceptions=True
        )
        raise
    return {
        "exit_code": values["exit_code"].result(),
        "signal": values["signal"].result() or "",
        "usage": values["usage"].result() or {},
        "sandbox": values["sandbox"].result() or "",
        "output_lines": lines,
        "stopped_early": bool(values["output_truncated"].result()),
    }


class NativeActionShell(BashShell):
    """Persistent Bash carried by A11's native ``spawn_process`` action."""

    def __init__(self, registry: a11.ActionRegistry, cwd: pathlib.Path) -> None:
        super().__init__("bash")
        self._registry = registry
        self._cwd = cwd
        self._action: a11.Action | None = None
        self._queue: asyncio.Queue[str | None] = asyncio.Queue()
        self._readers: list[asyncio.Task[None]] = []
        self._waiter: asyncio.Task[None] | None = None
        self._alive = False

    @property
    def is_alive(self) -> bool:
        return self._alive and self._action is not None

    async def start(self) -> None:
        if self._action is not None:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="Shell is already started.",
            ).to_exception()
        action = make_process_action(self._registry)
        action.run()
        await _finalize_process_inputs(
            action,
            program="bash",
            arguments=["--noprofile", "--norc"],
            cwd=self._cwd,
            keep_stdin=True,
            keep_control=True,
            max_output_bytes=0,
        )
        self._action = action
        self._alive = True

        async def read(name: str) -> None:
            try:
                async for line in action[name]:
                    await self._queue.put(str(line))
            finally:
                await self._queue.put(None)

        self._readers = [
            asyncio.create_task(read("stdout_lines")),
            asyncio.create_task(read("stderr_lines")),
        ]

        async def drain(name: str) -> None:
            async for _ in action[name]:
                pass

        self._readers.extend(
            asyncio.create_task(drain(name))
            for name in ("pid", "exit_code", "signal", "usage", "sandbox")
        )

        async def wait() -> None:
            try:
                await action.wait()
            except StatusException:
                pass
            finally:
                self._alive = False

        self._waiter = asyncio.create_task(wait())
        await action["stdin"].put(
            b"set -m\n", mimetype="application/octet-stream"
        )

    async def execute(
        self, command: str, timeout: float | None = None
    ) -> AsyncIterator[str]:
        if not self.is_alive or self._action is None:
            raise Status(
                code=StatusCode.FAILED_PRECONDITION,
                message="Shell is not running.",
            ).to_exception()
        await self._exec_lock.acquire()
        token = f"__A11_END_{uuid.uuid4().hex}__"
        wrapper = (
            f"{{ {command}\n}} </dev/null\nprintf '{token} %d\\n' \"$?\"\n"
        )
        await self._action["stdin"].put(
            wrapper.encode(), mimetype="application/octet-stream"
        )
        deadline = (
            None
            if timeout is None
            else asyncio.get_running_loop().time() + timeout
        )
        try:
            while True:
                remaining = (
                    None
                    if deadline is None
                    else max(0, deadline - asyncio.get_running_loop().time())
                )
                try:
                    line = await asyncio.wait_for(
                        self._queue.get(), timeout=remaining
                    )
                except asyncio.TimeoutError as error:
                    await self.close()
                    raise Status(
                        code=StatusCode.DEADLINE_EXCEEDED,
                        message=f"Command timed out after {timeout:g} seconds.",
                    ).to_exception() from error
                if line is None:
                    if self.is_alive:
                        continue
                    raise Status(
                        code=StatusCode.UNAVAILABLE,
                        message="Shell exited before the command finished.",
                    ).to_exception()
                if line.startswith(f"{token} "):
                    try:
                        self._last_exit_code = int(
                            line.removeprefix(f"{token} ")
                        )
                    except ValueError:
                        self._last_exit_code = -1
                    return
                yield line
        except BaseException:
            await self.close()
            raise
        finally:
            self._exec_lock.release()

    async def close(self) -> None:
        action = self._action
        if action is None:
            return
        self._alive = False
        await action["stdin"].close()
        await action["control_events"].put({"command": "stop"})
        await action["control_events"].close()
        if self._waiter is not None:
            try:
                await asyncio.wait_for(self._waiter, timeout=2)
            except asyncio.TimeoutError:
                action.cancel()
                await asyncio.gather(self._waiter, return_exceptions=True)
        for reader in self._readers:
            if not reader.done():
                reader.cancel()
        await asyncio.gather(*self._readers, return_exceptions=True)
        self._action = None

    def terminate(self) -> None:
        if self._action is not None:
            self._action.cancel()
        self._alive = False

    def kill(self) -> None:
        self.terminate()

    async def wait_closed(self) -> None:
        if self._waiter is not None:
            await self._waiter
