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

"""Assemble one coding session from A11 registries, actions, and policy."""

from __future__ import annotations

import json
import pathlib
from collections.abc import Awaitable, Callable
from dataclasses import dataclass

import a11
from a11.cli.coding_agent.policy import ApprovalMode, Policy
from a11.cli.coding_agent.prompts import system_prompt
from a11.cli.coding_agent.sandbox import (
    SandboxMode,
    register_native_actions,
)
from a11.cli.coding_agent.tools import CodingContext, install
from a11.cli.coding_agent.workspace import Workspace
from a11.sdk import http

ApprovalCallback = Callable[[str, str], Awaitable[bool]]


@dataclass
class CodingAgent:
    """The client-side capabilities and state for an interactive agent."""

    context: CodingContext
    tool_names: list[str]
    prompt: str

    @classmethod
    def install(
        cls,
        registry: a11.ActionRegistry,
        *,
        cwd: str,
        add_dirs: tuple[str, ...],
        approval_mode: ApprovalMode,
        sandbox_mode: SandboxMode,
        approve: ApprovalCallback | None,
        command_tools: bool = True,
        flow_tools: bool = True,
    ) -> CodingAgent:
        """Install native and Python actions on a session registry."""
        workspace = Workspace.open(cwd, add_dirs)
        policy = Policy(approval_mode, approve)
        register_native_actions(
            registry,
            workspace.write_roots,
            cwd=workspace.cwd,
            mode=sandbox_mode,
            allow_run=command_tools,
        )
        http.WEB_FETCH_SCHEMA.description = (
            "Fetch an HTTP(S) resource with bounded outputs. This is the"
            " preferred HTTP action; do not use run_command or shell_execute"
            " to run curl, wget, or a language HTTP client when web-fetch is"
            " available. Put independent requests in one run_flow and filter"
            " its `text`, `json`, or `items` outputs there so unused bodies"
            " stay out of model context. Use it after relevant workspace"
            " inspection for current external facts or information the"
            " workspace cannot establish. When source URLs are unknown, use"
            " an authoritative search API."
        )
        http.WEB_FETCH_SCHEMA.inputs["url"].description = (
            "Absolute HTTP(S) URL. Put known independent URLs or API queries"
            " in one run_flow so their web-fetch calls run concurrently."
        )
        http_options = http.WEB_FETCH_SCHEMA.inputs["options"]
        http_options.description = "Optional request and response bounds."
        http_options.json_schema = json.dumps(
            {
                "type": "object",
                "properties": {
                    "max_redirects": {
                        "type": "integer",
                        "minimum": 0,
                        "description": (
                            "Maximum redirects to follow; default 5."
                        ),
                    },
                    "timeout": {
                        "type": "number",
                        "exclusiveMinimum": 0,
                        "description": "Whole-request timeout in seconds.",
                    },
                    "http_version": {
                        "type": "string",
                        "description": "Preferred HTTP protocol version.",
                    },
                    "headers": {
                        "type": "object",
                        "additionalProperties": {"type": "string"},
                        "description": "HTTP request headers.",
                    },
                    "max_body_bytes": {
                        "type": "integer",
                        "minimum": 1,
                        "description": (
                            "Maximum response bytes; default 32 MiB."
                        ),
                    },
                    "user_agent": {
                        "type": "string",
                        "description": "User-Agent header value.",
                    },
                    "omit": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": (
                            "Output ports to close without producing."
                        ),
                    },
                },
                "additionalProperties": False,
            }
        )
        http.register(registry, low_level=False, adapter=True, renderer=True)
        context = CodingContext(
            workspace=workspace,
            policy=policy,
            registry=registry,
            sandbox_mode=sandbox_mode,
        )
        names = install(
            registry,
            context,
            command_tools=command_tools,
            flow_tools=flow_tools,
        )
        names.extend((http.WEB_FETCH, http.WEB_RENDER))

        prompt = system_prompt(workspace, approval_mode)
        context.system_prompt = prompt
        context.model_tool_names = list(names)
        return cls(
            context=context,
            tool_names=names,
            prompt=prompt,
        )

    def provider_config(self, provider: str) -> dict[str, object] | None:
        """Constrain native provider agents to the common A11 tool policy."""
        workspace = self.context.workspace
        if provider == "codex":
            return {
                "cwd": str(workspace.cwd),
                "add_dirs": [str(path) for path in workspace.write_roots[1:]],
                "sandbox": "read-only",
            }
        if provider == "claude_code":
            return {
                "builtin_tools": False,
                "permission_mode": "plan",
                "cwd": str(workspace.cwd),
                "add_dirs": [str(path) for path in workspace.write_roots[1:]],
            }
        return None

    def status(self) -> dict[str, object]:
        """Return the current user-visible session state."""
        return self.context.workspace.summary() | {
            "approval_mode": self.context.policy.mode.value,
            "sandbox": self.context.sandbox_mode.value,
            "completion": self.context.completion,
            "decisions": list(self.context.policy.decisions),
        }
