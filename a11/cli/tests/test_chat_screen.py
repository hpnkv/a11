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

"""Tests for A11 Chat's retained, double-buffered terminal surface."""

import asyncio

import pytest
from prompt_toolkit.formatted_text import HTML
from prompt_toolkit.history import InMemoryHistory
from prompt_toolkit.layout.screen import Point
from prompt_toolkit.keys import Keys
from prompt_toolkit.mouse_events import MouseButton, MouseEvent, MouseEventType

from a11.cli.chat_screen import ChatScreen
from a11.status import Status, StatusCode


async def _submit(_: str) -> bool:
    return True


@pytest.mark.asyncio
async def test_question_arrow_navigation_and_enter_submission():
    request = {
        "request_id": "question-1",
        "question": "Which one?",
        "options": [{"label": "First"}, {"label": "Second"}],
    }
    submitted = []

    async def submit(text):
        submitted.append(text)
        return True

    screen = ChatScreen(
        history=InMemoryHistory(),
        completer=None,
        prompt=lambda: HTML("› "),
        toolbar=lambda: HTML("status"),
        submit=submit,
        cancel=lambda: None,
        question=lambda: request,
    )
    assert screen._selected_answer() == "First"
    down = screen.app.key_bindings.get_bindings_for_keys((Keys.Down,))[-1]
    down.handler(None)
    assert screen._selected_answer() == "Second"
    up = screen.app.key_bindings.get_bindings_for_keys((Keys.Up,))[-1]
    up.handler(None)
    assert screen._selected_answer() == "First"
    down.handler(None)
    enter = screen.app.key_bindings.get_bindings_for_keys((Keys.ControlM,))[-1]
    enter.handler(None)
    await asyncio.gather(*screen._tasks)
    assert submitted == ["Second"]
    request["request_id"] = "question-2"
    assert screen._selected_answer() == "First"
    screen.buffer.text = "My own answer"
    enter.handler(None)
    await asyncio.gather(*screen._tasks)
    assert submitted[-1] == "My own answer"


def test_ctrl_c_interrupts_agent_work_outside_submit_tasks():
    interrupted = []
    screen = ChatScreen(
        history=InMemoryHistory(),
        completer=None,
        prompt=lambda: HTML("› "),
        toolbar=lambda: HTML("Working"),
        submit=_submit,
        cancel=lambda: interrupted.append(True),
        working=lambda: True,
    )
    assert not screen._tasks
    screen._interrupt(screen.app)
    assert interrupted == [True]


def test_activity_has_a_blank_row_before_the_composer():
    screen = _screen()
    body = screen.app.layout.container.content
    activity = body.children[1]
    assert len(activity.content.children) == 2
    assert activity.content.children[1].height == 1
    assert activity.content.children[1].style == ""


def test_question_has_a_blank_row_before_the_composer():
    screen = _screen()
    body = screen.app.layout.container.content
    question = body.children[2]
    assert len(question.content.children) == 2
    assert question.content.children[1].height == 1
    assert question.content.children[1].style == ""


def _screen(context_usage=None) -> ChatScreen:
    return ChatScreen(
        history=InMemoryHistory(),
        completer=None,
        prompt=lambda: HTML("&gt; "),
        toolbar=lambda: HTML("status"),
        submit=_submit,
        cancel=lambda: None,
        context_usage=context_usage,
    )


def test_mouse_support_scrolls_retained_history_and_can_resume_tail():
    screen = _screen()
    screen.append_ansi("".join(f"line {number}\n" for number in range(40)))

    assert screen.app.mouse_support()
    assert screen.following_tail

    screen._transcript_window.content.mouse_handler(
        MouseEvent(
            position=Point(x=0, y=0),
            event_type=MouseEventType.SCROLL_UP,
            button=MouseButton.NONE,
            modifiers=frozenset(),
        )
    )
    first_position = screen._scroll_top
    assert not screen.following_tail
    assert first_position > 0

    screen.append_ansi("new output\n")
    assert screen._scroll_top == first_position
    assert not screen.following_tail

    screen._scroll_transcript(10_000)
    assert screen.following_tail


def test_page_scrolling_keeps_the_transcript_as_application_content():
    screen = _screen()
    screen.append_ansi("oldest\n" + "middle\n" * 30 + "newest\n")

    screen._scroll_transcript(-screen._scroll_page_size())

    assert "oldest" in screen._plain_transcript()
    assert "newest" in screen._plain_transcript()
    assert not screen.following_tail


def test_wheel_events_scroll_both_directions_uniformly():
    screen = _screen()
    assert not screen._transcript_window.wrap_lines()
    screen.append_ansi("".join(f"line {number}\n" for number in range(200)))
    control = screen._transcript_window.content
    screen._scroll_transcript(-100)

    movements = []
    for event_type in (
        MouseEventType.SCROLL_DOWN,
        MouseEventType.SCROLL_DOWN,
        MouseEventType.SCROLL_UP,
        MouseEventType.SCROLL_UP,
    ):
        before = screen._scroll_top
        control.mouse_handler(
            MouseEvent(
                position=Point(x=0, y=0),
                event_type=event_type,
                button=MouseButton.NONE,
                modifiers=frozenset(),
            )
        )
        movements.append(screen._scroll_top - before)

    assert movements == [3, 3, -3, -3]


def test_scrolling_reuses_parsed_syntax_rich_transcript():
    screen = _screen()
    styled = "".join(
        f"\x1b[48;2;23;59;45m{line:<99}\x1b[0m\n"
        for line in (f"{number} + changed" for number in range(80))
    )
    screen.append_ansi(styled)
    fragments, plain = screen._parsed_transcript()
    control = screen._transcript_window.content
    control.create_content(100, 20)
    fragment_lines = control._fragment_lines

    for _ in range(20):
        screen._scroll_transcript(-3)
        assert screen._parsed_transcript() == (fragments, plain)
        assert screen._parsed_transcript()[0] is fragments
        control.create_content(100, 20)
        assert control._fragment_lines is fragment_lines

    assert screen._parsed_revision == screen._content_revision


def test_drag_selects_highlights_and_copies_transcript_text():
    screen = _screen()
    screen.append_ansi("\x1b[32mhello\x1b[0m world\n")
    control = screen._transcript_window.content

    for event_type, x in (
        (MouseEventType.MOUSE_DOWN, 0),
        (MouseEventType.MOUSE_MOVE, 5),
        (MouseEventType.MOUSE_UP, 5),
    ):
        control.mouse_handler(
            MouseEvent(
                position=Point(x=x, y=0),
                event_type=event_type,
                button=MouseButton.LEFT,
                modifiers=frozenset(),
            )
        )

    assert screen.selected_text == "hello"
    assert screen.app.clipboard.get_data().text == "hello"
    highlighted = "".join(
        value
        for style, value, *_ in screen._formatted_transcript()
        if "class:selection" in style
    )
    assert highlighted == "hello"


def test_markdown_link_metadata_is_not_visible_in_transcript():
    from rich.markdown import Markdown

    screen = _screen()
    screen.set_active(Markdown(
        "See the [overview](/Users/helena/dev/a11/README.md)."
    ))
    assert "\x1b]8;id=" in screen._source()
    assert screen._plain_transcript().strip() == "See the overview."
    fragments, _ = screen._parsed_transcript()
    link = [(style, text) for style, text in fragments if "underline" in style]
    assert "".join(text for _, text in link) == "overview"
    screen.commit_active()
    assert screen._plain_transcript().strip() == "See the overview."


@pytest.mark.parametrize("terminator", ["\x07", "\x1b\\", "\x9c"])
def test_osc_links_are_hidden_and_selection_uses_only_visible_text(terminator):
    screen = _screen()
    screen.append_ansi("See \x1b]8;id=3071448;/tmp/README.md")
    assert screen._plain_transcript() == "See "
    screen.append_ansi(
        f"{terminator}\x1b[4;34moverview\x1b[0m"
        f"\x1b]8;;{terminator}. id=123 is ordinary text."
    )
    assert screen._plain_transcript() == (
        "See overview. id=123 is ordinary text."
    )
    screen._selection_anchor = screen._transcript_index(Point(x=4, y=0))
    screen._selection_focus = screen._transcript_index(Point(x=12, y=0))
    assert screen.selected_text == "overview"


def test_composer_starts_at_three_rows_and_grows_with_multiline_input():
    screen = _screen()

    dimension = screen._composer.preferred_height(
        width=100, max_available_height=30
    )

    assert dimension.min == dimension.max == 3

    screen.buffer.text = "one\ntwo\nthree"
    dimension = screen._composer.preferred_height(
        width=100, max_available_height=30
    )

    assert dimension.min == dimension.max == 5


def test_composer_and_footer_match_codex_insets():
    screen = _screen()

    assert screen._composer_row is screen._composer
    assert screen._toolbar_row.children[0].width == 2
    assert len(screen._toolbar_row.children) == 2
    assert not hasattr(screen, "_rule_row")
    input_row = screen._composer.children[1]
    assert input_row.children[0].width == 2
    assert input_row.children[-1].width == 1


@pytest.mark.parametrize("width", [30, 80, 120])
@pytest.mark.parametrize("text", ["hello", "hello\nworld"])
@pytest.mark.asyncio
async def test_context_counter_renders_at_right_without_moving_input(
    width, text
):
    from prompt_toolkit.application.current import set_app
    from prompt_toolkit.layout.mouse_handlers import MouseHandlers
    from prompt_toolkit.layout.screen import Screen, WritePosition

    screen = _screen(context_usage=lambda: "172.1K tok")
    screen.buffer.text = text
    output = Screen()
    with set_app(screen.app):
        screen._composer.write_to_screen(
            output, MouseHandlers(), WritePosition(0, 0, width, 4),
            "", True, None,
        )
        output.draw_all_floats()
        await screen.app.cancel_and_wait_for_background_tasks()
    line = "".join(output.data_buffer[1][x].char for x in range(width))
    assert line.startswith("> hello")
    assert line.endswith("172.1K tok ")
    assert "class:input" in output.data_buffer[1][width - 2].style


def test_chat_uses_a_full_height_inline_terminal_viewport():
    screen = _screen()

    assert screen.app.renderer.full_screen
    assert screen.app.output.__class__.__name__ == "_InlineViewportOutput"


@pytest.mark.asyncio
async def test_ctrl_c_interrupts_an_active_agent_and_quits_when_idle():
    interrupted = 0

    def cancel() -> None:
        nonlocal interrupted
        interrupted += 1

    screen = ChatScreen(
        history=InMemoryHistory(),
        completer=None,
        prompt=lambda: HTML("&gt; "),
        toolbar=lambda: HTML("status"),
        submit=_submit,
        cancel=cancel,
    )
    exited = 0

    async def active() -> None:
        await asyncio.Future()

    task = asyncio.create_task(active())
    screen._tasks.add(task)
    screen._interrupt(screen.app)
    assert interrupted == 1

    task.cancel()
    await asyncio.gather(task, return_exceptions=True)
    screen._tasks.clear()

    def exit_app() -> None:
        nonlocal exited
        exited += 1

    screen._interrupt(type("App", (), {"exit": staticmethod(exit_app)})())
    assert exited == 1


@pytest.mark.asyncio
async def test_submit_failure_is_rendered_without_escaping_background_task():
    async def failed_submit(_: str) -> bool:
        raise Status(
            code=StatusCode.FAILED_PRECONDITION,
            message="The peer aborted the stream",
        ).to_exception()

    screen = ChatScreen(
        history=InMemoryHistory(),
        completer=None,
        prompt=lambda: HTML("&gt; "),
        toolbar=lambda: HTML("status"),
        submit=failed_submit,
        cancel=lambda: None,
    )

    await screen._accept("/status")

    assert "error: The peer aborted the stream" in screen._plain_transcript()


def test_repeated_peer_abort_is_one_inline_disconnect_error():
    screen = _screen()
    loop = asyncio.new_event_loop()
    error = Status(
        code=StatusCode.FAILED_PRECONDITION,
        message="The peer aborted the stream",
    ).to_exception()
    try:
        screen._handle_loop_exception(loop, {"exception": error})
        screen._handle_loop_exception(loop, {"exception": error})
    finally:
        loop.close()

    assert screen._plain_transcript().count("gateway disconnected") == 1
