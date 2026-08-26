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
