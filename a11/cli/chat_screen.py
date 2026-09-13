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

"""One double-buffered terminal surface for A11 Chat."""

from __future__ import annotations

import asyncio
import base64
import io
from collections.abc import Awaitable, Callable

from prompt_toolkit.application import Application
from prompt_toolkit.buffer import Buffer
from prompt_toolkit.clipboard import InMemoryClipboard
from prompt_toolkit.formatted_text import ANSI, HTML, StyleAndTextTuples
from prompt_toolkit.history import History
from prompt_toolkit.key_binding import KeyBindings
from prompt_toolkit.layout import (
    Float,
    FloatContainer,
    HSplit,
    Layout,
    VSplit,
    Window,
)
from prompt_toolkit.layout.controls import BufferControl, FormattedTextControl
from prompt_toolkit.layout.dimension import Dimension
from prompt_toolkit.layout.menus import CompletionsMenu
from prompt_toolkit.layout.screen import Point
from prompt_toolkit.mouse_events import MouseButton, MouseEvent, MouseEventType
from prompt_toolkit.styles import Style
from rich.console import Console, RenderableType

from a11.status import StatusCode, StatusException


class _TranscriptControl(FormattedTextControl):
    """Route scrolling and selection over the persistent transcript."""

    def __init__(
        self,
        *args,
        scroll: Callable[[int], None],
        select: Callable[[MouseEvent], None],
        **kwargs,
    ) -> None:
        super().__init__(*args, **kwargs)
        self._scroll = scroll
        self._select = select

    def mouse_handler(self, mouse_event: MouseEvent):
        if mouse_event.event_type == MouseEventType.SCROLL_UP:
            self._scroll(-3)
            return None
        if mouse_event.event_type == MouseEventType.SCROLL_DOWN:
            self._scroll(3)
            return None
        if mouse_event.event_type in {
            MouseEventType.MOUSE_DOWN,
            MouseEventType.MOUSE_MOVE,
            MouseEventType.MOUSE_UP,
        }:
            self._select(mouse_event)
            return None
        return super().mouse_handler(mouse_event)


class ChatScreen:
    """Own the terminal while chat is interactive.

    Prompt-toolkit computes terminal diffs and writes one complete frame. Rich
    may format a renderable into ANSI in memory, but never writes to the live
    terminal, so streaming output and the composer cannot race the cursor.
    """

    def __init__(
        self,
        *,
        history: History,
        completer,
        prompt: Callable[[], HTML],
        toolbar: Callable[[], HTML],
        submit: Callable[[str], Awaitable[bool]],
        cancel: Callable[[], None],
    ) -> None:
        self._transcript: list[str] = []
        self._active = ""
        self._follow_tail = True
        self._scroll_top = 0
        self._selection_anchor: int | None = None
        self._selection_focus: int | None = None
        self._last_disconnect_error = ""
        self._previous_exception_handler = None
        self._submit = submit
        self._tasks: set[asyncio.Task[None]] = set()
        self.buffer = Buffer(
            history=history,
            completer=completer,
            complete_while_typing=True,
            multiline=False,
        )
        bindings = KeyBindings()

        @bindings.add("enter")
        def accept(event) -> None:
            text = self.buffer.text.strip()
            if not text:
                return
            self.buffer.append_to_history()
            self.buffer.reset()
            task = asyncio.create_task(self._accept(text))
            self._tasks.add(task)
            task.add_done_callback(self._tasks.discard)

        @bindings.add("c-c")
        def interrupt(event) -> None:
            cancel()

        @bindings.add("c-d")
        def eof(event) -> None:
            if not self.buffer.text:
                event.app.exit()

        @bindings.add("tab")
        def complete(event) -> None:
            if self.buffer.complete_state:
                self.buffer.complete_next()
            else:
                self.buffer.start_completion(select_first=False)

        @bindings.add("pageup")
        def page_up(event) -> None:
            self._scroll_transcript(-self._scroll_page_size())

        @bindings.add("pagedown")
        def page_down(event) -> None:
            self._scroll_transcript(self._scroll_page_size())

        @bindings.add("c-home")
        def history_start(event) -> None:
            self._follow_tail = False
            self._scroll_top = 0
            self.invalidate()

        @bindings.add("c-end")
        def history_end(event) -> None:
            self._follow_tail = True
            self.invalidate()

        transcript = _TranscriptControl(
            self._formatted_transcript,
            get_cursor_position=self._transcript_cursor,
            focusable=False,
            scroll=self._scroll_transcript,
            select=self._select_transcript,
        )
        self._transcript_window = Window(
            transcript,
            wrap_lines=True,
            always_hide_cursor=True,
            allow_scroll_beyond_bottom=False,
            get_vertical_scroll=self._vertical_scroll,
        )
        composer = HSplit(
            [
                Window(height=1, char=" ", style="class:input"),
                VSplit(
                    [
                        Window(width=2, char=" ", style="class:input"),
                        Window(
                            FormattedTextControl(prompt),
                            width=Dimension(min=5, max=36),
                            dont_extend_width=True,
                            style="class:input",
                        ),
                        Window(
                            BufferControl(buffer=self.buffer),
                            height=Dimension(min=1, max=6),
                            wrap_lines=True,
                            style="class:input",
                        ),
                        Window(width=1, char=" ", style="class:input"),
                    ],
                    height=Dimension(min=1, max=6),
                ),
                Window(height=1, char=" ", style="class:input"),
            ],
            height=Dimension(min=3, max=8),
        )
        self._composer = composer
        body = HSplit(
            [
                self._transcript_window,
                Window(height=1, char="─", style="class:rule"),
                composer,
                Window(
                    FormattedTextControl(toolbar),
                    height=1,
                    style="class:toolbar",
                ),
            ]
        )
        root = FloatContainer(
            content=body,
            floats=[
                Float(
                    xcursor=True,
                    ycursor=True,
                    content=CompletionsMenu(max_height=10, scroll_offset=1),
                )
            ],
        )
        self.app: Application[None] = Application(
            layout=Layout(root, focused_element=self.buffer),
            key_bindings=bindings,
            full_screen=True,
            mouse_support=True,
            clipboard=InMemoryClipboard(),
            style=Style.from_dict({
                "toolbar": "bg:#252525 #b8b8b8",
                "rule": "#555555",
                "input": "bg:#3a3a3a #e4e4e4",
                "selection": "bg:#d7d7d7 #202020",
                "completion-menu.completion": "bg:#303030 #dddddd",
                "completion-menu.completion.current": (
                    "bg:#0060a8 #ffffff bold"
                ),
            }),
        )

    @property
    def is_running(self) -> bool:
        return self.app.is_running

    @property
    def default_buffer(self) -> Buffer:
        """PromptSession-compatible access used by voice transcription."""
        return self.buffer

    @property
    def following_tail(self) -> bool:
        """Whether new transcript output remains pinned to the viewport."""
        return self._follow_tail

    @property
    def selected_text(self) -> str:
        """Return the selected transcript text, if any."""
        bounds = self._selection_bounds()
        if bounds is None:
            return ""
        start, end = bounds
        return self._plain_transcript()[start:end]

    async def _accept(self, text: str) -> None:
        self.append_ansi(f"\x1b[36m›\x1b[0m {text}\n")
        try:
            keep_running = await self._submit(text)
        except StatusException as error:
            self._show_error(error.status.message)
            return
        except Exception as error:  # noqa: BLE001 - keep the TUI usable
            self._show_error(str(error))
            return
        if not keep_running:
            self.app.exit()

    async def run(self) -> None:
        loop = asyncio.get_running_loop()
        self._previous_exception_handler = loop.get_exception_handler()
        loop.set_exception_handler(self._handle_loop_exception)
        try:
            await self.app.run_async(set_exception_handler=False)
        finally:
            loop.set_exception_handler(self._previous_exception_handler)
            pending = list(self._tasks)
            for task in pending:
                if not task.done():
                    task.cancel()
            await asyncio.gather(*pending, return_exceptions=True)

    def _show_error(self, message: str) -> None:
        self.print(f"error: {message}", style="red", markup=False)

    def _handle_loop_exception(self, loop, context) -> None:
        """Keep expected transport shutdowns inside the chat transcript."""
        error = context.get("exception")
        if isinstance(error, StatusException) and (
            error.status.code
            in {StatusCode.CANCELLED, StatusCode.FAILED_PRECONDITION}
            and any(
                phrase in error.status.message.casefold()
                for phrase in (
                    "peer aborted the stream",
                    "endpoint has already terminated",
                )
            )
        ):
            message = error.status.message
            if message != self._last_disconnect_error:
                self._last_disconnect_error = message
                self._show_error(f"gateway disconnected: {message}")
            return
        if self._previous_exception_handler is not None:
            self._previous_exception_handler(loop, context)
        else:
            loop.default_exception_handler(context)

    def print(self, *objects, **kwargs) -> None:
        """Format Rich-compatible objects into the transcript."""
        self.append_ansi(self.render(*objects, **kwargs))

    def render(self, *objects, **kwargs) -> str:
        width = 100
        if self.app.is_running:
            width = max(40, self.app.output.get_size().columns)
        stream = io.StringIO()
        console = Console(
            file=stream,
            force_terminal=True,
            color_system="truecolor",
            width=width,
            highlight=False,
        )
        console.print(*objects, **kwargs)
        return stream.getvalue()

    def append_ansi(self, value: str) -> None:
        self._transcript.append(value)
        self.invalidate()

    def set_active(self, renderable: RenderableType | None) -> None:
        self._active = self.render(renderable) if renderable is not None else ""
        self.invalidate()

    def commit_active(self) -> None:
        if self._active:
            self._transcript.append(self._active)
        self._active = ""
        self.invalidate()

    def invalidate(self) -> None:
        if self.app.is_running:
            self.app.invalidate()

    def _source(self) -> str:
        return "".join(self._transcript) + self._active

    def _formatted_transcript(self) -> StyleAndTextTuples:
        fragments = ANSI(self._source()).__pt_formatted_text__()
        bounds = self._selection_bounds()
        if bounds is None:
            return fragments
        start, end = bounds
        selected: StyleAndTextTuples = []
        offset = 0
        for style, value, *rest in fragments:
            next_offset = offset + len(value)
            before = max(0, min(len(value), start - offset))
            after = max(before, min(len(value), end - offset))
            if before:
                selected.append((style, value[:before], *rest))
            if after > before:
                selected.append(
                    (f"{style} class:selection", value[before:after], *rest)
                )
            if after < len(value):
                selected.append((style, value[after:], *rest))
            offset = next_offset
        return selected

    def _transcript_cursor(self) -> Point:
        text = self._plain_transcript()
        lines = text.split("\n")
        if self._follow_tail:
            return Point(x=len(lines[-1]), y=max(0, len(lines) - 1))
        line = min(self._scroll_top, max(0, len(lines) - 1))
        return Point(x=0, y=line)

    def _plain_transcript(self) -> str:
        fragments = ANSI(self._source()).__pt_formatted_text__()
        return "".join(fragment[1] for fragment in fragments)

    def _selection_bounds(self) -> tuple[int, int] | None:
        if self._selection_anchor is None or self._selection_focus is None:
            return None
        start, end = sorted((self._selection_anchor, self._selection_focus))
        return (start, end) if start != end else None

    def _transcript_index(self, point: Point) -> int:
        lines = self._plain_transcript().splitlines(keepends=True)
        if not lines:
            return 0
        row = min(max(0, point.y), len(lines) - 1)
        prefix = sum(len(line) for line in lines[:row])
        content = lines[row].rstrip("\r\n")
        return prefix + min(max(0, point.x), len(content))

    def _select_transcript(self, event: MouseEvent) -> None:
        index = self._transcript_index(event.position)
        if event.event_type == MouseEventType.MOUSE_DOWN:
            if event.button != MouseButton.LEFT:
                return
            self._selection_anchor = index
            self._selection_focus = index
        elif event.event_type == MouseEventType.MOUSE_MOVE:
            if (
                self._selection_anchor is None
                or event.button == MouseButton.NONE
            ):
                return
            self._selection_focus = index
        elif event.event_type == MouseEventType.MOUSE_UP:
            if self._selection_anchor is None:
                return
            self._selection_focus = index
            self._copy_selection()
        self.invalidate()

    def _copy_selection(self) -> None:
        value = self.selected_text
        if not value:
            return
        self.app.clipboard.set_text(value)
        if self.app.is_running:
            encoded = base64.b64encode(value.encode("utf-8")).decode("ascii")
            self.app.output.write_raw(f"\x1b]52;c;{encoded}\x07")
            self.app.output.flush()

    def _vertical_scroll(self, window: Window) -> int:
        if self._follow_tail:
            return window.vertical_scroll
        return self._scroll_top

    def _scroll_page_size(self) -> int:
        info = self._transcript_window.render_info
        return max(3, (info.window_height - 2) if info is not None else 10)

    def _scroll_transcript(self, amount: int) -> None:
        """Move within A11's retained transcript and manage follow-tail."""
        info = self._transcript_window.render_info
        line_count = len(self._plain_transcript().split("\n"))
        window_height = info.window_height if info is not None else 10
        max_top = max(0, line_count - window_height)
        if self._follow_tail:
            current = (
                self._transcript_window.vertical_scroll
                if info is not None
                else max_top
            )
        else:
            current = self._scroll_top
        self._scroll_top = min(max_top, max(0, current + amount))
        self._follow_tail = amount > 0 and self._scroll_top >= max_top
        self.invalidate()


__all__ = ["ChatScreen"]
