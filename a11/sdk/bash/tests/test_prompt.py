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

"""Tests for the shell-tool system prompt."""

from a11.sdk import bash
from a11.sdk.bash.manager import MAX_GLOBAL_SHELLS, MAX_SHELLS_PER_SESSION
from a11.sdk.bash.schemas import A11ShellExecuteParameters


def test_prompt_covers_when_to_use_the_shell():
    prompt = bash.get_system_prompt().lower()
    # The situations the model should reach for the shell in.
    assert "coding" in prompt
    assert "environment" in prompt
    assert "test" in prompt


def test_prompt_explains_persistence_and_the_tools():
    prompt = bash.get_system_prompt()
    for name in ("shell_start", "shell_execute", "shell_list", "shell_exit"):
        assert name in prompt
    assert "x-a11-shell-id" in prompt
    lowered = prompt.lower()
    assert "persist" in lowered
    assert "throwaway" in lowered or "transient" in lowered


def test_prompt_states_the_limits_from_the_real_constants():
    prompt = bash.get_system_prompt()
    assert str(MAX_SHELLS_PER_SESSION) in prompt
    assert f"{A11ShellExecuteParameters.DEFAULT}s" in prompt
    assert f"{A11ShellExecuteParameters.MAX}s" in prompt


def test_prompt_can_advertise_the_global_cap():
    prompt = bash.get_system_prompt(max_shells=MAX_GLOBAL_SHELLS)
    assert str(MAX_GLOBAL_SHELLS) in prompt
