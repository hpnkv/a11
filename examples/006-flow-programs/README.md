# Flow as a program

Nine programs, in a language with no arithmetic beyond `+` and `-`, no way to
define a function, and no way to call out to code.

```sh
a11-flow-run greet.flow -- Helena
a11-flow-run wc.flow < some-file
some-command | a11-flow-run grep.flow -- warning
a11-flow-run --allow-write copy.flow -- from.bin to.bin
a11-flow-run --root /var/log --timeout 30s watch.flow -- /var/log/system.log
a11-flow-run span.flow -- "END OF" < build.log
a11-flow-run extract.flow -- BEGIN END < some-file
a11-flow-run --root /var/log poll.flow -- /var/log/system.log
```

## Two ways to run one, and what each can offer

`a11-flow-run` is the standalone binary. `a11 flow run` is the same interpreter
called from Python, and every line above works with it unchanged:

```sh
a11 flow run greet.flow -- Helena
```

The difference is what the *host* can offer. A program may only call actions that
exist where it runs, and the standalone binary has exactly the Flow standard
library. The Python one has whatever Python has:

```sh
a11 flow run ask.flow --allow-llm --allow-net \
    --allow-env ANTHROPIC_API_KEY -- "why is the sky blue"
```

`ask.flow` is the ninth program and it runs *only* this way, because
`interact_with_llm` needs a provider SDK and a credential and both live in
Python. `--allow-llm` is its own flag rather than part of `--allow-net` for a
reason worth knowing: a host-registered action is **not** bounded by the flow
policy. The policy governs the standard library; it can say nothing about what a
Python handler does. So offering one is a separate decision.

The first five are pipelines. The last four are the ones that needed something a
pipeline does not have:

| program        | the thing it needed                                        |
| -------------- | ---------------------------------------------------------- |
| `span.flow`    | a question about *neighbouring* values, not about one       |
| `extract.flow` | a state that changes as the stream goes past                |
| `poll.flow`    | stopping an endless source on purpose, and reporting success |
| `ask.flow`     | an action only the *host* can provide                       |

## The nameless flow

A file says which part of it is the program by declaring a flow with no name:

```a11flow
flow {
  describe "What this program does."
  ...
}
```

Everything else in the file — named flows, `struct`s — is what that one uses. A
file may declare one entry flow, and the language will say so if it finds two.

It has no name **on purpose**, and that is not a spelling quirk: a flow nothing
can name is a flow nothing can `run` or `call`, so a program's entry point
cannot be reached as a library by another flow and cannot recurse into itself.

Its arguments arrive as two ports nobody declared:

| port   | type              | what it holds                                  |
| ------ | ----------------- | ---------------------------------------------- |
| `argc` | `integer`         | how many arguments, counting the program's name |
| `argv` | `string` (stream) | the arguments, the program's own name first     |

`argv[0]` is the file, the way a C program's `argv[0]` is itself, so what a
person typed starts at index 1 — which is why every example here opens with
`argv | drop 1`.

An `out exit_code: integer` port, if the flow declares one, is the process's exit
code. `grep.flow` uses it to be usable in a shell `if`.

## What a program may expect to be bound

Everything in the Flow standard library, plus this process's own standard
streams: `read_stdin`, `write_stdout`, `write_stderr`, `read_file`,
`write_file`, `list_directory`, `stat_path`, `make_directory`, `remove_path`,
`move_path`, `copy_path`, `make_temp`, `spawn_process`, `ticker`, `sleep_for`,
`env_get`, `random_bytes`, `new_uuid`.

## What it may *do* is decided outside the file

```sh
a11-flow-run --root /srv/data --allow-write --timeout 1m program.flow
```

There is deliberately no way to widen that from inside the file — a capability a
script can grant itself is not a capability anybody granted. So a file can be
read for what it *will* do, and the command line is where what it *may* do is
written. `a11-flow-run --help` lists them; the defaults are the working
directory, read-only, no processes, no network.

## Three things these examples are shaped by

**`skip` is not `omit`.** `skip` reads a port and keeps nothing — the action
still produced the value. `options.omit` closes the port before anything is
written to it. `copy.flow` needs the second: `read_file`'s `text` and `lines`
ports cannot be produced at all for a binary file, because they are text and a
file is bytes.

**A pipeline off a node fans out; two conditions reading it do not.** In
`grep.flow` the match count feeds two pipelines and both see it. Two `if`s
reading the same node would take one value each, and the second would find the
stream already over.

**Nothing here holds its input.** `wc.flow` counts a stream, `copy.flow` moves
one chunk at a time, and a port write does not return until the store took the
chunk — so the reader is held behind the writer without either of them saying
anything about it. These cost the same memory for a ten-byte file and a
ten-gigabyte one. `span.flow` and `extract.flow` keep that property: a window
holds `n` values and a `scan` holds one state, neither of which grows.

## Three things the later examples are about

**A boundary hides a question about neighbours.** `| batch 2` and `| window 2`
both look at two lines at a time, and only one of them can find a phrase that
runs across a line break: `batch` has to put a boundary *somewhere*, and a match
sitting on one is silently not found. `span.flow` is that difference.

**`scan` is the only way to write a state machine over a stream.** The two
constructs that look like they should do it cannot. `repeat` carries a value
from pass to pass but reads its stream from the beginning on every pass, so pass
two sees value one again. `for` walks a stream one value at a time but carries
nothing between passes, so a pass cannot know what the last one saw. `scan` does
both, which is what `extract.flow` needs to know whether a line is inside a
block.

**Stopping something endless has a right answer and two wrong ones.** Letting it
run out does not apply, and `cancel` works but ends the run `cancelled` with a
non-zero exit code — a deliberate stop reported as a crash. The right answer is
to ask it to finish on its control port: every action in the standard library
treats `{"command": "stop"}` there as its stream having ended, so ports close
normally and the run succeeds. `poll.flow` does that, by writing to a node the
same loop is reading the results of — which is how a decision gets fed back into
what produced it.

The language has no keyword for it, on purpose. `control_events` is a convention
of the standard library, not of the language, and a compiler that knew about it
would be a compiler that knew one library's habits.

And note what does **not** end a step: `| first n`, and a `for`'s `until`. Both
stop *reading*, and both deliberately leave the producer alone — a step feeding a
node that nobody drains would stall, so a `first 3` must not be able to wedge
what it reads from. `span.flow` does not care, because standard input ends on its
own. `poll.flow` does, because a ticker does not.
