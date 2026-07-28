"""Public facade for the native action registry."""

from a11 import _native

ActionRegistry = _native.ActionRegistry
ActionRegistry.__module__ = __name__

__all__ = ["ActionRegistry"]
