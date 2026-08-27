# Copyright 2026 The A11 Authors.

"""Register A11 Flow syntax highlighting for the documentation build.

MkDocs resolves fenced-code language names through Pygments. This hook adds
A11's generated lexer to the Pygments registry, enabling ```a11flow blocks.

The lexer itself is at ``editors/pygments/a11flow_lexer.py`` and is
**generated** from the language's word tables
(`a11 flow syntax --target pygments`); see ``editors/README.md``. The editor
support tests require regeneration after vocabulary changes.

Pygments resolves aliases through ``LEXERS`` module entries. The documentation
build does not install this lexer as a module, so the hook inserts it into the
loader cache.
"""

import pathlib
import sys

#: The generated lexer, which lives with the other editor definitions.
LEXER_DIR = pathlib.Path(__file__).resolve().parents[2] / "editors" / "pygments"


def on_config(config):
    """Register the Flow lexer before any page is rendered."""
    if str(LEXER_DIR) not in sys.path:
        sys.path.insert(0, str(LEXER_DIR))

    from pygments.lexers import LEXERS, _lexer_cache

    from a11flow_lexer import A11FlowLexer

    LEXERS[A11FlowLexer.__name__] = (
        "a11flow_lexer",
        A11FlowLexer.name,
        tuple(A11FlowLexer.aliases),
        tuple(A11FlowLexer.filenames),
        tuple(A11FlowLexer.mimetypes),
    )
    _lexer_cache[A11FlowLexer.name] = A11FlowLexer
    return config
