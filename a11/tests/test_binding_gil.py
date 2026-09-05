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
