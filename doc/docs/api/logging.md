# Logging

A11 emits native C++ logs as ordinary Python `logging.LogRecord` values. The
native module forwards Abseil entries to the `a11.native` logger, where levels,
`dictConfig`, formatters, handlers, and pytest's `caplog` apply.

## Importing A11 uses the process configuration

`import a11` selects a level from the surrounding process in this order:

1. **absl-py, if the process configured it** — `set_verbosity()` was called,
   `--verbosity` was passed, or `absl.logging`'s handler is on the root logger.
2. **The standard library** — the effective level of the `a11` logger, which
   inherits `logging.basicConfig` and `logging.config.dictConfig`.
3. **`A11_LOG_LEVEL`** — a name (`debug`, `info`, ...) or an integer, read only
   when neither of the above says anything.

Without a configured level, A11 uses `WARNING`. Importing A11 prints nothing and
installs no handler.

```python
import logging

logging.basicConfig(level=logging.INFO)

import a11  # follows the line above
```

## Enabling logging explicitly

```python
import a11

a11.enable_logging("debug")
a11.get_logger(__name__).info("ready")
```

[`enable`][a11.logging.enable] installs a handler only when the root logger has
none, and the one it installs is `absl.logging`'s, so output keeps Abseil's
shape:

```text
I0810 11:05:21.115550 8533073600 shell.py:64] opened shell 1
I0810 11:05:21.115635 8533073600 http2.cc:1691] HTTP/2 listener error
```

Under an application that has already configured logging, A11 installs nothing
and its records — native ones included — flow into the handlers you set up.

[`set_level`][a11.logging.set_level] moves the `a11` logger, `absl.logging`'s
verbosity, and the native `VLOG` threshold together;
[`disable`][a11.logging.disable] silences both sides.

## Filtering and reconfiguration

Levels are resolved per record, so a `setLevel` or `dictConfig` that lands
after import takes effect with nothing to re-synchronise:

```python
logging.getLogger("a11").setLevel(logging.DEBUG)     # native logs included
logging.getLogger("a11.native").setLevel(logging.ERROR)  # native logs only
```

A `logging.Filter` reaches native records the same way it reaches any other:
on a handler, or on the `a11.native` logger itself. Python does not consult an
ancestor logger's filters for a record that merely propagates through it, so
one attached to `a11` will not see them.

Only `VLOG` is gated natively, because it is the one genuinely costly tier.
Call [`sync`][a11.logging.sync] after external logging reconfiguration to pass a
sub-`DEBUG` level to the native runtime:

```python
logging.config.dictConfig(my_config)
a11.logging.sync()
```

## Levels below DEBUG

A11 follows absl-py's convention: a standard level under `DEBUG` selects an
Abseil `VLOG` tier, so `logging.DEBUG - 1` enables `VLOG(2)`.

## What actions log

`Action.log` and `Action.logf` report action progress, while the logging bridge
reports runtime events. Action logs travel as chunks on a reserved port, so a
remote caller receives structured data instead of process-local stderr text.

```python
await action.log("searching", channel="fetch")
await action.logf("read %d of %d pages", done, total)
await action.log({"hits": 12}, level="debug", internal=True)
```

The reserved port is created only when used and requires no schema declaration.
Only a running action may log; logging before `run` or from the calling side of
`call` has no active writer or reader.

What is consumed in this process becomes a record on the `a11.action` logger, so
`setLevel`, `dictConfig` and your existing handlers apply. The chunk's whole
description travels with it as record attributes -- `a11_action`, `a11_channel`,
`a11_internal`, `a11_mimetype`, `a11_data` -- so a handler can filter on the
channel or drop A11's internal lines without parsing the message back apart.

`A11_ACTION_LOG=0` leaves them on the native log instead.
[set_action_log_sink][a11.logging.set_action_log_sink] takes them somewhere else
entirely. A single sink prevents duplicate delivery. Calling
`action.get_log_node()` returns the chunks directly and suppresses the sink for
that action.

In Flow the same log is two statements and two pipeline stages:

```
log warning "no readable content"
logf "read %s pages" read after search
pages | log debug it -> kept
```

## Escape hatch

`A11_LOG_BRIDGE=0`, or `enable(bridge=False)`, leaves native entries on
Abseil's own stderr path instead of routing them through `logging`. Use it when
debugging the bridge, or when native threads must not touch the GIL.

`LOG(FATAL)` always goes straight to stderr with a backtrace, bridged or not:
it precedes process death, and there is nowhere else for it to go.

::: a11.logging
