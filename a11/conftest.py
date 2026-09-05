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

import os

import pytest

import a11


@pytest.fixture(scope="session", autouse=True)
def init_logging():
    os.environ["A11_DEBUG"] = "1"
    a11.enable_logging("debug")
    a11.get_logger(__name__).info("Logging initialized")


@pytest.fixture(scope="session", autouse=True)
def fiber_watchdog():
    """Report parked fibers when A11_FIBER_WATCHDOG is set.

    A hung test otherwise reaches the harness timeout with nothing to look at:
    the frames that explain it belong to fibers no OS thread points at. Set
    ``A11_FIBER_WATCHDOG=<seconds>``, and ``A11_FIBER_WATCHDOG_ABORT=1`` to fail
    the run instead of waiting. See ``a11.debug``.
    """
    seconds = os.environ.get("A11_FIBER_WATCHDOG")
    if seconds is None:
        yield
        return
    import a11.debug

    a11.debug.install_fiber_watchdog(
        stall_threshold_seconds=float(seconds),
        abort_on_stall=os.environ.get("A11_FIBER_WATCHDOG_ABORT", "0") != "0",
    )
    yield
