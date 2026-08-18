# Copyright 2026 The A11 Authors.

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
