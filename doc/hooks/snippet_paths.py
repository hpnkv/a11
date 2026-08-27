# Copyright 2026 The A11 Authors.

"""Resolve documentation snippets relative to the repository.

A page embeds a file from the checkout with `--8<-- "path/to/file"`, and
`pymdownx.snippets` resolves `base_path` from the current working directory.
Converting the configured path to an absolute repository path keeps both
``doc/build.sh`` and ``mkdocs build -f doc/mkdocs.yml`` consistent.
"""

import pathlib

#: Directory containing ``mkdocs.yml`` and the base for its relative paths.
CONFIG_DIR = pathlib.Path(__file__).resolve().parents[1]

#: The extension whose paths are being anchored.
EXTENSION = "pymdownx.snippets"


def on_config(config):
    """Make `base_path` absolute before any page is read."""
    settings = config.get("mdx_configs", {}).get(EXTENSION)
    if settings is None:
        return config
    settings["base_path"] = [
        str((CONFIG_DIR / entry).resolve())
        for entry in settings.get("base_path", [])
    ]
    return config
