"""Cross-platform audio input capture with fan-out subscriptions.

A thin, asyncio-shaped surface over A11's native PortAudio backend. Enumerate
devices with [list_devices][a11.sdk.audio.client.list_devices], inspect a
device's channels and sample rate before committing, then open an
[AudioInput][a11.sdk.audio.client.AudioInput] and
[subscribe][a11.sdk.audio.client.AudioInput.subscribe] to receive fixed-size
[AudioBuffer][a11.sdk.audio.client.AudioBuffer] blocks. Capture runs in the
background while at least one subscription is alive.
"""

from .client import *  # noqa: F403
