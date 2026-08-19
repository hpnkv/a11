# Logging

A11's runtime is C++, but its logs are ordinary `logging.LogRecord`s. A sink
inside the native module hands each Abseil entry to Python, which emits it on
the `a11.native` logger, so the whole of `logging` applies to native output —
levels, `dictConfig`, a JSON formatter, a file handler, pytest's `caplog`.

## Importing A11 adopts your configuration

`import a11` reads the level from the surrounding process rather than choosing
one:

1. **absl-py, if the process configured it** — `set_verbosity()` was called,
   `--verbosity` was passed, or `absl.logging`'s handler is on the root logger.
2. **The standard library** — the effective level of the `a11` logger, which
   inherits `logging.basicConfig` and `logging.config.dictConfig`.
3. **`A11_LOG_LEVEL`** — a name (`debug`, `info`, ...) or an integer, read only
   when neither of the above says anything.

With none of those the level is `WARNING`, the same default a bare interpreter
gives you, and importing A11 prints nothing and installs no handler.

```python
import logging

logging.basicConfig(level=logging.INFO)

import a11  # follows the line above
```

## Turning it on yourself

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
Call [`sync`][a11.logging.sync] after reconfiguring logging behind A11's back
if you want a sub-`DEBUG` level to reach the runtime:

```python
logging.config.dictConfig(my_config)
a11.logging.sync()
```

## Levels below DEBUG

A11 follows absl-py's convention: a standard level under `DEBUG` selects an
Abseil `VLOG` tier, so `logging.DEBUG - 1` enables `VLOG(2)`.

## Escape hatch

`A11_LOG_BRIDGE=0`, or `enable(bridge=False)`, leaves native entries on
Abseil's own stderr path instead of routing them through `logging`. Use it when
debugging the bridge, or when native threads must not touch the GIL.

`LOG(FATAL)` always goes straight to stderr with a backtrace, bridged or not:
it precedes process death, and there is nowhere else for it to go.

::: a11.logging
