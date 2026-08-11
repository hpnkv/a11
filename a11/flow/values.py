"""Evaluating Flow expressions, and the fixed function set they may use.

Flow expressions are pure: they read values, compare them, take them apart, and
build new ones. There is no way to call out to arbitrary code, which is what
keeps a flow safe to accept from somewhere else and run -- it can only do what
its calls and its pipes say it does.
"""

from __future__ import annotations

import enum as _enum
import json as _json
import re
from collections.abc import Mapping, Sequence
from typing import Any

from a11 import timing
from a11.data import types
from a11.flow import plan as _plan
from a11.flow.lexer import DURATION_UNITS
from a11.flow import syntax
from a11.status import Status, StatusCode

#: A value that is not there: an absent key, an empty stream, a missing header.
MISSING = None


def _fail(message: str):
    return Status(
        code=StatusCode.INVALID_ARGUMENT, message=message
    ).to_exception()


def lookup(value: Any, key: Any) -> Any:
    """Take ``key`` out of ``value``: a mapping key, an index, or an attribute.

    Returns ``None`` when it is not there, because a flow reading a field a
    producer did not send should be able to say ``if not thing.field`` rather
    than fall over.
    """
    if value is None:
        return None
    if isinstance(value, Mapping):
        return value.get(key, None)
    if isinstance(key, int) and isinstance(value, Sequence) and not isinstance(
        value, (str, bytes)
    ):
        return value[key] if -len(value) <= key < len(value) else None
    if isinstance(key, str):
        if isinstance(value, (str, bytes)):
            return None
        return getattr(value, key, None)
    return None


def as_text(value: Any) -> str:
    """Render a value as text the way the ``text`` stage and builtin do."""
    if value is None:
        return ""
    if isinstance(value, str):
        return value
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return repr(value)
    if isinstance(value, timing.Duration):
        return format_duration(value)
    if isinstance(value, timing.Time):
        return format_time(value)
    if isinstance(value, _enum.Enum):
        # A field of a type a flow cast something into: what it is on the wire
        # is the value, not `Role.USER`.
        return as_text(value.value)
    return _json.dumps(value, default=str, sort_keys=True)


def as_number(value: Any) -> float | int:
    """Coerce to a number, or 0 when there is nothing to coerce."""
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, (int, float)):
        return value
    if isinstance(value, timing.Duration):
        return duration_seconds(value)
    if isinstance(value, timing.Time):
        return value.nanoseconds_since_epoch / 1e9
    if isinstance(value, str):
        try:
            trimmed = value.strip()
            if trimmed.lstrip("-").isdigit():
                return int(trimmed)
            return float(trimmed)
        except ValueError:
            return 0
    if value is None:
        return 0
    try:
        return len(value)
    except TypeError:
        return 0


def as_json(value: Any) -> Any:
    """Parse text as JSON; leave anything already decoded alone."""
    if isinstance(value, (str, bytes)):
        try:
            return _json.loads(value)
        except ValueError:
            return value
    return value


def truncate(value: Any, size: int) -> Any:
    """Keep the first ``size`` of a value: characters, bytes, or elements.

    This is the stage a caller reaches for when a step's output is far larger
    than the next step needs -- a page of HTML into a model prompt, say. Cutting
    it here means the rest is never serialised, never sent, and never paid for.
    """
    if isinstance(value, (str, bytes)):
        return value[:size]
    if isinstance(value, Mapping):
        return dict(list(value.items())[:size])
    if isinstance(value, Sequence):
        return value[:size]
    return value


# --- Times and durations ------------------------------------------------------


def _is_timelike(value: Any) -> bool:
    return isinstance(value, (timing.Time, timing.Duration))


def duration_seconds(value: timing.Duration) -> float:
    """A duration as a number of seconds, for every duration there is.

    `Duration.float_seconds` refuses a negative one and answers ``None`` for an
    infinite one, both of which a flow can hold: subtracting two instants the
    other way round gives the first, and a timeout that never fires is the
    second. The language turns values into numbers rather than dying on them,
    so this is the conversion the rest of the module uses.
    """
    if value.is_infinite():
        return float("inf") if value > timing.zero_duration() else float("-inf")
    return value.nanoseconds_value / 1e9


#: One piece of a written duration: a number and the unit it carries.
_DURATION_PIECE = re.compile(
    r"([+-]?[0-9]+(?:\.[0-9]+)?)\s*([a-zA-Z]*)"
)


def parse_duration(text: str) -> timing.Duration | None:
    """A `Duration` from the way the language writes one, or ``None``.

    The spelling is the source's and the formatter's, both ways round:
    ``30s``, ``250ms``, ``1m30s``, ``1m30s500ms``, ``forever``, and a bare
    number of seconds. A duration a flow put on a port comes back as text often
    enough -- through a header, a JSON field, a model's answer -- that reading
    it back has to be as ordinary as writing it.
    """
    trimmed = text.strip()
    if not trimmed:
        return None
    lowered = trimmed.lower()
    if lowered in ("forever", "infinite", "inf"):
        return timing.infinite_duration()
    total = 0.0
    sign = 1.0
    position = 0
    pieces = 0
    while position < len(trimmed):
        if trimmed[position] in " \t":
            position += 1
            continue
        match = _DURATION_PIECE.match(trimmed, position)
        if match is None:
            return None
        number, unit = match.group(1), match.group(2)
        if unit and unit not in DURATION_UNITS:
            return None
        # `-1m30s` is a minute and a half, backwards, not a minute back and
        # half a second forwards: the sign belongs to the whole.
        if pieces == 0 and number.startswith("-"):
            sign = -1.0
        total += abs(float(number)) * DURATION_UNITS.get(unit, 1.0)
        position = match.end()
        pieces += 1
    if not pieces:
        return None
    return seconds_duration(sign * total)


def as_duration(value: Any) -> timing.Duration:
    """A `Duration` from a duration, from written text, or from seconds."""
    if isinstance(value, timing.Duration):
        return value
    if isinstance(value, str):
        parsed = parse_duration(value)
        if parsed is not None:
            return parsed
    return seconds_duration(float(as_number(value)))


def seconds_duration(total: float) -> timing.Duration:
    """A duration of ``total`` seconds, negative ones included.

    `Duration.seconds` reads a negative number as *infinite*, because in A11 a
    negative timeout is one that never fires. Here a number is a length: what
    `-30` means beside a duration is thirty seconds the other way, so a
    negative one is built by subtracting.
    """
    if total >= 0:
        return timing.Duration.seconds(total)
    return timing.zero_duration() - timing.Duration.seconds(-total)


def as_time(value: Any) -> timing.Time:
    """An instant from an instant, from RFC 3339 text, or from epoch seconds.

    The other half of [format_time][a11.flow.values.format_time]: a timestamp
    reaches a flow as a string far more often than as a `Time`, and until it is
    one it cannot be compared with `now()` or have a duration added to it.
    """
    if isinstance(value, timing.Time):
        return value
    if isinstance(value, str):
        parsed = _parse_time(value)
        if parsed is not None:
            return parsed
    return timing.Time.from_nanoseconds_since_epoch(
        int(float(as_number(value)) * 1e9)
    )


def _parse_time(text: str) -> timing.Time | None:
    """An instant from RFC 3339 text, as `format_time` writes it."""
    import datetime

    trimmed = text.strip()
    if not trimmed:
        return None
    candidate = trimmed[:-1] + "+00:00" if trimmed.endswith("Z") else trimmed
    try:
        when = datetime.datetime.fromisoformat(candidate)
    except ValueError:
        return None
    if when.tzinfo is None:
        # A timestamp with no zone is UTC here, which is the zone every
        # instant this language writes is in.
        when = when.replace(tzinfo=datetime.timezone.utc)
    return timing.Time.from_nanoseconds_since_epoch(
        int(round(when.timestamp() * 1e9))
    )


def _arithmetic(op: str, left: Any, right: Any) -> Any:
    """``+`` and ``-`` over numbers, durations and instants.

    The combinations that mean something are the ones A11's own types allow:
    a duration plus a duration, an instant plus a duration, and one instant
    minus another giving the duration between them. A number on either side of
    a duration is read as seconds, so ``waited + 30`` needs no ceremony.
    Anything else falls back to numbers, which is what the rest of the
    language does rather than dying on a bad comparison.
    """
    if _is_timelike(left) or _is_timelike(right):
        if isinstance(left, timing.Time) and isinstance(right, timing.Time):
            if op == "-":
                return left - right
            raise _fail("Two instants can be subtracted, but not added.")
        if isinstance(left, timing.Time):
            other = as_duration(right)
            return left + other if op == "+" else left - other
        if isinstance(right, timing.Time):
            if op == "+":
                return right + as_duration(left)
            raise _fail("An instant cannot be subtracted from a duration.")
        first, second = as_duration(left), as_duration(right)
        return first + second if op == "+" else first - second
    first, second = as_number(left), as_number(right)
    return first + second if op == "+" else first - second


#: The units a duration is formatted in, largest first for the compact
#: rendering and by name for a `{:unit}` slot. The lexer's table, ordered: the
#: units a duration may be *written* in are the ones it is rendered in.
_DURATION_UNITS: tuple[tuple[str, float], ...] = tuple(
    sorted(DURATION_UNITS.items(), key=lambda pair: -pair[1])
)


def format_duration(value: timing.Duration, spec: str = "") -> str:
    """A duration as text: ``1m30s`` by default, or one unit if asked.

    ``{:s}`` is the number of seconds, ``{:ms}`` of milliseconds, and so on
    down to nanoseconds -- a bare number, so it can go straight into a metric.
    Without a unit it is the compact form a person reads.
    """
    if value.is_infinite():
        return "forever"
    # Counted in nanoseconds, which is what a `Duration` is: `1500us + 500ns`
    # renders as `1500.5us`, and not as the float arithmetic's opinion of it.
    total = value.nanoseconds_value
    for name, size in _DURATION_UNITS:
        if spec == name:
            return _trim_number(total / (size * 1e9))
    if total == 0:
        return "0s"
    sign = "-" if total < 0 else ""
    total = abs(total)
    pieces: list[str] = []
    coarse = False
    for name, size in _DURATION_UNITS:
        # Microseconds of an hour are noise; microseconds of a millisecond and
        # a half are the value. So the fine units are dropped only once a whole
        # second or more has already been written.
        if name in ("us", "ns") and coarse:
            break
        if size >= 1.0 and pieces:
            coarse = True
        step = int(round(size * 1e9))
        count = total // step
        if count:
            pieces.append(f"{count}{name}")
            total -= count * step
        if total <= 0:
            break
    return sign + ("".join(pieces) or "0s")


def _trim_number(value: float) -> str:
    """A float without a pointless trailing ``.0``."""
    return str(int(value)) if float(value).is_integer() else repr(value)


def format_time(value: timing.Time, spec: str = "") -> str:
    """An instant as text: RFC 3339 in UTC, or a `strftime` pattern.

    ``{:%H:%M:%S}`` is the usual thing to want in a log line; ``{:epoch}`` is
    the seconds since the epoch, for somewhere that wants a number.
    """
    import datetime

    nanoseconds = value.nanoseconds_since_epoch
    if spec == "epoch":
        return _trim_number(nanoseconds / 1e9)
    when = datetime.datetime.fromtimestamp(
        nanoseconds / 1e9, tz=datetime.timezone.utc
    )
    if spec:
        return when.strftime(spec)
    return when.isoformat().replace("+00:00", "Z")


#: One conversion: `%[N$][(spec)][flags][width][.precision]conversion`, which is
#: printf's own shape with one addition -- the parenthesised spec.
_SLOT = re.compile(
    r"""
    %
    (?:(\d+)\$)?          # which value, 1-based, as POSIX writes it
    (?:\(([^)]*)\))?      # a unit or a pattern for the value
    ([-+ 0\#]*)           # flags
    (\d*)                 # width
    (?:\.(\d+))?          # precision
    ([sdifeEgGxXo])       # what to render it as
    """,
    re.VERBOSE,
)

#: What each conversion wants of a value before printf sees it.
_AS_NUMBER = frozenset("difeEgGxXo")


def strformat(template: Any, values: Sequence[Any]) -> str:
    """``template`` with each ``%`` conversion replaced by one of ``values``.

    printf's syntax, because a format string is a thing people already know how
    to read, and because it is *only* a format string: no attribute access, no
    indexing, nothing a template can reach through. A flow's templates can come
    from a model, so that matters more here than the convenience of
    [str.format][] would.

    * ``%s`` takes the next value as text -- which for a `Duration` is ``1m30s``
      and for a `Time` is RFC 3339 -- and ``%d``, ``%f``, ``%x`` and the rest
      render it as a number. Flags, width and precision are printf's own:
      ``%-8s``, ``%06.2f``.
    * ``%2$s`` takes the second value by number, counting from one, and mixing
      that with the automatic order is fine.
    * ``%(SPEC)s`` applies a spec to the value first: a duration unit
      (``%(ms)d``), a strftime pattern for an instant (``%(%H:%M)s``), or
      ``%(epoch)d``.
    * ``%%`` is a literal percent, and a ``%`` that starts nothing recognisable
      is left alone -- ``"100% done"`` says what it looks like.

    A conversion with no value behind it is left as it was written, on the
    grounds that a visible ``%3$s`` in the output is easier to diagnose than a
    flow that died formatting a log line.
    """
    text = as_text(template)
    position = 0

    def replace(match: "re.Match[str]") -> str:
        nonlocal position
        number, spec, flags, width, precision, conversion = match.groups()
        if number:
            index = int(number) - 1
        else:
            index, position = position, position + 1
        if not 0 <= index < len(values):
            return match.group(0)
        value = values[index]
        if spec:
            value = _with_spec(value, spec)
        return _printf(value, flags, width, precision, conversion)

    pieces = []
    for part in re.split(r"(%%)", text):
        pieces.append("%" if part == "%%" else _SLOT.sub(replace, part))
    return "".join(pieces)


def _with_spec(value: Any, spec: str) -> Any:
    """A value with a ``%(SPEC)`` applied: a duration unit, or a time pattern.

    Only times and durations have anything a spec could mean -- printf's own
    flags and precision cover the rest -- so for anything else the value is
    handed on untouched rather than a spec being invented for it.
    """
    if isinstance(value, timing.Duration):
        return format_duration(value, spec)
    if isinstance(value, timing.Time):
        return format_time(value, spec)
    return value


def _printf(
    value: Any, flags: str, width: str, precision: str | None, conversion: str
) -> str:
    """One value through one printf conversion, coercing rather than failing."""
    if conversion == "i":
        conversion = "d"  # printf's synonym; Python's `%` has only `%d`.
    if conversion in _AS_NUMBER:
        number = as_number(value)
        prepared: Any = (
            int(number) if conversion in "dxXo" else float(number)
        )
    else:
        prepared = as_text(value)
    pattern = (
        "%"
        + flags
        + width
        + (f".{precision}" if precision is not None else "")
        + conversion
    )
    try:
        return pattern % (prepared,)
    except (TypeError, ValueError):
        return as_text(value)


def _builtin(name: str, args: list[Any]) -> Any:
    def arg(index: int, default: Any = None) -> Any:
        return args[index] if len(args) > index else default

    if name == "strformat":
        return strformat(arg(0, ""), args[1:])
    if name == "now":
        return timing.now()
    if name == "duration":
        return as_duration(arg(0))
    if name == "time":
        return as_time(arg(0))
    if name == "seconds":
        return duration_seconds(as_duration(arg(0)))

    if name == "len":
        value = arg(0)
        try:
            return len(value)
        except TypeError:
            return 0
    if name == "lower":
        return as_text(arg(0)).lower()
    if name == "upper":
        return as_text(arg(0)).upper()
    if name == "trim":
        return as_text(arg(0)).strip()
    if name == "text":
        return as_text(arg(0))
    if name == "number":
        return as_number(arg(0))
    if name == "bool":
        return bool(arg(0))
    if name == "keys":
        value = arg(0)
        return sorted(value) if isinstance(value, Mapping) else []
    if name == "values":
        value = arg(0)
        if isinstance(value, Mapping):
            return [value[key] for key in sorted(value)]
        return list(value) if isinstance(value, Sequence) else []
    if name == "get":
        found = lookup(arg(0), arg(1))
        return arg(2) if found is None else found
    if name == "join":
        value = arg(0)
        separator = as_text(arg(1, ""))
        if isinstance(value, (str, bytes)):
            return as_text(value)
        if isinstance(value, Sequence):
            return separator.join(as_text(item) for item in value)
        return as_text(value)
    if name == "split":
        separator = arg(1)
        text = as_text(arg(0))
        return text.split(as_text(separator)) if separator else text.split()
    if name == "merge":
        merged: dict[str, Any] = {}
        for value in args:
            if isinstance(value, Mapping):
                merged.update(value)
        return merged
    if name == "contains":
        container, member = arg(0), arg(1)
        try:
            return member in container
        except TypeError:
            return False
    if name in ("starts-with", "ends-with"):
        # A list of candidates is one question, not three: a piece that ends a
        # sentence ends with any of `[".", "?", "!"]`.
        wanted = arg(1)
        options = (
            tuple(as_text(one) for one in wanted)
            if isinstance(wanted, (list, tuple))
            else (as_text(wanted),)
        )
        text = as_text(arg(0))
        if name == "starts-with":
            return text.startswith(options)
        return text.endswith(options)
    if name == "replace":
        return as_text(arg(0)).replace(as_text(arg(1)), as_text(arg(2, "")))
    if name == "slice":
        value = arg(0)
        start = int(as_number(arg(1, 0)))
        stop = arg(2)
        end = None if stop is None else int(as_number(stop))
        if isinstance(value, (str, bytes)) or isinstance(value, Sequence):
            return value[start:end]
        return value
    if name == "default":
        value = arg(0)
        return arg(1) if value in (None, "", [], {}) else value
    if name == "to_chunk":
        return _registry().to_chunk(arg(0), as_text(arg(1, "")))
    if name == "from_chunk":
        value = arg(0)
        if not isinstance(value, types.Chunk):
            # Anything already decoded is already what this asks for.
            return value
        return _registry().from_chunk(value, "", None)
    raise _fail(f"Unknown function {name!r}.")


def _registry():
    """The process-wide serialization registry, fetched when it is needed."""
    from a11.data.serialization import get_global_serialization_registry

    return get_global_serialization_registry()


def coerce(value: Any, type_expression: syntax.TypeExpression) -> Any:
    """Make ``value`` a value of the type ``type_expression`` names.

    What a flow writes by hand is a partial thing -- the fields it cared about
    -- and what a port wants is the type. The two are bridged here: a built-in
    name coerces the way the matching builtin does, and a registered tag is
    validated into its own class, which is where defaults are filled in and a
    mistake is caught.
    """
    name = type_expression.name
    parameters = type_expression.parameters
    lowered = name.lower() if name.isupper() else name

    if lowered in _SCALARS:
        return _SCALARS[lowered](value)
    if lowered in ("list", "array"):
        items = list(value) if isinstance(value, (list, tuple)) else [value]
        if parameters:
            return [coerce(item, parameters[0]) for item in items]
        return items
    if lowered in ("object", "json"):
        return dict(value) if isinstance(value, Mapping) else value
    if lowered == "any":
        return value

    target = _registry().resolve_type(name)
    if target is None:
        raise _fail(
            f"Nothing here knows the type {name!r}. A tag names a type a"
            " serialization registry has been told about, so the module"
            " defining it has to be imported where the flow runs."
        )
    if isinstance(value, target):
        return value
    if target is timing.Duration:
        return as_duration(value)
    if target is timing.Time:
        return as_time(value)
    validate = getattr(target, "model_validate", None)
    if validate is not None:
        return validate(value)
    if isinstance(value, Mapping):
        return target(**value)
    return target(value)


#: How a built-in type name coerces, when one is named in a cast.
_SCALARS = {
    "string": as_text,
    "text": as_text,
    "number": as_number,
    "integer": lambda value: int(as_number(value)),
    "int": lambda value: int(as_number(value)),
    "bool": bool,
    "boolean": bool,
    "bytes": lambda value: (
        value if isinstance(value, bytes) else as_text(value).encode("utf-8")
    ),
}


def evaluate(
    node: syntax.Node,
    ref_values: Mapping[int, Any] | None = None,
    it: Any = MISSING,
) -> Any:
    """Evaluate a resolved expression.

    Args:
        node: The expression, with names already bound to refs.
        ref_values: The first value of each ref the expression reads, by uid.
        it: The value a ``where``/``map`` stage is looking at.
    """
    values = ref_values or {}

    if isinstance(node, _plan.RefValue):
        return values.get(node.ref.uid)
    if isinstance(node, syntax.It):
        return it
    if isinstance(node, syntax.Literal):
        return node.value
    if isinstance(node, syntax.ListLiteral):
        return [evaluate(item, values, it) for item in node.items]
    if isinstance(node, syntax.ObjectLiteral):
        return {
            key: evaluate(value, values, it) for key, value in node.pairs
        }
    if isinstance(node, syntax.Attr):
        return lookup(evaluate(node.base, values, it), node.name)
    if isinstance(node, syntax.Index):
        return lookup(
            evaluate(node.base, values, it), evaluate(node.index, values, it)
        )
    if isinstance(node, syntax.Builtin):
        return _builtin(
            node.name,
            [evaluate(argument, values, it) for argument in node.args],
        )
    if isinstance(node, syntax.TypedValue):
        return coerce(evaluate(node.value, values, it), node.type)
    if isinstance(node, syntax.Unary):
        return not evaluate(node.operand, values, it)
    if isinstance(node, syntax.Binary):
        if node.op == "and":
            left = evaluate(node.left, values, it)
            return evaluate(node.right, values, it) if left else left
        if node.op == "or":
            left = evaluate(node.left, values, it)
            return left if left else evaluate(node.right, values, it)
        left = evaluate(node.left, values, it)
        right = evaluate(node.right, values, it)
        if node.op == "==":
            return left == right
        if node.op == "!=":
            return left != right
        if node.op == "in":
            try:
                return left in right
            except TypeError:
                return False
        if node.op in ("+", "-"):
            return _arithmetic(node.op, left, right)
        # Two times, or two durations, compare as themselves: turning them into
        # text would order `9s` after `10s`, and into numbers would lose the
        # distinction between an instant and a length.
        if _is_timelike(left) and _is_timelike(right):
            pass
        # Ordering compares numbers as numbers and everything else as text, so
        # a flow never dies on `"3" < 5`.
        elif isinstance(left, (int, float)) or isinstance(right, (int, float)):
            left, right = as_number(left), as_number(right)
        else:
            left, right = as_text(left), as_text(right)
        if node.op == "<":
            return left < right
        if node.op == "<=":
            return left <= right
        if node.op == ">":
            return left > right
        return left >= right
    raise _fail(f"Cannot evaluate {type(node).__name__}.")


__all__ = [
    "MISSING",
    "as_duration",
    "as_json",
    "as_number",
    "as_text",
    "as_time",
    "coerce",
    "duration_seconds",
    "evaluate",
    "format_duration",
    "format_time",
    "lookup",
    "parse_duration",
    "seconds_duration",
    "strformat",
    "truncate",
]
