"""Logging configuration, resolved from the process A11 is imported into.

Most of these assert what ``import a11`` decides, which a single interpreter
can only answer once, so each case runs its own subprocess.
"""

from __future__ import annotations

import logging
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

import a11

ROOT = Path(__file__).resolve().parents[2]

# Abseil severities as the native side numbers them.
_NATIVE_INFO = 0
_NATIVE_ERROR = 2


def _run(body: str, **env: str) -> subprocess.CompletedProcess[str]:
    """Run ``body`` in a fresh interpreter and return its result."""
    import os

    return subprocess.run(
        [sys.executable, "-c", textwrap.dedent(body)],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        timeout=120,
        env={**os.environ, **env},
    )


def _level(body: str = "", **env: str) -> int:
    # Dedent first: the caller's block and the lines appended here have to
    # share an indentation level before textwrap.dedent sees them.
    prelude = textwrap.dedent(body)
    result = _run(
        f"{prelude}\nimport a11\nprint('LEVEL', a11.get_log_level())", **env
    )
    assert result.returncode == 0, result.stderr
    line = next(
        item for item in result.stdout.splitlines() if item.startswith("LEVEL ")
    )
    return int(line.split()[1])


def test_bare_import_stays_at_the_python_default() -> None:
    result = _run("import a11\nprint('LEVEL', a11.get_log_level())")

    assert result.returncode == 0, result.stderr
    assert "LEVEL 30" in result.stdout
    assert result.stderr == ""


def test_absl_verbosity_set_before_import_wins() -> None:
    assert (
        _level("from absl import logging as l\nl.set_verbosity(l.DEBUG)")
        == logging.DEBUG
    )


def test_stdlib_configuration_is_adopted() -> None:
    assert (
        _level("import logging\nlogging.basicConfig(level=logging.DEBUG)")
        == logging.DEBUG
    )


def test_absl_outranks_the_stdlib() -> None:
    body = """
        import logging
        logging.basicConfig(level=logging.ERROR)
        from absl import logging as l
        l.set_verbosity(l.DEBUG)
    """
    assert _level(body) == logging.DEBUG


def test_environment_applies_only_when_nothing_else_is_configured() -> None:
    assert _level(A11_LOG_LEVEL="debug") == logging.DEBUG
    assert (
        _level(
            "import logging\nlogging.basicConfig(level=logging.ERROR)",
            A11_LOG_LEVEL="debug",
        )
        == logging.ERROR
    )


def test_native_entries_arrive_as_python_records() -> None:
    result = _run(
        """
        import logging
        logging.basicConfig(
            level=logging.DEBUG, format="%(name)s|%(levelname)s|%(message)s"
        )
        import a11
        a11._native.emit_log(2, "native failure")
        """
    )

    assert result.returncode == 0, result.stderr
    assert "a11.native|ERROR|native failure" in result.stderr


def test_a_handler_filter_suppresses_a_native_record() -> None:
    result = _run(
        """
        import logging
        logging.basicConfig(level=logging.DEBUG, format="%(message)s")

        class Drop(logging.Filter):
            def filter(self, record):
                return "secret" not in record.getMessage()

        logging.getLogger().handlers[0].addFilter(Drop())
        import a11
        a11._native.emit_log(2, "visible")
        a11._native.emit_log(2, "secret")
        """
    )

    assert result.returncode == 0, result.stderr
    assert "visible" in result.stderr
    assert "secret" not in result.stderr


def test_reconfiguring_after_import_takes_effect_without_a_resync() -> None:
    result = _run(
        """
        import a11
        a11._native.emit_log(0, "before")
        import logging
        logging.basicConfig(level=logging.DEBUG, format="%(message)s")
        logging.getLogger("a11").setLevel(logging.DEBUG)
        a11._native.emit_log(0, "after")
        """
    )

    assert result.returncode == 0, result.stderr
    assert "before" not in result.stderr
    assert "after" in result.stderr


def test_vlog_follows_a_level_below_debug() -> None:
    result = _run(
        """
        import logging
        logging.basicConfig(level=logging.DEBUG - 1, format="%(message)s")
        import a11
        a11.set_log_level(logging.DEBUG - 1)
        a11._native.emit_log(0, "verbose two", 2)
        """
    )

    assert result.returncode == 0, result.stderr
    assert "verbose two" in result.stderr


def test_the_bridge_can_be_turned_off() -> None:
    result = _run(
        """
        import logging
        logging.basicConfig(level=logging.DEBUG, format="PY|%(message)s")
        import a11
        a11.set_log_level(logging.INFO)
        a11._native.emit_log(2, "straight to stderr")
        """,
        A11_LOG_BRIDGE="0",
    )

    assert result.returncode == 0, result.stderr
    # Abseil's own prefix, and no Python formatting around it.
    assert "straight to stderr" in result.stderr
    assert "PY|" not in result.stderr
    assert "] straight to stderr" in result.stderr


def test_enable_installs_the_absl_handler_only_when_root_has_none() -> None:
    adopted = _run(
        """
        import logging
        logging.basicConfig(level=logging.INFO, format="APP %(message)s")
        import a11
        a11.enable_logging("info")
        a11._native.emit_log(0, "native")
        """
    )
    assert adopted.returncode == 0, adopted.stderr
    assert "APP native" in adopted.stderr

    installed = _run(
        """
        import a11
        a11.enable_logging("info")
        a11._native.emit_log(0, "native")
        """
    )
    assert installed.returncode == 0, installed.stderr
    # Abseil's shape: severity letter, timestamp, source location, then text.
    assert "] native" in installed.stderr
    assert installed.stderr.startswith("I")


def test_disable_silences_both_sides() -> None:
    result = _run(
        """
        import a11
        a11.enable_logging("debug")
        a11.disable_logging()
        a11._native.emit_log(2, "must not appear")
        a11.get_logger("x").error("nor this")
        print("done")
        """
    )

    assert result.returncode == 0, result.stderr
    assert "done" in result.stdout
    assert "must not appear" not in result.stderr
    assert "nor this" not in result.stderr


@pytest.mark.parametrize(
    ("value", "expected"),
    [
        (logging.DEBUG, logging.DEBUG),
        ("debug", logging.DEBUG),
        ("WARNING", logging.WARNING),
        ("15", 15),
    ],
)
def test_parse_level_accepts_names_numbers_and_strings(
    value: int | str, expected: int
) -> None:
    assert a11.logging.parse_level(value) == expected


@pytest.mark.parametrize("value", ["", "chatty", None])
def test_parse_level_rejects_nonsense(value: object) -> None:
    with pytest.raises((TypeError, ValueError)):
        a11.logging.parse_level(value)  # type: ignore[arg-type]


def test_get_logger_keeps_names_under_the_a11_namespace() -> None:
    assert a11.get_logger().name == "a11"
    assert a11.get_logger("gateway").name == "a11.gateway"
    assert a11.get_logger("a11.gateway").name == "a11.gateway"


def test_native_records_reach_caplog(
    caplog: pytest.LogCaptureFixture,
) -> None:
    with caplog.at_level(logging.INFO, logger="a11.native"):
        a11._native.emit_log(_NATIVE_INFO, "captured info")
        a11._native.emit_log(_NATIVE_ERROR, "captured error")

    assert "captured info" in caplog.text
    assert "captured error" in caplog.text
    assert {record.levelno for record in caplog.records} >= {
        logging.INFO,
        logging.ERROR,
    }
