# GDB setup for this repository, loaded when gdb starts in the repo root.
#
# GDB refuses a directory-local .gdbinit unless its path is in `auto-load
# safe-path`, because it runs arbitrary commands. GDB prints the exact line to
# add to your ~/.gdbinit when it declines; it is:
#
#     add-auto-load-safe-path /path/to/a11/.gdbinit
#
# CLion reads startup commands from
# Settings | Build, Execution, Deployment | Debugger; put the source line there
# to get the same commands without the opt-in.

# Adds `a11-fibers` and `a11-hang`. A blocked A11 fiber's stack is parked where
# no OS thread points at it, so the Frames pane and `bt` both miss it; these
# unwind it. See doc/docs/guides/debugging-concurrency.md.
source scripts/a11_fibers.py
