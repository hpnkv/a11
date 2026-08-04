"""Attach an idiomatic Python protocol onto a bound native class.

A11's public, stateful runtime types --
[AsyncNode][a11.nodes.async_node.AsyncNode],
[Session][a11.service.session.Session],
[ChunkStore][a11.stores.chunk_store.ChunkStore]
and friends -- *are* the classes exported by the native ``a11._native``
extension, not Python subclasses of them (see ``AGENTS.md``, "Python boundary").
The idiomatic asynchronous, streaming, and Pydantic-flavoured behaviour that
makes them pleasant to use from Python is *attached* onto those bound classes
rather than implemented in a parallel hierarchy.

Historically that attachment was written as a flat sequence of module-level
functions (``def _put(node, ...)``) followed by a block of
``NativeClass.put = _put`` assignments, which obscured the shape of the class it
was building. `attach_protocol` lets the same behaviour be written as an
ordinary ``class`` body -- with ``self``, type annotations, and docstrings --
and then copied, member for member, onto the native class:

.. code-block:: python

    class _WidgetProtocol:
        \"\"\"What the native Widget looks like from Python.\"\"\"

        async def do(self, value: int) -> None:
            ...

    attach_protocol(Widget, _WidgetProtocol)

The copy is a plain `setattr` of each member object, so the result is
byte-for-byte equivalent to the explicit assignments it replaces: methods still
receive the native instance as their first argument, attribute lookup is
unchanged, and there is no wrapper on the hot path. The protocol class is only a
readable, statically analysable *description*; instances are never created from
it.
"""

from __future__ import annotations

import inspect
from typing import TypeVar

_NativeClass = TypeVar("_NativeClass", bound=type)

# Names CPython injects into every ``class`` body. Copying these onto the native
# class would clobber its identity (``__module__``, ``__dict__``, ``__doc__``)
# or attach meaningless state, so they are never carried over. Everything else
# -- including deliberate dunders such as ``__init__``, ``__aenter__`` and
# ``__getitem__`` -- is part of the protocol and is copied.
_CLASS_MACHINERY = frozenset(
    {
        "__module__",
        "__qualname__",
        "__doc__",
        "__dict__",
        "__weakref__",
        "__annotations__",
        # Emitted by CPython 3.13+ for every class body.
        "__firstlineno__",
        "__static_attributes__",
    }
)


def attach_protocol(native: _NativeClass, protocol: type) -> _NativeClass:
    """Copy every member of ``protocol`` onto the bound native class ``native``.

    Each function, ``property``, ``staticmethod``, ``classmethod``, or plain
    attribute defined in ``protocol`` is assigned onto ``native``. Function
    docstrings are dedented first so stub generators do not mistake their
    class-body indentation for a literal code block. Members otherwise retain
    their original types and signatures. Class-body machinery such as
    ``__module__`` and ``__dict__`` is skipped.

    The members are copied by reference -- no wrapping -- so behaviour and
    performance are identical to assigning them onto ``native`` directly. The
    functions run bound to native instances; ``protocol`` itself is never
    instantiated.

    Args:
        native: The bound ``a11._native`` class to extend in place.
        protocol: A class whose body describes the Python-facing behaviour.

    Returns:
        ``native``, so callers can assign or chain from the return value.
    """
    for name, member in vars(protocol).items():
        if name in _CLASS_MACHINERY:
            continue
        if inspect.isfunction(member) and member.__doc__:
            member.__doc__ = inspect.cleandoc(member.__doc__)
        setattr(native, name, member)
    return native


__all__ = ["attach_protocol"]
