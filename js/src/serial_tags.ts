/**
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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

/** Wire tag for {@link ChunkMetadata}. */
export const CHUNK_METADATA_TAG = 'a11.ChunkMetadata';
/** Wire tag for {@link Chunk}. */
export const CHUNK_TAG = 'a11.Chunk';
/** Wire tag for {@link NodeRef}. */
export const NODE_REF_TAG = 'a11.NodeRef';
/** Wire tag for {@link NodeFragment}. */
export const NODE_FRAGMENT_TAG = 'a11.NodeFragment';
/** Wire tag for {@link Port}. */
export const PORT_TAG = 'a11.Port';
/** Wire tag for {@link ActionMessage}. */
export const ACTION_MESSAGE_TAG = 'a11.ActionMessage';
/** Wire tag for {@link WireMessage}. */
export const WIRE_MESSAGE_TAG = 'a11.WireMessage';
/** Wire tag for a Status carried as data. */
export const STATUS_TAG = 'a11.Status';
/** Wire tag for an A11 timestamp. */
export const TIME_TAG = 'a11.Time';
/** Wire tag for an A11 duration. */
export const DURATION_TAG = 'a11.Duration';

// --- Model-interaction SDK ---------------------------------------------------

/** Wire tag for an SDK interaction. */
export const INTERACTION_TAG = 'a11.sdk.Interaction';
/** Wire tag for an SDK peer address. */
export const PEER_TAG = 'a11.sdk.Peer';
/** Wire tag for an action routing configuration. */
export const ACTION_CONFIG_TAG = 'a11.sdk.ActionConfig';
/** Wire tag for provider-independent token accounting. */
export const USAGE_METADATA_TAG = 'a11.sdk.UsageMetadata';

/** Wire tag for a Claude interaction configuration. */
export const INTERACT_WITH_CLAUDE_CONFIG_TAG = 'a11.sdk.InteractWithClaudeConfig';
/** Wire tag for a Claude Code session configuration. */
export const INTERACT_WITH_CLAUDE_CODE_CONFIG_TAG = 'a11.sdk.InteractWithClaudeCodeConfig';
/** Wire tag for a Gemini interaction configuration. */
export const INTERACT_WITH_GEMINI_CONFIG_TAG = 'a11.sdk.InteractWithGeminiConfig';
/** Wire tag for an OpenAI interaction configuration. */
export const INTERACT_WITH_GPT_CONFIG_TAG = 'a11.sdk.InteractWithGptConfig';
/** Wire tag for a Codex interaction configuration. */
export const INTERACT_WITH_CODEX_CONFIG_TAG = 'a11.sdk.InteractWithCodexConfig';
/** Wire tag for an Ollama chat configuration. */
export const INTERACT_WITH_OLLAMA_CONFIG_TAG = 'a11.sdk.InteractWithOllamaConfig';
/** Wire tag for a vLLM chat configuration. */
export const INTERACT_WITH_VLLM_CONFIG_TAG = 'a11.sdk.InteractWithVllmConfig';
/** Wire tag for an in-browser Gemma configuration. */
export const INTERACT_WITH_GEMMA_CONFIG_TAG = 'a11.sdk.InteractWithGemmaConfig';

// --- Audio SDK ---------------------------------------------------------------

/** Wire tag for an SDK audio buffer. */
export const AUDIO_BUFFER_TAG = 'a11.sdk.AudioBuffer';
/** Wire tag for audio input options. */
export const AUDIO_INPUT_OPTIONS_TAG = 'a11.sdk.AudioInputOptions';
/** Wire tag for speech recognizer options. */
export const SPEECH_RECOGNIZER_OPTIONS_TAG = 'a11.sdk.SpeechRecognizerOptions';
/** Wire tag for audio device information. */
export const AUDIO_DEVICE_INFO_TAG = 'a11.sdk.AudioDeviceInfo';
/** Wire tag for an audio control event. */
export const AUDIO_CONTROL_EVENT_TAG = 'a11.sdk.AudioControlEvent';
/** Wire tag for an audio capture event. */
export const AUDIO_CAPTURE_EVENT_TAG = 'a11.sdk.AudioCaptureEvent';
/** Wire tag for a transcription event. */
export const TRANSCRIPTION_EVENT_TAG = 'a11.sdk.TranscriptionEvent';
