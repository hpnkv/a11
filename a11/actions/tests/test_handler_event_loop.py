"""A Python handler may be registered before any event loop exists.

A11 resolves the asyncio loop when the handler runs, not when it is registered.
This supports ordinary module-level registration, including
`@registry.action` at import time, before an application creates its loop.

These tests run in subprocesses to observe behaviour before the first process
loop exists; pytest-asyncio has already created one in the test process.
"""

from __future__ import annotations

import subprocess
import sys
import textwrap

import pytest

#: Long enough that a wrong-loop post shows up as a failure, short enough that a
#: broken build does not stall the suite.
_TIMEOUT_SECONDS = 60

_PREAMBLE = """
import asyncio
import a11
from a11.actions import ActionRegistry

registry = ActionRegistry()

@registry.action
async def shout(text: str) -> str:
    \"\"\"Shout a line.\"\"\"
    return text.upper()

async def drive():
    action = registry.make_action("shout")
    await action.get_input("text", bind_stream=False).finalize("hi")
    action.run()
    await asyncio.wait_for(action.wait(), 10)
    return await action["output"].consume(str)
"""


def _run(body: str) -> str:
    """Run ``body`` after the preamble in a fresh interpreter."""
    source = textwrap.dedent(_PREAMBLE) + textwrap.dedent(body)
    finished = subprocess.run(
        [sys.executable, "-c", source],
        capture_output=True,
        text=True,
        timeout=_TIMEOUT_SECONDS,
    )
    assert finished.returncode == 0, finished.stderr
    return finished.stdout.strip()


def test_a_handler_registered_before_any_loop_still_runs() -> None:
    """A handler registered at import time runs under ``asyncio.run``."""
    assert _run("print(asyncio.run(drive()))") == "HI"


def test_registration_outside_a_loop_warns_about_nothing() -> None:
    """Registration outside a loop does not ask asyncio to create one.

    `asyncio.get_event_loop` inventing a loop is a DeprecationWarning on the
    Pythons that still allow it, which makes the absence of one the sharpest
    available assertion that nothing was invented.
    """
    output = _run(
        """
        import warnings
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            fresh = ActionRegistry()
            fresh.register("shout", shout.action_schema, shout.action_handler)
        print([str(w.message) for w in caught])
        print(asyncio.run(drive()))
        """
    )
    assert output.splitlines() == ["[]", "HI"]


def test_a_loop_set_but_not_running_is_still_honoured() -> None:
    """`set_event_loop` then `run_until_complete`: a loop never seen running."""
    assert (
        _run(
            """
            loop = asyncio.new_event_loop()
            asyncio.set_event_loop(loop)
            print(loop.run_until_complete(drive()))
            """
        )
        == "HI"
    )


def test_one_registration_serves_two_successive_loops() -> None:
    """A resolved loop is re-checked, so a closed one is not posted to twice."""
    assert (
        _run(
            """
            print(asyncio.run(drive()))
            print(asyncio.run(drive()))
            """
        )
        == "HI\nHI"
    )


def test_a_handler_registered_inside_a_loop_is_unaffected() -> None:
    """Registration inside a running loop captures that loop eagerly."""
    assert (
        _run(
            """
            async def main():
                local = ActionRegistry()

                @local.action
                async def whisper(text: str) -> str:
                    \"\"\"Whisper a line.\"\"\"
                    return text.lower()

                action = local.make_action("whisper")
                await action.get_input("text", bind_stream=False).finalize("Y")
                action.run()
                await asyncio.wait_for(action.wait(), 10)
                return await action["output"].consume(str)
            print(asyncio.run(main()))
            """
        )
        == "y"
    )


def test_with_no_loop_at_all_the_handler_fails_instead_of_hanging() -> None:
    """An unanswerable question is an error, not a wait.

    Awaiting completion needs a loop, so this test polls after removing the
    loop. ``run()`` is fire-and-forget; the action must still end with a useful
    failure.
    """
    output = _run(
        """
        import time
        loop = asyncio.new_event_loop()
        action = registry.make_action("shout")
        loop.run_until_complete(
            action.get_input("text", bind_stream=False).finalize("hi")
        )
        loop.close()
        asyncio.set_event_loop(None)
        action.run()
        deadline = time.monotonic() + 10
        while not action.is_done() and time.monotonic() < deadline:
            time.sleep(0.01)
        status = action.get_status()
        explained = "event loop" in status.message
        print(action.is_done(), status.code.name, explained)
        """
    )
    assert output == "True FAILED_PRECONDITION True"


@pytest.mark.asyncio
async def test_registration_inside_a_running_loop_needs_no_subprocess() -> None:
    """Exercise registration inside a running loop in-process."""
    from a11.actions import ActionRegistry

    registry = ActionRegistry()

    @registry.action
    async def shout(text: str) -> str:
        """Shout a line."""
        return text.upper()

    action = registry.make_action("shout")
    await action.get_input("text", bind_stream=False).finalize("hi")
    action.run()
    await action.wait()
    assert await action["output"].consume(str) == "HI"
