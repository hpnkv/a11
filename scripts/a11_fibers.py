#!/usr/bin/env python3
"""Print A11 fiber wait state and parked stacks from a debugger.

A blocked A11 fiber's stack is an mmap'd region no OS thread points at, so
``thread apply all bt`` shows only pool workers parked in the scheduler. This
script walks the fiber registry in the target's memory and unwinds each parked
stack from the frame pointer the fiber recorded when it blocked.

The in-process report (``thread::FormatFiberReport``, ``A11_FIBER_WATCHDOG``,
``kill -USR2``) covers most cases and needs no debugger. Use this one when
nothing inside the process can run, or for a core file.

LLDB::

    (lldb) command script import scripts/a11_fibers.py
    (lldb) a11-fibers

GDB::

    (gdb) source scripts/a11_fibers.py
    (gdb) a11-fibers

The repository's ``.lldbinit`` and ``.gdbinit`` run the import for you; both
debuggers require a one-line opt-in before reading a directory-local init file.
See ``doc/docs/guides/debugging-concurrency.md``.

``a11-hang`` prints thread backtraces and then the fiber report, which is the
pair a hang usually needs.

Reads ``a11_fiber_layout`` for field offsets rather than a compiled-in copy of
the struct, so it keeps working when a field is added to
``cpp/thread/thread/fiber_diagnostics.h``.
"""

from __future__ import annotations

import struct

#: Bump when a11_fiber_layout.version changes incompatibly.
SUPPORTED_LAYOUT_VERSION = 1

#: Field order of the A11FiberLayout struct in cpp/thread/thread/introspect.cc.
LAYOUT_FIELDS = (
    "version",
    "record_size",
    "name_capacity",
    "max_selectables",
    "id",
    "parent_id",
    "context",
    "stack_lo",
    "stack_hi",
    "creation_pc",
    "name",
    "epoch",
    "wait_kind",
    "wait_object",
    "wait_owner_context",
    "wait_fp",
    "wait_started_nanos",
    "wait_deadline_nanos",
    "selectables",
    "selectable_count",
    "waits_completed",
    "reg_next",
    "reg_prev",
)

WAIT_KINDS = {
    0: "running",
    1: "condvar",
    2: "mutex",
    3: "select",
    4: "sleep",
    5: "join",
    6: "os-thread",
}

#: Matches thread::internal::kMaxWalkedFrames.
MAX_FRAMES = 128
#: Matches the fallback in thread/internal/stack_walk.cc.
FALLBACK_SPAN_LIMIT = 16 * 1024 * 1024


# Bare "file:line", the shape LLDB and GDB print themselves and which an IDE
# console resolves through its own file index. The debug-info directory is not
# used: -ffile-prefix-map makes it relative to the source tree, and a debugger
# joins it with the build directory, giving a path that does not exist.
def _source_location(filename, line):
    return f"{filename}:{line}" if filename and line else ""


class Target:
    """What this script needs from a debugger, so both are driven alike."""

    def read(self, address: int, size: int) -> bytes | None:
        raise NotImplementedError

    def symbol_address(self, name: str) -> int | None:
        raise NotImplementedError

    def describe(self, pc: int) -> str:
        raise NotImplementedError

    @property
    def pointer_size(self) -> int:
        return 8

    @property
    def frame_alignment(self) -> int:
        return 16

    def write(self, text: str) -> None:
        print(text, end="")


class Record:
    """One FiberDiagnostics, read out of the target."""

    def __init__(self, target: Target, layout: dict[str, int], address: int):
        self.address = address
        blob = target.read(address, layout["record_size"])
        if blob is None:
            raise ValueError(f"cannot read fiber record at {address:#x}")

        def u64(field: str) -> int:
            return struct.unpack_from("<Q", blob, layout[field])[0]

        def u32(field: str) -> int:
            return struct.unpack_from("<I", blob, layout[field])[0]

        self.id = u64("id")
        self.parent_id = u64("parent_id")
        self.context = u64("context")
        self.stack_lo = u64("stack_lo")
        self.stack_hi = u64("stack_hi")
        self.creation_pc = u64("creation_pc")
        self.epoch = u32("epoch")
        self.wait_kind = blob[layout["wait_kind"]]
        self.wait_object = u64("wait_object")
        self.wait_owner_context = u64("wait_owner_context")
        self.wait_fp = u64("wait_fp")
        self.waits_completed = u64("waits_completed")
        self.reg_next = u64("reg_next")

        name = blob[layout["name"] : layout["name"] + layout["name_capacity"]]
        self.name = name.split(b"\0", 1)[0].decode("utf-8", "replace")

        count = min(u32("selectable_count"), layout["max_selectables"])
        self.selectables = [
            struct.unpack_from("<Q", blob, layout["selectables"] + index * 8)[0]
            for index in range(count)
        ]

    @property
    def kind_name(self) -> str:
        return WAIT_KINDS.get(self.wait_kind, f"kind-{self.wait_kind}")

    @property
    def parked(self) -> bool:
        return self.wait_kind not in (0, 6) and (self.epoch & 1) == 1


def read_layout(target: Target) -> dict[str, int]:
    address = target.symbol_address("a11_fiber_layout")
    if address is None:
        raise LookupError(
            "a11_fiber_layout not found: the target is not an A11 binary, or "
            "was built before thread/introspect.h existed"
        )
    blob = target.read(address, 4 * len(LAYOUT_FIELDS))
    if blob is None:
        raise LookupError(f"cannot read a11_fiber_layout at {address:#x}")
    values = struct.unpack_from(f"<{len(LAYOUT_FIELDS)}I", blob)
    layout = dict(zip(LAYOUT_FIELDS, values))
    if layout["version"] != SUPPORTED_LAYOUT_VERSION:
        raise LookupError(
            f"a11_fiber_layout version {layout['version']} is not the "
            f"{SUPPORTED_LAYOUT_VERSION} this script reads; update "
            "scripts/a11_fibers.py"
        )
    return layout


def read_registry(target: Target, layout: dict[str, int]) -> list[Record]:
    head_symbol = target.symbol_address("a11_fiber_registry_head")
    if head_symbol is None:
        raise LookupError("a11_fiber_registry_head not found")
    blob = target.read(head_symbol, 8)
    if blob is None:
        raise LookupError("cannot read a11_fiber_registry_head")

    records: list[Record] = []
    seen: set[int] = set()
    address = struct.unpack("<Q", blob)[0]
    # Bounded, and duplicate-checked: a list read from a live process can be
    # mid-update.
    while address != 0 and address not in seen and len(records) < 100_000:
        seen.add(address)
        try:
            record = Record(target, layout, address)
        except ValueError:
            break
        records.append(record)
        address = record.reg_next
    return records


def walk_frames(target: Target, record: Record) -> list[int]:
    """The validated frame-pointer walk of thread/internal/stack_walk.cc."""
    if not record.parked or record.wait_fp == 0:
        return []
    mask = (1 << 56) - 1 if target.frame_alignment == 16 else (1 << 64) - 1
    low, high = record.stack_lo & mask, record.stack_hi & mask
    previous, frame = 0, record.wait_fp & mask
    pcs: list[int] = []
    while len(pcs) < MAX_FRAMES:
        if frame == 0 or frame % target.frame_alignment != 0:
            break
        if frame <= previous:
            break
        if low or high:
            if frame < low or frame + 16 > high:
                break
        elif frame - previous >= FALLBACK_SPAN_LIMIT:
            break
        blob = target.read(frame, 16)
        if blob is None:
            break
        next_frame, return_address = struct.unpack("<QQ", blob)
        if return_address == 0:
            break
        pcs.append(return_address)
        previous, frame = frame, next_frame & mask
    return pcs


def report(target: Target, max_frames: int = 24) -> None:
    layout = read_layout(target)
    records = read_registry(target, layout)
    by_context = {r.context: r.id for r in records if r.context}
    by_address = {r.address: r.id for r in records}

    census: dict[str, int] = {}
    for record in records:
        census[record.kind_name] = census.get(record.kind_name, 0) + 1

    target.write(f"=== A11 fiber dump: {len(records)} live fibers\n")
    target.write(
        "census: " + " ".join(f"{k}={v}" for k, v in sorted(census.items()))
    )
    target.write("\n")

    waits_for: dict[int, int] = {}
    for record in records:
        blocker = by_context.get(record.wait_owner_context)
        if blocker is None and record.kind_name == "join":
            blocker = by_address.get(record.wait_object)
        if blocker is not None and blocker != record.id:
            waits_for[record.id] = blocker

    for cycle in find_cycles(waits_for):
        target.write(f"\n--- deadlock: wait cycle of {len(cycle)} fibers ---\n")
        for fiber_id in cycle:
            target.write(f"  F#{fiber_id} waits for F#{waits_for[fiber_id]}\n")

    target.write("\n")
    for record in sorted(records, key=lambda r: r.id):
        if record.kind_name in ("running", "os-thread"):
            continue
        label = (
            f'F#{record.id} "{record.name}"'
            if record.name
            else f"F#{record.id}"
        )
        target.write(f"{label}  parent=F#{record.parent_id}")
        if record.creation_pc:
            target.write(f"  created-at {target.describe(record.creation_pc)}")
        target.write(f"\n     {record.kind_name}({record.wait_object:#x})")
        blocker = waits_for.get(record.id)
        if blocker is not None:
            target.write(f" held-by=F#{blocker}")
        target.write("\n")
        for selectable in record.selectables:
            target.write(f"     case {selectable:#x}\n")
        frames = walk_frames(target, record)
        if not frames:
            target.write("     (no frames recovered)\n")
        for index, pc in enumerate(frames[:max_frames]):
            target.write(f"     #{index:<2} {target.describe(pc)}\n")


def find_cycles(waits_for: dict[int, int]) -> list[list[int]]:
    cycles: list[list[int]] = []
    done: set[int] = set()
    for start in waits_for:
        if start in done:
            continue
        path: list[int] = []
        position: dict[int, int] = {}
        node = start
        while True:
            if node in position:
                cycles.append(path[position[node] :])
                break
            if node in done or node not in waits_for:
                break
            position[node] = len(path)
            path.append(node)
            node = waits_for[node]
        done.update(path)
    return cycles


# --------------------------------------------------------------------------
# LLDB
# --------------------------------------------------------------------------

try:
    import lldb
except ImportError:
    lldb = None


class LldbTarget(Target):
    def __init__(self, debugger, result):
        self._target = debugger.GetSelectedTarget()
        self._process = self._target.GetProcess()
        self._result = result

    def read(self, address, size):
        error = lldb.SBError()
        data = self._process.ReadMemory(address, size, error)
        return data if error.Success() else None

    def symbol_address(self, name):
        for symbol_name in (name, f"_{name}"):
            symbols = self._target.FindSymbols(symbol_name)
            for index in range(symbols.GetSize()):
                symbol = symbols.GetContextAtIndex(index).GetSymbol()
                address = symbol.GetStartAddress().GetLoadAddress(self._target)
                if address != lldb.LLDB_INVALID_ADDRESS:
                    return address
        return None

    def describe(self, pc):
        address = lldb.SBAddress(pc, self._target)
        context = self._target.ResolveSymbolContextForAddress(
            address, lldb.eSymbolContextEverything
        )
        function = (
            context.GetFunction().GetName() or context.GetSymbol().GetName()
        )
        line = context.GetLineEntry()
        location = _source_location(
            line.GetFileSpec().GetFilename(), line.GetLine()
        )
        if function and line.IsValid() and location:
            return f"{function} at {location}"
        return f"{function or '(unsymbolized)'} ({pc:#x})"

    @property
    def frame_alignment(self):
        triple = self._target.GetTriple() or ""
        return (
            16 if triple.startswith("arm") or triple.startswith("aarch") else 8
        )

    def write(self, text):
        self._result.AppendMessage(text.rstrip("\n")) if text.strip() else None


def _lldb_command(debugger, command, result, internal_dict):
    target = LldbTarget(debugger, result)
    if not target._process.IsValid():
        result.SetError("no running process or core file")
        return
    lines: list[str] = []
    target.write = lambda text: lines.append(text)  # type: ignore[method-assign]
    try:
        report(target, max_frames=int(command) if command.strip() else 24)
    except (LookupError, ValueError) as error:
        result.SetError(str(error))
        return
    result.AppendMessage("".join(lines))


def _lldb_hang_command(debugger, command, result, internal_dict):
    # Both halves: the OS threads a debugger can see, and the fibers it cannot.
    # A hang is usually only explained by the two together.
    debugger.HandleCommand("thread backtrace all")
    _lldb_command(debugger, command, result, internal_dict)


def __lldb_init_module(debugger, internal_dict):  # noqa: N807
    debugger.HandleCommand(
        f"command script add -f {__name__}._lldb_command a11-fibers"
    )
    debugger.HandleCommand(
        f"command script add -f {__name__}._lldb_hang_command a11-hang"
    )
    print("a11-fibers: print A11 fiber wait state and parked stacks")
    print("a11-hang:   thread backtraces followed by a11-fibers")


# --------------------------------------------------------------------------
# GDB
# --------------------------------------------------------------------------

try:
    import gdb
except ImportError:
    gdb = None


if gdb is not None:

    class GdbTarget(Target):
        def read(self, address, size):
            try:
                return bytes(gdb.selected_inferior().read_memory(address, size))
            except gdb.MemoryError:
                return None

        def symbol_address(self, name):
            try:
                return int(gdb.parse_and_eval(f"&{name}"))
            except gdb.error:
                return None

        def describe(self, pc):
            try:
                block = gdb.block_for_pc(pc)
            except gdb.error:
                block = None
            name = None
            while block is not None and name is None:
                name = block.function.name if block.function else None
                block = block.superblock
            try:
                line = gdb.find_pc_line(pc)
                if name and line.symtab is not None:
                    return f"{name} at {line.symtab.filename}:{line.line}"
            except gdb.error:
                pass
            return f"{name or '(unsymbolized)'} ({pc:#x})"

        @property
        def frame_alignment(self):
            architecture = gdb.selected_frame().architecture().name()
            return 16 if "aarch64" in architecture else 8

    class A11Fibers(gdb.Command):
        """Print A11 fiber wait state and parked stacks."""

        def __init__(self):
            super().__init__("a11-fibers", gdb.COMMAND_STATUS)

        def invoke(self, argument, from_tty):
            target = GdbTarget()
            try:
                report(target, max_frames=int(argument) if argument else 24)
            except (LookupError, ValueError) as error:
                raise gdb.GdbError(str(error)) from error

    class A11Hang(gdb.Command):
        """Thread backtraces followed by A11 fiber wait state.

        A hang is usually only explained by the two together: the OS threads a
        debugger can see, and the fibers it cannot.
        """

        def __init__(self):
            super().__init__("a11-hang", gdb.COMMAND_STATUS)

        def invoke(self, argument, from_tty):
            gdb.execute("thread apply all bt")
            gdb.execute(f"a11-fibers {argument}".strip())

    A11Fibers()
    A11Hang()
    print("a11-fibers: print A11 fiber wait state and parked stacks")
    print("a11-hang:   thread backtraces followed by a11-fibers")
