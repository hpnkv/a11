#!/usr/bin/env python3
"""Validate a release tag against its component's version source."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION_RE = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+(?:[a-zA-Z0-9.+-]*)?")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("component", choices=("a11", "npm"))
    parser.add_argument("tag")
    args = parser.parse_args()

    prefix = f"{args.component}-v"
    if not args.tag.startswith(prefix):
        raise SystemExit(f"release tag must match {prefix}<version>")
    tagged_version = args.tag.removeprefix(prefix)
    if VERSION_RE.fullmatch(tagged_version) is None:
        raise SystemExit(f"invalid release version: {tagged_version}")

    if args.component == "a11":
        source_version = (ROOT / "VERSION").read_text().strip()
    else:
        package = json.loads((ROOT / "js/package.json").read_text())
        source_version = package["version"]

    if tagged_version != source_version:
        raise SystemExit(
            f"tag version {tagged_version!r} does not match "
            f"{args.component} version {source_version!r}"
        )

    print(source_version)


if __name__ == "__main__":
    main()
