#!/usr/bin/env bash
# SessionStart / UserPromptSubmit hook: point the agent at the fiber tooling.
#
# A blocked A11 fiber's stack is parked where no OS thread points at it, so an
# agent that reaches for `bt`, a print statement or a longer timeout is looking
# in the wrong place. Wired in .claude/settings.json.
#
# Usage: hook_fiber_debugging.sh session|prompt
set -uo pipefail

mode=${1:-session}
payload=$(cat)

instruction='A11 concurrency debugging is not optional guesswork: use thread/introspect.h.

A blocked fiber'"'"'s stack is an mmap'"'"'d region no OS thread points at, so `bt`,
`thread apply all bt`, a core dump and faulthandler all miss the frames that
explain a hang. Before adding print statements, raising a timeout, or reading
code speculatively:

1. Run the failing thing with A11_FIBER_WATCHDOG=<seconds>. It logs every live
   fiber, its wait kind and object, how long it has waited, any deadlock cycle,
   and the unwound parked stacks.
2. Or call thread::FormatFiberReport() / a11.debug.fiber_report(), or send
   SIGUSR2 after thread::InstallFiberDumpSignalHandler().
3. For a process too wedged to run anything, or a core file, use
   scripts/a11_fibers.py under LLDB or GDB (`a11-fibers`, or `a11-hang` for
   thread backtraces plus the fiber report). The repo'"'"'s .lldbinit and .gdbinit
   load it.

Read the report before forming a hypothesis. No wait cycle and every fiber
parked in select means there is no native deadlock, and the bug is above the
fiber layer. A new blocking path in Thread needs a THREAD_WAIT_SCOPE where it
blocks, or it reports as `running`.

See doc/docs/guides/debugging-concurrency.md.'

if [ "$mode" = "prompt" ]; then
  prompt=$(printf '%s' "$payload" | jq -r '.prompt // ""')
  if ! printf '%s' "$prompt" | grep -qiE \
      'hang|hung|deadlock|dead-lock|stuck|wedge|freeze|frozen|never returns|no response|times? out|timing out|timed out|black hole|race condition'; then
    exit 0
  fi
  jq -n --arg text "$instruction" '{
    hookSpecificOutput: {
      hookEventName: "UserPromptSubmit",
      additionalContext: $text
    }
  }'
  exit 0
fi

jq -n --arg text "$instruction" '{
  hookSpecificOutput: {
    hookEventName: "SessionStart",
    additionalContext: $text
  }
}'
