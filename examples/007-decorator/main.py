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

"""One flow, registered as an action.

    a11 serve main.py

Two ways to put an action in a registry, and this file has one of each. A
function becomes an action through its signature -- `@REGISTRY.action` reads the
annotations and derives the schema and the handler from them. A flow needs
neither: it declares its own ports and *is* its own handler, so `REGISTRY.flow`
takes the text and nothing else.

`client.py` calls `greet` from another process.
"""

import a11

REGISTRY = a11.ActionRegistry()

REGISTRY.flow("""
flow greet {
  describe "Say hello to somebody."

  in  name:  string required "Who to greet."
  out reply: string stream
  
  nodes scratch
  scratch_reply = node() in scratch

  "Hello, " then name then "!" -> scratch_reply
  log info "done" after scratch_reply
  
  scratch_reply -> reply
}
""")


@REGISTRY.action(name="shout")
async def shout(text: str) -> str:
    """Say it louder."""
    return text.upper()
