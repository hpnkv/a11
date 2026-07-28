import ast
import subprocess
import sys
from pathlib import Path

import a11

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
