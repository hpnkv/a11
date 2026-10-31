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

"""Load the compact Flow reference for agent system prompts."""

from functools import cache
from importlib.resources import files


@cache
def short_reference() -> str:
    """Return the packaged Markdown reference without changing its contents."""
    return (
        files("a11.flow")
        .joinpath("reference_short.md")
        .read_text(encoding="utf-8")
    )
