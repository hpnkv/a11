# repo_search

Search one query across the A11 monorepo and return bounded matching lines
from each tree: the Python contract (`a11/`), the native implementation
(`cpp/`), and the docs (`doc/`). The three searches run concurrently.

## When to use

Locate where a symbol, API, or concept lives before editing. The Python API
and its C++ implementation are kept in step, so a change usually involves more
than one tree, and the docs state the intent behind both.

## Inputs

- `QUERY` (string, required): text to search for, matched as plain text, not a
  regex.

## Outputs

- `python`: matching lines from `a11/`, at most 25, each truncated to 240
  characters.
- `native`: matching lines from `cpp/`, at most 25, each truncated to 240
  characters.
- `docs`: matching lines from `doc/`, at most 15, each truncated to 240
  characters.
