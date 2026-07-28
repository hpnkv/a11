"""Python protocol conveniences for the native Action implementation."""

from __future__ import annotations

from typing import Any

from a11 import _native
from a11.status import StatusException

Action = _native.Action
ActionSettings = _native.ActionSettings

_native_get_header = Action.get_header
_native_run = Action.run
_native_settings = Action.__dict__["settings"]


class _ActionDoneEvent:
    """Event-shaped view of native Action completion."""

    __slots__ = ("_action",)

    def __init__(self, action: Action) -> None:
        self._action = action

    def is_set(self) -> bool:
        return self._action.is_done()

    async def wait(self) -> bool:
        try:
            await self._action.wait()
        except StatusException:
            # Completion events signal lifecycle, while Action.wait() also
            # reports the operation status. Preserve asyncio.Event semantics.
            pass
        return True


def _done(action: Action) -> _ActionDoneEvent:
    event = action.__dict__.get("_a11_done_event")
    if event is None:
        event = _ActionDoneEvent(action)
        action.__dict__["_a11_done_event"] = event
    return event


def _await_action(action: Action):
    return action.wait().__await__()


def _get_header(
    action: Action, name: str, decode: bool = False
) -> bytes | str | None:
    value = _native_get_header(action, name)
    if value is not None and decode:
        return value.decode()
    return value


def _get_settings(action: Action) -> ActionSettings:
    settings = _native_settings.__get__(action, type(action))
    settings.__dict__["_a11_action_owner"] = action
    return settings


def _set_settings(action: Action, settings: ActionSettings) -> None:
    _native_settings.__set__(action, settings)


def _install_live_setting(name: str) -> None:
    descriptor = ActionSettings.__dict__[name]

    def get(settings: ActionSettings) -> Any:
        return descriptor.__get__(settings, type(settings))

    def set(settings: ActionSettings, value: Any) -> None:
        previous = descriptor.__get__(settings, type(settings))
        descriptor.__set__(settings, value)
        owner = settings.__dict__.get("_a11_action_owner")
        if owner is None:
            return
        try:
            _native_settings.__set__(owner, settings)
        except BaseException:
            descriptor.__set__(settings, previous)
            raise

    setattr(ActionSettings, name, property(get, set))


for _field in (
    "bind_streams_on_inputs_by_default",
    "bind_streams_on_outputs_by_default",
    "clear_inputs_after_run",
    "clear_outputs_after_run",
):
    _install_live_setting(_field)

Action.__module__ = "a11.actions.action"
Action.done = property(_done)
Action.__await__ = _await_action
Action.get_header = _get_header
Action.run_in_background = _native_run
Action.settings = property(_get_settings, _set_settings)

__all__ = ["Action"]
