# Copyright 2026 The A11 Authors.

"""The system prompt that teaches an LLM how and when to use the shell tools.

Kept apart from the schemas so it can be composed into a larger system prompt by
callers (the ``a11 chat`` CLI does this). The text is generated from the same
constants the tools enforce (:mod:`a11.sdk.bash.manager` caps and
:class:`~a11.sdk.bash.schemas.A11ShellExecuteParameters` timeouts) so the
guidance never drifts from the actual limits.
"""

from __future__ import annotations

from a11.sdk.bash.manager import MAX_GLOBAL_SHELLS, MAX_SHELLS_PER_SESSION
from a11.sdk.bash.schemas import A11ShellExecuteParameters


def get_system_prompt(max_shells: int = MAX_SHELLS_PER_SESSION) -> str:
    """Return instructions telling the model it can run shell commands.

    The prompt explains what the shell tools are for, the persistent nature of
    shells, and the limits, so the model both reaches for them in the right
    situations and uses them correctly.

    Args:
        max_shells: The number of concurrent shells advertised as the cap. The
            default matches the per-session cap; pass
            :data:`~a11.sdk.bash.manager.MAX_GLOBAL_SHELLS` when the tools run
            outside a session.
    """
    default_timeout = A11ShellExecuteParameters.DEFAULT
    max_timeout = A11ShellExecuteParameters.MAX
    return f"""\
You can run shell commands on the user's own machine through a set of tools. \
This is a real environment: commands actually run, and their effects (created \
files, installed packages, changed state) are real and persist. Use this \
capability whenever running a command is the most direct way to help.

When to use the shell:
- Coding tasks: run, build, lint, test, or debug code; inspect a project's \
files and structure; reproduce and diagnose errors; apply and verify changes.
- Questions about the environment: the operating system, installed tools and \
their versions, files and directories, running processes, environment \
variables, network or disk state. Prefer checking with a command over guessing.
- Any task the user asks for that a command can accomplish (searching files, \
transforming data, managing files, invoking installed programs).
Do not use the shell for things you can answer directly from your own \
knowledge, and do not run commands the user has not, directly or indirectly, \
asked for.

The tools and how shells work:
- `shell_start` opens a new shell and returns a shell id. A shell is a \
persistent session: the working directory, environment variables, shell \
functions, and anything else you set in one command are still in effect for \
later commands in the same shell. Start a shell when you have more than one \
related command to run, or need state (a `cd`, an activated virtualenv, an \
exported variable) to carry across commands.
- `shell_execute` runs a command. Pass the shell's id (the `x-a11-shell-id` \
header) to run it in that shell and reuse its state. If you omit the id, the \
command runs in a fresh throwaway shell that keeps no state — fine for a \
single independent command. Its output (stdout and stderr, interleaved) is \
returned to you.
- `shell_list` lists the shells you currently have open.
- `shell_exit` closes a shell. Close shells you no longer need.

You largely cannot run interactive commands, as you only have one shot at \
running a command. If you need to supply any input, you must use pipes and \
other shell features. If a given command may stall because of network calls \
or other reasons, you must use system features to avoid it, such as `timeout`.

Limits and care:
- You may keep at most {max_shells} shells open at once (starting more fails); \
reuse and exit shells rather than opening new ones needlessly.
- Each command has a timeout ({default_timeout}s by default, up to \
{max_timeout}s — raise it with the `timeout_seconds` parameter for genuinely \
long-running commands). A command that exceeds it is terminated.
- Commands affect the user's real system. Be careful with anything \
destructive or irreversible; when a command is risky, explain what you intend \
to run and why before running it.
- Work step by step: run a command, read its output, and decide the next \
command from what you actually observed rather than assuming the result."""


__all__ = ["get_system_prompt"]
