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
import re
from collections.abc import Awaitable, Callable

from prompt_toolkit.application import Application
from prompt_toolkit.application.current import get_app_session
from prompt_toolkit.buffer import Buffer
from prompt_toolkit.clipboard import InMemoryClipboard
from prompt_toolkit.formatted_text import ANSI, HTML, StyleAndTextTuples
from prompt_toolkit.formatted_text.utils import split_lines
from prompt_toolkit.history import History
from prompt_toolkit.filters import Condition
from prompt_toolkit.key_binding import KeyBindings
from prompt_toolkit.layout import (
    Float,
    FloatContainer,
    ConditionalContainer,
    HSplit,
    Layout,
    VSplit,
    Window,
)
from prompt_toolkit.layout.controls import (
    BufferControl,
    FormattedTextControl,
    UIContent,
)
from prompt_toolkit.layout.dimension import Dimension
from prompt_toolkit.layout.menus import CompletionsMenu
from prompt_toolkit.layout.screen import Point
from prompt_toolkit.mouse_events import MouseButton, MouseEvent, MouseEventType
from prompt_toolkit.styles import Style
from rich.console import Console, RenderableType

from a11.status import StatusCode, StatusException


# Prompt-toolkit parses SGR styles, but exposes OSC payloads as visible text.
# Strip terminal metadata, including hyperlink wrappers, before that parser.
_OSC_SEQUENCE = re.compile(
    r"(?:\x1b\]|\x9d).*?(?:\x07|\x1b\\|\x9c|$)", re.DOTALL
)


class _InlineViewportOutput:
    """Give Prompt Toolkit a full-height viewport on the main screen."""

    def __init__(self, output) -> None:
        self._output = output

    def __getattr__(self, name: str):
        return getattr(self._output, name)

    def enter_alternate_screen(self) -> None:
        self._output.cursor_goto(0, 0)
        self._output.erase_down()

    def quit_alternate_screen(self) -> None:
        pass


class _TranscriptControl(FormattedTextControl):
    """Route scrolling and selection over the persistent transcript."""

    def __init__(
        self,
        *args,
        scroll: Callable[[int], None],
        select: Callable[[MouseEvent], None],
        revision: Callable[[], object],
        **kwargs,
    ) -> None:
        super().__init__(*args, **kwargs)
        self._scroll = scroll
        self._select = select
        self._revision = revision
        self._line_revision: object | None = None
        self._fragment_lines: list[StyleAndTextTuples] = []

    def create_content(self, width: int, height: int | None) -> UIContent:
        """Build line geometry only when transcript content actually changes."""
        fragments = self._get_formatted_text_cached()
        revision = self._revision()
        if revision != self._line_revision:
            self._fragment_lines = [
                [(part[0], part[1]) for part in line]
                for line in split_lines(fragments)
            ]
            self._line_revision = revision
        self._fragments = fragments
        cursor = self.get_cursor_position()
        return UIContent(
            get_line=self._fragment_lines.__getitem__,
            line_count=len(self._fragment_lines),
            show_cursor=self.show_cursor,
            cursor_position=cursor,
        )

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
        question: Callable[[], dict | None] | None = None,
        working: Callable[[], bool] | None = None,
        activity: Callable[[], HTML | None] | None = None,
        context_usage: Callable[[], str] | None = None,
    ) -> None:
        self._transcript: list[str] = []
        self._active = ""
        self._content_revision = 0
        self._parsed_revision = -1
        self._cached_fragments: StyleAndTextTuples = []
        self._cached_plain = ""
        self._cached_line_count = 1
        self._follow_tail = True
        self._scroll_top = 0
        self._selection_anchor: int | None = None
        self._selection_focus: int | None = None
        self._last_disconnect_error = ""
        self._previous_exception_handler = None
        self._submit = submit
        self._cancel = cancel
        self._question = question or (lambda: None)
        self._working = working or (lambda: False)
        self._activity = activity or (lambda: None)
        self._context_usage = context_usage or (lambda: "")
        self._question_id = None
        self._question_index = 0
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
            text = self.buffer.text.strip() or self._selected_answer()
            if not text:
                return
            self.buffer.append_to_history()
            self.buffer.reset()
            task = asyncio.create_task(self._accept(text))
            self._tasks.add(task)
            task.add_done_callback(self._tasks.discard)

        @bindings.add("c-c")
        def interrupt(event) -> None:
            self._interrupt(event.app)

        choosing = Condition(
            lambda: (
                bool(self._question_options())
                and not self.buffer.text
                and self.buffer.complete_state is None
            )
        )

        @bindings.add("up", filter=choosing)
        def previous_option(event) -> None:
            self._move_question(-1)

        @bindings.add("down", filter=choosing)
        def next_option(event) -> None:
            self._move_question(1)

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
            revision=lambda: (
                self._content_revision,
                self._selection_anchor,
                self._selection_focus,
            ),
        )
        self._transcript_window = Window(
            transcript,
            wrap_lines=False,
            always_hide_cursor=True,
            allow_scroll_beyond_bottom=False,
            get_vertical_scroll=self._vertical_scroll,
        )
        composer = HSplit(
            [
                Window(height=1, char=" ", style="class:input"),
                VSplit(
                    [
                        Window(
                            FormattedTextControl(prompt),
                            width=2,
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
                        Window(
                            FormattedTextControl(self._context_usage),
                            width=lambda: Dimension(
                                min=0,
                                preferred=len(self._context_usage()),
                                max=len(self._context_usage()),
                            ),
                            style="class:input #aaaaaa",
                            dont_extend_width=True,
                        ),
                        Window(width=1, char=" ", style="class:input"),
                    ],
                    height=lambda: Dimension.exact(
                        min(6, max(1, self.buffer.document.line_count))
                    ),
                ),
                Window(height=1, char=" ", style="class:input"),
            ],
            height=lambda: Dimension.exact(
                min(8, max(3, self.buffer.document.line_count + 2))
            ),
        )
        self._composer = composer
        composer_row = composer
        toolbar_row = VSplit(
            [
                Window(width=2),
                Window(
                    FormattedTextControl(toolbar),
                    height=1,
                ),
            ],
            height=1,
        )
        self._composer_row = composer_row
        self._toolbar_row = toolbar_row
        body = HSplit([
            self._transcript_window,
            ConditionalContainer(
                HSplit([
                    Window(
                        FormattedTextControl(lambda: self._activity() or ""),
                        height=1,
                    ),
                    Window(height=1),
                ]),
                filter=Condition(lambda: self._activity() is not None),
            ),
            ConditionalContainer(
                HSplit([
                    Window(
                        FormattedTextControl(self._question_fragments),
                        wrap_lines=True,
                        dont_extend_height=True,
                    ),
                    Window(height=1),
                ]),
                filter=Condition(lambda: self._question() is not None),
            ),
            composer_row,
            toolbar_row,
        ])
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
            max_render_postpone_time=0,
            clipboard=InMemoryClipboard(),
            output=_InlineViewportOutput(get_app_session().output),
            style=Style.from_dict({
                "input": "bg:#3a3a3a #e4e4e4",
                "selection": "bg:#d7d7d7 #202020",
                "question.selected": "bg:#304744 #b4e6ed bold",
                "completion-menu.completion": "bg:#303030 #dddddd",
                "completion-menu.completion.current": (
                    "bg:#0060a8 #ffffff bold"
                ),
            }),
        )

    def _question_options(self) -> list[dict]:
        request = self._question() or {}
        if request.get("request_id") != self._question_id:
            self._question_id = request.get("request_id")
            self._question_index = 0
        options = request.get("options") or []
        self._question_index = min(
            self._question_index, max(0, len(options) - 1)
        )
        return options

    def _move_question(self, delta: int) -> None:
        options = self._question_options()
        if options:
            self._question_index = (self._question_index + delta) % len(options)
            self.invalidate()

    def _selected_answer(self) -> str:
        options = self._question_options()
        return str(options[self._question_index]["label"]) if options else ""

    def _question_fragments(self) -> StyleAndTextTuples:
        request = self._question() or {}
        fragments = [("bold ansicyan", str(request.get("question", "")) + "\n")]
        options = self._question_options()
        for index, option in enumerate(options):
            selected = index == self._question_index
            prefix = "› " if selected else "  "
            description = str(option.get("description") or "")
            text = prefix + str(option["label"])
            if description:
                text += " — " + description
            fragments.append((
                "class:question.selected" if selected else "",
                text + "\n",
            ))
        hint = "↑/↓ select · Enter submit" if options else "Type your answer"
        if options and request.get("allow_free_text", True):
            hint += " · or type an answer"
        fragments.append(("ansigray", hint))
        return fragments

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

    def _interrupt(self, app) -> None:
        """Cancel active agent work, or close an idle chat screen."""
        if self._working() or any(not task.done() for task in self._tasks):
            self._cancel()
        else:
            app.exit()

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
            no_color=False,
            width=width,
            highlight=False,
        )
        console.print(*objects, **kwargs)
        return stream.getvalue()

    def append_ansi(self, value: str) -> None:
        self._transcript.append(value)
        self._content_revision += 1
        self.invalidate()

    def set_active(self, renderable: RenderableType | None) -> None:
        self._active = self.render(renderable) if renderable is not None else ""
        self._content_revision += 1
        self.invalidate()

    def commit_active(self) -> None:
        if self._active:
            self._transcript.append(self._active)
        self._active = ""
        self._content_revision += 1
        self.invalidate()

    def invalidate(self) -> None:
        if self.app.is_running:
            self.app.invalidate()

    def _source(self) -> str:
        return "".join(self._transcript) + self._active

    def _formatted_transcript(self) -> StyleAndTextTuples:
        fragments, _ = self._parsed_transcript()
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
                selected.append((
                    f"{style} class:selection",
                    value[before:after],
                    *rest,
                ))
            if after < len(value):
                selected.append((style, value[after:], *rest))
            offset = next_offset
        return selected

    def _transcript_cursor(self) -> Point:
        _, text = self._parsed_transcript()
        line_count = self._cached_line_count
        if self._follow_tail:
            last_break = text.rfind("\n")
            return Point(
                x=len(text) if last_break < 0 else len(text) - last_break - 1,
                y=max(0, line_count - 1),
            )
        line = min(self._scroll_top, max(0, line_count - 1))
        return Point(x=0, y=line)

    def _plain_transcript(self) -> str:
        _, plain = self._parsed_transcript()
        return plain

    def _parsed_transcript(self) -> tuple[StyleAndTextTuples, str]:
        """Return ANSI fragments and plain text cached by content revision."""
        if self._parsed_revision != self._content_revision:
            source = _OSC_SEQUENCE.sub("", self._source())
            fragments = ANSI(source).__pt_formatted_text__()
            self._cached_fragments = fragments
            self._cached_plain = "".join(part[1] for part in fragments)
            self._cached_line_count = self._cached_plain.count("\n") + 1
            self._parsed_revision = self._content_revision
        return self._cached_fragments, self._cached_plain

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
        self._plain_transcript()
        line_count = self._cached_line_count
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
