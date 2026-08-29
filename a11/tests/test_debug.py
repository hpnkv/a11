"""The fiber-introspection surface in :mod:`a11.debug`."""

from __future__ import annotations

import a11.debug


def test_report_is_produced_with_or_without_fibers():
    report = a11.debug.fiber_report(include_running=True)
    assert "A11 fiber dump" in report
    assert "census:" in report


def test_snapshot_entries_have_the_documented_shape():
    for fiber in a11.debug.fiber_snapshot():
        assert set(fiber) >= {
            "id",
            "parent_id",
            "name",
            "wait",
            "wait_object",
            "blocking_fiber_id",
            "waited_seconds",
            "stack",
            "selectables",
            "trace_raced",
            "waits_completed",
        }
        assert isinstance(fiber["id"], int)
        assert isinstance(fiber["name"], str)
        assert isinstance(fiber["stack"], list)
        # A raced trace reports no frames.
        assert not fiber["trace_raced"] or not fiber["stack"]


def test_waiting_fibers_excludes_running_and_thread_placeholders():
    for fiber in a11.debug.waiting_fibers():
        assert fiber["wait"] not in ("running", "os-thread")


def test_waiting_fibers_is_ordered_by_wait_length():
    waits = [fiber["waited_seconds"] for fiber in a11.debug.waiting_fibers()]
    assert waits == sorted(waits, reverse=True)


def test_no_deadlock_in_a_healthy_process():
    assert a11.debug.find_fiber_deadlock() == []


def test_current_fiber_id_is_zero_off_a_fiber():
    assert a11.debug.current_fiber_id() == 0
    # Naming is a no-op rather than an error off a fiber.
    a11.debug.set_current_fiber_name("not-a-fiber")


def test_watchdog_installs_and_serves_a_dump_request():
    with a11.debug.fiber_watchdog(stall_threshold_seconds=30.0):
        a11.debug.request_fiber_dump()
    # Re-entering adjusts the running watchdog rather than starting another.
    with a11.debug.fiber_watchdog(stall_threshold_seconds=30.0):
        pass


def test_dump_signal_handler_installs():
    assert a11.debug.install_fiber_dump_signal_handler() is True


def test_max_frames_zero_skips_the_unwind():
    for fiber in a11.debug.fiber_snapshot(max_frames=0):
        assert fiber["stack"] == []
