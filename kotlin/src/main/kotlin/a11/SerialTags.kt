package a11

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
 * carry the same tag in every implementation — this object is that table for
 * Kotlin, and its siblings hold the identical strings:
 *
 *  * Python — `a11/data/serial_tags.py` (declared per class with an
 *    `A11_SERIAL_TAG` ClassVar).
 *  * C++ — `cpp/a11/data/serial_tags.h` (returned by the `A11SerialTag` ADL
 *    customization point).
 *  * TypeScript — `js/src/serial_tags.ts`.
 *
 * `testdata/serial_tags.json` holds the same table once more, and each
 * language's test suite asserts its own constants against it — so a tag added or
 * renamed in one language and forgotten in another fails a test rather than a
 * conversation.
 *
 * Conventions:
 *
 *  * `a11.<Class>` for the runtime's own data and status types.
 *  * `a11.sdk.<Class>` for the SDKs, with subpackages omitted and names chosen
 *    for what the type *is* rather than where it lives.
 *  * JSON-native values keep their language-neutral tags (`object`, `array`,
 *    `string`, …); they are not listed here.
 *
 * A Kotlin type publishes its tag by implementing [A11Serializable].
 */
object SerialTags {
    // --- Core runtime --------------------------------------------------------

    const val CHUNK_METADATA = "a11.ChunkMetadata"
    const val CHUNK = "a11.Chunk"
    const val NODE_REF = "a11.NodeRef"
    const val NODE_FRAGMENT = "a11.NodeFragment"
    const val PORT = "a11.Port"
    const val ACTION_MESSAGE = "a11.ActionMessage"
    const val WIRE_MESSAGE = "a11.WireMessage"
    const val STATUS = "a11.Status"
    const val TIME = "a11.Time"
    const val DURATION = "a11.Duration"

    // --- Model-interaction SDK -----------------------------------------------

    const val INTERACTION = "a11.sdk.Interaction"
    const val PEER = "a11.sdk.Peer"
    const val ACTION_CONFIG = "a11.sdk.ActionConfig"
    const val USAGE_METADATA = "a11.sdk.UsageMetadata"

    const val INTERACT_WITH_CLAUDE_CONFIG = "a11.sdk.InteractWithClaudeConfig"
    const val INTERACT_WITH_CLAUDE_CODE_CONFIG = "a11.sdk.InteractWithClaudeCodeConfig"
    const val INTERACT_WITH_GEMINI_CONFIG = "a11.sdk.InteractWithGeminiConfig"
    const val INTERACT_WITH_OLLAMA_CONFIG = "a11.sdk.InteractWithOllamaConfig"
    const val INTERACT_WITH_VLLM_CONFIG = "a11.sdk.InteractWithVllmConfig"
    const val INTERACT_WITH_GEMMA_CONFIG = "a11.sdk.InteractWithGemmaConfig"

    // --- Audio SDK -----------------------------------------------------------

    const val AUDIO_BUFFER = "a11.sdk.AudioBuffer"
    const val AUDIO_INPUT_OPTIONS = "a11.sdk.AudioInputOptions"
    const val SPEECH_RECOGNIZER_OPTIONS = "a11.sdk.SpeechRecognizerOptions"
    const val AUDIO_DEVICE_INFO = "a11.sdk.AudioDeviceInfo"
    const val AUDIO_CONTROL_EVENT = "a11.sdk.AudioControlEvent"
    const val AUDIO_CAPTURE_EVENT = "a11.sdk.AudioCaptureEvent"
    const val TRANSCRIPTION_EVENT = "a11.sdk.TranscriptionEvent"
}

/**
 * A type that names itself on the wire.
 *
 * This is Kotlin's counterpart of Python's `A11_SERIAL_TAG` ClassVar and C++'s
 * `A11SerialTag` ADL customization point: a value that implements it can be
 * nested inside a serialized A11 value and rebuilt as itself by a peer in any
 * language, instead of arriving as an anonymous map.
 */
interface A11Serializable {
    /** This value's entry in [SerialTags]. */
    val a11SerialTag: String
}
