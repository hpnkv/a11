import ast
import re
import subprocess
import sys
from pathlib import Path

import pytest

import a11
from a11.status import StatusCode, StatusException

ROOT = Path(__file__).resolve().parents[2]


def test_native_stub_and_pep561_marker_are_available() -> None:
    package = Path(a11.__file__).resolve().parent
    stub = package / "_native.pyi"
    marker = package / "py.typed"

    assert marker.is_file()
    source = stub.read_text()
    ast.parse(source, filename=str(stub))
    for declaration in (
        "class Http2Client:",
        "class Http2Server:",
        "class WebRtcWireStream(WireStream):",
        "class WebSocketSignallingServer:",
        "class WebSocketWireStream(WireStream):",
        "async def __aenter__(self)",
        "async def put_chunk(",
    ):
        assert declaration in source


def test_native_stub_is_fresh() -> None:
    subprocess.run(
        [sys.executable, ROOT / "scripts/generate_stubs.py", "--check"],
        cwd=ROOT,
        check=True,
    )


def _stub_signatures() -> str:
    """The stub with runs of whitespace collapsed, so wrapping is irrelevant."""
    source = (Path(a11.__file__).resolve().parent / "_native.pyi").read_text()
    return re.sub(r"\s+", " ", source)


def test_a_reader_resolves_its_result_from_obj_type() -> None:
    """``T`` defaults to ``Any``, so one signature covers both call shapes."""
    source = _stub_signatures()

    assert 'T = typing_extensions.TypeVar("T", default=typing.Any)' in source
    for reader in ("next", "next_object"):
        assert (
            f"async def {reader}( self, obj_type: type[T] | None = None,"
            " timeout: Duration | None = None, mimetype_patterns:"
            ' str | typing.Sequence[str] = "", ) -> T | None:' in source
        ), reader


def test_allow_none_decides_whether_a_reader_can_return_none() -> None:
    source = _stub_signatures()

    for reader, complete in (
        ("consume", "T"),
        ("consume_chunk", "Chunk"),
        ("consume_fragment", "NodeFragment"),
    ):
        assert f"async def {reader}(" in source.replace(
            "@typing.overload async def", "async def"
        ), reader
        assert (
            f"allow_none: typing.Literal[False] = False, ) -> {complete}:"
            in source
        ), reader
        assert (
            f"allow_none: typing.Literal[True] = True, )"
            f" -> {complete} | None: ..." in source
        ), reader
        # A computed flag still type-checks, at the cost of the union. The
        # trailing comma depends on whether black wrapped the signature.
        assert re.search(
            rf"allow_none: bool = False,? \) -> {re.escape(complete)}"
            r" \| None: \.\.\.",
            source,
        ), reader


def test_decode_decides_whether_a_header_reads_as_str_or_bytes() -> None:
    source = _stub_signatures()

    assert (
        "self, name: str, decode: typing.Literal[False] = False )"
        " -> bytes | None:" in source
    )
    assert (
        "self, name: str, decode: typing.Literal[True] = True )"
        " -> str | None: ..." in source
    )


def test_a_future_names_what_it_resolves_to() -> None:
    source = _stub_signatures()

    assert "-> asyncio.Future[int]:" in source
    assert "-> asyncio.Future[None]:" in source
    assert "-> asyncio.Future[Status]:" in source


def test_only_a_genuinely_untyped_result_is_left_as_any() -> None:
    """``__anext__`` has no ``obj_type`` to resolve against; nothing else."""
    source = (Path(a11.__file__).resolve().parent / "_native.pyi").read_text()

    assert source.count("-> typing.Any") == 1
    index = source.index("-> typing.Any")
    assert "async def __anext__(" in source[index - 200 : index]


def test_overloads_do_not_change_runtime_dispatch() -> None:
    """The declarations are annotations only; the implementation still runs."""
    action = a11.Action(a11.ActionSchema(name="probe"))
    action.set_header("x-probe", b"value")

    assert action.get_header("x-probe") == b"value"
    assert action.get_header("x-probe", decode=True) == "value"
    assert action.get_header("x-probe", True) == "value"
    assert action.get_header("absent") is None


@pytest.mark.asyncio
async def test_allow_none_still_decides_the_runtime_result() -> None:
    empty = a11.AsyncNode.create("empty-node")
    await empty.drain_and_close()

    assert await empty.consume(allow_none=True) is None
    with pytest.raises(StatusException) as raised:
        await empty.consume()
    assert raised.value.status.code == StatusCode.FAILED_PRECONDITION

    whole = a11.AsyncNode.create("whole-node")
    await whole.put_final("only")
    await whole.drain_and_close()
    assert await whole.consume(obj_type=str) == "only"
