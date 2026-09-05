#!/usr/bin/env python3
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
