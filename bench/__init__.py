# Copyright 2026 The A11 Authors.

"""A11's benchmark suite.

What this is for: deciding where the next week of engineering should go. Every
benchmark here answers a question somebody could otherwise only guess at --
*how many streams does a service hold*, *what does a flow step cost against the
action it wraps*, *is SQLite fast enough to be a node's store* -- and answers it
in a unit that survives being written down (ops/s, microseconds at a
percentile, bytes per live object).

Layout:

* [bench.harness][bench.harness] is the measuring apparatus: timing loops,
  percentiles, resident-memory sampling, and the `Result` record everything
  reports in.
* `bench/suites/` holds one module per component. A suite is a list of
  `@benchmark`-decorated coroutines; nothing else registers work.
* `python -m bench` runs them. `--json` writes records that `--baseline`
  compares against on a later run, which is what turns a number into a trend.

The rule for a benchmark in here: it measures A11, not the harness. Anything
timed at a rate where the timing call itself is a visible cost is measured as a
*batch* (total wall clock over N operations) and reports throughput only;
latency percentiles come from a separate, smaller loop.
"""
