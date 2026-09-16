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

"""Terminal presentation stays compact while retaining useful detail."""

import io
import re

from rich.console import Console

from a11.cli.presentation_render import render_block
from a11.sdk.presentation import BlockKind, PresentationBlock


def _render(block: PresentationBlock) -> str:
    stream = io.StringIO()
    console = Console(
        file=stream,
        force_terminal=True,
        color_system="truecolor",
        no_color=False,
        width=100,
    )
    console.print(render_block(block))
    return stream.getvalue()


def test_flow_preview_shows_source_and_nested_action_lifecycle():
    rendered = _render(
        PresentationBlock(
            kind=BlockKind.TOOL_RUN,
            tool_name="run_flow",
            tool_arguments={
                "source": (
                    """flow inspect {
  found = run search_text(query: \"needle\")
  found.result -> result
}"""
                )
            },
            text=(
                "Running Flow `inspect`\n"
                "  ◦ search_text\n"
                "  • search_text\n"
                "Flow `inspect` completed → result\n"
            ),
        )
    )

    plain = re.sub(r"\x1b\[[0-9;]*m", "", rendered)
    assert "Ran A11 Flow" in plain
    assert "flow inspect" in plain
    assert "search_text" in plain
    assert "completed" in plain
    assert "\x1b[" in rendered


def test_completed_tool_without_a_log_uses_a_solid_marker():
    rendered = _render(
        PresentationBlock(
            kind=BlockKind.TOOL_RUN,
            tool_name="workspace_info",
            partial=False,
        )
    )

    assert "• Workspace info" in rendered


def test_apply_patch_renders_file_counts_and_numbered_diff_rows():
    rendered = _render(
        PresentationBlock(
            kind=BlockKind.TOOL_RUN,
            tool_name="apply_patch",
            tool_arguments={
                "patch": """--- a/a11/example.py
+++ b/a11/example.py
@@ -2,2 +2,2 @@
 keep = True
-answer = 41
+answer = 42
"""
            },
        )
    )

    plain = re.sub(r"\x1b\[[0-9;]*m", "", rendered)
    assert "Edited a11/example.py (+1 -1)" in plain
    assert "3 - answer = 41" in plain
    assert "3 + answer = 42" in plain
    assert "48;2;86;37;31" in rendered
    assert "48;2;23;59;45" in rendered
    assert max(map(len, plain.splitlines())) == 99


def test_report_completion_uses_a_structured_final_panel():
    rendered = _render(
        PresentationBlock(
            kind=BlockKind.TOOL_RUN,
            tool_name="report_completion",
            tool_arguments={
                "summary": "Implemented the requested renderer.",
                "checks": "Tests passed.",
                "remaining": "No remaining work.",
            },
        )
    )

    assert "Task completed" in rendered
    assert "Implemented the requested renderer." in rendered
    assert "Verification" in rendered
    assert "Tests passed." in rendered
    assert "Remaining" in rendered
    assert "No remaining work." in rendered


def test_report_completion_waits_for_its_summary():
    rendered = _render(
        PresentationBlock(
            kind=BlockKind.TOOL_RUN,
            tool_name="report_completion",
            tool_arguments={"checks": "Still receiving inputs."},
            partial=True,
        )
    )

    assert "Task completed" not in rendered
    assert "Report completion" in rendered


def test_user_input_uses_a_structured_prompt_panel():
    rendered = _render(
        PresentationBlock(
            kind=BlockKind.TOOL_RUN,
            id="request-1",
            tool_name="request_user_input",
            tool_arguments={
                "question": "Which implementation should I use?",
                "options": [
                    {
                        "label": "Native",
                        "description": "Use the A11-native implementation.",
                    },
                    {
                        "label": "Adapter",
                        "description": "Keep the compatibility adapter.",
                    },
                ],
                "allow_free_text": True,
            },
            partial=True,
        )
    )

    assert "Input required" in rendered
    assert "Which implementation should I use?" in rendered
    assert "[1] Native" in rendered
    assert "Use the A11-native implementation." in rendered
    assert "[2] Adapter" in rendered
    assert "free response accepted" in rendered
