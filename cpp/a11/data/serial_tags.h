// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief The canonical cross-language serialization tags.
 *
 * A serialized A11 value names its type with a *tag*: the @c type parameter of
 * the chunk's MIME type (@c "application/json;type=a11.Chunk"). That is the
 * only place a type is ever named. Nothing inside the payload repeats it -- a
 * declared model's fields say what they hold, and schemaless data is just data.
 * A value the format already describes (an object, an array, a string) carries
 * no tag at all, so a bare @c "application/json" is a complete description.
 *
 * The tag is what a peer in another language matches on, so the same class must
 * carry the same tag in every implementation. This header is that table for
 * C++, and its siblings hold the identical strings:
 *
 *   - Python -- @c a11/data/serial_tags.py (declared per class with an
 *     @c A11_SERIAL_TAG ClassVar).
 *   - TypeScript -- @c js/src/serial_tags.ts.
 *   - Kotlin -- @c kotlin/src/main/kotlin/a11/SerialTags.kt.
 *
 * @c testdata/serial_tags.json holds the same table once more, and each
 * language's test suite asserts its own constants against it -- so a tag added
 * or renamed in one language and forgotten in another fails a test.
 *
 * Conventions:
 *   - @c a11.<Class> for the runtime's own data and status types.
 *   - @c a11.sdk.<Class> for the SDKs, with subpackages omitted and names
 *     chosen for what the type *is* rather than where it lives.
 *   - JSON-native values keep their language-neutral tags (@c object,
 *     @c array, @c string, ...); they are not listed here.
 *
 * A C++ type publishes its tag by returning one of these constants from its
 * @c A11SerialTag ADL customization point (see a11/data/serializable.h).
 */

#ifndef A11_DATA_SERIAL_TAGS_H_
#define A11_DATA_SERIAL_TAGS_H_

#include <string_view>

namespace a11::data {

// --- Core runtime -----------------------------------------------------------

inline constexpr std::string_view kChunkMetadataTag = "a11.ChunkMetadata";
inline constexpr std::string_view kChunkTag = "a11.Chunk";
inline constexpr std::string_view kNodeRefTag = "a11.NodeRef";
inline constexpr std::string_view kNodeFragmentTag = "a11.NodeFragment";
inline constexpr std::string_view kPortTag = "a11.Port";
inline constexpr std::string_view kActionMessageTag = "a11.ActionMessage";
inline constexpr std::string_view kWireMessageTag = "a11.WireMessage";
inline constexpr std::string_view kStatusTag = "a11.Status";
inline constexpr std::string_view kTimeTag = "a11.Time";
inline constexpr std::string_view kDurationTag = "a11.Duration";

// --- Model-interaction SDK --------------------------------------------------

inline constexpr std::string_view kInteractionTag = "a11.sdk.Interaction";
inline constexpr std::string_view kPeerTag = "a11.sdk.Peer";
inline constexpr std::string_view kActionConfigTag = "a11.sdk.ActionConfig";
inline constexpr std::string_view kUsageMetadataTag = "a11.sdk.UsageMetadata";
inline constexpr std::string_view kInteractWithClaudeConfigTag =
    "a11.sdk.InteractWithClaudeConfig";
inline constexpr std::string_view kInteractWithGeminiConfigTag =
    "a11.sdk.InteractWithGeminiConfig";
inline constexpr std::string_view kInteractWithOllamaConfigTag =
    "a11.sdk.InteractWithOllamaConfig";
inline constexpr std::string_view kInteractWithGemmaConfigTag =
    "a11.sdk.InteractWithGemmaConfig";

// --- Audio SDK --------------------------------------------------------------

inline constexpr std::string_view kAudioBufferTag = "a11.sdk.AudioBuffer";
inline constexpr std::string_view kAudioInputOptionsTag =
    "a11.sdk.AudioInputOptions";
inline constexpr std::string_view kSpeechRecognizerOptionsTag =
    "a11.sdk.SpeechRecognizerOptions";
inline constexpr std::string_view kAudioDeviceInfoTag =
    "a11.sdk.AudioDeviceInfo";
inline constexpr std::string_view kAudioControlEventTag =
    "a11.sdk.AudioControlEvent";
inline constexpr std::string_view kAudioCaptureEventTag =
    "a11.sdk.AudioCaptureEvent";
inline constexpr std::string_view kTranscriptionEventTag =
    "a11.sdk.TranscriptionEvent";

}  // namespace a11::data

#endif  // A11_DATA_SERIAL_TAGS_H_
