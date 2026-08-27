# Getting the faster allocator

Replacing the C library's `malloc` improved A11's native server throughput by
about **25%** on the reference Linux benchmark: from 15.7k to 19.8k operations
per second at 256 concurrent clients, while per-operation CPU fell from 286 µs
to 234 µs.

A11 enables the allocator for its executables and CLI. Applications embedding
A11 in another process must preload it before startup.

## A11 executables and the CLI

**A11's native executables**, including `a11_bench` and `a11-flow`, link the
allocator directly.

**The `a11` command** re-executes itself once at startup with the allocator
preloaded, so commands such as `a11 gateway` use it automatically.

## Applications that embed A11

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

## Check whether the allocator is active

```python
import a11.allocator

a11.allocator.is_active()  # True only if it really is
```

This asks the dynamic loader whether the allocator's symbols resolve in the
current process. It remains accurate when the environment variable is set but
the allocator was not loaded.

## Platform considerations

**macOS System Integrity Protection strips `DYLD_INSERT_LIBRARIES`** from signed
interpreters. A Homebrew or `uv`-managed Python normally keeps it; the system
`/usr/bin/python3` will not, and it fails silently. `is_active()` is how you tell.

**It is a native-throughput win.** What gets replaced is the allocator A11's C++
uses, not CPython's own object allocator. A workload that spends its time in
Python will see little of it; one that pushes data through sessions, nodes and
stores will see most of it.

**It is disabled under sanitizers.** ASan and TSan supply their own
allocators to track memory errors and thread safety issues.

**To turn it off entirely**, set `A11_NO_ALLOCATOR_PRELOAD=1`. That also stops the
`a11` command from re-executing itself, which is occasionally useful when
debugging process startup.

## Choosing a different allocator when building from source

`-DA11_ALLOCATOR=` takes `auto` (the default), `mimalloc`, `tcmalloc` or
`system`. `auto` is a *preference order*, per platform, following the
measurements in `bench/PERF_PLAN.md`: tcmalloc then mimalloc on Linux, mimalloc
on macOS. Whichever of them the build can actually reach is used, and the system
allocator is the fallback. A dependency prefix containing only mimalloc, as
produced by `scripts/bootstrap_wheel_deps.sh`, therefore selects mimalloc on
Linux. An explicit selection is required to exist:
`-DA11_ALLOCATOR=tcmalloc` fails when the prefix does not contain tcmalloc.

jemalloc, mimalloc and tcmalloc all measured within a few percent of each other
and all beat glibc by 20–30%, so which one is chosen matters much less than not
being on glibc.
