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

"""Cross-platform audio input capture with fan-out subscriptions.

A thin, asyncio-shaped surface over A11's native PortAudio backend. Enumerate
devices with [list_devices][a11.sdk.audio.client.list_devices], inspect a
device's channels and sample rate before committing, then open an
[AudioInput][a11.sdk.audio.client.AudioInput] and
[subscribe][a11.sdk.audio.client.AudioInput.subscribe] to receive fixed-size
[AudioBuffer][a11.sdk.audio.client.AudioBuffer] blocks. Capture runs in the
background while at least one subscription is alive.

The same primitives are also published as Actions -- see
[a11.sdk.audio.actions][], reachable as ``audio.actions`` once this package is
imported.
"""

from .client import *  # noqa: F403
from . import actions  # noqa: E402,F401  (after .client -- actions imports it)
