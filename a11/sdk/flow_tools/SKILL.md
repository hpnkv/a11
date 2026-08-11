---
description: Compose the tools you already have into one A11 Flow and run it as a
  single step, so the values that move between them never pass through your context.
  Use it when a task needs several tools and each one's output feeds the next, when
  the same shape of work repeats over a list of things, or when an intermediate result
  is large and you only need what it reduces to. The tools are flow_actions, flow_check
  and flow_run.
name: a11-flow

---
# Composing your tools into one step

You have three tools -- `flow_actions`, `flow_check`, `flow_run` -- that let you write a *flow*: a composition of the actions you can already call, dispatched as one step. A flow is text, in the A11 Flow language, and you write it in the moment for the task in front of you. There is nothing to install and nothing to deploy.

## Why bother

Calling three tools one after another means every intermediate result passes through you: you read it, then you quote it into the next call. A flow moves those values directly between the actions. Fetch four pages, summarise them, and what comes back to you is the summary -- the pages were never in your context, were never charged for, and were never yours to copy correctly.

Reach for a flow when:

- **Data flows tool to tool.** The output of one call is the input of the next, and you do not otherwise need to see it.
- **The same shape repeats.** The same two or three calls for each item in a list. `for` runs a block per value, `parallel n` runs several at a time.
- **An intermediate is large.** A page, a transcript, a file listing, a recording. Trim it inside the flow with `| truncate`, `| first`, `| where`, and return what it reduces to.
- **Steps should overlap.** Steps in a flow run concurrently; order comes from the data, not from the order you wrote them in. A summariser is dispatched at once and reads pages as they arrive.

Do not reach for one when a single tool call does the job, or when you need to *decide* something between two calls: a flow cannot ask you a question in the middle. Read what it returns and run another one.

## How to write one

1. **`flow_actions`** first, unless you already did it this turn. It lists every action a flow may name and -- this is the part your tool definitions do not tell you -- what each one's **output ports** are called, and whether it is `runnable`. You need port names on both sides of a `->`, and you need the verb.
2. **Write the flow.** Declare inputs you will pass and outputs you want back.
3. **`flow_check`** it. It compiles the flow and describes what it resolves to without dispatching anything, and reports a syntax error with the line and column. Use it whenever the flow is more than a couple of lines, or whenever running it would have real effects, because running one really does call the actions it names.
4. **`flow_run`** it, passing `source` and any `inputs` (an object keyed by port name -- a list for a port declared `stream`, a single value for any other). What comes back is an object of the flow's declared outputs.

## What will bite you

- **Declare small outputs.** The flow's outputs are what you read. If you declare an output that carries every fetched page, you have paid for the pages after all. Return the answer, the count, the three best hits.
- **`run` what is `runnable`, `call` what is not.** The two verbs are two different things: `run` executes the action where the flow is running, `call` puts it on the stream the flow is attached to and lets the peer do it. `flow_actions` marks each one, and almost everything it offers you is `runnable: true` -- so `run` is the usual verb, and a `run` of something without a handler is refused rather than quietly sent elsewhere.
- **You may only name actions you may name.** A flow naming anything else is refused before it runs. `flow_actions` is the list.
- **Every output port of every step is read.** You do not have to name them all -- the runtime drains what you ignore -- but `skip x.debug` says so plainly, and is worth writing when an output is large. `skip 1 x.rows` is the other one: it drops a port's first value for *every* reader, which is how you throw away a header line, and several of them naming one port add up.
- **A failing step ends the flow** unless you wrote `try`. When a failure is one the composition should handle, write `try run` (or `try call`) and then `wait` to find out how it went.
- **Give a port the type it wants.** When an action's input is a real type rather than a bag of keys, write `a11.sdk.Interaction{role: "user", content: [...]}` (or `{...} as a11.sdk.Interaction`) and it is validated into that type, defaults and all. `to_chunk(value)` makes the `Chunk` such a type's content is made of. Which tags exist is the host's -- ask `flow_actions` what the ports are and match them.
- **A stream of fragments is not a stream of things.** `| group EXPR` gathers values until `EXPR` holds of the one just added, then hands over the list: `| group ends-with(trim(it), [".", "?"]) | map trim(join(it, " "))` is partial utterances becoming whole sentences. Reach for it whenever one value is only part of what you need.
- **Order between two streams is `| then`.** Two statements writing to the same port interleave by arrival. When one lot has to come before another -- a conversation's history before the new message -- write `history | then asked -> port` and the flow reads them in that order.
- **No arithmetic, no functions, no code.** Expressions read values, compare them, take them apart, and build new ones. That is the whole of it. If the task needs real computation, that is what an action is for.
- **Bound your loops.** `repeat` takes `max n`, and it is there for a reason.

## The language

```
A11 Flow -- a composition of actions that is itself an action.
Every keyword may be written in lower case or UPPER CASE, but not Mixed.

flow NAME {
  describe "what this does"
  in  PORT: TYPE [stream] [required] "description"     # no `stream` = one value
  out PORT: TYPE [stream] [required] "description"
  header "x-header-name" as ALIAS default LITERAL

  X = run some-action(port: SOURCE, ...) MODIFIERS   # a handler registered here
  X = call some-action(port: SOURCE, ...) MODIFIERS  # on the attached stream
  N = node([ID]) [in MAP]                  # a stream of the flow's own
  nodes MAP [{ ... }]                      # a node map; keeps traffic local
  SOURCE | STAGE | STAGE -> DEST, DEST     # pipe a stream into node(s)
  skip SOURCE                              # read to the end, keep nothing
  skip N PORT                              # drop its first N, for all readers
  S = wait SUBJECT [timeout 30s]           # finished; S is how it went
  S = drain NODE                           # end a node, and say how it ended
  cancel X                                 # ask X to stop
  fail [CODE] [MESSAGE]                    # end the flow with a status
  for V in SOURCE [parallel N] { ... }     # once per value
  repeat S = START [max N] { ... S <- SOURCE ... until EXPR }
  if EXPR { ... } else { ... }
}

SOURCE is a port (in-port, X.out-port), a node, a loop variable, a header alias,
a literal, `status SUBJECT`, `N.id`, or any of those with `.field` / `[i]`.
DEST is an out-port, X.in-port, or a node.
SUBJECT is a call, a node, a port, or a named wait/drain.
MODIFIERS: tee | via MAP | timeout 30s | after X, Y (a step, or a port/node
           to wait for) |
           id EXPR | with "header": EXPR, ... |
           forward headers "x-name", "x-family-*" (send on the headers this
           flow was called with, as they arrived; `*` matches a family, and an
           explicit `with` of the same name wins. Every `x-a11-` header already
           reaches a step, so this is for the others.)
           ("try run"/"try call" tolerate
           failure). `run` needs a handler registered where the flow runs and
           keeps its nodes off the wire; `call` needs none and goes to the peer.
           Either may name another flow of the same file, in any order, and
           needs nothing registered for it: a composition can be factored into
           several flows and still arrive as one text.
STAGES: first N | last N | drop N | truncate N | batch N | group EXPR |
        then SOURCE | where EXPR | map EXPR | join "sep" | strformat "fmt" |
        mime "text/*" | collect | count | distinct | text | json | packb
      then and where may drop the `|`: `history then asked`, `hits where
      it.ok`. Every other stage keeps it.
      strformat "fmt" is `map strformat(fmt, it)`, the one-value shorthand.
      then SOURCE reads this stream and then that one, in that order --
      `history | then asked` is how a conversation keeps its turns straight,
      which two writers to one node cannot.
      group EXPR gathers values into a list and closes it when EXPR holds of
      the one just added -- `| group ends-with(it, [".", "?"]) | map join(it)`
      is how partial pieces become whole sentences. packb writes a value as
      application/x-msgpack.
TYPES: string text number integer bool object json list bytes any, a quoted
      mimetype, or a registry tag written unquoted: a11.sdk.AudioBuffer.
      A container says what it holds: list[string], list[a11.NodeFragment].
      A value is made into one with TYPE{field: expr, ...} or EXPR as TYPE --
      partial in, valid value of that type out. Not `TYPE{` where a `{` would
      open a block (an if/for header); put it in brackets there.
EXPR: literals, it (the value a where/map sees), .field, [i], (pipe | count),
      == != < <= > >= in, and/or/not, + and - (numbers, durations, instants;
      `-` needs its spaces, since `text-upper` is one name), and the functions
      len lower upper trim text number bool keys values get join split merge
      contains starts-with ends-with replace slice default to_chunk from_chunk
      strformat now duration time seconds
      starts-with/ends-with take one ending or a list of them; to_chunk(v[,
      mime]) makes a Chunk, from_chunk(c) reads one back.
      strformat("%s of %s", a, b) is printf: %s as text, %d %f %x as numbers,
      printf's flags/width/precision (%-8s, %06.2f), %2$s to pick a value by
      number, %% for a literal percent, and %(SPEC)s to apply a duration unit
      or a strftime pattern first. Not a Python template: there is nothing for
      a slot to read into, which is what makes one safe to accept from a model.
TIME: durations are written 500ns, 250ms, 30s, 2m, 1h, compound as 1m30s500ms,
      and are values like any other. now() is the clock; instant - instant is a
      duration; instant +/- duration is an instant; duration +/- duration is a
      duration, and a bare number on either side counts as seconds. A duration
      the other way round is below zero and says so.
      duration(x) and time(x) read a value in: a number of seconds, or the text
      the language itself writes -- duration("1m30s"), time("2026-08-11T09:14:22Z")
      -- so a duration or an instant that arrived as a string is a value again.
      seconds(d) is the number of seconds.
      Formatting: %s gives `1m30s` and `2026-08-11T09:14:22Z`,
      %(ns)d %(us)d %(ms)d %(s)d %(m)d %(h)d give a duration as one unit, and
      %(%H:%M:%S)s or %(epoch)d formats an instant.
STATUS: a record {"ok": bool, "code": "NOT_FOUND", "number": 5, "message": str}.
      `try` keeps a failure from ending the flow; `wait`/`status` say what
      happened; `fail CODE MSG`, `fail NUMBER MSG` or `fail STATUS` ends it.
      Codes are Abseil's: ok cancelled unknown invalid_argument not_found
      deadline_exceeded already_exists permission_denied resource_exhausted
      failed_precondition aborted out_of_range unimplemented internal
      unavailable data_loss unauthenticated.

Steps run concurrently; order comes from the data. Every output of a step is
read, whether the flow uses it or not. Stages that shrink a stream (first,
truncate, where) do so before the next step ever sees it, `skip N PORT` does it
for every reader at once, and `nodes` blocks keep a step's traffic off the wire.
```

## An example

The action names below are examples -- use the ones `flow_actions` gives you.

```
flow answer-from-the-web {
  describe "Search, read the best hits, and answer from them."

  in  question: string required
  out answer:   string
  out sources:  string stream

  search = run web-search(query: question, limit: 3)
  brief  = run summarize(question: question)

  nodes fetched {
    for hit in search.hits parallel 2 {
      page = try run web-fetch(url: hit.url)
      hit.url -> sources
      page.text | truncate 2000 -> brief.pages
      skip page.bytes
    }
  }

  brief.summary -> answer
  skip search.debug
}
```

Run that with `inputs` of `{"question": "..."}` and you get back `{"answer": "...", "sources": ["...", "..."]}` -- two small values, out of three pages you never had to read. The `nodes fetched` block is what keeps the pages off the wire; without it they would be replicated to whoever dispatched the composition.
