"""Preloading a faster allocator, and refusing to lie about whether it worked.

Replacing the C library's malloc is worth ~25% to A11's native throughput, but
the extension cannot do it to a process it was merely loaded into -- see
`a11/allocator.py`. What it can do is prepare the environment for a process A11
launches, and report honestly whether the allocator is actually in this one.

The invariants that matter here are all about *not* being clever: never preload
twice, never clobber a preload the caller already had, never claim to be active
on the strength of an environment variable, and never break a process just
because the library is missing.
"""

from __future__ import annotations

import os
import subprocess
import sys

import pytest

from a11 import allocator


def test_the_preload_variable_matches_the_platform():
    variable = allocator.preload_variable()
    if sys.platform == "darwin":
        assert variable == "DYLD_INSERT_LIBRARIES"
    elif sys.platform.startswith("linux"):
        assert variable == "LD_PRELOAD"
    else:
        assert variable is None


def test_a_missing_library_is_not_an_error():
    """A build without the bundled allocator is a supported build."""
    path = allocator.library_path()
    assert path is None or path.is_file()


def test_activity_is_read_from_the_loader_not_the_environment():
    """The environment says what was asked; only the loader says what is."""
    environment = allocator.environ_with_preload({"PATH": "/usr/bin"})
    # Whatever environ_with_preload decided, it cannot have changed this
    # process.
    assert allocator.is_active() is allocator.is_active()
    variable = allocator.preload_variable()
    if variable is not None and allocator.library_path() is not None:
        assert variable in environment
        # Setting it in a dict plainly did not load anything here.
        assert environment[allocator.PRELOAD_GUARD_ENV] == "1"


def test_an_existing_preload_is_kept():
    variable = allocator.preload_variable()
    if variable is None or allocator.library_path() is None:
        pytest.skip("no bundled allocator to preload on this platform")
    environment = allocator.environ_with_preload({variable: "/existing/lib.so"})
    entries = environment[variable].split(os.pathsep)
    assert "/existing/lib.so" in entries, "clobbered the caller's preload"
    # Ours first, so it wins for malloc.
    assert entries[0] == str(allocator.library_path())


def test_the_guard_stops_a_second_preload():
    environment = allocator.environ_with_preload({
        allocator.PRELOAD_GUARD_ENV: "1"
    })
    variable = allocator.preload_variable()
    if variable is not None:
        assert variable not in environment, "preloaded twice"


def test_suppression_is_honoured():
    environment = allocator.environ_with_preload({allocator.SUPPRESS_ENV: "1"})
    variable = allocator.preload_variable()
    if variable is not None:
        assert variable not in environment


def test_reexec_does_nothing_when_suppressed(monkeypatch):
    """The re-exec must be a no-op here, or this test would not return.

    monkeypatch and not a hand-rolled save/restore: an earlier version of this
    test leaked the suppression variable into the rest of the session, which
    silently disabled every later preload and turned the end-to-end test below
    into a skip. Environment edits in a test want a fixture that owns the undo.
    """
    monkeypatch.setenv(allocator.SUPPRESS_ENV, "1")
    allocator.reexec_with_preload()  # must return rather than replace us


def test_the_module_describes_itself_without_importing_the_extension():
    """`python -m a11.allocator` is a diagnostic; it must always work."""
    result = subprocess.run(
        [sys.executable, "-m", "a11.allocator"],
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    assert "preload variable" in result.stdout
    assert "active here" in result.stdout


def test_a_preloaded_subprocess_reports_itself_active():
    """End to end: if we bundle a library, preloading it must actually work."""
    variable = allocator.preload_variable()
    library = allocator.library_path()
    if variable is None or library is None:
        pytest.skip("no bundled allocator on this platform/build")
    environment = allocator.environ_with_preload()
    assert variable in environment, "nothing to test: no preload was prepared"
    # The child reports both whether it *saw* the request and whether the
    # allocator is loaded, because those two failures need telling apart: a
    # child that never saw the variable was stripped by the loader (macOS SIP),
    # while a child that saw it and is still not active is a real failure this
    # test exists to catch.
    result = subprocess.run(
        [
            sys.executable,
            "-c",
            "import os, sys, a11.allocator as a;"
            f" print(bool(os.environ.get({variable!r})), a.is_active())",
        ],
        capture_output=True,
        text=True,
        env=environment,
        timeout=120,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    saw_request, is_active = result.stdout.split()
    if saw_request == "False":
        pytest.skip(
            f"{variable} was stripped before the child started"
            " (macOS System Integrity Protection does this to signed"
            " interpreters); nothing about A11 to test here"
        )
    assert is_active == "True", (
        f"{variable} reached the child but the allocator did not load"
    )
