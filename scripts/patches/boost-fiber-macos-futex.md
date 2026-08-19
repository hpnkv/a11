# Boost.Fiber macOS futex — investigation & status

Companion to `boost-fiber-macos-futex.patch`.

## TL;DR (RESOLVED)

The patch does two things:

1. Makes Boost.Fiber's futex spinlocks *available* on macOS via the Darwin
   `os_sync_wait_on_address` / `os_sync_wake_by_address_any` primitives.
2. **Fixes a memory-ordering bug in Boost.Fiber's own futex spinlocks** that
   corrupts `boost::fibers::mutex` ownership and the scheduler queues on arm64.

With fix (2) in place, selecting a futex spinlock
(`BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_FUTEX` or `..._TTAS_FUTEX`) is **safe** on
Apple silicon. A shared_work + fiber `mutex` + `condition_variable` stress
reproducer that fails **30/30** without the fix passes **60/60** with it, matching
the non-futex spinlock exactly.

## Root cause

`boost/fiber/detail/spinlock_ttas_futex.hpp` and
`spinlock_ttas_adaptive_futex.hpp` release the lock on their **uncontended unlock
fast path** with:

```cpp
void unlock() noexcept {
    if ( 1 != value_.fetch_sub( 1, std::memory_order_acquire) ) {  // BUG
        value_.store( 0, std::memory_order_release);
        futex_wake( & value_);
    }
}
```

For a read-modify-write, `memory_order_acquire` gives the **load** acquire
semantics and leaves the **store** relaxed. On the fast path (lock value `1 -> 0`,
no waiters) the `fetch_sub` is the *only* operation, so the store that releases
the lock carries **no release ordering**. The next holder acquires with acquire
(`compare_exchange`/`exchange`), but with no paired release there is no
happens-before edge — so writes made inside the critical section are not
guaranteed visible to it. The critical sections here are tiny but load-bearing:
`boost::fibers::mutex::owner_`, and the intrusive wait-queue / ready-queue /
sleep-queue links that every `spinlock` in the fiber runtime protects.

Why it only bites A11 on macOS/arm64:

- **arch:** on x86 a `lock`-prefixed RMW is a full barrier, so the missing release
  is masked. On arm64 the `ldxr/stxr` pair with acquire-only does not order the
  prior stores after the releasing store — a real data race.
- **spinlock choice:** only the *futex* spinlocks have this fast path; the
  non-futex `spinlock_ttas` / `spinlock_ttas_adaptive` unlock with
  `store(release)` and are correct. Futex spinlocks are opt-in and rarely used
  (default is non-futex), and were historically only enabled on Linux/Windows —
  usually x86 — which is why the bug lay dormant upstream.
- **contention regime:** the bug is on the *uncontended* fast path. Boost's
  internal spinlocks are held for a few instructions and are almost always
  uncontended, so this path dominates in the fiber runtime. A high-contention
  microbenchmark instead exercises the *contended* unlock branch
  (`store(0, release)` — which is correctly ordered) and therefore looks clean,
  which is why an earlier investigation concluded the primitive was "correct in
  isolation."

The fix: use `memory_order_acq_rel` for the `fetch_sub` so the releasing store
publishes the critical section, pairing with the acquire on the lock side.

## Symptom mapping

- a11: `boost::fibers::mutex` unlock throws `operation not permitted` — a fiber
  finds `owner_` is no longer itself, i.e. mutual exclusion on the fiber mutex was
  violated because the previous holder's `owner_` write was not published.
- Reproducer (debug/asserts build): scheduler-invariant failures
  (`ctx->is_resumable()` in `scheduler::dispatch`, "schedule while running") —
  the same missing publish corrupts the intrusive scheduler queues, so a
  running/consumed context gets handed back out by `pick_next`.

The earlier "dispatcher livelock, wall-clock `wait_until` classification" theory
was a red herring: the failure is memory visibility on the spinlock, not the
scheduler's timeout logic. (The `now() < sleep_tp` classification in
`scheduler::wait_until` is still latency-fragile, but it only produces
cv-legal spurious timeouts and is not the cause here.)

## Verifying

Reproducer: `shared_work` scheduler across 8 OS threads, N producer/consumer
fibers over one `boost::fibers::mutex` + `condition_variable` (timed
`wait_for`), built with `define=BOOST_FIBERS_SPINLOCK_TTAS_ADAPTIVE_FUTEX` and a
14.4 deployment target on arm64.

| spinlock                         | reproducer            |
|----------------------------------|-----------------------|
| non-futex `TTAS_ADAPTIVE`        | 30/30 pass            |
| `TTAS_ADAPTIVE_FUTEX`, unpatched | 30/30 corrupt         |
| `TTAS_ADAPTIVE_FUTEX`, acq_rel   | 60/60 pass            |

## Upstream

This is an upstream Boost.Fiber bug (present at least through 1.90.0). Worth
reporting to boostorg/fiber: the futex spinlocks' unlock fast path should release
(`acq_rel`/`release`), not `acquire`.
