"""Tests for the dual-key token-bucket rate limiter."""

from a11.demos.rate_limit import RateLimiter


def test_device_hourly_limit():
    """Per-device hourly budget is enforced."""
    limiter = RateLimiter(hourly_limit=3, daily_limit=10,
                          ip_hourly_limit=100, ip_daily_limit=100)
    for _ in range(3):
        assert limiter.check("ip-a", "dev-a").allowed
    decision = limiter.check("ip-a", "dev-a")
    assert not decision.allowed
    assert "Hourly" in decision.reason


def test_device_daily_limit():
    """Per-device daily budget is enforced."""
    limiter = RateLimiter(hourly_limit=100, daily_limit=2,
                          ip_hourly_limit=100, ip_daily_limit=100)
    assert limiter.check("ip-b", "dev-b").allowed
    assert limiter.check("ip-b", "dev-b").allowed
    decision = limiter.check("ip-b", "dev-b")
    assert not decision.allowed
    assert "Daily" in decision.reason


def test_ip_floor_blocks_despite_new_fingerprint():
    """Rotating the fingerprint does not bypass the IP-only floor."""
    limiter = RateLimiter(hourly_limit=100, daily_limit=100,
                          ip_hourly_limit=2, ip_daily_limit=100)
    assert limiter.check("shared-ip", "dev-1").allowed
    assert limiter.check("shared-ip", "dev-2").allowed
    # Third call with yet another fingerprint — IP floor exhausted.
    decision = limiter.check("shared-ip", "dev-3")
    assert not decision.allowed
    assert "Hourly" in decision.reason


def test_separate_ips_independent():
    """Different IPs get independent budgets."""
    limiter = RateLimiter(hourly_limit=1, daily_limit=10,
                          ip_hourly_limit=1, ip_daily_limit=10)
    assert limiter.check("ip-alice", "dev-alice").allowed
    assert not limiter.check("ip-alice", "dev-alice").allowed
    # Different IP is independent.
    assert limiter.check("ip-bob", "dev-bob").allowed


def test_ip_only_mode():
    """When device_key is None, only the IP pool is checked."""
    limiter = RateLimiter(hourly_limit=1, daily_limit=10,
                          ip_hourly_limit=2, ip_daily_limit=10)
    assert limiter.check("ip-x").allowed
    assert limiter.check("ip-x").allowed
    decision = limiter.check("ip-x")
    assert not decision.allowed


def test_daily_token_returned_on_hourly_denial():
    """When the hourly bucket denies, the daily token is refunded."""
    limiter = RateLimiter(hourly_limit=1, daily_limit=2,
                          ip_hourly_limit=100, ip_daily_limit=100)
    assert limiter.check("ip", "dev").allowed
    assert not limiter.check("ip", "dev").allowed
    # The device daily bucket should still have one token.
    buckets = limiter._get_buckets(
        "dev", hourly_cap=1, hourly_period=3600.0,
        daily_cap=2, daily_period=86400.0,
    )
    assert buckets.daily.tokens >= 0.99


def test_ip_tokens_returned_on_device_denial():
    """When the device budget denies, the IP tokens are rolled back."""
    limiter = RateLimiter(hourly_limit=1, daily_limit=1,
                          ip_hourly_limit=10, ip_daily_limit=10)
    assert limiter.check("ip", "dev").allowed
    # Device exhausted; IP should be rolled back.
    decision = limiter.check("ip", "dev")
    assert not decision.allowed
    ip_buckets = limiter._get_buckets(
        "ip", hourly_cap=10, hourly_period=3600.0,
        daily_cap=10, daily_period=86400.0,
    )
    assert ip_buckets.hourly.tokens >= 8.99
    assert ip_buckets.daily.tokens >= 8.99


def test_prune_removes_stale_entries():
    limiter = RateLimiter(hourly_limit=5, daily_limit=10,
                          ip_hourly_limit=15, ip_daily_limit=30)
    limiter.check("stale-ip", "stale-dev")
    assert len(limiter._buckets) >= 1
    removed = limiter.prune(max_idle_seconds=0.0)
    assert removed >= 1
    assert len(limiter._buckets) == 0


def test_retry_after_is_positive_on_denial():
    limiter = RateLimiter(hourly_limit=1, daily_limit=10,
                          ip_hourly_limit=100, ip_daily_limit=100)
    limiter.check("ip", "dev")
    decision = limiter.check("ip", "dev")
    assert not decision.allowed
    assert decision.retry_after_seconds > 0
