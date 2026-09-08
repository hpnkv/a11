# Native allocator

The reference Linux benchmark reports 19.8k operations per second with the
packaged allocator and 15.7k with glibc `malloc` at 256 concurrent clients.
Per-operation CPU is 234 µs and 286 µs respectively.

A11 enables the allocator for its executables and CLI. Applications embedding
A11 in another process must preload it before startup.

## A11 executables and the CLI

**A11's native executables**, including `a11_bench` and `a11-flow`, link the
allocator directly.

**The `a11` command** re-executes itself once at startup with the allocator
preloaded, so commands such as `a11 gateway` use it automatically.

## Preload for applications that embed A11

When A11 runs inside an existing process — `python myserver.py`, a notebook,
uvicorn, or pytest — preload the allocator before starting that process.

The interpreter loads `a11._native` after allocating memory through the C
library. Replacing `free` at that point could route existing allocations to the
wrong allocator, so A11 does not enable it after process startup.

Preloading replaces the allocator before the process starts and avoids mixing
allocators. Configure it when launching the process:

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

`python -m a11.allocator` prints the platform-specific launch command and
reports whether the allocator is active.

For application-managed worker processes, build the child environment with the
helper:

```python
import subprocess

import a11.allocator

subprocess.run(
    ["python", "worker.py"],
    env=a11.allocator.environ_with_preload(),
)
```

## Check whether the allocator is active

```python
import a11.allocator

a11.allocator.is_active()  # True only if it really is
```

This asks the dynamic loader whether the allocator's symbols resolve in the
current process. It remains accurate when the environment variable is set but
the allocator was not loaded.

## Account for platform constraints

**macOS System Integrity Protection strips `DYLD_INSERT_LIBRARIES`** from signed
interpreters. A Homebrew or `uv`-managed Python normally keeps it; the system
`/usr/bin/python3` strips it without an error. Use `is_active()` to verify the
result.

**The change applies to native allocations.** It replaces the allocator used by
A11's C++ runtime, while CPython retains its object allocator. The effect depends
on how much work occurs in sessions, nodes, and stores.

**It is disabled under sanitizers.** ASan and TSan supply their own
allocators to track memory errors and thread safety issues.

**To turn it off entirely**, set `A11_NO_ALLOCATOR_PRELOAD=1`. That also stops the
`a11` command from re-executing itself, which is occasionally useful when
debugging process startup.

## Choose an allocator when building from source

`-DA11_ALLOCATOR=` takes `auto` (the default), `mimalloc`, `tcmalloc` or
`system`. `auto` is a *preference order*, per platform, following the
measurements in `bench/PERF_PLAN.md`: tcmalloc then mimalloc on Linux, mimalloc
on macOS. Whichever of them the build can actually reach is used, and the system
allocator is the fallback. A dependency prefix containing only mimalloc, as
produced by `scripts/bootstrap_wheel_deps.sh`, therefore selects mimalloc on
Linux. An explicit selection is required to exist:
`-DA11_ALLOCATOR=tcmalloc` fails when the prefix does not contain tcmalloc.

The reference benchmark places jemalloc, mimalloc, and tcmalloc within a few
percent of each other and 20–30% above glibc.
