# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

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
