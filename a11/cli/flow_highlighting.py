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

"""Register the generated A11 Flow lexer for terminal Markdown."""

from __future__ import annotations


def register_flow_lexer() -> None:
    """Make ``a11flow`` fences resolve through Pygments and Rich."""
    from pygments.lexers import LEXERS, _lexer_cache

    from editors.pygments.a11flow_lexer import A11FlowLexer

    LEXERS[A11FlowLexer.__name__] = (
        "editors.pygments.a11flow_lexer",
        A11FlowLexer.name,
        tuple(A11FlowLexer.aliases),
        tuple(A11FlowLexer.filenames),
        tuple(A11FlowLexer.mimetypes),
    )
    _lexer_cache[A11FlowLexer.name] = A11FlowLexer


__all__ = ["register_flow_lexer"]
