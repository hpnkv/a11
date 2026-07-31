# Copyright 2026 The A11 Authors.

"""Console entry point for the ``a11`` command (and ``python -m a11.cli``)."""

from __future__ import annotations

import sys

from a11.cli.app import main


def _entry() -> int:
    return main(sys.argv[1:])


if __name__ == "__main__":
    raise SystemExit(_entry())
