// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Action-based interface to the audio SDK.
 *
 * Three Actions wrap the capture and recognition primitives:
 *   - @c list_audio_inputs     -- stream the available input devices.
 *   - @c capture_audio         -- stream raw AudioBuffers with a control input.
 *   - @c capture_transcription -- stream recognized text with a control input.
 *
 * Their schemas and handlers are plain a11::actions values, so they can be
 * registered on any ActionRegistry (in C++ or through the Python binding) and
 * run like any other Action. @ref RegisterAudioActions installs all three and
 * ensures the audio value-type codecs are present in the process-wide
 * serialization registry that backs the ports.
 */

#ifndef A11_SDK_AUDIO_ACTIONS_AUDIO_ACTIONS_H_
#define A11_SDK_AUDIO_ACTIONS_AUDIO_ACTIONS_H_

#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"

namespace a11::sdk::audio {

/** @brief Name of the absolute-execution-deadline header. */
inline constexpr std::string_view kDeadlineHeader = "x-a11-deadline";

/**
 * @brief Parse an @c x-a11-deadline header value into an absolute time.
 *
 * The value is a base-10 count of milliseconds since the Unix epoch, or a
 * base-10 count of nanoseconds when suffixed with @c "ns". An empty value
 * means no deadline and returns @c absl::InfiniteFuture().
 *
 * @return The absolute deadline, or InvalidArgument for a malformed value.
 */
absl::StatusOr<absl::Time> ParseDeadlineHeader(std::string_view value);

/** @brief Registered name of the list-inputs Action. */
inline constexpr std::string_view kListAudioInputsAction = "list_audio_inputs";
/** @brief Registered name of the capture Action. */
inline constexpr std::string_view kCaptureAudioAction = "capture_audio";
/** @brief Registered name of the transcription Action. */
inline constexpr std::string_view kCaptureTranscriptionAction =
    "capture_transcription";
/** @brief Registered name of the buffer-stream transcription Action. */
inline constexpr std::string_view kTranscribeAudioAction = "transcribe_audio";

/** @brief Schema for @c list_audio_inputs. */
a11::actions::ActionSchema ListAudioInputsSchema();
/** @brief Schema for @c capture_audio. */
a11::actions::ActionSchema CaptureAudioSchema();
/** @brief Schema for @c capture_transcription. */
a11::actions::ActionSchema CaptureTranscriptionSchema();
/** @brief Schema for @c transcribe_audio. */
a11::actions::ActionSchema TranscribeAudioSchema();

/** @brief Handler for @c list_audio_inputs. */
a11::actions::ActionHandler ListAudioInputsHandler();
/** @brief Handler for @c capture_audio. */
a11::actions::ActionHandler CaptureAudioHandler();
/** @brief Handler for @c capture_transcription. */
a11::actions::ActionHandler CaptureTranscriptionHandler();
/** @brief Handler for @c transcribe_audio. */
a11::actions::ActionHandler TranscribeAudioHandler();

/**
 * @brief Ensures the audio value-type codecs are in the global serialization
 *   registry the default port nodes use. Idempotent (a second call is a no-op).
 */
absl::Status EnsureAudioTypesRegistered();

/**
 * @brief Registers all three audio Actions (schema + handler) into @p registry
 *   and ensures their value-type codecs are available.
 * @return OK, or the first registration error.
 */
absl::Status RegisterAudioActions(a11::actions::ActionRegistry& registry);

}  // namespace a11::sdk::audio

#endif  // A11_SDK_AUDIO_ACTIONS_AUDIO_ACTIONS_H_
