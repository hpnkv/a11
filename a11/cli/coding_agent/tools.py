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

"""Install bounded workspace and command actions for a coding session."""

from __future__ import annotations

import asyncio
import json
import pathlib
import shlex
import uuid
from dataclasses import dataclass, field

import a11
from a11.cli.coding_agent.policy import ApprovalMode, Policy
from a11.cli.coding_agent.sandbox import (
    SandboxMode,
    register_native_actions,
    run_native_process,
)
from a11.cli.coding_agent.workspace import Workspace
from a11.status import Status, StatusCode

MAX_FILE_BYTES = 128 * 1024
MAX_TOOL_TEXT = 128 * 1024
MAX_MATCHES = 200
MAX_FILES = 1000


def _port(
    name: str,
    mimetype: str,
    description: str,
    *,
    typeinfo: type = str,
    required: bool = False,
    unary: bool = True,
    json_schema: dict[str, object] | None = None,
) -> a11.ActionPortSchema:
    return a11.ActionPortSchema(
        name,
        mimetype,
        description=description,
        typeinfo=typeinfo,
        required=required,
        unary=unary,
        json_schema=(json.dumps(json_schema) if json_schema else ""),
    )


WORKSPACE_INFO_SCHEMA = a11.ActionSchema(
    name="workspace_info",
    description=(
        "Inspect the coding workspace, repository instructions, Git state,"
        " agent changes, and completed checks. Returns them on the unary"
        " `result` output."
    ),
    outputs={
        "result": _port(
            "result", "application/json", "Workspace facts.", typeinfo=dict
        )
    },
    output_to_json_field={"result": "$"},
)

LIST_FILES_SCHEMA = a11.ActionSchema(
    name="list_files",
    description=(
        "List files below a workspace directory, respecting Git ignores when"
        " available. Uses portable fallbacks when ripgrep is unavailable."
        " The unary `result` output is `{files: [string], truncated: bool}`;"
        " inside Flow read it as `step.result.files`."
    ),
    inputs={
        "path": _port(
            "path",
            "text/plain",
            "Directory relative to the current working directory; default '.'.",
        ),
        "limit": _port(
            "limit",
            "application/json",
            "Maximum files from 1 to 1000.",
            typeinfo=int,
            json_schema={"type": "integer", "minimum": 1, "maximum": 1000},
        ),
    },
    outputs={
        "result": _port(
            "result",
            "application/json",
            "File list and truncation state.",
            typeinfo=dict,
        )
    },
    output_to_json_field={"result": "$"},
)

SEARCH_TEXT_SCHEMA = a11.ActionSchema(
    name="search_text",
    description=(
        "Search workspace text with bounded results. Uses grep when ripgrep"
        " is unavailable. The unary `result` output is"
        " `{matches: [string], truncated: bool}`; a Flow can select less with"
        " `step.result.matches | flatten | first N`."
    ),
    inputs={
        "query": _port(
            "query", "text/plain", "Text or regular expression.", required=True
        ),
        "path": _port(
            "path",
            "text/plain",
            "File or directory relative to the current working directory;"
            " default '.'.",
        ),
        "regex": _port(
            "regex",
            "application/json",
            "Interpret query as a regex.",
            typeinfo=bool,
        ),
        "limit": _port(
            "limit",
            "application/json",
            "Maximum matches from 1 to 200.",
            typeinfo=int,
            json_schema={"type": "integer", "minimum": 1, "maximum": 200},
        ),
    },
    outputs={
        "result": _port(
            "result",
            "application/json",
            "Matches and truncation state.",
            typeinfo=dict,
        )
    },
    output_to_json_field={"result": "$"},
)

APPLY_PATCH_SCHEMA = a11.ActionSchema(
    name="apply_patch",
    description=(
        "Apply one git-format unified diff after workspace, preimage, and"
        " permission checks. Use this action for file edits. The patch must"
        " use `diff --git`, `--- a/path`, and `+++ b/path` headers; a new file"
        " uses `--- /dev/null` and `+++ b/path`. Never use `*** Begin Patch`,"
        " `*** Add File`, or similar wrapper syntax."
    ),
    inputs={
        "patch": _port(
            "patch",
            "text/plain",
            "Raw git-format unified diff with workspace-relative `a/` and"
            " `b/` paths. Example for a new file: `diff --git a/file b/file`,"
            " `new file mode 100644`, `--- /dev/null`, `+++ b/file`,"
            " `@@ -0,0 +1 @@`, then `+content`. Do not wrap it in Markdown or"
            " `*** Begin Patch` markers.",
            required=True,
        ),
    },
    outputs={
        "result": _port(
            "result", "application/json", "Changed paths.", typeinfo=dict
        )
    },
    output_to_json_field={"result": "$"},
)

FILE_DIFF_SCHEMA = a11.ActionSchema(
    name="file_diff",
    description=(
        "Read the current Git diff for a path or the agent's files. The unary"
        " `result` output contains `diff` text and a `truncated` flag; filter"
        " or truncate `step.result.diff` inside Flow for an excerpt."
    ),
    inputs={
        "path": _port(
            "path",
            "text/plain",
            "Optional path relative to the current working directory.",
        ),
        "staged": _port(
            "staged", "application/json", "Read the staged diff.", typeinfo=bool
        ),
    },
    outputs={
        "result": _port(
            "result", "application/json", "Bounded diff text.", typeinfo=dict
        )
    },
    output_to_json_field={"result": "$"},
)

RUN_COMMAND_SCHEMA = a11.ActionSchema(
    name="run_command",
    description=(
        "Run a one-shot command in the kernel sandbox. Output streams by line;"
        " the unary `result` reports exit status, timeout, and truncation."
        " In Flow, filter `step.output_lines` and leave it undeclared when"
        " only completion data is needed. Use web-fetch for HTTP retrieval;"
        " do not run curl, wget, or a language HTTP client here when"
        " web-fetch is available."
    ),
    inputs={
        "command": _port(
            "command",
            "text/plain",
            "Shell command for builds, tests, and local processing. Do not"
            " use curl, wget, or a language HTTP client when web-fetch is"
            " available.",
            required=True,
        ),
        "cwd": _port(
            "cwd",
            "text/plain",
            "Workspace directory relative to the current working directory;"
            " default '.'.",
        ),
        "timeout_seconds": _port(
            "timeout_seconds",
            "application/json",
            "Wall-clock timeout from 1 to 600 seconds.",
            typeinfo=int,
            json_schema={"type": "integer", "minimum": 1, "maximum": 600},
        ),
    },
    outputs={
        "output_lines": _port(
            "output_lines",
            "text/plain",
            "Interleaved stdout and stderr lines.",
            required=False,
            unary=False,
        ),
        "result": _port(
            "result",
            "application/json",
            "Command completion data.",
            typeinfo=dict,
        ),
    },
)

REPORT_COMPLETION_SCHEMA = a11.ActionSchema(
    name="report_completion",
    description=(
        "Record the coding task outcome, files changed, checks, and remaining"
        " work. Call once the repository state has been verified."
    ),
    inputs={
        "summary": _port(
            "summary", "text/plain", "Completed outcome.", required=True
        ),
        "checks": _port("checks", "text/plain", "Checks run and their status."),
        "remaining": _port(
            "remaining", "text/plain", "Remaining work or blocker."
        ),
    },
    outputs={
        "result": _port(
            "result", "application/json", "Recorded completion.", typeinfo=dict
        )
    },
    output_to_json_field={"result": "$"},
)

REQUEST_USER_INPUT_SCHEMA = a11.ActionSchema(
    name="request_user_input",
    description=(
        "Pause for a user decision only when work cannot safely continue."
        " Offer concise choices when possible; free text remains available"
        " when allow_free_text is true."
    ),
    inputs={
        "question": _port(
            "question",
            "text/plain",
            "One specific question whose answer unblocks the current task.",
            required=True,
        ),
        "options": _port(
            "options",
            "application/json",
            "Two to eight distinct choices. Each has a short label and"
            " may have one sentence explaining its consequence. Omit for"
            " a free-text-only question.",
            typeinfo=list,
            json_schema={
                "type": "array",
                "minItems": 2,
                "maxItems": 8,
                "items": {
                    "type": "object",
                    "properties": {
                        "label": {
                            "type": "string",
                            "minLength": 1,
                            "description": "Text shown on the choice control.",
                        },
                        "description": {
                            "type": "string",
                            "description": "Optional consequence or tradeoff.",
                        },
                    },
                    "required": ["label"],
                    "additionalProperties": False,
                },
            },
        ),
        "allow_free_text": _port(
            "allow_free_text",
            "application/json",
            "Whether the user may type an answer instead of selecting a"
            " choice. Defaults to true and is always true without options.",
            typeinfo=bool,
        ),
    },
    outputs={
        "result": _port(
            "result",
            "application/json",
            "The user's answer and matching option label, if one was chosen.",
            typeinfo=dict,
            json_schema={
                "type": "object",
                "properties": {
                    "answer": {"type": "string"},
                    "selected_option": {"type": ["string", "null"]},
                },
                "required": ["answer", "selected_option"],
                "additionalProperties": False,
            },
        )
    },
    output_to_json_field={"result": "$"},
)

RESPOND_USER_INPUT_SCHEMA = a11.ActionSchema(
    name="respond_user_input",
    description=(
        "Answer a pending coding-agent question. Intended for Chat, Studio,"
        " and IDE clients; it is never offered to the model."
    ),
    inputs={
        "request_id": _port(
            "request_id",
            "text/plain",
            "Tool-call id shown with the pending request.",
            required=True,
        ),
        "answer": _port(
            "answer",
            "text/plain",
            "A listed option label or its 1-based number, or a free-text"
            " answer when permitted.",
            required=True,
        ),
    },
    outputs={
        "result": _port(
            "result",
            "application/json",
            "The accepted answer and request id.",
            typeinfo=dict,
        )
    },
    output_to_json_field={"result": "$"},
)

PENDING_USER_INPUTS_SCHEMA = a11.ActionSchema(
    name="pending_user_inputs",
    description=(
        "List unresolved coding-agent questions so a reconnecting Studio or"
        " other A11 client can render and answer them."
    ),
    outputs={
        "result": _port(
            "result",
            "application/json",
            "Pending request ids, questions, choices, and free-text policy.",
            typeinfo=dict,
        )
    },
    output_to_json_field={"result": "$"},
)

CONFIGURE_CODING_AGENT_SCHEMA = a11.ActionSchema(
    name="configure_coding_agent",
    description=(
        "User-facing control for the coding agent's permission level. This"
        " changes approval policy, never the gateway's kernel-sandbox ceiling."
    ),
    inputs={
        "approval_mode": _port(
            "approval_mode",
            "text/plain",
            "Permission level: 'suggest' lets the agent inspect and propose"
            " effects; 'auto' permits effects within the configured"
            " sandbox.",
            required=True,
            json_schema={
                "type": "string",
                "enum": ["suggest", "auto"],
            },
        )
    },
    outputs={
        "result": _port(
            "result",
            "application/json",
            "Effective approval mode and immutable sandbox boundaries.",
            typeinfo=dict,
        )
    },
    output_to_json_field={"result": "$"},
)

CODING_AGENT_INFO_SCHEMA = a11.ActionSchema(
    name="coding_agent_info",
    description=(
        "Get the gateway coding agent's workspace, policy, instructions, and"
        " model-facing action names. Clients use this to present the same"
        " agent."
    ),
    outputs={
        "result": _port(
            "result",
            "application/json",
            "Current coding-agent capabilities and state.",
            typeinfo=dict,
        )
    },
    output_to_json_field={"result": "$"},
)

DISCOVER_ACTIONS_SCHEMA = a11.ActionSchema(
    name="discover_actions",
    description=(
        "Get exact input and output port names, types, and stream shapes for"
        " selected actions. Use it only for actions planned in the next Flow"
        " whose concise descriptions lack needed port information."
    ),
    inputs={
        "names": _port(
            "names",
            "application/json",
            "Planned action names whose ports are unclear (maximum 12).",
            typeinfo=list,
            required=True,
            json_schema={
                "type": "array",
                "items": {"type": "string"},
                "maxItems": 12,
                "uniqueItems": True,
            },
        )
    },
    outputs={
        "result": _port(
            "result",
            "application/json",
            "Requested action schemas.",
            typeinfo=dict,
        )
    },
    output_to_json_field={"result": "$"},
)

RUN_FLOW_SCHEMA = a11.ActionSchema(
    name="run_flow",
    description=(
        "Compile and run an A11 Flow for parallel or streaming multi-action"
        " work. Prefer it when operations compose or when a deterministic"
        " filter, slice, projection, or bound can keep unneeded action output"
        " out of model context—even for one action. Use Flow to expose only"
        " the smallest output needed instead of requiring a full tool result"
        " in context. Every value the model should receive must be declared"
        " as an `out` port and routed there with `->`; undeclared values stay"
        " internal. Every local coding-action call has the exact form"
        " `step = run action(port: value)`. A flow name has no `()` and a"
        " string stream is `string stream`, never `[string]`. This runner has"
        " no attached peer stream, so its compositions use `run`, not `call`."
        " For workspace research, batch independent reads and searches and"
        " project bounded outputs. For external research, put independent"
        " `web-fetch` calls in the"
        " entry Flow and project bounded parts of their `text`, `json`, or"
        " `items` outputs. Start with relevant workspace evidence when it may"
        " answer the request, then add remote evidence for current facts or"
        " gaps. Common coding outputs are `read_file.lines`,"
        " `list_files.result.files`, `search_text.result.matches`, and"
        " `run_command.output_lines`."
    ),
    inputs={
        "source": _port(
            "source",
            "text/plain",
            "Raw, unfenced Flow source; the first named flow is invoked."
            " When workspace sources may be relevant, start with bounded"
            " `read_file`, `list_files`, or `search_text` calls; add"
            " web-fetch for current external evidence or missing context."
            " Start with `flow NAME {`, not `flow NAME() {`. Declare"
            " `in` and `out` ports before statements. Types include"
            " `string`, `any`, and `list[string]`; append `stream` for a"
            " stream port, as in `out lines: string stream`. Never spell"
            " that `[string]`. Every local call uses exactly"
            " `step = run action(port: value)`. Use `try run` only when"
            " handling `status step`; do not use `read` as a verb. This"
            " runner never has an attached peer stream, so do not use"
            " `call`. Never use bare"
            " `run action port: value`, an unbound action call, invented"
            " stages such as `lines` or `slice`, or `slice A..B`."
            " Documented stages include `where EXPR`, `map EXPR`,"
            " `flatten`, `drop N`, `first N`, `truncate N`, and `collect`."
            " Declare each returned value as a typed `out` port and route"
            " it there with `->`; only declared outputs reach the model."
            " The program may declare sibling flows and compose them with"
            " `run`. Copyable bounded-read example:\n"
            "flow inspect {\n"
            "  in path: string required\n"
            "  out lines: string stream\n"
            "  file = run read_file(path: path)\n"
            "  file.lines | drop 3 | first 9 | truncate 400 -> lines\n"
            "}\n"
            " For matching file lines use `file.lines | where"
            ' contains(lower(it), "needle") | first 30 | truncate 500`.'
            " Never return whole text or an unbounded line stream when a"
            " bounded selection answers the question. `first N` bounds"
            " value count; `truncate N` bounds each value. Put every"
            " independent action call in the first flow so they run"
            " concurrently. Later sibling declarations are definitions"
            " and do nothing unless that entry flow runs them. Use `after"
            " step`, `wait"
            " step timeout 30s`, or `cancel step` for control. Pipe only"
            " needed outputs so intermediate values stay out of model"
            " context. Do not include Markdown backticks. Use absolute"
            " workspace paths for filesystem actions. A bounded whole-text"
            " pattern is `page = run web-fetch(url: url)` then"
            ' `page.text | map split(it, "\\n") | flatten | drop 29 |'
            " first 21 -> lines` for lines 30–50.",
            required=True,
        ),
        "inputs": _port(
            "inputs",
            "application/json",
            "Values for declared entry-flow `in` ports, keyed by port"
            " name; omit when the entry flow has no inputs.",
            typeinfo=dict,
            json_schema={"type": "object", "additionalProperties": True},
        ),
        "effectful": _port(
            "effectful",
            "application/json",
            "Optional effect hint. The runner infers commands and writes"
            " from the compiled graph and enforces policy either way.",
            typeinfo=bool,
        ),
        "timeout_seconds": _port(
            "timeout_seconds",
            "application/json",
            "Whole-flow deadline from 1 to 600 seconds; default 120.",
            typeinfo=int,
            json_schema={"type": "integer", "minimum": 1, "maximum": 600},
        ),
    },
    outputs={
        "result": _port(
            "result",
            "application/json",
            "Collected entry-flow outputs.",
            typeinfo=dict,
        )
    },
    output_to_json_field={"result": "$"},
)

FLOW_GUIDE_SCHEMA = a11.ActionSchema(
    name="flow_guide",
    description=(
        "Get one compact A11 Flow guide section for advanced syntax or error"
        " recovery. Ordinary Flows use the run_flow template and compiler"
        " diagnostics without this action. Request one topic after two failed"
        " attempts or when the needed construct is absent from that template."
        " Topics: quickstart—structure and a complete"
        " example; programs—multiple flows and sibling composition;"
        " permissions—current mode and effectful-action availability;"
        " slicing—line/value selection; concurrency—parallelism and ordering;"
        " control—failure, deadline, cancellation, and loops."
    ),
    inputs={
        "topic": _port(
            "topic",
            "text/plain",
            "Section to return: quickstart for structure and an example;"
            " programs for multiple flows and sibling composition;"
            " permissions for current-mode action availability; slicing"
            " for line/value selection; concurrency for parallel execution"
            " and ordering; control for failures, deadlines, cancellation,"
            " and loops.",
            json_schema={
                "type": "string",
                "enum": [
                    "quickstart",
                    "programs",
                    "permissions",
                    "slicing",
                    "concurrency",
                    "control",
                ],
            },
        )
    },
    outputs={
        "result": _port(
            "result",
            "text/plain",
            "A concise compiler-valid Flow guide and example.",
        )
    },
)

TOOL_SCHEMAS = (
    WORKSPACE_INFO_SCHEMA,
    LIST_FILES_SCHEMA,
    SEARCH_TEXT_SCHEMA,
    APPLY_PATCH_SCHEMA,
    FILE_DIFF_SCHEMA,
    RUN_COMMAND_SCHEMA,
    DISCOVER_ACTIONS_SCHEMA,
    FLOW_GUIDE_SCHEMA,
    RUN_FLOW_SCHEMA,
    REPORT_COMPLETION_SCHEMA,
    REQUEST_USER_INPUT_SCHEMA,
)


@dataclass
class PendingUserInput:
    question: str
    options: list[dict[str, str]]
    allow_free_text: bool
    answer: asyncio.Future[dict[str, object]]

    def describe(self, request_id: str) -> dict[str, object]:
        return {
            "request_id": request_id,
            "question": self.question,
            "options": self.options,
            "allow_free_text": self.allow_free_text,
        }


@dataclass
class CodingContext:
    """State shared by the coding actions installed for one chat session."""

    workspace: Workspace
    policy: Policy
    registry: a11.ActionRegistry
    sandbox_mode: SandboxMode
    completion: dict[str, object] | None = None
    action_log: list[dict[str, object]] = field(default_factory=list)
    system_prompt: str = ""
    model_tool_names: list[str] = field(default_factory=list)
    pending_user_inputs: dict[str, PendingUserInput] = field(
        default_factory=dict
    )


async def _optional(action: a11.Action, name: str, value_type: type, default):
    value = await action[name].consume(value_type, allow_none=True)
    return default if value is None else value


def _bounded(value: int, low: int, high: int, name: str) -> int:
    if value < low or value > high:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"{name} must be between {low} and {high}.",
        ).to_exception()
    return value


async def _native_lines(
    context: CodingContext,
    program: str,
    arguments: list[str],
    cwd: pathlib.Path,
    *,
    stdin_data: bytes | None = None,
    environment: dict[str, str] | None = None,
    timeout: int = 120,
    line_limit: int | None = None,
) -> tuple[list[str], dict[str, object]]:
    """Run a fixed tool command in the native sandbox and collect its lines."""
    output = a11.AsyncNode(
        a11.LocalChunkStore(f"coding-tool-{uuid.uuid4().hex}")
    )

    async def collect() -> list[str]:
        return [str(line) async for line in output]

    reader = asyncio.create_task(collect())
    try:
        result = await run_native_process(
            context.registry,
            program=program,
            arguments=arguments,
            cwd=cwd,
            timeout_seconds=timeout,
            output=output,
            stdin_data=stdin_data,
            environment=environment,
            max_output_lines=line_limit,
        )
    finally:
        await output.close()
    return await reader, result


async def workspace_info(action: a11.Action, context: CodingContext) -> None:
    await action["result"].finalize(
        context.workspace.summary()
        | {
            "approval_mode": context.policy.mode.value,
            "sandbox": context.sandbox_mode.value,
            "network": True,
        }
    )


def _program_is_missing(
    program: str, lines: list[str], result: dict[str, object]
) -> bool:
    """Recognise native process-launch failures eligible for a fallback."""
    detail = "\n".join(lines).casefold()
    return result.get("exit_code") == 127 or (
        program.casefold() in detail
        and any(
            marker in detail
            for marker in (
                "no such file or directory",
                "command not found",
                "not found",
            )
        )
    )


async def list_files(action: a11.Action, context: CodingContext) -> None:
    raw_path = await _optional(action, "path", str, ".")
    limit = _bounded(
        await _optional(action, "limit", int, 200), 1, MAX_FILES, "limit"
    )
    path = context.workspace.resolve(raw_path, directory=True)
    lines, result = await _native_lines(
        context,
        "rg",
        ["--files", str(path)],
        context.workspace.cwd,
        line_limit=limit + 1,
    )
    if _program_is_missing("rg", lines, result):
        try:
            relative = path.relative_to(context.workspace.root)
        except ValueError:
            relative = None
        if relative is not None:
            pathspec = str(relative) if relative.parts else "."
            lines, result = await _native_lines(
                context,
                "git",
                [
                    "-C",
                    str(context.workspace.root),
                    "ls-files",
                    "--cached",
                    "--others",
                    "--exclude-standard",
                    "--",
                    pathspec,
                ],
                context.workspace.cwd,
                line_limit=limit + 1,
            )
            git_succeeded = result["exit_code"] in {0, 1} and not any(
                line.startswith("stderr: ") for line in lines
            )
            if git_succeeded:
                lines = [str(context.workspace.root / line) for line in lines]
        else:
            git_succeeded = False
        if not git_succeeded:
            lines, result = await _native_lines(
                context,
                "find",
                [
                    str(path),
                    "-type",
                    "f",
                    "-not",
                    "-path",
                    "*/.git/*",
                ],
                context.workspace.cwd,
                line_limit=limit + 1,
            )
    if result["exit_code"] not in {0, 1} and not result.get("stopped_early"):
        raise Status(
            code=StatusCode.INTERNAL,
            message=f"File listing failed: {'; '.join(lines)}",
        ).to_exception()
    paths = [
        context.workspace.relative(pathlib.Path(line).resolve())
        for line in lines
    ]
    await action["result"].finalize({
        "files": paths[:limit],
        "truncated": len(paths) > limit,
    })


async def search_text(action: a11.Action, context: CodingContext) -> None:
    query = await action["query"].consume(str)
    raw_path = await _optional(action, "path", str, ".")
    regex = await _optional(action, "regex", bool, False)
    limit = _bounded(
        await _optional(action, "limit", int, 50), 1, MAX_MATCHES, "limit"
    )
    if not query:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="The search query cannot be empty.",
        ).to_exception()
    path = context.workspace.resolve(raw_path)
    command = ["rg", "-n", "--no-heading", "--color", "never"]
    if not regex:
        command.append("-F")
    command.extend([query, str(path)])
    lines, result = await _native_lines(
        context,
        command[0],
        command[1:],
        context.workspace.cwd,
        line_limit=limit + 1,
    )
    if _program_is_missing("rg", lines, result):
        grep_arguments = ["-R", "-n", "-I"]
        grep_arguments.append("-E" if regex else "-F")
        grep_arguments.extend(["--", query, str(path)])
        lines, result = await _native_lines(
            context,
            "grep",
            grep_arguments,
            context.workspace.cwd,
            line_limit=limit + 1,
        )
    if result["exit_code"] not in {0, 1} and not result.get("stopped_early"):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Search failed: {'; '.join(lines)}",
        ).to_exception()
    await action["result"].finalize({
        "matches": lines[:limit],
        "truncated": len(lines) > limit,
    })


def _patch_paths(patch: str) -> list[str]:
    paths: set[str] = set()
    for line in patch.splitlines():
        if not line.startswith(("--- ", "+++ ")):
            continue
        raw = line[4:].split("\t", 1)[0]
        if raw == "/dev/null":
            continue
        try:
            value = shlex.split(raw)[0]
        except (ValueError, IndexError) as error:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Invalid patch path: {raw}",
            ).to_exception() from error
        if value.startswith(("a/", "b/")):
            value = value[2:]
        if (
            not value
            or pathlib.PurePath(value).is_absolute()
            or ".." in pathlib.PurePath(value).parts
        ):
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message=f"Patch path must be workspace-relative: {value}",
            ).to_exception()
        paths.add(value)
    if not paths:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="The unified patch contains no file paths.",
        ).to_exception()
    return sorted(paths)


async def apply_patch(action: a11.Action, context: CodingContext) -> None:
    patch = await action["patch"].consume(str)
    paths = _patch_paths(patch)
    resolved = [context.workspace.resolve(path, write=True) for path in paths]
    await context.policy.authorize("workspace patch", ", ".join(paths))
    for path in resolved:
        context.workspace.resolve(path, write=True)

    environment = {"TMPDIR": str(context.workspace.root)}
    arguments = ["-p1", "--forward", "--batch"]
    lines, result = await _native_lines(
        context,
        "/usr/bin/patch",
        [*arguments, "--dry-run"],
        context.workspace.root,
        stdin_data=patch.encode(),
        environment=environment,
    )
    if result["exit_code"]:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message=f"Patch preimage check failed: {'; '.join(lines)}",
        ).to_exception()
    lines, result = await _native_lines(
        context,
        "/usr/bin/patch",
        arguments,
        context.workspace.root,
        stdin_data=patch.encode(),
        environment=environment,
    )
    if result["exit_code"]:
        raise Status(
            code=StatusCode.ABORTED,
            message=f"Patch application failed: {'; '.join(lines)}",
        ).to_exception()
    context.workspace.changed_files.update(paths)
    context.action_log.append({"action": "apply_patch", "paths": paths})
    await action.log(f"Applied a patch to {', '.join(paths)}.")
    await action["result"].finalize({"changed_files": paths})


async def file_diff(action: a11.Action, context: CodingContext) -> None:
    if context.workspace.repo_root is None:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="file_diff requires a Git workspace.",
        ).to_exception()
    raw_path = await _optional(action, "path", str, "")
    staged = await _optional(action, "staged", bool, False)
    command = ["git", "-C", str(context.workspace.repo_root), "diff"]
    if staged:
        command.append("--cached")
    command.append("--")
    if raw_path:
        path = context.workspace.resolve(raw_path)
        command.append(str(path.relative_to(context.workspace.repo_root)))
    elif context.workspace.changed_files:
        command.extend(sorted(context.workspace.changed_files))
    lines, result = await _native_lines(
        context,
        command[0],
        command[1:],
        context.workspace.repo_root,
    )
    if result["exit_code"]:
        raise Status(
            code=StatusCode.INTERNAL,
            message=f"Git diff failed: {'; '.join(lines)}",
        ).to_exception()
    text = "\n".join(lines)
    await action["result"].finalize({
        "diff": text[:MAX_TOOL_TEXT],
        "truncated": len(text) > MAX_TOOL_TEXT,
    })


async def run_command(action: a11.Action, context: CodingContext) -> None:
    command = await action["command"].consume(str)
    if not command.strip():
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="The command cannot be empty.",
        ).to_exception()
    raw_cwd = await _optional(action, "cwd", str, ".")
    timeout = _bounded(
        await _optional(action, "timeout_seconds", int, 120),
        1,
        600,
        "timeout_seconds",
    )
    cwd = context.workspace.resolve(raw_cwd, directory=True)
    await context.policy.authorize(
        "sandboxed command", f"{cwd}: {command.strip()}"
    )
    output = action["output_lines"]
    try:
        result = await run_native_process(
            context.registry,
            program="bash",
            arguments=["--noprofile", "--norc", "-c", command],
            cwd=cwd,
            timeout_seconds=timeout,
            output=output,
            on_line=action.log,
        )
    finally:
        await output.close()
    result["cwd"] = context.workspace.relative(cwd)
    context.workspace.checks.append({"command": command, **result})
    context.action_log.append({
        "action": "run_command",
        "command": command,
        **result,
    })
    await action.log(
        f"Command exited with {result['exit_code']} after"
        f" {result['output_lines']} output lines."
    )
    await action["result"].finalize(result)


async def report_completion(action: a11.Action, context: CodingContext) -> None:
    summary = await action["summary"].consume(str)
    checks = await _optional(action, "checks", str, "")
    remaining = await _optional(action, "remaining", str, "")
    if not summary.strip():
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="A completion summary is required.",
        ).to_exception()
    context.completion = {
        "summary": summary.strip(),
        "changed_files": sorted(context.workspace.changed_files),
        "checks": checks.strip(),
        "recorded_checks": list(context.workspace.checks),
        "remaining": remaining.strip(),
    }
    context.action_log.append({
        "action": "report_completion",
        **context.completion,
    })
    report = summary.strip()
    if checks.strip():
        report += f"\nChecks: {checks.strip()}"
    if remaining.strip():
        report += f"\nRemaining: {remaining.strip()}"
    await action.log(report)
    await action["result"].finalize(context.completion)


async def request_user_input(
    action: a11.Action, context: CodingContext
) -> None:
    """Suspend one model tool call until an A11 client answers it."""
    question = (await action["question"].consume(str)).strip()
    raw_options = await _optional(action, "options", list, [])
    allow_free_text = await _optional(action, "allow_free_text", bool, True)
    if not question:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="question must not be empty.",
        ).to_exception()
    options: list[dict[str, str]] = []
    for raw in raw_options:
        if not isinstance(raw, dict) or not str(raw.get("label", "")).strip():
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="Every option requires a non-empty label.",
            ).to_exception()
        options.append({
            "label": str(raw["label"]).strip(),
            "description": str(raw.get("description", "")).strip(),
        })
    if raw_options and not 2 <= len(options) <= 8:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="options must contain between 2 and 8 choices.",
        ).to_exception()
    if len({option["label"] for option in options}) != len(options):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Option labels must be distinct.",
        ).to_exception()
    if not options:
        allow_free_text = True

    request_id = action.id
    pending = PendingUserInput(
        question=question,
        options=options,
        allow_free_text=allow_free_text,
        answer=asyncio.get_running_loop().create_future(),
    )
    context.pending_user_inputs[request_id] = pending
    await action.log("Waiting for user input.")
    try:
        result = await pending.answer
    finally:
        context.pending_user_inputs.pop(request_id, None)
    await action["result"].finalize(result)


async def respond_user_input(
    action: a11.Action, context: CodingContext
) -> None:
    """Resolve a request which is waiting inside another action call."""
    request_id = (await action["request_id"].consume(str)).strip()
    answer = (await action["answer"].consume(str)).strip()
    pending = context.pending_user_inputs.get(request_id)
    if pending is None:
        raise Status(
            code=StatusCode.NOT_FOUND,
            message=f"No pending user-input request {request_id!r}.",
        ).to_exception()
    if not answer:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="answer must not be empty.",
        ).to_exception()
    labels = [option["label"] for option in pending.options]
    selected = next(
        (label for label in labels if label.casefold() == answer.casefold()),
        None,
    )
    if selected is None and answer.isdigit():
        index = int(answer) - 1
        if 0 <= index < len(labels):
            selected = labels[index]
    if selected is None and labels and not pending.allow_free_text:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=f"Choose one of: {', '.join(labels)}.",
        ).to_exception()
    result: dict[str, object] = {
        "answer": selected or answer,
        "selected_option": selected,
    }
    if pending.answer.done():
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="This user-input request has already been answered.",
        ).to_exception()
    pending.answer.set_result(result)
    await action["result"].finalize({"request_id": request_id} | result)


async def pending_user_inputs(
    action: a11.Action, context: CodingContext
) -> None:
    await action["result"].finalize({
        "requests": [
            pending.describe(request_id)
            for request_id, pending in context.pending_user_inputs.items()
        ]
    })


async def configure_coding_agent(
    action: a11.Action, context: CodingContext
) -> None:
    """Apply a user-selected approval mode without widening the sandbox."""
    raw_mode = await action["approval_mode"].consume(str)
    try:
        mode = ApprovalMode(raw_mode)
    except ValueError as error:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="approval_mode must be 'suggest' or 'auto'.",
        ).to_exception() from error
    if mode == ApprovalMode.ASK:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="Studio permission mode must be 'suggest' or 'auto'.",
        ).to_exception()
    context.policy.mode = mode
    from a11.cli.coding_agent.prompts import system_prompt

    context.system_prompt = system_prompt(context.workspace, mode)
    context.action_log.append({
        "action": "configure_coding_agent",
        "approval_mode": mode.value,
    })
    result = {
        "approval_mode": mode.value,
        "sandbox_ceiling": context.sandbox_mode.value,
        "network": True,
    }
    await action.log(
        f"Coding permission is now {mode.value}; the sandbox remains"
        f" {context.sandbox_mode.value}."
    )
    await action["result"].finalize(result)


async def coding_agent_info(action: a11.Action, context: CodingContext) -> None:
    """Describe the gateway-owned agent to terminal and Studio clients."""
    await action["result"].finalize(
        context.workspace.summary()
        | {
            "approval_mode": context.policy.mode.value,
            "sandbox": context.sandbox_mode.value,
            "network": True,
            "system_prompt": context.system_prompt,
            "tool_names": list(context.model_tool_names),
            "completion": context.completion,
            "pending_user_inputs": [
                pending.describe(request_id)
                for request_id, pending in context.pending_user_inputs.items()
            ],
        }
    )


async def discover_actions(action: a11.Action, context: CodingContext) -> None:
    from a11.actions import describe

    names = await action["names"].consume(list)
    if len(names) > 12 or any(not isinstance(name, str) for name in names):
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message="names must contain at most 12 action names.",
        ).to_exception()
    schemas = {}
    for name in names:
        if not context.registry.is_registered(name):
            raise Status(
                code=StatusCode.NOT_FOUND,
                message=f"No action named {name!r} is available.",
            ).to_exception()
        schemas[name] = describe.schema_to_json(
            context.registry.get_schema(name), runnable=True
        )
    await action["result"].finalize({"actions": schemas})


async def run_flow(action: a11.Action, context: CodingContext) -> None:
    from a11 import flow

    source = await action["source"].consume(str)
    stripped = source.strip()
    if stripped.startswith("```") and stripped.endswith("```"):
        marker, separator, body = stripped.partition("\n")
        if separator and marker in {"```", "```a11flow", "```a11-flow"}:
            source = body[:-3].rstrip()
    inputs = await _optional(action, "inputs", dict, {})
    effectful = await _optional(action, "effectful", bool, False)
    timeout = _bounded(
        await _optional(action, "timeout_seconds", int, 120),
        1,
        600,
        "timeout_seconds",
    )
    try:
        program = flow.loads(source, "coding-agent.flow")
        if not program.names:
            raise Status(
                code=StatusCode.INVALID_ARGUMENT,
                message="run_flow requires at least one named flow.",
            ).to_exception()
        nested_actions = sorted(
            set().union(
                *(
                    _flow_action_names(program[name].describe())
                    for name in program.names
                )
            )
            - set(program.names)
        )
        effect_actions = {
            "apply_patch",
            "copy_path",
            "make_directory",
            "make_temp",
            "move_path",
            "remove_path",
            "run_command",
            "spawn_process",
            "write_file",
        }
        inferred_effects = effect_actions.intersection(nested_actions)
        effectful = effectful or bool(inferred_effects)
        if effectful:
            await context.policy.authorize(
                "A11 Flow", "Run an effectful, kernel-sandboxed Flow program."
            )

        registry = a11.ActionRegistry()
        register_native_actions(
            registry,
            context.workspace.write_roots,
            cwd=context.workspace.cwd,
            mode=(context.sandbox_mode if effectful else SandboxMode.READ_ONLY),
            allow_run=effectful,
        )
        allowed = {
            "workspace_info",
            "list_files",
            "search_text",
            "file_diff",
            "web-fetch",
        }
        if effectful:
            allowed.update({"apply_patch", "run_command"})
        for name in allowed:
            registry.register(
                name,
                context.registry.get_schema(name),
                context.registry.get_handler(name),
            )
        action_summary = ", ".join(nested_actions) or "none"
        await action.log(
            f"Running Flow `{program.main.name}`\n"
            f"  Actions in composition: {action_summary}\n"
        )
        result = await asyncio.wait_for(
            program.main.invoke(inputs, registry=registry), timeout=timeout
        )
    except asyncio.TimeoutError as error:
        raise Status(
            code=StatusCode.DEADLINE_EXCEEDED,
            message=f"Flow exceeded its {timeout}-second deadline.",
        ).to_exception() from error
    encoded = json.dumps(result, default=str)
    if len(encoded) > MAX_TOOL_TEXT:
        raise Status(
            code=StatusCode.RESOURCE_EXHAUSTED,
            message="Flow output exceeded the 128 KiB result limit.",
        ).to_exception()
    context.action_log.append({
        "action": "run_flow",
        "effectful": effectful,
        "outputs": list(result),
    })
    ports = ", ".join(sorted(result)) or "no outputs"
    await action.log(f"Flow `{program.main.name}` completed → {ports}\n")
    await action["result"].finalize(result)


async def flow_guide(action: a11.Action, context: CodingContext) -> None:
    """Return bounded Flow syntax without spending context on SDK source."""
    topic = await _optional(action, "topic", str, "quickstart")
    mode = context.policy.mode
    if mode == ApprovalMode.SUGGEST:
        permission_notice = (
            "Current permission mode is `suggest`. Effectful actions such as "
            "`run_command`, `spawn_process`, `apply_patch`, and write/move/"
            "remove filesystem actions are unavailable in executable Flows. "
            "Do not reference them in a Flow passed to `run_flow`; use only "
            "read-only actions, or present the proposed Flow without running "
            "it."
        )
    elif mode == ApprovalMode.AUTO:
        permission_notice = (
            "Current permission mode is `auto`. Effectful actions may be used "
            "in Flows within the configured kernel-sandbox boundary."
        )
    else:
        permission_notice = (
            "Current permission mode is `ask`. An effectful Flow requires "
            "user approval and may be denied; read-only Flows do not."
        )
    sections = {
        "quickstart": (
            """A Flow composes registered actions. Every action call
must be bound and parenthesized: `step = run action(port: value)`. This local
runner has no peer stream, so use `run`, not `call`. Bare
`run action port: value` is invalid. A flow name has no parentheses, and a
string stream port is `out lines: string stream`, never `out lines: [string]`.
Connect `step.output` to another input or the Flow output, and return only what
the caller needs. Every unreferenced action output is drained.

```a11flow
flow inspect {
  in path: string required
  out lines: string stream
  file = run read_file(path: path)
  file.lines | drop 3 | first 9 -> lines
}
```

To propose or explain a Flow, return a fenced `a11flow` source block without
calling `run_flow`. For coding work that benefits from composition or bounded
output, execute it proactively as raw source without the backticks."""
        ),
        "programs": (
            """One Flow source is a program and may declare several
named flows. The first declaration is the entry flow invoked by `run_flow`.
Any flow may `run` a sibling declared before or after it; sibling flows need no
registry entry. Factor repeated pipelines into focused sibling flows:

```a11flow
flow main {
  out result: string required
  part = run helper(value: "ok")
  part.result -> result
}

flow helper {
  in value: string required
  out result: string required
  value -> result
}
```

The whole multi-flow program is passed as one raw `run_flow.source`."""
        ),
        "permissions": (
            """Flow can compose only actions available under the
current coding-agent policy. `run_flow` inspects the compiled action graph and
enforces permission before execution; setting `effectful` to false cannot
bypass that boundary. When effects are unavailable, compose read-only actions
or return a fenced `a11flow` proposal for the user instead of executing it."""
        ),
        "slicing": (
            """Flow line streams are zero-copy pipelines. `drop N`
discards the first N values, `first N` keeps N values, and `truncate N` bounds
each text value by characters. Lines 4 through 12 inclusive are:
`file.lines | drop 3 | first 9 -> lines`. There is no generic `lines` or `slice`
stage and no `slice A..B` form. If an action returns one whole text
value, turn it into lines without exposing the whole value to the model:
`page.text | map split(it, "\\n") | flatten | first 20 -> lines`."""
        ),
        "concurrency": (
            """Independent Flow steps start concurrently. Use
`for item in items parallel 4 { ... }` for bounded fan-out. Data dependencies,
`after step`, and `wait all of a, b` establish ordering; do not serialize
independent reads in the model context."""
        ),
        "control": (
            """Use `try run action(...)` when a failure is data the
Flow handles, then inspect `status step`. `wait step timeout 30s` bounds a
step. `cancel step` stops work early. A `repeat` must have `until`, `while`, or
`max N`. Put `fail`, `cancel`, and `log` behind a condition or `after`."""
        ),
    }
    if topic not in sections:
        raise Status(
            code=StatusCode.INVALID_ARGUMENT,
            message=(
                "topic must be quickstart, programs, permissions, slicing,"
                " concurrency, or control."
            ),
        ).to_exception()
    await action["result"].finalize(f"{permission_notice}\n\n{sections[topic]}")


def _flow_action_names(value: object) -> set[str]:
    """Collect action names from a recursive ``flow.plan/v1`` description."""
    if isinstance(value, dict):
        names: set[str] = set()
        if isinstance(value.get("action"), str):
            names.add(value["action"])
        for nested in value.values():
            names.update(_flow_action_names(nested))
        return names
    if isinstance(value, list):
        names: set[str] = set()
        for nested in value:
            names.update(_flow_action_names(nested))
        return names
    return set()


HANDLERS = {
    "workspace_info": workspace_info,
    "list_files": list_files,
    "search_text": search_text,
    "apply_patch": apply_patch,
    "file_diff": file_diff,
    "run_command": run_command,
    "discover_actions": discover_actions,
    "flow_guide": flow_guide,
    "run_flow": run_flow,
    "report_completion": report_completion,
    "request_user_input": request_user_input,
}


def install(
    registry: a11.ActionRegistry,
    context: CodingContext,
    *,
    command_tools: bool = True,
    flow_tools: bool = True,
) -> list[str]:
    """Register all coding actions and return their allowed-action names."""
    schemas = [
        schema
        for schema in TOOL_SCHEMAS
        if (command_tools or schema.name != "run_command")
        and (flow_tools or schema.name not in {"flow_guide", "run_flow"})
    ]
    for schema in schemas:
        handler = HANDLERS[schema.name]

        async def installed(
            action: a11.Action,
            handler=handler,
        ) -> None:
            await handler(action, context)

        registry.register(schema.name, schema, installed)

    async def configure(action: a11.Action) -> None:
        await configure_coding_agent(action, context)

    # Studio can call this directly. It is intentionally absent from the names
    # returned below, so the model cannot grant itself broader permissions.
    registry.register(
        CONFIGURE_CODING_AGENT_SCHEMA.name,
        CONFIGURE_CODING_AGENT_SCHEMA,
        configure,
    )

    async def agent_info(action: a11.Action) -> None:
        await coding_agent_info(action, context)

    registry.register(
        CODING_AGENT_INFO_SCHEMA.name,
        CODING_AGENT_INFO_SCHEMA,
        agent_info,
    )

    async def respond(action: a11.Action) -> None:
        await respond_user_input(action, context)

    registry.register(
        RESPOND_USER_INPUT_SCHEMA.name,
        RESPOND_USER_INPUT_SCHEMA,
        respond,
    )

    async def pending(action: a11.Action) -> None:
        await pending_user_inputs(action, context)

    registry.register(
        PENDING_USER_INPUTS_SCHEMA.name,
        PENDING_USER_INPUTS_SCHEMA,
        pending,
    )
    return [
        "read_file",
        "list_directory",
        "stat_path",
        *(schema.name for schema in schemas),
    ]
