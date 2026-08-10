"""Keep overload-only stub members in the generated API reference.

`a11/_native.pyi` narrows a few readers with `typing.overload` — `consume`
returns `T` when `allow_none` is false and `T | None` when it is true — and a
stub may not carry the implementation those overloads belong to, because a type
checker rejects one there.

griffe collects such a function into ``parent.overloads`` but creates no member
for it, so the method would vanish from the page and every cross-reference to
it would fail the strict build. This promotes the first overload to the member,
leaving ``overloads`` in place so mkdocstrings still renders each signature.
"""

from __future__ import annotations

from typing import Any

from griffe import Class, Extension, Module


def _promote(obj: Class | Module) -> None:
    overloads: dict[str, list[Any]] = getattr(obj, "overloads", None) or {}
    for name, signatures in overloads.items():
        if not signatures or name in obj.members:
            continue
        # The first overload carries the docstring, by convention and by how
        # scripts/generate_stubs.py emits them.
        member = signatures[0]
        member.overloads = list(signatures)
        obj.set_member(name, member)


class PromoteOverloads(Extension):
    """Give every overload-only function a member to hang its docs on."""

    def on_class_members(self, *, cls: Class, **_: Any) -> None:
        _promote(cls)

    def on_module_members(self, *, mod: Module, **_: Any) -> None:
        _promote(mod)
