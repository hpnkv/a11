"""Public facade for the native action registry."""

from a11 import _native

from a11._native import ActionRegistry

ActionRegistry.__module__ = __name__

__all__ = ["ActionRegistry"]
