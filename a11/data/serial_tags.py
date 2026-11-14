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

"""Define the wire type tags shared across A11 languages.

A type tag appears in a chunk's MIME ``type`` parameter, such as
``application/json;type=a11.Chunk``. Payloads carry no type name. JSON-native
shapes need no tag; bare ``application/json`` describes them completely.

Peers match the same class tag in every language. This Python table and the C++,
TypeScript, and Kotlin tables are checked against `testdata/serial_tags.json`.

Conventions:

  * ``a11.<Class>`` for the runtime's own data and status types.
  * ``a11.sdk.<Class>`` for the model-interaction SDK, with subpackages omitted
    and names chosen for the value rather than its module. A provider request
    config uses ``a11.sdk.InteractWith<Provider>Config``.
  * JSON-native values keep their language-neutral tags (``object``, ``array``,
    ``string``, ...); they are not listed here.

A Python class declares its tag with an ``A11_SERIAL_TAG`` ClassVar:

```python
class Interaction(BaseModel):
    A11_SERIAL_TAG: ClassVar[str] = serial_tags.INTERACTION
```

Native (pybind11) classes cannot carry one, so
`a11.data.serialization.SerializationRegistry` pins theirs from `CORE_TAGS`.

Renaming a tag changes the wire format. Keep legacy aliases for old names.
"""

from __future__ import annotations

# --- Core runtime ------------------------------------------------------------

CHUNK_METADATA = "a11.ChunkMetadata"
CHUNK = "a11.Chunk"
NODE_REF = "a11.NodeRef"
NODE_FRAGMENT = "a11.NodeFragment"
PORT = "a11.Port"
ACTION_MESSAGE = "a11.ActionMessage"
WIRE_MESSAGE = "a11.WireMessage"
STATUS = "a11.Status"
TIME = "a11.Time"
DURATION = "a11.Duration"

# --- Model-interaction SDK ---------------------------------------------------

INTERACTION = "a11.sdk.Interaction"
PEER = "a11.sdk.Peer"
ACTION_CONFIG = "a11.sdk.ActionConfig"
USAGE_METADATA = "a11.sdk.UsageMetadata"

INTERACT_WITH_CLAUDE_CONFIG = "a11.sdk.InteractWithClaudeConfig"
INTERACT_WITH_CLAUDE_CODE_CONFIG = "a11.sdk.InteractWithClaudeCodeConfig"
INTERACT_WITH_GEMINI_CONFIG = "a11.sdk.InteractWithGeminiConfig"
INTERACT_WITH_GPT_CONFIG = "a11.sdk.InteractWithGptConfig"
INTERACT_WITH_CODEX_CONFIG = "a11.sdk.InteractWithCodexConfig"
INTERACT_WITH_OLLAMA_CONFIG = "a11.sdk.InteractWithOllamaConfig"
INTERACT_WITH_OPENROUTER_CONFIG = "a11.sdk.InteractWithOpenRouterConfig"
INTERACT_WITH_VLLM_CONFIG = "a11.sdk.InteractWithVllmConfig"
INTERACT_WITH_GEMMA_CONFIG = "a11.sdk.InteractWithGemmaConfig"

# --- Audio SDK ---------------------------------------------------------------

AUDIO_BUFFER = "a11.sdk.AudioBuffer"
AUDIO_INPUT_OPTIONS = "a11.sdk.AudioInputOptions"
SPEECH_RECOGNIZER_OPTIONS = "a11.sdk.SpeechRecognizerOptions"
AUDIO_DEVICE_INFO = "a11.sdk.AudioDeviceInfo"
AUDIO_CONTROL_EVENT = "a11.sdk.AudioControlEvent"
AUDIO_CAPTURE_EVENT = "a11.sdk.AudioCaptureEvent"
TRANSCRIPTION_EVENT = "a11.sdk.TranscriptionEvent"


#: The ClassVar a Python class sets to declare its own tag.
SERIAL_TAG_ATTRIBUTE = "A11_SERIAL_TAG"


__all__ = [
    "SERIAL_TAG_ATTRIBUTE",
    "CHUNK_METADATA",
    "CHUNK",
    "NODE_REF",
    "NODE_FRAGMENT",
    "PORT",
    "ACTION_MESSAGE",
    "WIRE_MESSAGE",
    "STATUS",
    "TIME",
    "DURATION",
    "INTERACTION",
    "PEER",
    "ACTION_CONFIG",
    "USAGE_METADATA",
    "INTERACT_WITH_CLAUDE_CONFIG",
    "INTERACT_WITH_CLAUDE_CODE_CONFIG",
    "INTERACT_WITH_GEMINI_CONFIG",
    "INTERACT_WITH_GPT_CONFIG",
    "INTERACT_WITH_CODEX_CONFIG",
    "INTERACT_WITH_OLLAMA_CONFIG",
    "INTERACT_WITH_OPENROUTER_CONFIG",
    "INTERACT_WITH_VLLM_CONFIG",
    "INTERACT_WITH_GEMMA_CONFIG",
    "AUDIO_BUFFER",
    "AUDIO_INPUT_OPTIONS",
    "SPEECH_RECOGNIZER_OPTIONS",
    "AUDIO_DEVICE_INFO",
    "AUDIO_CONTROL_EVENT",
    "AUDIO_CAPTURE_EVENT",
    "TRANSCRIPTION_EVENT",
]
