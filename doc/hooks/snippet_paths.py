# Copyright 2026 The A11 Authors.

"""Anchor snippet paths to the repository rather than to the shell.

A page embeds a file from the checkout with `--8<-- "path/to/file"`, and
`pymdownx.snippets` resolves that against its `base_path` -- which it resolves
against the **current working directory**. Written in ``mkdocs.yml`` as ``..``
that means "the repository" only while mkdocs is run from ``doc/``, which
``doc/build.sh`` does and a person typing ``mkdocs build -f doc/mkdocs.yml``
does not. The failure is a hard error naming a file that is plainly there.

So the relative entry is replaced with the absolute path it was always meant to
be, worked out from where this file is. Every cwd then reads the same snippet.
"""

import pathlib

#: The directory ``mkdocs.yml`` is in, which is what its relative paths mean.
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
