"""Nanosecond-aware time values backed by Abseil in [a11._native][a11._native]
."""

from a11 import _native

from a11._native import Duration
from a11._native import Time


def _copy_immutable(self):
    return self


def _deepcopy_immutable(self, _memo):
    return self


Duration.__copy__ = _copy_immutable
Duration.__deepcopy__ = _deepcopy_immutable
Time.__copy__ = _copy_immutable
Time.__deepcopy__ = _deepcopy_immutable

_ZERO_DURATION = Duration(0)
_INFINITE_DURATION = Duration._positive_infinity()
_INFINITE_FUTURE = Time._infinite_future()
_INFINITE_PAST = Time._infinite_past()


def zero_duration() -> Duration:
    """Return the immutable zero-duration value."""

    return _ZERO_DURATION


def infinite_duration() -> Duration:
    """Return positive infinite duration."""

    return _INFINITE_DURATION


def now() -> Time:
    """Return the current wall-clock time from Abseil's native clock."""

    return Time._now()


def infinite_future() -> Time:
    """Return the time sentinel later than every finite time."""

    return _INFINITE_FUTURE


def infinite_past() -> Time:
    """Return the time sentinel earlier than every finite time."""

    return _INFINITE_PAST


__all__ = [
    "Duration",
    "Time",
    "infinite_duration",
    "infinite_future",
    "infinite_past",
    "now",
    "zero_duration",
]
