# Copyright 2026 The A11 Authors.

"""The canonical cross-language serialization tags.

A serialized A11 value names its type with a *tag*: the ``type`` parameter of
the chunk's MIME type (``application/json;type=a11.Chunk``). That is the only
place a type is ever named. Nothing inside the payload repeats it -- a declared
model's fields say what they hold, and schemaless data is just data. A value
the format already describes (an object, an array, a string) carries no tag at
all, so a bare ``application/json`` is a complete description.

Peers match the same class tag in every language. This Python table and the C++,
TypeScript, and Kotlin tables are checked against `testdata/serial_tags.json`.

Conventions:

  * ``a11.<Class>`` for the runtime's own data and status types.
  * ``a11.sdk.<Class>`` for the model-interaction SDK, with subpackages omitted
    and names chosen for what the type *is* rather than where it lives — a
    provider's request-config model is ``a11.sdk.InteractWith<Provider>Config``
    whatever its class happens to be called in that language.
  * JSON-native values keep their language-neutral tags (``object``, ``array``,
    ``string``, ...); they are not listed here.

A Python class declares its tag with an ``A11_SERIAL_TAG`` ClassVar:

```python
class Interaction(BaseModel):
    A11_SERIAL_TAG: ClassVar[str] = serial_tags.INTERACTION
```

Native (pybind11) classes cannot carry one, so
`a11.data.serialization.SerializationRegistry` pins theirs from `CORE_TAGS`.

Renaming a tag is a wire-format change: a peer that has not been rebuilt will
fail to resolve the new name rather than silently mis-read it.
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
INTERACT_WITH_OLLAMA_CONFIG = "a11.sdk.InteractWithOllamaConfig"
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
    "INTERACT_WITH_OLLAMA_CONFIG",
    "INTERACT_WITH_GEMMA_CONFIG",
    "AUDIO_BUFFER",
    "AUDIO_INPUT_OPTIONS",
    "SPEECH_RECOGNIZER_OPTIONS",
    "AUDIO_DEVICE_INFO",
    "AUDIO_CONTROL_EVENT",
    "AUDIO_CAPTURE_EVENT",
    "TRANSCRIPTION_EVENT",
]
