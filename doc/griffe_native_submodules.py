"""Give the spliced native submodules back their module shape for griffe.

`a11._native` is one compiled extension with submodules (`flow`), and
`scripts/generate_stubs.py` splices each of those into the single checked-in
`a11/_native.pyi` as a private class plus an attribute annotated with it --
`class _FlowModule: ...` and `flow: _FlowModule` -- rather than keeping a stub
*package* in a directory beside `_native.cpython-*.so`.

That reads correctly to a type checker, but griffe resolves aliases by walking
member paths: `from a11._native.flow import FlowPlan` asks it for
`a11._native` → `flow` → `FlowPlan`, and `flow` is an attribute, which has no
members. The alias never resolves and the strict build fails on the API page
that documents the class.

So this undoes the splice for documentation only: each `<name>: _XModule`
attribute becomes a real `griffe.Module` carrying the class's members, which is
what the runtime module actually is.
"""

from __future__ import annotations

from typing import Any

from griffe import Alias, Attribute, Class, Extension, Module


def _annotation_name(attribute: Attribute) -> str | None:
    """The bare name a module attribute is annotated with, if it is one."""
    annotation = attribute.annotation
    if annotation is None:
        return None
    name = getattr(annotation, "name", None)
    return name if isinstance(name, str) else None


class NativeSubmodules(Extension):
    """Turn each spliced `_XModule` class back into `a11._native.<x>`."""

    def on_module_members(self, *, mod: Module, **_: Any) -> None:
        if mod.path != "a11._native":
            return
        for name, member in list(mod.members.items()):
            if isinstance(member, Alias) or not isinstance(member, Attribute):
                continue
            spliced = mod.members.get(_annotation_name(member) or "")
            if not isinstance(spliced, Class) or spliced.name == name:
                continue

            submodule = Module(
                name,
                filepath=mod.filepath,
                docstring=spliced.docstring,
                parent=mod,
            )
            for child_name, child in list(spliced.members.items()):
                submodule.set_member(child_name, child)
            # The attribute and the class both go: what is left is the module,
            # under the name the attribute had, with the members the class held.
            del mod.members[spliced.name]
            mod.set_member(name, submodule)
