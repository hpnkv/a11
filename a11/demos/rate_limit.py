"""Token-bucket rate limiting for the demo server.

Each caller is rate-limited by **two independent keys**:

* **IP-only** — a hard floor that limits every device behind one address,
  regardless of fingerprint.  Rotating the browser fingerprint does not
  help.
* **IP + fingerprint** — a per-device budget so that distinct devices
  behind a shared NAT get their own allowance instead of sharing one.

A request must pass *both* limiters.  The IP-only limiter is deliberately
more generous (the combined traffic of several devices), while the
per-device limiter is tighter (the allowance of one).

Buckets are held in memory, which is sufficient for the single demo-server
instance running today; the interface — ``RateLimiter.check`` taking two
opaque identity strings and returning a decision — is designed so a
distributed backend (Redis, DynamoDB, a shared counter service) can
replace the in-memory store without changing callers.

Both time windows refill continuously (one token per ``period / capacity``
interval), so a caller who used their allowance gets a token back after a
predictable wait rather than a cliff at the period boundary.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field


@dataclass
class _Bucket:
    """A single token bucket with continuous refill."""

    capacity: int
    period_seconds: float
    tokens: float = 0.0
    last_refill: float = 0.0

    def __post_init__(self) -> None:
        self.tokens = float(self.capacity)
        self.last_refill = time.monotonic()

    def _refill(self, now: float) -> None:
        elapsed = now - self.last_refill
        if elapsed <= 0:
            return
        rate = self.capacity / self.period_seconds
        self.tokens = min(self.capacity, self.tokens + elapsed * rate)
        self.last_refill = now

    def try_consume(self, now: float | None = None) -> bool:
        """Remove one token if available; return whether it succeeded."""
        now = now if now is not None else time.monotonic()
        self._refill(now)
        if self.tokens >= 1.0:
            self.tokens -= 1.0
            return True
        return False

    def seconds_until_token(self, now: float | None = None) -> float:
        """Seconds until the next token becomes available."""
        now = now if now is not None else time.monotonic()
        self._refill(now)
        if self.tokens >= 1.0:
            return 0.0
        rate = self.capacity / self.period_seconds
        return (1.0 - self.tokens) / rate


@dataclass
class _CallerBuckets:
    """Hourly and daily buckets for one caller identity."""

    hourly: _Bucket
    daily: _Bucket


@dataclass
class RateLimitDecision:
    """The outcome of a rate-limit check."""

    allowed: bool
    #: Human-readable reason when denied; empty when allowed.
    reason: str = ""
    #: Seconds until the next token is available (hint for the caller).
    retry_after_seconds: float = 0.0


@dataclass
class RateLimiter:
    """In-memory token-bucket rate limiter with dual-key enforcement.

    Two independent bucket pools are checked for every request:

    1. **ip_key** — keyed on IP alone, with higher limits.  This is the
       hard floor: rotating the browser fingerprint does not buy extra
       budget.
    2. **device_key** — keyed on IP + fingerprint, with tighter limits.
       Distinct devices behind the same NAT each get their own budget.

    Architecturally ready for distribution: replacing the ``_buckets``
    dict with a shared store requires only changing ``_get_buckets`` and
    persisting the bucket state.
    """

    #: Per-device (IP + fingerprint) limits.
    hourly_limit: int = 5
    hourly_period_seconds: float = 3600.0
    daily_limit: int = 10
    daily_period_seconds: float = 86400.0

    #: Per-IP hard-floor limits (more generous).
    ip_hourly_limit: int = 15
    ip_hourly_period_seconds: float = 3600.0
    ip_daily_limit: int = 30
    ip_daily_period_seconds: float = 86400.0

    _buckets: dict[str, _CallerBuckets] = field(
        default_factory=dict, repr=False
    )

    def _get_buckets(
        self, identity: str, *, hourly_cap: int, hourly_period: float,
        daily_cap: int, daily_period: float,
    ) -> _CallerBuckets:
        """Return (or create) the buckets for *identity*.

        This is the seam a distributed implementation replaces: read from
        and write to a shared store instead of the local dict.
        """
        if identity not in self._buckets:
            self._buckets[identity] = _CallerBuckets(
                hourly=_Bucket(
                    capacity=hourly_cap,
                    period_seconds=hourly_period,
                ),
                daily=_Bucket(
                    capacity=daily_cap,
                    period_seconds=daily_period,
                ),
            )
        return self._buckets[identity]

    @staticmethod
    def _try_consume_pair(
        buckets: _CallerBuckets, now: float,
    ) -> RateLimitDecision | None:
        """Try to consume one token from both daily and hourly buckets.

        Returns a denial ``RateLimitDecision`` if either bucket is empty,
        or ``None`` on success (both consumed).
        """
        if not buckets.daily.try_consume(now):
            wait = buckets.daily.seconds_until_token(now)
            return RateLimitDecision(
                allowed=False,
                reason=(
                    "Daily demo limit reached. Please try again later"
                    " or use your own API key."
                ),
                retry_after_seconds=wait,
            )
        if not buckets.hourly.try_consume(now):
            # Give the daily token back — hourly is the binding
            # constraint.
            buckets.daily.tokens = min(
                buckets.daily.capacity, buckets.daily.tokens + 1.0
            )
            wait = buckets.hourly.seconds_until_token(now)
            return RateLimitDecision(
                allowed=False,
                reason=(
                    "Hourly demo limit reached. Please try again in a"
                    " few minutes or use your own API key."
                ),
                retry_after_seconds=wait,
            )
        return None

    def check(
        self, ip_key: str, device_key: str | None = None,
    ) -> RateLimitDecision:
        """Try to consume one token for both *ip_key* and *device_key*.

        The IP-only pool is checked first (the hard floor).  If it
        passes and a *device_key* is given, the per-device pool is
        checked too.  Both must allow the request.
        """
        now = time.monotonic()

        ip_buckets = self._get_buckets(
            ip_key,
            hourly_cap=self.ip_hourly_limit,
            hourly_period=self.ip_hourly_period_seconds,
            daily_cap=self.ip_daily_limit,
            daily_period=self.ip_daily_period_seconds,
        )
        ip_denial = self._try_consume_pair(ip_buckets, now)
        if ip_denial is not None:
            return ip_denial

        if device_key and device_key != ip_key:
            dev_buckets = self._get_buckets(
                device_key,
                hourly_cap=self.hourly_limit,
                hourly_period=self.hourly_period_seconds,
                daily_cap=self.daily_limit,
                daily_period=self.daily_period_seconds,
            )
            dev_denial = self._try_consume_pair(dev_buckets, now)
            if dev_denial is not None:
                # Roll back the IP token — the device limit is the
                # binding constraint.
                ip_buckets.hourly.tokens = min(
                    ip_buckets.hourly.capacity,
                    ip_buckets.hourly.tokens + 1.0,
                )
                ip_buckets.daily.tokens = min(
                    ip_buckets.daily.capacity,
                    ip_buckets.daily.tokens + 1.0,
                )
                return dev_denial

        return RateLimitDecision(allowed=True)

    def prune(self, max_idle_seconds: float = 172_800.0) -> int:
        """Remove bucket entries idle for longer than *max_idle_seconds*.

        Returns the number of entries removed. Call periodically to bound
        memory in a long-running server.
        """
        now = time.monotonic()
        stale = [
            key
            for key, entry in self._buckets.items()
            if (now - entry.hourly.last_refill > max_idle_seconds
                and now - entry.daily.last_refill > max_idle_seconds)
        ]
        for key in stale:
            del self._buckets[key]
        return len(stale)
