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

"""Start the A11 CLI before importing the Python runtime API."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


def _relaunch_argv() -> list[str]:
    original = getattr(sys, "orig_argv", None)
    if original:
        return list(original[1:])
    if sys.argv and sys.argv[0].endswith("__main__.py"):
        return ["-m", "a11_cli", *sys.argv[1:]]
    return list(sys.argv)


def _preload_allocator() -> None:
    package = importlib.util.find_spec("a11")
    if package is None or package.origin is None:
        return
    path = Path(package.origin).with_name("allocator.py")
    spec = importlib.util.spec_from_file_location("_a11_cli_allocator", path)
    if spec is None or spec.loader is None:
        return
    allocator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(allocator)
    allocator.reexec_with_preload(_relaunch_argv())


def _entry() -> int:
    """Initialize the allocator, then load and dispatch the selected command."""
    _preload_allocator()
    from a11_cli.app import main

    return main(sys.argv[1:])
