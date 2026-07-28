#!/usr/bin/env python
"""Run the pytest suite repeatedly to hunt intermittent freezes.

A freeze is detected as a run that exceeds ``--timeout`` seconds (the suite
normally finishes in ~2s). On a freeze we send SIGABRT so the child's
faulthandler dumps every thread's traceback, then SIGKILL, and stop.

Exit code 0 means the requested number of consecutive clean runs was reached.
"""

import argparse
import os
import signal
import subprocess
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=50)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--pytest-args", default="-q -p no:cacheprovider")
    args = ap.parse_args()

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    py = os.path.join(repo, ".venv", "bin", "python")
    env = dict(os.environ, PYTHONFAULTHANDLER="1")

    clean = 0
    for i in range(1, args.runs + 1):
        cmd = [py, "-X", "faulthandler", "-m", "pytest",
               *args.pytest_args.split()]
        start = time.monotonic()
        proc = subprocess.Popen(
            cmd, cwd=repo, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            out, _ = proc.communicate(timeout=args.timeout)
            elapsed = time.monotonic() - start
            rc = proc.returncode
            if rc != 0:
                sys.stdout.write(out.decode("utf-8", "replace"))
                print(f"\n[run {i}] FAILED rc={rc} ({elapsed:.1f}s)")
                return 2
            print(f"[run {i}] ok {elapsed:.2f}s  (consecutive clean="
                  f"{clean + 1})")
            clean += 1
        except subprocess.TimeoutExpired:
            print(f"\n[run {i}] *** FREEZE after {args.timeout:.0f}s — "
                  f"dumping thread tracebacks ***")
            # faulthandler dumps all threads on SIGABRT, then the process dies.
            os.killpg(proc.pid, signal.SIGABRT)
            try:
                out, _ = proc.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                out, _ = proc.communicate()
            sys.stdout.write(out.decode("utf-8", "replace"))
            print(f"[run {i}] freeze captured")
            return 1

    print(f"\nSUCCESS: {clean} consecutive clean runs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
