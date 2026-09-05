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

"""Console entry point for the ``a11`` command (and ``python -m a11.cli``)."""

from __future__ import annotations

import sys

from a11 import allocator
from a11.cli.app import main


def _relaunch_argv() -> list[str]:
    """Arguments that re-run this command under a fresh interpreter.

    ``python -m a11.cli`` has to be reconstructed as ``-m`` rather than as the
    path of this file, or the relaunched process gets a different
    ``sys.path[0]`` than the one it was started with.
    """
    if __package__ and sys.argv and sys.argv[0].endswith("__main__.py"):
        return ["-m", __package__, *sys.argv[1:]]
    return list(sys.argv)


def _entry() -> int:
    # Before anything else: swapping the C library's allocator is worth ~25% to
    # A11's native throughput, and it can only be done by launching the process
    # with the library preloaded. This is A11's own process, so A11 can relaunch
    # it -- once, guarded, and skipped entirely if it is already active, already
    # attempted, unavailable on this platform, or switched off with
    # A11_NO_ALLOCATOR_PRELOAD. See a11/allocator.py for why the extension
    # cannot simply do it in-process.
    allocator.reexec_with_preload(_relaunch_argv())
    return main(sys.argv[1:])


if __name__ == "__main__":
    raise SystemExit(_entry())
