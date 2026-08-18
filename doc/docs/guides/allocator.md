# Getting the faster allocator

Replacing the C library's `malloc` makes A11's native server about **25% faster**
for no change to A11's own code. That is measured, not estimated — on the
reference Linux box the server benchmark went from 15.7k to 19.8k operations per
second at 256 concurrent clients, and per-operation CPU fell from 286 µs to
234 µs.

A11 takes that win for you wherever it can. There is one case where it cannot,
and this page is mostly about being clear on which case you are in.

## What you get for free

**A11's native executables** — `a11_bench`, the `a11-flow` tool — link the
allocator directly. Nothing to do.

**The `a11` command** re-executes itself once at startup with the allocator
preloaded, so everything run through the CLI gets the full effect. This matters
most for `a11 gateway`, which is the native server. Again, nothing to do.

## What you have to do yourself

If you run A11 inside **your own** process — `python myserver.py`, a notebook,
uvicorn, pytest — then A11 cannot switch the allocator on for you, and the reason
is worth understanding rather than working around.

`a11._native` is a shared library that your interpreter loads *after* it has
already allocated memory through the C library. A module that replaced `free` for
the whole process at that point would, sooner or later, be handed a pointer that
libc allocated before it arrived — and free it with the wrong allocator. A11 will
not do that to your process.

Replacing the allocator *before* the process starts has none of that problem.
That is a property of how the process is launched, so only whoever launches it can
do it. One line:

=== "Linux"

    ```shell
    LD_PRELOAD=$(python -c 'import a11.allocator as a; print(a.library_path())') \
        python myserver.py
    ```

=== "macOS"

    ```shell
    DYLD_INSERT_LIBRARIES=$(python -c 'import a11.allocator as a; print(a.library_path())') \
        python myserver.py
    ```

`python -m a11.allocator` prints the exact line for your platform, along with
whether it is currently active.

If you spawn worker processes yourself, build their environment instead of
writing the variable by hand:

```python
import subprocess

import a11.allocator

subprocess.run(
    ["python", "worker.py"],
    env=a11.allocator.environ_with_preload(),
)
```

## Checking rather than assuming

```python
import a11.allocator

a11.allocator.is_active()  # True only if it really is
```

This asks the dynamic loader whether the allocator's symbols resolve in this
process. Trust it over the environment variable, because there is a case where
the variable is set and the allocator is not loaded — see below.

## Things that will surprise you otherwise

**macOS System Integrity Protection strips `DYLD_INSERT_LIBRARIES`** from signed
interpreters. A Homebrew or `uv`-managed Python normally keeps it; the system
`/usr/bin/python3` will not, and it fails silently. `is_active()` is how you tell.

**It is a native-throughput win.** What gets replaced is the allocator A11's C++
uses, not CPython's own object allocator. A workload that spends its time in
Python will see little of it; one that pushes data through sessions, nodes and
stores will see most of it.

**It is off under sanitizers, deliberately.** ASan and TSan supply their own
allocators in order to detect exactly the kind of bug this would be papering
over.

**To turn it off entirely**, set `A11_NO_ALLOCATOR_PRELOAD=1`. That also stops the
`a11` command from re-executing itself, which is occasionally useful when
debugging process startup.

## Choosing a different allocator when building from source

`-DA11_ALLOCATOR=` takes `auto` (the default), `mimalloc`, `tcmalloc` or
`system`. `auto` picks per platform, following the measurements in
`bench/PERF_PLAN.md`: tcmalloc on Linux, mimalloc on macOS. jemalloc, mimalloc
and tcmalloc all measured within a few percent of each other and all beat glibc
by 20–30%, so the per-platform default matters much less than not being on glibc.
