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

"""Coding-agent policy, session, Flow, and native-sandbox boundaries."""

from __future__ import annotations

import asyncio
import datetime
import json
import pathlib
import subprocess
import uuid

import pytest

import a11
from a11 import flow
from a11.cli.coding_agent import ApprovalMode, CodingAgent, SandboxMode
from a11.cli.coding_agent import tools as tools_mod
from a11.cli.coding_agent.sandbox import run_native_process
from a11.cli.coding_agent.session import SessionRecord, SessionStore
from a11.sdk.llm import Interaction, Role
from a11.status import StatusCode, StatusException


async def _invoke(
    registry: a11.ActionRegistry, name: str, inputs: dict[str, object]
) -> object:
    action = (
        a11
        .Action(registry.get_schema(name))
        .bind_registry(registry)
        .bind_handler(registry.get_handler(name))
        .run()
    )
    for port_name in registry.get_schema(name).inputs:
        if port_name in inputs:
            await action[port_name].finalize(inputs[port_name])
        else:
            await action[port_name].finalize()
    result = await action["result"].consume(allow_none=True)
    await action.wait()
    return result


def _agent(root: pathlib.Path, mode: ApprovalMode = ApprovalMode.AUTO):
    registry = a11.ActionRegistry()
    agent = CodingAgent.install(
        registry,
        cwd=str(root),
        add_dirs=(),
        approval_mode=mode,
        sandbox_mode=SandboxMode.WORKSPACE_WRITE,
        approve=None,
    )
    return registry, agent


def test_prompt_names_instruction_files_without_embedding_repository_state(
    tmp_path,
):
    instructions = "UNIQUE REPOSITORY INSTRUCTION BODY"
    (tmp_path / "AGENTS.md").write_text(instructions)
    subprocess.run(["git", "init", "-q"], cwd=tmp_path, check=True)
    (tmp_path / "untracked.txt").write_text("large state marker")

    _, agent = _agent(tmp_path)

    assert "Repository instruction files may apply" in agent.prompt
    assert str(tmp_path) not in agent.prompt
    assert "AGENTS.md" in agent.prompt
    assert instructions not in agent.prompt
    assert "untracked.txt" not in agent.prompt
    assert "workspace_info" in agent.prompt


def test_prompt_includes_the_current_date(tmp_path):
    _, agent = _agent(tmp_path)

    assert f"Current date: {datetime.date.today().isoformat()}." in agent.prompt
    assert "do not present an old" in agent.prompt
    assert "snapshot as current" in agent.prompt
    assert "comparison dimensions the request" in agent.prompt
    assert "stale or tangential" in agent.prompt


def test_structured_action_inputs_have_json_schemas(tmp_path):
    registry, agent = _agent(tmp_path)

    assert registry.is_registered("run_command")
    assert registry.is_registered("web-fetch")
    fetch_options = json.loads(
        registry.get_schema("web-fetch").inputs["options"].json_schema
    )
    assert fetch_options["properties"]["max_body_bytes"]["description"]
    fetch_schema = registry.get_schema("web-fetch")
    assert "independent requests in one run_flow" in fetch_schema.description
    assert "do not use run_command or shell_execute" in fetch_schema.description
    assert "known independent URLs" in fetch_schema.inputs["url"].description
    for name in (
        "shell_start",
        "shell_execute",
        "shell_list",
        "shell_exit",
    ):
        assert not registry.is_registered(name)

    read_options = json.loads(
        registry.get_schema("read_file").inputs["options"].json_schema
    )
    process_arguments = json.loads(
        registry.get_schema("spawn_process").inputs["arguments"].json_schema
    )
    flow_inputs = json.loads(
        registry.get_schema("run_flow").inputs["inputs"].json_schema
    )

    assert "chunk_bytes" in read_options["properties"]
    assert read_options["properties"]["chunk_bytes"]["description"]
    assert read_options["properties"]["offset"]["description"]
    assert (
        registry.get_schema("read_file").inputs["options"].description
        == "Optional read bounds and representation."
    )
    assert "relative paths use the current directory" in (
        registry.get_schema("read_file").inputs["path"].description
    )
    assert process_arguments["oneOf"][1]["items"] == {"type": "string"}
    assert flow_inputs == {"type": "object", "additionalProperties": True}
    flow_schema = registry.get_schema("run_flow")
    assert "even for one action" in flow_schema.description
    assert "step = run action(port: value)" in flow_schema.description
    assert "not `flow NAME() {`" in flow_schema.inputs["source"].description
    assert (
        "out lines: string stream" in flow_schema.inputs["source"].description
    )
    assert (
        "Never spell that `[string]`"
        in flow_schema.inputs["source"].description
    )
    assert "map split" in flow_schema.inputs["source"].description
    assert (
        "where contains(lower(it)" in flow_schema.inputs["source"].description
    )
    assert "sibling declarations are definitions" in (
        flow_schema.inputs["source"].description
    )
    assert (
        "only declared outputs reach the model"
        in flow_schema.inputs["source"].description
    )
    assert "Flow call syntax is not shell syntax" in agent.prompt
    assert "use the available tools to achieve the goal" in agent.prompt
    assert "declare and route only what the next decision needs" in agent.prompt
    assert "Do not load flow_guide for an ordinary Flow" in agent.prompt
    assert "call` requires an attached peer stream" in agent.prompt
    assert "Only the first declared flow is invoked" in agent.prompt
    assert "<repo-root>/.a11/flows/" in agent.prompt
    assert "flow-name.flow" in agent.prompt
    assert "flow-name.md" in agent.prompt
    assert "each input and output" in agent.prompt
    assert "which reusable Flows are available" in agent.prompt
    assert "Filesystem actions accept relative paths" in agent.prompt
    assert "Never return an entire text value" in agent.prompt
    assert "`read action(...)` is invalid" in agent.prompt
    assert "Do not run curl, wget" in agent.prompt
    assert "Put known independent URLs" in agent.prompt
    assert "run_flow so the web-fetch calls run concurrently" in agent.prompt
    assert "Start with the configured workspace" in agent.prompt
    assert "authoritative remote sources after local evidence" in agent.prompt
    assert "Explore relevant workspace sources first" in agent.prompt
    assert "Source selection starts with relevant workspace evidence" in (
        agent.prompt
    )
    assert "Common coding outputs" in flow_schema.description
    assert "`list_files.result.files`" in flow_schema.description
    assert "For workspace research" in flow_schema.description
    assert "start with bounded `read_file`, `list_files`, or `search_text`" in (
        flow_schema.inputs["source"].description
    )
    for name in ("read_file", "list_files", "search_text"):
        assert "Do not use this as the first source" not in (
            registry.get_schema(name).description
        )
    assert "Use web-fetch for HTTP retrieval" in (
        registry.get_schema("run_command").description
    )
    patch_schema = registry.get_schema("apply_patch")
    assert "git-format unified diff" in patch_schema.description
    assert "diff --git" in patch_schema.description
    assert "--- /dev/null" in patch_schema.description
    assert "Never use `*** Begin Patch`" in patch_schema.description
    assert (
        "Do not wrap it in Markdown" in patch_schema.inputs["patch"].description
    )
    assert "Do not use curl, wget" in (
        registry.get_schema("run_command").inputs["command"].description
    )
    assert "A11 Flow — a composition" not in agent.prompt
    assert "struct NAME" not in agent.prompt
    assert "An AGENTS.md applies to its directory" in agent.prompt
    assert "brief progress updates" in agent.prompt
    assert "ambitious but relevant initiative" in agent.prompt
    for action_name in registry.list_registered_actions():
        action_schema = registry.get_schema(action_name)
        for port in action_schema.inputs.values():
            if port.type == "application/json":
                assert port.json_schema, (
                    f"{action_name}.{port.name} has no JSON Schema"
                )
                assert isinstance(json.loads(port.json_schema), dict)

    random_options = json.loads(
        registry.get_schema("random_bytes").inputs["options"].json_schema
    )
    assert random_options["properties"]["count"]["maximum"] == 1024 * 1024
    assert random_options["properties"]["format"]["enum"] == [
        "hex",
        "base64",
        "base64url",
        "raw",
    ]


@pytest.mark.asyncio
async def test_studio_can_configure_gateway_agent_permissions(tmp_path):
    registry, agent = _agent(tmp_path, ApprovalMode.SUGGEST)

    info = await _invoke(registry, "coding_agent_info", {})
    configured = await _invoke(
        registry,
        "configure_coding_agent",
        {"approval_mode": "auto"},
    )

    assert info["approval_mode"] == "suggest"
    assert "run_command" in info["tool_names"]
    assert "web-fetch" in info["tool_names"]
    assert "configure_coding_agent" not in info["tool_names"]
    assert configured == {
        "approval_mode": "auto",
        "sandbox_ceiling": "workspace-write",
        "network": True,
    }
    assert agent.context.policy.mode == ApprovalMode.AUTO
    schema = registry.get_schema("configure_coding_agent")
    assert json.loads(schema.inputs["approval_mode"].json_schema) == {
        "type": "string",
        "enum": ["suggest", "auto"],
    }


@pytest.mark.asyncio
async def test_user_input_request_can_be_answered_by_any_a11_client(tmp_path):
    registry, agent = _agent(tmp_path)
    request = asyncio.create_task(
        _invoke(
            registry,
            "request_user_input",
            {
                "question": "Which API?",
                "options": [
                    {"label": "Public", "description": "Stable surface."},
                    {"label": "Internal", "description": "More control."},
                ],
                "allow_free_text": False,
            },
        )
    )
    for _ in range(20):
        if agent.context.pending_user_inputs:
            break
        await asyncio.sleep(0)
    request_id = next(iter(agent.context.pending_user_inputs))

    pending = await _invoke(registry, "pending_user_inputs", {})
    assert pending["requests"][0]["request_id"] == request_id
    assert pending["requests"][0]["options"][0]["label"] == "Public"

    accepted = await _invoke(
        registry,
        "respond_user_input",
        {"request_id": request_id, "answer": "public"},
    )
    assert accepted["selected_option"] == "Public"
    assert await request == {
        "answer": "Public",
        "selected_option": "Public",
    }
    assert agent.context.pending_user_inputs == {}
    assert "respond_user_input" not in agent.tool_names
    assert "pending_user_inputs" not in agent.tool_names


@pytest.mark.asyncio
async def test_native_process_is_confined_and_reports_the_kernel_policy(
    tmp_path,
):
    registry, _ = _agent(tmp_path)
    outside = tmp_path.parent / f"outside-{uuid.uuid4().hex}"
    output = a11.AsyncNode(
        a11.LocalChunkStore(f"coding-sandbox-{uuid.uuid4().hex}")
    )
    lines = []

    async def read_output() -> None:
        async for line in output:
            lines.append(line)

    reader = asyncio.create_task(read_output())
    result = await run_native_process(
        registry,
        program="bash",
        arguments=["--noprofile", "--norc", "-c", "printf 'ok\\n'"],
        cwd=tmp_path.resolve(),
        timeout_seconds=5,
        output=output,
    )
    await output.close()
    await reader

    assert result["exit_code"] == 0
    assert lines == ["ok"]
    assert "network allowed" in str(result["sandbox"]) or (
        "network not confined" in str(result["sandbox"])
    )
    assert "seatbelt" in str(result["sandbox"]) or "landlock" in str(
        result["sandbox"]
    )

    denied_output = a11.AsyncNode(
        a11.LocalChunkStore(f"coding-denied-{uuid.uuid4().hex}")
    )

    async def drain_denial() -> None:
        async for _ in denied_output:
            pass

    denial_reader = asyncio.create_task(drain_denial())
    denied = await run_native_process(
        registry,
        program="bash",
        arguments=[
            "--noprofile",
            "--norc",
            "-c",
            f"printf nope > {outside}",
        ],
        cwd=tmp_path.resolve(),
        timeout_seconds=5,
        output=denied_output,
    )
    await denied_output.close()
    await denial_reader
    assert denied["exit_code"] != 0
    assert not outside.exists()

    temporary = pathlib.Path("/tmp") / f"a11-agent-{uuid.uuid4().hex}"
    temporary_output = a11.AsyncNode(
        a11.LocalChunkStore(f"coding-temporary-{uuid.uuid4().hex}")
    )

    async def drain_temporary() -> None:
        async for _ in temporary_output:
            pass

    temporary_reader = asyncio.create_task(drain_temporary())
    try:
        temporary_result = await run_native_process(
            registry,
            program="bash",
            arguments=[
                "--noprofile",
                "--norc",
                "-c",
                f"printf temporary > {temporary}",
            ],
            cwd=tmp_path.resolve(),
            timeout_seconds=5,
            output=temporary_output,
        )
        await temporary_output.close()
        await temporary_reader
        assert temporary_result["exit_code"] == 0
        assert temporary.read_text(encoding="utf-8") == "temporary"
    finally:
        temporary.unlink(missing_ok=True)


@pytest.mark.asyncio
async def test_native_process_stops_after_the_requested_output_lines(tmp_path):
    registry, _ = _agent(tmp_path)
    output = a11.AsyncNode(
        a11.LocalChunkStore(f"coding-bounded-{uuid.uuid4().hex}")
    )

    async def collect() -> list[str]:
        return [str(line) async for line in output]

    reader = asyncio.create_task(collect())
    try:
        result = await run_native_process(
            registry,
            program="bash",
            arguments=[
                "--noprofile",
                "--norc",
                "-c",
                "while true; do printf 'line\\n'; done",
            ],
            cwd=tmp_path.resolve(),
            timeout_seconds=5,
            output=output,
            max_output_lines=6,
        )
    finally:
        await output.close()

    assert await reader == ["line"] * 6
    assert result["stopped_early"] is True


@pytest.mark.asyncio
async def test_native_process_refuses_sensitive_home_data(
    tmp_path, monkeypatch
):
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    home = tmp_path / "home"
    secret = home / ".ssh" / "id_test"
    secret.parent.mkdir(parents=True)
    secret.write_text("do-not-read", encoding="utf-8")
    monkeypatch.setenv("HOME", str(home))
    registry, _ = _agent(workspace)
    output = a11.AsyncNode(
        a11.LocalChunkStore(f"coding-sensitive-{uuid.uuid4().hex}")
    )
    lines = []

    async def read_output() -> None:
        async for line in output:
            lines.append(str(line))

    reader = asyncio.create_task(read_output())
    result = await run_native_process(
        registry,
        program="bash",
        arguments=["--noprofile", "--norc", "-c", f"cat {secret}"],
        cwd=workspace,
        timeout_seconds=5,
        output=output,
    )
    await output.close()
    await reader

    assert result["exit_code"] != 0
    assert "do-not-read" not in "\n".join(lines)


@pytest.mark.asyncio
async def test_file_diff_can_read_the_platform_git_toolchain(tmp_path):
    repository = tmp_path / "repository"
    repository.mkdir()
    subprocess.run(
        ["git", "init", "--quiet", str(repository)],
        check=True,
        capture_output=True,
        text=True,
    )
    target = repository / "tracked.txt"
    target.write_text("sandboxed diff\n", encoding="utf-8")
    subprocess.run(
        ["git", "-C", str(repository), "add", "tracked.txt"],
        check=True,
        capture_output=True,
        text=True,
    )
    registry, _ = _agent(repository)

    result = await _invoke(registry, "file_diff", {"staged": True})

    assert "sandboxed diff" in result["diff"]
    assert result["truncated"] is False


@pytest.mark.asyncio
async def test_flow_composes_native_file_actions_with_relative_paths(tmp_path):
    working = tmp_path / "working"
    working.mkdir()
    source = working / "answer.txt"
    source.write_text("forty-two", encoding="utf-8")
    registry, _ = _agent(working)

    result = await _invoke(
        registry,
        "run_flow",
        {
            "source": (
                """
                flow inspect {
                  in path: string required
                  out result: string required
                  got = run read_file(path: path)
                  got.text -> result
                }
            """
            ),
            "inputs": {"path": "answer.txt"},
            "effectful": False,
            "timeout_seconds": 5,
        },
    )

    assert result == {"result": "forty-two"}


@pytest.mark.asyncio
async def test_flow_action_defensively_accepts_a_display_fence(tmp_path):
    registry, _ = _agent(tmp_path)

    result = await _invoke(
        registry,
        "run_flow",
        {
            "source": (
                """```a11flow
flow answer {
  out value: string required
  "ok" -> value
}
```"""
            ),
            "effectful": False,
        },
    )

    assert result == {"value": "ok"}


@pytest.mark.asyncio
async def test_flow_program_composes_sibling_flows(tmp_path):
    registry, _ = _agent(tmp_path)

    result = await _invoke(
        registry,
        "run_flow",
        {
            "source": (
                """
                flow main {
                  out result: string required
                  part = run helper(value: "composed")
                  part.result -> result
                }

                flow helper {
                  in value: string required
                  out result: string required
                  value -> result
                }
            """
            ),
            "effectful": False,
        },
    )

    assert result == {"result": "composed"}


@pytest.mark.asyncio
async def test_flow_infers_effectful_run_command_and_registers_it(tmp_path):
    registry, _ = _agent(tmp_path)

    result = await _invoke(
        registry,
        "run_flow",
        {
            "source": (
                """
                flow command {
                  out result: any required
                  command = run run_command(command: "printf flow-ok")
                  command.result -> result
                }
            """
            ),
            # Models can omit or misclassify this hint. The action graph is
            # authoritative for policy and sandbox selection.
            "effectful": False,
            "timeout_seconds": 10,
        },
    )

    assert result["result"]["exit_code"] == 0


@pytest.mark.asyncio
async def test_file_tools_fall_back_when_ripgrep_is_unavailable(
    tmp_path, monkeypatch
):
    calls: list[str] = []

    async def native_lines(context, program, arguments, cwd, **kwargs):
        del context, cwd, kwargs
        calls.append(program)
        if program == "rg":
            return ["execvp() of 'rg' failed: No such file or directory"], {
                "exit_code": 127
            }
        if program == "git":
            return ["src/example.py"], {"exit_code": 0}
        assert program == "grep"
        assert arguments[:4] == ["-R", "-n", "-I", "-F"]
        return [f"{tmp_path}/src/example.py:1:needle"], {"exit_code": 0}

    monkeypatch.setattr(tools_mod, "_native_lines", native_lines)
    registry, _ = _agent(tmp_path)

    listed = await _invoke(registry, "list_files", {"path": "."})
    matches = await _invoke(
        registry,
        "search_text",
        {"query": "needle", "path": ".", "regex": False},
    )

    assert listed["files"] == ["src/example.py"]
    assert matches["matches"] == [f"{tmp_path}/src/example.py:1:needle"]
    assert calls == ["rg", "git", "rg", "grep"]


@pytest.mark.asyncio
async def test_flow_guide_supports_progressive_syntax_discovery(tmp_path):
    registry, _ = _agent(tmp_path)

    guide = await _invoke(registry, "flow_guide", {"topic": "slicing"})

    assert "file.lines | drop 3 | first 9 -> lines" in guide
    assert "There is no generic `lines` or `slice`" in guide
    assert "stage and no `slice A..B` form" in guide
    quickstart = await _invoke(registry, "flow_guide", {"topic": "quickstart"})
    assert "step = run action(port: value)" in quickstart
    assert "`run action port: value` is invalid" in quickstart
    schema = registry.get_schema("flow_guide")
    topic = json.loads(schema.inputs["topic"].json_schema)
    assert topic["enum"] == [
        "quickstart",
        "programs",
        "permissions",
        "slicing",
        "concurrency",
        "control",
    ]
    for choice in topic["enum"]:
        assert choice in schema.description

    programs = await _invoke(registry, "flow_guide", {"topic": "programs"})
    assert "first declaration is the entry flow" in programs
    assert "part = run helper" in programs
    source = programs.split("```a11flow\n", 1)[1].split("\n```", 1)[0]
    program = flow.loads(source, "guide.flow")
    assert program.main.name == "main"
    assert set(program.flows) == {"main", "helper"}

    suggest_registry, _ = _agent(tmp_path, ApprovalMode.SUGGEST)
    permissions = await _invoke(
        suggest_registry, "flow_guide", {"topic": "permissions"}
    )
    assert "Current permission mode is `suggest`" in permissions
    assert "`run_command`" in permissions
    assert "Do not reference them" in permissions


@pytest.mark.asyncio
async def test_suggest_mode_refuses_a_patch_before_writing(tmp_path):
    target = tmp_path / "original.txt"
    target.write_text("old\n", encoding="utf-8")
    registry, _ = _agent(tmp_path, ApprovalMode.SUGGEST)

    with pytest.raises(StatusException) as raised:
        await _invoke(
            registry,
            "apply_patch",
            {
                "patch": (
                    """--- a/original.txt
+++ b/original.txt
@@ -1 +1 @@
-old
+new
"""
                )
            },
        )

    assert raised.value.status.code == StatusCode.PERMISSION_DENIED
    assert target.read_text(encoding="utf-8") == "old\n"


@pytest.mark.asyncio
async def test_auto_mode_applies_a_checked_patch_inside_the_sandbox(tmp_path):
    target = tmp_path / "original.txt"
    target.write_text("old\n", encoding="utf-8")
    registry, agent = _agent(tmp_path)

    result = await _invoke(
        registry,
        "apply_patch",
        {
            "patch": (
                """--- a/original.txt
+++ b/original.txt
@@ -1 +1 @@
-old
+new
"""
            )
        },
    )

    assert result == {"changed_files": ["original.txt"]}
    assert target.read_text(encoding="utf-8") == "new\n"
    assert agent.context.workspace.changed_files == {"original.txt"}


def test_session_round_trip_is_private_and_preserves_interactions(tmp_path):
    interaction = Interaction(
        role=Role.USER,
        content=[a11.to_chunk("continue the task")],
    )
    store = SessionStore(tmp_path)
    record = SessionRecord(
        workspace="/workspace",
        provider="ollama",
        model="model",
        task="task",
        history=[interaction],
    )

    path = store.save(record)
    restored = store.load(record.id)

    assert path.stat().st_mode & 0o777 == 0o600
    assert restored.task == "task"
    assert restored.history[0].role == Role.USER
