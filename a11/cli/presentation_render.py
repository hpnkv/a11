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

"""Rendering `PresentationBlock`s with `rich`, for the terminal clients.

The whole terminal-specific half of presentation: a `match` over `BlockKind` and
nothing else. Because the blocks come from
[a11.sdk.presentation][a11.sdk.presentation], the same function draws a live
turn and a replayed one, and `a11 chat` gets tool-call rendering it never had.
"""

from __future__ import annotations

import json
from collections.abc import Sequence
from dataclasses import dataclass

from rich.console import (
    Console,
    ConsoleOptions,
    Group,
    RenderableType,
    RenderResult,
)
from rich.markdown import Markdown
from rich.panel import Panel
from rich.syntax import Syntax
from rich.text import Text

from a11.cli.flow_highlighting import register_flow_lexer
from a11.sdk.presentation import BlockKind, PresentationBlock

register_flow_lexer()


@dataclass(frozen=True)
class _PatchRow:
    """One Codex-style numbered diff row, expanded to the terminal width."""

    kind: str
    number: int | None
    number_width: int
    content: str
    path: str

    def __rich_console__(
        self, console: Console, options: ConsoleOptions
    ) -> RenderResult:
        number = self.number or ""
        row = Text()
        row.append(f"{number:>{self.number_width + 4}}", style="dim")
        marker = {"added": "+", "removed": "-"}.get(self.kind, " ")
        marker_style = {"added": "green", "removed": "red"}.get(self.kind)
        row.append(f" {marker} ", style=marker_style)
        row.append_text(_highlight_patch_line(self.content, self.path))
        background = {
            "added": "on #173b2d",
            "removed": "on #56251f",
        }.get(self.kind)
        if background:
            # A line exactly as wide as prompt-toolkit's Window becomes a
            # second wrapped display row when its newline arrives.
            row.truncate(
                max(1, options.max_width - 1), overflow="ellipsis", pad=True
            )
            row.stylize(background, 0, len(row))
        yield row


def render_block(
    block: PresentationBlock, *, verbose: bool = False
) -> RenderableType | None:
    """Render one block, or ``None`` when it should not be drawn.

    Args:
        block: The block to render.
        verbose: Draw token usage in addition to the normal streamed content.

    Returns:
        A `rich` renderable, or ``None`` to omit the block.
    """
    match block.kind:
        case BlockKind.TEXT:
            return Markdown(block.text) if block.text else None
        case BlockKind.THOUGHT:
            if not block.text:
                return None
            return Group(
                Text("• Thinking", style="bold magenta"),
                _indented_detail(
                    _bounded_detail(block.text, lines=8, chars=1200),
                    "dim italic",
                ),
            )
        case BlockKind.TOOL_RUN:
            return _tool_panel(block)
        case BlockKind.TOOL_RESULT:
            # The run panel already carries the tool's own account of itself;
            # the raw result is for the model, not the reader.
            return None
        case BlockKind.IMAGE:
            label = block.mime_type or "image"
            return Text(f"[{label}]", style="dim")
        case BlockKind.ERROR:
            message = block.status.message if block.status else "failed"
            return Text(f"error: {message}", style="red")
        case BlockKind.USAGE:
            if not verbose or block.usage is None:
                return None
            return Text(_usage_line(block), style="dim")
    return None


def render_blocks(
    blocks: Sequence[PresentationBlock], *, verbose: bool = False
) -> RenderableType:
    """Render a whole turn, skipping blocks that should not be drawn."""
    rendered = [
        renderable
        for renderable in (
            render_block(block, verbose=verbose) for block in blocks
        )
        if renderable is not None
    ]
    return Group(*rendered)


def _tool_panel(block: PresentationBlock) -> RenderableType:
    """Render a compact action row with bounded, indented detail.

    The presentation mirrors an agent activity transcript: the operation is
    scannable on one strong line and detail remains subordinate beneath it.
    """
    if block.tool_name == "run_flow":
        return _flow_panel(block)
    if block.tool_name in {"apply_patch", "ide__apply_patch"}:
        patch = _argument(block.tool_arguments or {}, "patch", "")
        if isinstance(patch, str) and patch.strip():
            return _patch_panel(block, patch)
    if block.tool_name == "request_user_input":
        return _user_input_panel(block)
    if (
        block.tool_name == "report_completion"
        and str(
            _completion_argument(block.tool_arguments or {}, "summary")
        ).strip()
    ):
        return _completion_panel(block)
    if block.status is not None and not block.status.is_ok():
        return Group(
            Text(f"× {_tool_summary(block)}", style="bold red"),
            _indented_detail(block.status.message or "failed", "red"),
        )
    running = block.partial
    body: list[RenderableType] = [
        Text(
            f"{'◦' if running else '•'} {_tool_summary(block)}",
            style="bold cyan" if running else "bold green",
        )
    ]
    if block.tool_arguments and not _arguments_are_summarized(block):
        try:
            encoded = json.dumps(
                block.tool_arguments,
                ensure_ascii=False,
                indent=2,
                default=str,
            )
        except (TypeError, ValueError):
            encoded = str(block.tool_arguments)
        body.append(_indented_detail(_bounded_detail(encoded), "dim"))
    if block.text:
        body.append(_indented_detail(_bounded_detail(block.text), "dim"))
    return Group(*body)


def _patch_panel(block: PresentationBlock, patch: str) -> RenderableType:
    """Draw an applied patch with Codex-style file summaries and diff rows."""
    files = _parse_patch(patch)
    if not files:
        return Group(Text("• Applied patch", style="bold green"))
    verb = "Applying" if block.partial else "Edited"
    rows: list[RenderableType] = []
    for index, (path, file_added, file_removed, lines) in enumerate(files):
        if index:
            rows.append(Text())
        heading = Text()
        heading.append(f"{'◦' if block.partial else '•'} ", style="dim")
        heading.append(f"{verb} ", style="bold")
        heading.append(path, style="dim")
        heading.append(" (")
        heading.append(f"+{file_added}", style="green")
        heading.append(" ")
        heading.append(f"-{file_removed}", style="red")
        heading.append(")")
        rows.append(heading)
        width = max(
            (len(str(line[1] or line[2] or 0)) for line in lines), default=1
        )
        for kind, old_line, new_line, content in lines:
            number = old_line if kind == "removed" else new_line
            rows.append(_PatchRow(kind, number, width, content, path))
    if block.status is not None and not block.status.is_ok():
        rows.append(_indented_detail(block.status.message or "failed", "red"))
    rows.append(Text())
    return Group(*rows)


def _highlight_patch_line(content: str, path: str) -> Text:
    """Syntax-highlight one diff line using the edited file's extension."""
    try:
        highlighted = Syntax(
            content,
            path.rsplit(".", 1)[-1],
            theme="monokai",
            word_wrap=False,
        ).highlight(content)
        if highlighted.plain.endswith("\n"):
            highlighted.right_crop(1)
        return highlighted
    except Exception:
        return Text(content)


def _parse_patch(
    patch: str,
) -> list[tuple[str, int, int, list[tuple[str, int | None, int | None, str]]]]:
    files: list[list[object]] = []
    current: list[object] | None = None
    old_line = new_line = 0
    in_hunk = False
    for raw in patch.replace("\r\n", "\n").splitlines():
        if raw.startswith("--- "):
            path = raw[4:].split("\t", 1)[0].strip()
            if path.startswith("a/"):
                path = path[2:]
            current = [path, 0, 0, []]
            files.append(current)
            in_hunk = False
            continue
        if raw.startswith("+++ ") and current is not None:
            path = raw[4:].split("\t", 1)[0].strip()
            if path.startswith("b/"):
                path = path[2:]
            if path != "/dev/null":
                current[0] = path
            continue
        if raw.startswith("@@ ") and current is not None:
            try:
                ranges = raw.split("@@", 2)[1].strip().split()
                old_line = int(ranges[0][1:].split(",", 1)[0])
                new_line = int(ranges[1][1:].split(",", 1)[0])
                in_hunk = True
            except (IndexError, ValueError):
                in_hunk = False
            continue
        if current is None or not in_hunk or raw.startswith("\\ No newline"):
            continue
        lines = current[3]
        assert isinstance(lines, list)
        if raw.startswith("+"):
            lines.append(("added", None, new_line, raw[1:]))
            current[1] = int(current[1]) + 1
            new_line += 1
        elif raw.startswith("-"):
            lines.append(("removed", old_line, None, raw[1:]))
            current[2] = int(current[2]) + 1
            old_line += 1
        elif raw.startswith(" "):
            lines.append(("context", old_line, new_line, raw[1:]))
            old_line += 1
            new_line += 1
    return [
        (str(path), int(added), int(removed), lines)  # type: ignore[arg-type]
        for path, added, removed, lines in files
    ]


def _completion_panel(block: PresentationBlock) -> RenderableType:
    """Draw the coding agent's structured completion as a final report."""
    arguments = block.tool_arguments or {}
    summary = str(_completion_argument(arguments, "summary")).strip()
    checks = str(_completion_argument(arguments, "checks")).strip()
    remaining = str(_completion_argument(arguments, "remaining")).strip()
    failed = block.status is not None and not block.status.is_ok()
    body: list[RenderableType] = [Markdown(summary)]
    if checks:
        body.extend((Text("Verification", style="bold"), Text(checks)))
    if remaining:
        body.extend((Text("Remaining", style="bold yellow"), Text(remaining)))
    if failed:
        body.extend((
            Text("Report error", style="bold red"),
            Text(block.status.message or "failed", style="red"),
        ))
    return Panel(
        Group(*body),
        title="Completion report failed" if failed else "✓ Task completed",
        title_align="left",
        border_style="red" if failed else "green",
        padding=(0, 1),
    )


def _user_input_panel(block: PresentationBlock) -> RenderableType:
    arguments = block.tool_arguments or {}
    question = str(_argument(arguments, "question", "Input requested"))
    body: list[RenderableType] = [Text(question, style="bold")]
    options = arguments.get("options", [])
    if isinstance(options, list):
        for index, option in enumerate(options, 1):
            if not isinstance(option, dict):
                continue
            label = str(option.get("label", "")).strip()
            description = str(option.get("description", "")).strip()
            line = f"[{index}] {label}"
            if description:
                line += f" — {description}"
            body.append(Text(line, style="yellow"))
    allow_free_text = bool(arguments.get("allow_free_text", True))
    if block.partial:
        instruction = "Choose with a number or label"
        if allow_free_text:
            instruction += " · free response accepted"
        body.append(Text(instruction, style="dim"))
    elif block.status is not None and not block.status.is_ok():
        body.append(Text(block.status.message or "failed", style="red"))
    failed = block.status is not None and not block.status.is_ok()
    return Panel(
        Group(*body),
        title=(
            "Input request failed"
            if failed
            else "? Input required"
            if block.partial
            else "✓ Input received"
        ),
        title_align="left",
        border_style=(
            "red" if failed else "yellow" if block.partial else "green"
        ),
        padding=(0, 1),
    )


def _flow_panel(block: PresentationBlock) -> RenderableType:
    """Show the composition and its nested action lifecycle as one activity."""
    arguments = block.tool_arguments or {}
    source = _argument(arguments, "source", "")
    body: list[RenderableType] = [
        Text(
            f"{'◦' if block.partial else '•'} Ran A11 Flow",
            style="bold cyan" if block.partial else "bold green",
        )
    ]
    if source:
        body.append(
            Syntax(
                str(source).strip(),
                "a11flow",
                background_color="default",
                word_wrap=True,
                padding=(0, 2),
            )
        )
    if block.text:
        body.append(_indented_detail(_bounded_detail(block.text), "dim"))
    if block.status is not None and not block.status.is_ok():
        body.append(_indented_detail(block.status.message or "failed", "red"))
    return Group(*body)


def _tool_summary(block: PresentationBlock) -> str:
    """Human action summary for common coding tools."""
    arguments = block.tool_arguments or {}
    name = block.tool_name or "tool"
    if name in {"run_command", "shell_execute"}:
        return f"Ran {_argument(arguments, 'command', name)}"
    if name == "read_file":
        return f"Read {_argument(arguments, 'path', 'file')}"
    if name in {"list_files", "list_directory"}:
        return f"Explored {_argument(arguments, 'path', '.')}"
    if name == "search_text":
        return f"Searched for {_argument(arguments, 'query', 'text')}"
    if name == "apply_patch":
        return "Applied patch"
    if name == "run_flow":
        return "Ran A11 Flow"
    return name.replace("_", " ").capitalize()


def _argument(arguments: dict[str, object], name: str, default: str) -> object:
    """Unwrap a named value from direct and object-port action inputs."""
    value = arguments.get(name)
    if value is None and len(arguments) == 1:
        value = next(iter(arguments.values()))
    if isinstance(value, dict):
        value = value.get(name, value.get("value", value))
    return default if value is None else value


def _completion_argument(arguments: dict[str, object], name: str) -> object:
    """Read one report field without treating a different field as its value."""
    if name in arguments:
        return arguments[name]
    if len(arguments) == 1:
        envelope = next(iter(arguments.values()))
        if isinstance(envelope, dict):
            return envelope.get(name, "")
    return ""


def _arguments_are_summarized(block: PresentationBlock) -> bool:
    return block.tool_name in {
        "run_command",
        "shell_execute",
        "read_file",
        "list_files",
        "list_directory",
        "search_text",
        "apply_patch",
        "run_flow",
    }


def _bounded_detail(value: str, *, lines: int = 24, chars: int = 4000) -> str:
    """Keep an action from taking over the entire terminal transcript."""
    source = value.rstrip()
    selected = source.splitlines()[:lines]
    bounded = "\n".join(selected)
    truncated = len(selected) < len(source.splitlines()) or len(bounded) > chars
    if len(bounded) > chars:
        bounded = bounded[:chars]
    return f"{bounded}\n… output truncated" if truncated else bounded


def _indented_detail(value: str, style: str) -> Text:
    lines = value.splitlines() or [""]
    return Text(
        "\n".join(
            f"  {'└' if index == 0 else ' '} {line}"
            for index, line in enumerate(lines)
        ),
        style=style,
        overflow="fold",
    )


def _usage_line(block: PresentationBlock) -> str:
    usage = block.usage
    assert usage is not None  # guarded by the caller
    parts = []
    for label, value in (
        ("in", getattr(usage, "input_tokens", None)),
        ("out", getattr(usage, "output_tokens", None)),
    ):
        if value:
            parts.append(f"{label} {value}")
    return f"tokens: {', '.join(parts)}" if parts else "tokens: n/a"


__all__ = ["render_block", "render_blocks"]
