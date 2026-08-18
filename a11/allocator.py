"""Give A11's native code a faster allocator inside a Python process.

Replacing the C library's ``malloc`` is worth about +25% throughput to A11's
native server, measured, for no change to A11's own code; see
``bench/PERF_PLAN.md``.
The native executables get it by linking mimalloc directly. The Python extension
cannot, and the reason is worth stating plainly rather than hiding:

``_native`` is a shared object that an interpreter ``dlopen``\\ s *after* that
interpreter has already allocated memory through the C library. A module that
replaced ``free`` for the whole process at that point would, sooner or later, be
handed a pointer that libc allocated before it arrived. So A11 does not do that
to your process.

Replacing the allocator *before* the process starts allocating has none of that
problem, and is what this module is for. It is a property of how the process was
launched, which is why it cannot be switched on from inside one.

What happens automatically
--------------------------

The ``a11`` command re-executes itself once with the allocator preloaded, so
anything run through the CLI -- the gateway above all -- gets the full effect
with no action from you. :func:`is_active` reports whether it worked.

What you must do yourself
-------------------------

If you embed A11 in your own process -- your own ``python myserver.py``, a
notebook, uvicorn, a test runner -- then only you can launch that process, so
only you can preload. One line, printed ready to paste by
``python -m a11.allocator``:

.. code-block:: shell

   # Linux
   export LD_PRELOAD=$(python -c 'import a11.allocator as a;
                                  print(a.library_path())')
   python myserver.py

   # macOS -- same, with DYLD_INSERT_LIBRARIES instead of LD_PRELOAD.

Or let this module build the environment for you when spawning a worker::

    import subprocess, a11.allocator
    subprocess.run(
        ["python", "myserver.py"],
        env=a11.allocator.environ_with_preload(),
    )

Caveats worth knowing before you measure
----------------------------------------

* On macOS, System Integrity Protection strips ``DYLD_INSERT_LIBRARIES`` from
  signed interpreters. A Homebrew or ``uv``-managed Python usually keeps it; the
  system ``/usr/bin/python3`` will not. :func:`is_active` is how you tell,
  instead of assuming.
* The win is a *native-throughput* win. A workload dominated by Python-level
  work will see little of it, because CPython's own object allocator is not what
  is being replaced.
"""

from __future__ import annotations

import ctypes
import os
import sys
from collections.abc import Mapping
from pathlib import Path

__all__ = [
    "PRELOAD_GUARD_ENV",
    "SUPPRESS_ENV",
    "environ_with_preload",
    "is_active",
    "library_path",
    "preload_variable",
]

#: Set by :func:`environ_with_preload` so a re-executed process does not do it
#: again. Its presence means "a preload has already been attempted", not that it
#: succeeded -- see :func:`is_active` for that.
PRELOAD_GUARD_ENV = "A11_ALLOCATOR_PRELOADED"

#: Set this to any non-empty value to stop A11 preloading anything, anywhere.
SUPPRESS_ENV = "A11_NO_ALLOCATOR_PRELOAD"

_LIBRARY_DIRECTORY = "_allocator"


def preload_variable() -> str | None:
    """Name of the dynamic-loader variable that preloads a library here.

    ``None`` on platforms with no such mechanism (Windows), which is a
    supported outcome and not an error: A11 simply runs on the system
    allocator there.
    """
    if sys.platform == "darwin":
        return "DYLD_INSERT_LIBRARIES"
    if sys.platform.startswith("linux"):
        return "LD_PRELOAD"
    return None


def library_path() -> Path | None:
    """Absolute path of the bundled allocator, or ``None`` if absent.

    Absent is normal in several cases -- a source build whose deps prefix
    predates the shared mimalloc, a platform A11 does not bundle one for, or a
    sanitizer build, which must keep its own allocator.
    """
    directory = Path(__file__).resolve().parent / _LIBRARY_DIRECTORY
    if not directory.is_dir():
        return None
    # Whatever the platform named it. mimalloc installs a versioned soname on
    # Linux and a plain dylib on macOS, and matching a pattern is steadier than
    # hard-coding either.
    patterns = ("libmimalloc*.dylib", "libmimalloc*.so*")
    for pattern in patterns:
        for candidate in sorted(directory.glob(pattern)):
            if candidate.is_file():
                return candidate
    return None


def is_active() -> bool:
    """Whether a mimalloc is actually loaded into *this* process.

    The honest check, and the one to trust over any environment variable: it
    asks the dynamic loader whether a mimalloc symbol resolves. That is false
    when a preload was requested but silently dropped -- most often by macOS
    System Integrity Protection -- which is exactly the case an environment
    variable cannot distinguish.
    """
    try:
        process = ctypes.CDLL(None)
    except OSError:  # pragma: no cover - no dlopen(NULL), e.g. Windows
        return False
    return hasattr(process, "mi_version")


def environ_with_preload(
    base: Mapping[str, str] | None = None,
) -> dict[str, str]:
    """A copy of the environment with the bundled allocator preloaded.

    Appends to any preload the caller already had rather than replacing it, and
    is a no-op -- returning a plain copy -- when the allocator is not bundled,
    the platform has no preload mechanism, a preload has already been attempted
    in this process tree, or :data:`SUPPRESS_ENV` is set.

    :param base: Environment to derive from; defaults to ``os.environ``.
    """
    environment = dict(os.environ if base is None else base)
    if environment.get(SUPPRESS_ENV):
        return environment
    if environment.get(PRELOAD_GUARD_ENV):
        return environment
    variable = preload_variable()
    library = library_path()
    if variable is None or library is None:
        return environment
    existing = environment.get(variable, "")
    entries = [entry for entry in existing.split(os.pathsep) if entry]
    if str(library) not in entries:
        # First, so it wins over anything else providing malloc.
        entries.insert(0, str(library))
    environment[variable] = os.pathsep.join(entries)
    environment[PRELOAD_GUARD_ENV] = "1"
    return environment


def reexec_with_preload(argv: list[str] | None = None) -> None:
    """Restart this process with the allocator preloaded, once.

    Returns without doing anything when there is nothing to do: already active,
    already attempted, suppressed, not bundled, or no preload mechanism. When it
    does act it replaces the process image, so it does not return at all.

    Only for A11's own entry points. A library has no business re-executing an
    interpreter it did not start, which is why nothing here runs on import.
    """
    if is_active() or os.environ.get(PRELOAD_GUARD_ENV):
        return
    if os.environ.get(SUPPRESS_ENV):
        return
    if preload_variable() is None or library_path() is None:
        return
    environment = environ_with_preload()
    if PRELOAD_GUARD_ENV not in environment:
        return
    arguments = [sys.executable, *(sys.argv if argv is None else argv)]
    try:
        os.execve(sys.executable, arguments, environment)
    except OSError:
        # Carrying on with the system allocator is a slower A11, not a broken
        # one; failing to start would be the worse outcome by far.
        return


def _describe() -> int:
    """Body of ``python -m a11.allocator``."""
    variable = preload_variable()
    library = library_path()
    print(f"platform:        {sys.platform}")
    print(f"preload variable: {variable or '(none on this platform)'}")
    print(f"bundled library:  {library or '(not bundled in this build)'}")
    print(f"active here:      {'yes' if is_active() else 'no'}")
    if variable and library:
        print()
        print("To give A11's native code the faster allocator, launch with:")
        print(f"  {variable}={library} python your_program.py")
        if sys.platform == "darwin":
            print()
            print(
                "On macOS this is dropped for interpreters protected by System"
                "\nIntegrity Protection; check with `active here` above after"
                "\nsetting it."
            )
    return 0


if __name__ == "__main__":  # pragma: no cover - exercised as a subprocess
    raise SystemExit(_describe())
