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
from prompt_toolkit.mouse_events import MouseButton, MouseEvent, MouseEventType

from a11.cli.chat_screen import ChatScreen
from a11.status import Status, StatusCode


async def _submit(_: str) -> bool:
    return True


def _screen() -> ChatScreen:
    return ChatScreen(
        history=InMemoryHistory(),
        completer=None,
        prompt=lambda: HTML("&gt; "),
        toolbar=lambda: HTML("status"),
        submit=_submit,
        cancel=lambda: None,
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


def test_composer_has_a_stable_three_row_minimum_height():
    screen = _screen()

    dimension = screen._composer.preferred_height(
        width=100, max_available_height=30
    )

    assert dimension.min >= 3


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
