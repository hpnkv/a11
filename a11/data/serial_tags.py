# Copyright 2026 The A11 Authors.

"""The canonical cross-language serialization tags.

A serialized A11 value names its type with a *tag*: the ``type`` parameter of
the chunk's MIME type (``application/json;type=a11.Chunk``), and the
``class_name`` of a nested ``a11.value``/``pydantic`` wire object. The tag is
what a peer in another language matches on, so the same class must carry the
same tag in every implementation — this module is that table for Python, and
its siblings hold the identical strings:

  * C++ — ``cpp/a11/data/serial_tags.h`` (returned by the ``A11SerialTag`` ADL
    customization point).
  * TypeScript — ``js/src/serial_tags.ts``.
  * Kotlin — ``kotlin/src/main/kotlin/a11/SerialTags.kt``.

`testdata/serial_tags.json` holds the same table once more, and each language's
test suite asserts its own constants against it — so a tag added or renamed in
one language and forgotten in another fails a test rather than a conversation.

Conventions:

  * ``a11.<Class>`` for the runtime's own data and status types.
  * ``a11.sdk.<Class>`` for the model-interaction SDK, with subpackages omitted
    and names chosen for what the type *is* rather than where it lives — a
    provider's request-config model is ``a11.sdk.InteractWith<Provider>Config``
    whatever its class happens to be called in that language.
  * JSON-native values keep their language-neutral tags (``object``, ``array``,
    ``string``, …); they are not listed here.

A Python class declares its tag with an ``A11_SERIAL_TAG`` ClassVar:

```python
class Interaction(BaseModel):
    A11_SERIAL_TAG: ClassVar[str] = serial_tags.INTERACTION
```

Native (pybind11) classes cannot carry one, so
`a11.data.serialization.SerializationRegistry` pins theirs from `CORE_TAGS`.

Renaming a tag is a wire-format change. Readers stay compatible with the
historical bare class names (``Chunk``, ``Status``) through the class-name
fallback in ``SerializationRegistry._resolve_type``; writers always emit the
canonical tag below.
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
