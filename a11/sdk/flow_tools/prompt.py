# Copyright 2026 The A11 Authors.

"""The instructions that teach a model when and how to compose its tools.

One text, in two shapes:
[get_system_prompt][a11.sdk.flow_tools.prompt.get_system_prompt] for a host
that composes a system prompt out of parts (the way ``a11 chat`` does with the
shell tools), and [get_skill][a11.sdk.flow_tools.prompt.get_skill] for one that
loads ``a11.sdk.skill.Skill``s. The language reference is not written out again
here -- it is ``a11.flow.REFERENCE``, the same cheat sheet [the language
ships][a11.flow] -- so the instructions cannot describe a Flow that the
compiler does not accept.

``SKILL.md`` beside this module is generated from these constants and checked
in, so a host that reads skills off disk gets the same words. The test suite
fails when the file and the constants disagree.
"""

from __future__ import annotations

import pathlib
from typing import TYPE_CHECKING

from a11 import flow

if TYPE_CHECKING:
    from a11.sdk.skill import Skill

#: The generated ``SKILL.md``, for a host that loads skills from disk.
SKILL_MD_PATH = pathlib.Path(__file__).with_name("SKILL.md")

#: The skill's name, in the SKILL.md sense: lowercase, hyphenated, stable.
SKILL_NAME = "a11-flow"

#: What the skill is for, and when to reach for it.
SKILL_DESCRIPTION = (
    "Compose the tools you already have into one A11 Flow and run it as a"
    " single step, so the values that move between them never pass through"
    " your context. Use it when a task needs several tools and each one's"
    " output feeds the next, when the same shape of work repeats over a list"
    " of things, or when an intermediate result is large and you only need"
    " what it reduces to. The tools are flow_actions, flow_check and flow_run."
)

_BODY = """\
# Composing your tools into one step

You have three tools — `flow_actions`, `flow_check`, `flow_run` — that let \
you write a *flow*: a composition of the actions you can already call, \
dispatched as one step. A flow is text, in the A11 Flow language, and you \
write it in the moment for the task in front of you. There is nothing to \
install and nothing to deploy.

## Why bother

Calling three tools one after another means every intermediate result passes \
through you: you read it, then you quote it into the next call. A flow moves \
those values directly between the actions. Fetch four pages, summarise them, \
and what comes back to you is the summary — the pages were never in your \
context, were never charged for, and were never yours to copy correctly.

Reach for a flow when:

- **Data flows tool to tool.** The output of one call is the input of the \
next, and you do not otherwise need to see it.
- **The same shape repeats.** The same two or three calls for each item in a \
list. `for` runs a block per value, `parallel n` runs several at a time.
- **An intermediate is large.** A page, a transcript, a file listing, a \
recording. Trim it inside the flow with `| truncate`, `| first`, `| where`, \
and return what it reduces to.
- **Steps should overlap.** Steps in a flow run concurrently; order comes from \
the data, not from the order you wrote them in. A summariser is dispatched at \
once and reads pages as they arrive.

Do not reach for one when a single tool call does the job, or when you need to \
*decide* something between two calls: a flow cannot ask you a question in the \
middle. Read what it returns and run another one.

## How to write one

1. **`flow_actions`** first, unless you already did it this turn. It lists \
every action a flow may name and — this is the part your tool definitions do \
not tell you — what each one's **output ports** are called, and whether it is \
`runnable`. You need port names on both sides of a `->`, and you need the verb.
2. **Write the flow.** Declare inputs you will pass and outputs you want back.
3. **`flow_check`** it. It compiles the flow and describes what it resolves to \
without dispatching anything, and reports a syntax error with the line and \
column. Use it whenever the flow is more than a couple of lines, or whenever \
running it would have real effects, because running one really does call the \
actions it names.
4. **`flow_run`** it, passing `source` and any `inputs` (an object keyed by \
port name — a list for a port declared `stream`, a single value for any \
other). What comes back is an object of the flow's declared outputs.

## What will bite you

- **Declare small outputs.** The flow's outputs are what you read. If you \
declare an output that carries every fetched page, you have paid for the pages \
after all. Return the answer, the count, the three best hits.
- **`run` what is `runnable`, `call` what is not.** The two verbs are two \
different things: `run` executes the action where the flow is running, `call` \
puts it on the stream the flow is attached to and lets the peer do it. \
`flow_actions` marks each one, and almost everything it offers you is \
`runnable: true` — so `run` is the usual verb, and a `run` of something \
without a handler is refused rather than quietly sent elsewhere.
- **You may only name actions you may name.** A flow naming anything else is \
refused before it runs. `flow_actions` is the list.
- **Every output port of every step is read.** You do not have to name them \
all — the runtime drains what you ignore — but `skip x.debug` says so \
plainly, and is worth writing when an output is large. `skip 1 x.rows` is the \
other one: it drops a port's first value for *every* reader, which is how you \
throw away a header line, and several of them naming one port add up.
- **A failing step ends the flow** unless you wrote `try`. When a failure is \
one the composition should handle, write `try run` (or `try call`) and then \
`wait` to find out how it went.
- **Give a port the type it wants.** When an action's input is a real type \
rather than a bag of keys, write `a11.sdk.Interaction{{role: "user", content: \
[...]}}` (or `{{...}} as a11.sdk.Interaction`) and it is validated into that \
type, defaults and all. `to_chunk(value)` makes the `Chunk` such a type's \
content is made of. Which tags exist is the host's — ask `flow_actions` what \
the ports are and match them.
- **A stream of fragments is not a stream of things.** `| group EXPR` gathers \
values until `EXPR` holds of the one just added, then hands over the list: \
`| group ends-with(trim(it), [".", "?"]) | map trim(join(it, " "))` is partial \
utterances becoming whole sentences. Reach for it whenever one value is only \
part of what you need.
- **Order between two streams is `| then`.** Two statements writing to the \
same port interleave by arrival. When one lot has to come before another — a \
conversation's history before the new message — write \
`history | then asked -> port` and the flow reads them in that order.
- **No arithmetic, no functions, no code.** Expressions read values, compare \
them, take them apart, and build new ones. That is the whole of it. If the \
task needs real computation, that is what an action is for.
- **Say when a loop stops.** A `repeat` needs an `until`/`while`, or a `max n`, or both. There is no default bound: a loop with neither is \
refused rather than quietly stopping after some number of passes and \
calling that success.
- **`fail`, `cancel` and `log` go in an `if`.** They wait for nothing, so at \
the top of a flow's body they run at once and race every other \
statement. Put one in an `if` or a loop body, or write `fail internal \
"..." after x` to say what it waits for. A `fail` alone at the end of a \
body reads like a last resort and is refused, because it is the first \
thing that would happen.
- **`log` needs no port.** `log "searching" after plan` and `logf "found %s" \
n after search` write to a log the flow already has: nothing declares it, \
nothing drains it, and it is not one of the outputs you pay for. As a stage, \
`| log` and `| logf "saw %s" it` say what is going past and pass every value \
on unchanged, which is the way to see into a pipeline without changing it.

## The language

{reference}

## An example

The action names below are examples — use the ones `flow_actions` gives you.

```
flow answer-from-the-web {{
  describe "Search, read the best hits, and answer from them."

  in  question: string required
  out answer:   string
  out sources:  string stream

  search = run web-search(query: question, limit: 3)
  brief  = run summarize(question: question)

  nodes fetched {{
    for hit in search.hits parallel 2 {{
      page = try run web-fetch(url: hit.url)
      hit.url -> sources
      page.text | truncate 2000 -> brief.pages
      skip page.bytes
    }}
  }}

  brief.summary -> answer
  skip search.debug
}}
```

Run that with `inputs` of `{{"question": "..."}}` and you get back \
`{{"answer": "...", "sources": ["...", "..."]}}` — two small values, out of \
three pages you never had to read. The `nodes fetched` block is what keeps the \
pages off the wire; without it they would be replicated to whoever dispatched \
the composition.
"""


def get_system_prompt() -> str:
    """Return instructions telling the model it can compose its tools.

    The text explains when a flow beats a sequence of tool calls, the order to
    use the three tools in, and the handful of rules that decide whether a
    composition works -- and embeds ``a11.flow.REFERENCE``, so the language it
    describes is the language that is implemented.
    """
    return _BODY.format(reference=f"```\n{flow.REFERENCE.strip()}\n```")


def get_skill() -> "Skill":
    """The same instructions as an ``a11.sdk.skill.Skill``.

    Imported where it is used rather than at the top of the module:
    ``a11.sdk.skill`` needs PyYAML for the frontmatter, and the three tools
    themselves do not, so a host that only wants the prompt text pays nothing
    for a dependency it will not use.
    """
    from a11.sdk.skill import Skill

    return Skill(
        name=SKILL_NAME,
        description=SKILL_DESCRIPTION,
        body=get_system_prompt(),
    )


__all__ = [
    "SKILL_DESCRIPTION",
    "SKILL_MD_PATH",
    "SKILL_NAME",
    "get_skill",
    "get_system_prompt",
]
