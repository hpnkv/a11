# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Process startup loads only the selected CLI implementation."""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def child(source: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-c", source],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
        env={**os.environ, "A11_NO_ALLOCATOR_PRELOAD": "1"},
    )


def test_bootstrap_and_top_level_help_do_not_import_the_runtime():
    result = child("""
import sys
from a11_cli import _preload_allocator
_preload_allocator()
assert "a11" not in sys.modules
from a11_cli.app import main
try:
    main(["--help"])
except SystemExit as error:
    assert error.code == 0
else:
    raise AssertionError("help did not exit")
assert "a11" not in sys.modules
""")
    assert "flow" in result.stdout and "chat" in result.stdout


def test_flow_startup_does_not_load_other_commands_or_http_servers(tmp_path):
    source = tmp_path / "minimal.flow"
    source.write_text("flow main {}")
    child(f"""
import sys
from a11_cli.app import main
assert main(["flow", "run", {str(source)!r}, "--flow", "main"]) == 0
assert "a11.cli.commands.flow" in sys.modules
for module in ["fastapi", "a11.cli.voice", "a11.cli.commands.chat",
               "a11.cli.commands.llm_context", "a11.cli.commands.gateway",
               "a11.cli.commands.serve", "a11.cli.commands.account"]:
    assert module not in sys.modules, module
""")


def test_all_deferred_commands_keep_their_help_and_argument_definitions():
    from a11_cli.app import build_parser
    from a11_cli.commands import COMMANDS

    for command in COMMANDS:
        assert command.loader is not None
        loaded = command.loader()
        assert loaded.name == command.name
        assert loaded.help == command.help
        parser = build_parser(COMMANDS, selected=command.name)
        try:
            parser.parse_args([command.name, "--help"])
        except SystemExit as error:
            assert error.code == 0
        else:
            raise AssertionError(f"no help for {command.name}")


def test_fastapi_status_encoding_works_when_fastapi_is_imported_later():
    child("""
import sys
from a11.status import Status, StatusCode
assert "fastapi" not in sys.modules
status = Status(code=StatusCode.NOT_FOUND, message="missing", details=[{"id": 1}])
from fastapi.encoders import jsonable_encoder
assert jsonable_encoder({"status": status}) == {
    "status": {"code": 5, "message": "missing", "details": [{"id": 1}]}
}
""")


def test_allocator_relaunch_preserves_python_options(monkeypatch):
    from a11_cli import _relaunch_argv

    original = [sys.executable, "-X", "dev", "-m", "a11_cli", "--help"]
    monkeypatch.setattr(sys, "orig_argv", original)
    assert _relaunch_argv() == original[1:]
