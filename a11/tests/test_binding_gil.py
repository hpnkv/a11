# Copyright 2026 The A11 Authors.

"""Verify that bindings release the GIL around native calls that can park.

`scripts/check_binding_gil.py` reads the sources without a native build. This
test runs that audit.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys

import pytest

_ROOT = pathlib.Path(__file__).resolve().parents[2]
_CHECKER = _ROOT / "scripts" / "check_binding_gil.py"


@pytest.mark.skipif(
    not (_ROOT / "cpp" / "python").is_dir(),
    reason="binding sources are absent from an installed package",
)
def test_no_binding_holds_the_gil_into_the_fiber_runtime() -> None:
    finished = subprocess.run(
        [sys.executable, str(_CHECKER), str(_ROOT)],
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert finished.returncode == 0, finished.stdout + finished.stderr
