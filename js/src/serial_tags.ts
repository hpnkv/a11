/**
 * The canonical cross-language serialization tags.
 *
 * A serialized A11 value names its type with a *tag*: the `type` parameter of
 * the chunk's MIME type (`application/json;type=a11.Chunk`). That is the only
 * place a type is ever named. Nothing inside the payload repeats it — a
 * declared model's fields say what they hold, and schemaless data is just data.
 * A value the format already describes (an object, an array, a string) carries
 * no tag at all, so a bare `application/json` is a complete description.
 *
 * The tag is what a peer in another language matches on, so the same class must
 * carry the same tag in every implementation — this module is that table for
 * TypeScript, and its siblings hold the identical strings:
 *
 *   - Python — `a11/data/serial_tags.py` (declared per class with an
 *     `A11_SERIAL_TAG` ClassVar).
 *   - C++ — `cpp/a11/data/serial_tags.h` (returned by the `A11SerialTag` ADL
 *     customization point).
 *   - Kotlin — `kotlin/src/main/kotlin/a11/SerialTags.kt`.
 *
 * `testdata/serial_tags.json` holds the same table once more, and each
 * language's test suite asserts its own constants against it — so a tag added
 * or renamed in one language and forgotten in another fails a test rather than
 * a conversation.
 *
 * Conventions:
 *
 *   - `a11.<Class>` for the runtime's own data and status types.
 *   - `a11.sdk.<Class>` for the SDKs, with subpackages omitted and names chosen
 *     for what the type *is* rather than where it lives.
 *   - JSON-native values keep their language-neutral tags (`object`, `array`,
 *     `string`, ...); they are not listed here.
 *
 * @packageDocumentation
 */

// --- Core runtime ------------------------------------------------------------

export const CHUNK_METADATA_TAG = 'a11.ChunkMetadata';
export const CHUNK_TAG = 'a11.Chunk';
export const NODE_REF_TAG = 'a11.NodeRef';
export const NODE_FRAGMENT_TAG = 'a11.NodeFragment';
export const PORT_TAG = 'a11.Port';
export const ACTION_MESSAGE_TAG = 'a11.ActionMessage';
export const WIRE_MESSAGE_TAG = 'a11.WireMessage';
export const STATUS_TAG = 'a11.Status';
export const TIME_TAG = 'a11.Time';
export const DURATION_TAG = 'a11.Duration';

// --- Model-interaction SDK ---------------------------------------------------

export const INTERACTION_TAG = 'a11.sdk.Interaction';
export const PEER_TAG = 'a11.sdk.Peer';
export const ACTION_CONFIG_TAG = 'a11.sdk.ActionConfig';
export const USAGE_METADATA_TAG = 'a11.sdk.UsageMetadata';

export const INTERACT_WITH_CLAUDE_CONFIG_TAG = 'a11.sdk.InteractWithClaudeConfig';
export const INTERACT_WITH_CLAUDE_CODE_CONFIG_TAG = 'a11.sdk.InteractWithClaudeCodeConfig';
export const INTERACT_WITH_GEMINI_CONFIG_TAG = 'a11.sdk.InteractWithGeminiConfig';
export const INTERACT_WITH_OLLAMA_CONFIG_TAG = 'a11.sdk.InteractWithOllamaConfig';
export const INTERACT_WITH_VLLM_CONFIG_TAG = 'a11.sdk.InteractWithVllmConfig';
export const INTERACT_WITH_GEMMA_CONFIG_TAG = 'a11.sdk.InteractWithGemmaConfig';

// --- Audio SDK ---------------------------------------------------------------

export const AUDIO_BUFFER_TAG = 'a11.sdk.AudioBuffer';
export const AUDIO_INPUT_OPTIONS_TAG = 'a11.sdk.AudioInputOptions';
export const SPEECH_RECOGNIZER_OPTIONS_TAG = 'a11.sdk.SpeechRecognizerOptions';
export const AUDIO_DEVICE_INFO_TAG = 'a11.sdk.AudioDeviceInfo';
export const AUDIO_CONTROL_EVENT_TAG = 'a11.sdk.AudioControlEvent';
export const AUDIO_CAPTURE_EVENT_TAG = 'a11.sdk.AudioCaptureEvent';
export const TRANSCRIPTION_EVENT_TAG = 'a11.sdk.TranscriptionEvent';

