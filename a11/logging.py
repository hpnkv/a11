# Copyright 2026 The A11 Authors.

"""Logging for A11, wired into the standard library's.

A11's runtime is C++, but its logs are ordinary ``logging.LogRecord`` values.
A sink inside the native module hands each Abseil entry to Python, which emits
it on the ``a11.native`` logger, so the whole of `logging` applies to native
output: levels, `logging.config.dictConfig`, pytest's ``caplog``, a JSON
formatter, a file handler. There is no second logging system to configure.

A `logging.Filter` reaches native records the same way it reaches any other:
on a handler, or on the ``a11.native`` logger itself. Python does not consult
an ancestor logger's filters for a record that merely propagates through it,
so one attached to ``a11`` will not see them.

Importing `a11` reads the level from the surrounding process rather than
choosing one, in this order:

1. **absl-py, if the process configured it** — ``set_verbosity()`` was called,
   ``--verbosity`` was passed, or `absl.logging`'s handler is on the root
   logger.
2. **The standard library** — the effective level of the ``a11`` logger, which
   inherits `logging.basicConfig` and `dictConfig`.
3. **``A11_LOG_LEVEL``** — a name (``debug``, ``info``, ...) or an integer, read
   only when neither of the above says anything.

With none of those, the level is `logging.WARNING`, the same default a bare
interpreter gives you, and ``import a11`` prints nothing.

To take control:

```python
import a11

a11.enable_logging("debug")             # or a11.logging.enable(logging.DEBUG)
a11.logging.get_logger(__name__).info("ready")
```

`enable` installs a handler only when the root logger has none, and the one it
installs is `absl.logging`'s, so the text keeps Abseil's shape. Under an
application that has already configured logging, A11's records simply flow
into it.

``A11_LOG_BRIDGE=0`` (or ``enable(bridge=False)``) leaves native entries on
Abseil's own stderr path instead of routing them through Python.
"""

from __future__ import annotations

import logging
import os
from typing import Any

from absl import logging as _absl
from absl.logging import converter as _converter

from a11 import _native

#: Root of A11's logger namespace. Set its level to control A11 as a whole.
LOGGER_NAME = "a11"

#: Logger carrying entries emitted by the C++ runtime.
NATIVE_LOGGER_NAME = "a11.native"

#: Level applied when nothing in the process has configured logging.
DEFAULT_LEVEL = logging.WARNING

_LEVEL_ENV = "A11_LOG_LEVEL"
_BRIDGE_ENV = "A11_LOG_BRIDGE"

# Abseil severities, as the native sink reports them.
_SEVERITY_INFO = 0
_SEVERITY_FATAL = 3

_bridged = False


def get_logger(name: str | None = None) -> logging.Logger:
    """The ``a11`` logger, or a child of it.

    A dotted module name is placed under the A11 namespace, so
    ``get_logger(__name__)`` inside ``a11.gateway.app`` yields
    ``a11.gateway.app`` and inherits whatever is configured for ``a11``.
    """
    if not name or name == LOGGER_NAME:
        return logging.getLogger(LOGGER_NAME)
    if name.startswith(f"{LOGGER_NAME}."):
        return logging.getLogger(name)
    return logging.getLogger(f"{LOGGER_NAME}.{name}")


def parse_level(level: int | str) -> int:
    """Coerce ``level`` to a standard logging level.

    Accepts an integer, a standard name (``"DEBUG"``, ``"warning"``), or a
    decimal string. Values below `logging.DEBUG` select Abseil's ``VLOG``
    tiers, matching `absl.logging`'s own convention.
    """
    if isinstance(level, bool):
        raise TypeError("level must be an int or a level name, not a bool")
    if isinstance(level, int):
        return level
    text = str(level).strip()
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return _converter.string_to_standard(text)
    except (AttributeError, KeyError, TypeError) as error:
        raise ValueError(f"unknown log level {level!r}") from error


def _to_python_level(severity: int, verbosity: int) -> int:
    """The standard level for one native entry."""
    if severity > _SEVERITY_INFO or verbosity <= 0:
        return _converter.absl_to_standard(-severity)
    # A VLOG(n) entry is an INFO whose verbosity picks a sub-DEBUG tier.
    return _converter.absl_to_standard(verbosity)


def _emit(
    severity: int,
    verbosity: int,
    filename: str,
    lineno: int,
    message: str,
    unix_seconds: float,
) -> None:
    """Turn one native log entry into a record on the native logger.

    Called by the C++ sink, on whichever thread wrote the entry. The level is
    resolved per record, so a `dictConfig` or ``setLevel`` that lands after
    import takes effect with nothing to re-synchronise.
    """
    logger = logging.getLogger(NATIVE_LOGGER_NAME)
    level = _to_python_level(severity, verbosity)
    if not logger.isEnabledFor(level):
        return
    record = logger.makeRecord(
        logger.name, level, filename, lineno, message, (), None
    )
    record.created = unix_seconds
    record.msecs = (unix_seconds - int(unix_seconds)) * 1000.0
    logger.handle(record)


def _push_native_level(level: int) -> None:
    """Mirror a standard level onto the native runtime's own filtering.

    While bridged, only ``VLOG`` is gated natively: everything from INFO up is
    handed to the sink unconditionally and filtered in Python, which is what
    lets a later ``setLevel`` or `logging.config.dictConfig` take effect with
    nothing to re-synchronise. Unbridged, Abseil does the filtering itself and
    the severity threshold has to carry the level.
    """
    # ``standard_to_absl`` returns absl's own scale, where DEBUG is 1 and each
    # step below it is the next VLOG tier. That number is the vlog threshold;
    # anything at INFO or coarser maps to 0, which turns VLOG off.
    absl_level = _converter.standard_to_absl(level)
    _native.set_vlog_level(max(absl_level, 0))
    if _bridged:
        _native.set_min_log_level(_SEVERITY_INFO)
        _native.set_stderr_threshold(_SEVERITY_FATAL)
        return
    severity = _converter.absl_to_cpp(absl_level)
    _native.set_min_log_level(severity)
    _native.set_stderr_threshold(severity)


def _bridge_allowed() -> bool:
    return os.environ.get(_BRIDGE_ENV, "").strip().lower() not in {
        "0",
        "false",
        "no",
    }


def _install_bridge(enabled: bool) -> None:
    """Route native entries through Python, or hand them back to Abseil.

    Thresholds belong to `_push_native_level`, which the caller runs next.
    """
    global _bridged
    if enabled != _bridged:
        _native.set_log_sink(_emit if enabled else None)
    _bridged = enabled


def set_level(level: int | str) -> int:
    """Set the level for A11's Python and native logging alike.

    Applies to the ``a11`` logger, to `absl.logging`'s verbosity (which A11's
    own modules log through), and to the native ``VLOG`` threshold. Returns
    the standard level applied.
    """
    resolved = parse_level(level)
    logging.getLogger(LOGGER_NAME).setLevel(resolved)
    _absl.set_verbosity(_converter.standard_to_absl(resolved))
    _push_native_level(resolved)
    return resolved


def get_level() -> int:
    """The effective standard level of the ``a11`` logger."""
    return logging.getLogger(LOGGER_NAME).getEffectiveLevel()


def sync() -> None:
    """Re-read the effective Python level and push it to the runtime.

    Records are filtered in Python, so this is only needed to pick up a
    ``VLOG`` tier after reconfiguring logging behind A11's back — a
    `logging.config.dictConfig` that puts the ``a11`` logger below
    `logging.DEBUG`.
    """
    _push_native_level(get_level())


def enable(level: int | str = logging.INFO, *, bridge: bool = True) -> int:
    """Turn A11's logging on at ``level`` and return the level applied.

    A handler is installed only when the root logger has none, and it is
    `absl.logging`'s, so output keeps Abseil's shape. Under an application
    that already configured logging, this only sets levels.

    Pass ``bridge=False`` to leave native entries on Abseil's own stderr path
    rather than routing them through `logging`.
    """
    if not logging.getLogger().handlers:
        # A11 is a library, not an absl app, so absl's flags are never parsed
        # and its "Logging before flag parsing" notice says nothing useful.
        _absl._warn_preinit_stderr = False
        _absl.use_absl_handler()
    _install_bridge(bridge and _bridge_allowed())
    resolved = set_level(level)
    logging.getLogger(NATIVE_LOGGER_NAME).setLevel(logging.NOTSET)
    return resolved


def disable() -> None:
    """Silence A11's logging, native side included."""
    _install_bridge(False)
    _native.set_min_log_level(_SEVERITY_FATAL)
    _native.set_stderr_threshold(_SEVERITY_FATAL)
    _native.set_vlog_level(0)
    logging.getLogger(LOGGER_NAME).setLevel(logging.CRITICAL + 1)


def _absl_was_configured() -> bool:
    """Whether the process drove `absl.logging` itself."""
    try:
        if not _absl.FLAGS["verbosity"].using_default_value:
            return True
    except (AttributeError, KeyError):
        pass
    try:
        return _absl.get_absl_handler() in logging.root.handlers
    except AssertionError:
        return False


def _level_from_env() -> int | None:
    raw = os.environ.get(_LEVEL_ENV, "").strip()
    if not raw:
        return None
    try:
        return parse_level(raw)
    except (TypeError, ValueError):
        return None


def _configure_from_context() -> int:
    """Adopt the importing process's logging configuration.

    Runs once, from ``a11/__init__.py``. See the module docstring for the
    order; the level is applied without installing a handler, so importing
    A11 never changes where anyone else's records go.
    """
    logger = logging.getLogger(LOGGER_NAME)
    stdlib_configured = bool(logging.root.handlers) or (
        logging.root.level != logging.WARNING
    )

    if _absl_was_configured():
        level = _converter.absl_to_standard(_absl.get_verbosity())
    elif stdlib_configured:
        level = logger.getEffectiveLevel()
    else:
        level = _level_from_env() or DEFAULT_LEVEL

    _install_bridge(_bridge_allowed())
    _push_native_level(level)
    # Pinned only when the resolved level is not what inheritance already
    # gives, so a later basicConfig on the root logger still reaches A11.
    if logger.level == logging.NOTSET and level != logger.getEffectiveLevel():
        logger.setLevel(level)
    return level


def __getattr__(name: str) -> Any:
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


__all__ = [
    "DEFAULT_LEVEL",
    "LOGGER_NAME",
    "NATIVE_LOGGER_NAME",
    "disable",
    "enable",
    "get_level",
    "get_logger",
    "parse_level",
    "set_level",
    "sync",
]
