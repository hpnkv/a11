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

"""One module per component; importing a module registers its benchmarks."""

from __future__ import annotations

import importlib

#: In dependency order, roughly bottom of the stack upwards. The order is the
#: order results appear in, which is the order somebody reads them in.
SUITE_MODULES = (
    "runtime",
    "data",
    "stores",
    "nodes",
    "wire",
    "actions",
    "service",
    "flow",
    "workload",
    # Both need a `bench.peer` agent on another host and skip without one, so
    # they go last: a default run on one machine ends with two skip lines
    # rather than starting with them.
    "link",
    "server",
    "scale",
)


def load_all() -> None:
    for name in SUITE_MODULES:
        importlib.import_module(f"bench.suites.{name}")
