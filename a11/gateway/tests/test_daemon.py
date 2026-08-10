# Copyright 2026 The A11 Authors.

"""Managing a gateway that is not in the foreground.

State-file bookkeeping and log reading are tested directly; spawning a real
child process is covered by one test marked slow, because it starts an
interpreter and waits for it to bind a port.
"""

from __future__ import annotations

import json
import os
import pathlib

import pytest

from a11.gateway import config, daemon


@pytest.fixture(autouse=True)
def _isolated_runtime(tmp_path, monkeypatch):
    """Point the daemon's state and log at a directory of this test's own."""
    monkeypatch.setenv("XDG_RUNTIME_DIR", str(tmp_path))
    return tmp_path


def test_the_runtime_directory_follows_xdg(tmp_path):
    assert daemon.runtime_dir() == tmp_path / "a11" / "gateway"
    assert daemon.state_file().name == "gateway.json"
    assert daemon.log_file().name == "gateway.log"


def test_the_runtime_directory_falls_back_to_the_cache(monkeypatch, tmp_path):
    monkeypatch.delenv("XDG_RUNTIME_DIR", raising=False)
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path))
    assert daemon.runtime_dir() == tmp_path / "a11" / "gateway"


def test_nothing_running_reports_not_running():
    current = daemon.status()
    assert not current.running
    assert current.pid is None
    # The fields are the scriptable contract, so they exist either way.
    assert current.as_fields()["running"] is False
    assert "log" in current.as_fields()


def test_a_recorded_gateway_reports_itself_running():
    settings = config.GatewayConfig(a11_port=9999)
    with daemon.recorded(settings, [settings.url]):
        current = daemon.status()
        assert current.running
        assert current.pid == os.getpid()
        assert current.url == "ws://127.0.0.1:9999/a11"
        assert current.uptime_seconds is not None
    # The record is cleaned up on the way out, so a crashed run does not leave a
    # gateway that looks alive forever.
    assert not daemon.status().running


def test_a_stale_record_is_reported_and_removed():
    """A pid that is gone must not read as a running gateway."""
    daemon.runtime_dir().mkdir(parents=True, exist_ok=True)
    # A pid that cannot exist: the kernel would never assign it.
    daemon.state_file().write_text(
        json.dumps({"pid": 2**30, "url": "ws://127.0.0.1:8011/a11"})
    )

    current = daemon.status()
    assert not current.running
    assert current.stale
    assert not daemon.state_file().exists()


def test_an_unreadable_record_is_treated_as_absent():
    daemon.runtime_dir().mkdir(parents=True, exist_ok=True)
    daemon.state_file().write_text("this is not json")
    assert not daemon.status().running


def test_stopping_nothing_is_an_error_not_a_silent_success():
    with pytest.raises(RuntimeError, match="no gateway is running"):
        daemon.stop()


def test_starting_while_one_runs_refuses_rather_than_doubling_up():
    settings = config.GatewayConfig()
    with daemon.recorded(settings, [settings.url]):
        with pytest.raises(RuntimeError, match="already running"):
            daemon.spawn()


def test_reading_logs_when_there_is_none_is_empty_not_an_error():
    assert daemon.read_logs() == []


def test_logs_return_the_trailing_lines():
    daemon.runtime_dir().mkdir(parents=True, exist_ok=True)
    daemon.log_file().write_text("\n".join(f"line {n}" for n in range(10)))

    assert daemon.read_logs(3) == ["line 7", "line 8", "line 9"]
    # Zero means everything, which is what `-n 0` asks for.
    assert len(daemon.read_logs(0)) == 10


def test_the_status_fields_are_single_line_and_stable():
    """`a11 gateway status | grep pid` has to keep working."""
    settings = config.GatewayConfig(a11_port=8123)
    with daemon.recorded(settings, [settings.url]):
        fields = daemon.status().as_fields()
    assert set(fields) >= {"running", "pid", "url", "log"}
    for value in fields.values():
        assert "\n" not in str(value)


def test_a_real_detached_gateway_starts_answers_and_stops(tmp_path):
    """The whole cycle, with an actual child process."""
    store = tmp_path / "conversations"
    started = daemon.spawn(
        [
            "--a11-port",
            "8097",
            "--conversation-store-root",
            str(store),
            "--no-shell-tools",
            "--no-audio-capture",
            "--no-speech-recognition",
        ]
    )
    try:
        assert started.running
        assert started.pid and started.pid != os.getpid()
        assert started.url == "ws://127.0.0.1:8097/a11"
        # It wrote its own record, so a separate `status` call finds it.
        assert daemon.status().pid == started.pid
        # And it is really serving: unbuffered output means the log has content
        # while it is still running, which is the point of `a11 gateway logs`.
        assert any("listening on" in line for line in daemon.read_logs(0))
    finally:
        daemon.stop()
    assert not daemon.status().running
