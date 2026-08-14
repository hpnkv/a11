# Copyright 2026 The A11 Authors.

"""Teach the documentation build to colour A11 Flow.

MkDocs highlights a fenced block by asking Pygments for a lexer with that name,
and Pygments only knows the lexers it ships with plus the ones installed as
entry points. A11 is not a Pygments plugin and has no reason to become one, so
this hook puts the language's own lexer into the registry Pygments looks in --
after which ```a11flow works in any page like any other language.

The lexer itself is at ``editors/pygments/a11flow_lexer.py`` and is
**generated** from the language's word tables
(`a11 flow syntax --target pygments`), which is the same arrangement the
Sublime grammar has: see ``editors/README.md``. So a
stage added to the grammar is a stage these pages colour once somebody has run
the generator, and `a11/flow/tests/test_editor_support.py` fails while nobody
has.

Registered rather than imported by name because Pygments resolves an alias
through ``LEXERS``, whose entries name a module to import. Ours is not on the
import path of whoever builds these docs, so it is put in the cache the loader
checks first and the module name is never used.
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
