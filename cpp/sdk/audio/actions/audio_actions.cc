// Copyright 2026 The A11 Authors.

#include "sdk/audio/actions/audio_actions.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/log/log.h>
#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "a11/data/serialization.h"
#include "a11/data/types.h"
#include "a11/nodes/async_node.h"
#include "sdk/audio/actions/audio_events.h"
#include "sdk/audio/actions/audio_serialization.h"
#include "sdk/audio/audio_buffer.h"
#include "sdk/audio/audio_input.h"
#include "sdk/audio/device.h"
#include "sdk/audio/model_registry.h"
#include "sdk/audio/speech_recognizer.h"
#include "thread/concurrency.h"

namespace a11::sdk::audio {
namespace {

using ::a11::actions::Action;
using ::a11::actions::ActionHandler;
using ::a11::actions::ActionPortSchema;
using ::a11::actions::ActionSchema;
using ::a11::nodes::AsyncNode;

/**
 * A progress callback that logs a model download about every 10%.
 *
 * A first-run fetch of a whisper model is tens or hundreds of megabytes and
 * happens on whichever machine serves the action, so silence there reads as a
 * hung action. Logging is coarse because this runs once per body chunk.
 */
OnModelProgress LogModelProgress(std::string_view what) {
  auto last_decile = std::make_shared<std::int64_t>(-1);
  return [what = std::string(what), last_decile](std::uint64_t done,
                                                 std::uint64_t total) {
    if (total == 0) {
      return;
    }
    const std::int64_t decile = static_cast<std::int64_t>(
        (done * 10) / std::max<std::uint64_t>(total, 1));
    if (decile == *last_decile) {
      return;
    }
    *last_decile = decile;
    LOG(INFO) << "downloading " << what << " model: " << (decile * 10) << "% ("
              << done << "/" << total << " bytes)";
  };
}

// The wire mimetype (media type + language-agnostic type tag) used for a port's
// declared `type`. The handler still chooses the encoding it Put()s.
std::string TaggedMimetype(std::string_view media_type, std::string_view tag) {
  return absl::StrCat(media_type, ";type=", tag);
}

ActionPortSchema Port(std::string name, std::string type, std::string desc,
                      bool required, bool unary) {
  return ActionPortSchema{.name = std::move(name),
                          .type = std::move(type),
                          .description = std::move(desc),
                          .required = required,
                          .unary = unary};
}

// ------------------------- shared stop machinery ---------------------------

// One-shot stop signal shared between the handler fiber, the control-events
// watcher fiber, and the action's cancellation callback. All stop sources
// funnel through RequestStop so PermanentEvent::Notify() fires exactly once.
struct CaptureStop {
  std::atomic<bool> requested{false};
  thread::PermanentEvent event;
  std::shared_ptr<AudioSubscription> sub;
  std::shared_ptr<AsyncNode> control;
};

void RequestStop(const std::shared_ptr<CaptureStop>& stop) {
  if (!stop->requested.exchange(true)) {
    stop->event.Notify();
    if (stop->sub) {
      stop->sub
          ->Close();  // idempotent; drains then ends Read() with OutOfRange
    }
  }
}

// Reads and parses the x-a11-deadline header from an action (see
// ParseDeadlineHeader). An absent header means no deadline (InfiniteFuture).
absl::StatusOr<absl::Time> DeadlineFromAction(
    const std::shared_ptr<Action>& action) {
  ABSL_ASSIGN_OR_RETURN(std::optional<data::Bytes> raw,
                        action->GetHeader(kDeadlineHeader));
  if (!raw.has_value()) {
    return absl::InfiniteFuture();
  }
  return ParseDeadlineHeader(*raw);
}

// Spawns a fiber that requests a graceful stop once `deadline` is reached,
// unless the run ends first (RequestStop notifies stop->event) or the fiber is
// cancelled. `stop` is captured by value so its event outlives the watcher.
a11::Task WatchDeadline(absl::Time deadline,
                        std::shared_ptr<CaptureStop> stop) {
  if (deadline >= absl::InfiniteFuture()) {
    return a11::ReadyTask();
  }
  return a11::SubmitTask([deadline, stop = std::move(stop)]() -> absl::Status {
    if (thread::SelectUntil(deadline,
                            {thread::OnCancel(), stop->event.OnEvent()}) < 0) {
      RequestStop(stop);
    }
    return absl::OkStatus();
  });
}

// Declares the x-a11-deadline header on an action schema.
void AddDeadlineHeader(ActionSchema& schema) {
  schema.headers.emplace(
      std::string(kDeadlineHeader),
      a11::actions::ActionHeaderSchema{
          .name = std::string(kDeadlineHeader),
          .description =
              "Absolute execution deadline: a base-10 count of milliseconds "
              "since the Unix epoch, or nanoseconds with an 'ns' suffix. The "
              "action stops gracefully once it is reached."});
}

// Reads the next value of a stream, decoding it in this translation unit (see
// audio_serialization.h for why the registry's typed path is avoided here). A
// closed stream or an explicit null marker yields nullopt.
template <typename T>
absl::StatusOr<std::optional<T>> NextValue(
    const std::shared_ptr<AsyncNode>& node) {
  ABSL_ASSIGN_OR_RETURN(std::optional<data::Chunk> chunk,
                        node->NextChunk().Await());
  if (!chunk.has_value() || chunk->IsNull()) {
    return std::optional<T>(std::nullopt);
  }
  ABSL_ASSIGN_OR_RETURN(T value, DecodeJsonChunk<T>(*chunk));
  return std::optional<T>(std::move(value));
}

// Spawns the fiber that consumes `control` and requests a graceful stop on a
// stop command. A nullopt (control stream ended) without a stop is ignored, so
// capture continues until the action is cancelled.
a11::Task WatchControlForStop(std::shared_ptr<AsyncNode> control,
                              std::shared_ptr<CaptureStop> stop) {
  return a11::SubmitTask([control = std::move(control),
                          stop = std::move(stop)]() -> absl::Status {
    while (!stop->requested.load()) {
      absl::StatusOr<std::optional<AudioControlEvent>> event =
          NextValue<AudioControlEvent>(control);
      if (!event.ok()) {
        return absl::OkStatus();  // reader cancelled during teardown
      }
      if (!event->has_value()) {
        return absl::OkStatus();  // control ended; keep capturing until cancel
      }
      if (event->value().is_stop()) {
        RequestStop(stop);
        return absl::OkStatus();
      }
    }
    return absl::OkStatus();
  });
}

// Reads a unary options input, returning `fallback` when the input is absent.
template <typename T>
absl::StatusOr<T> ReadOptionalOptions(const std::shared_ptr<Action>& action,
                                      const std::string& port, T fallback) {
  ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<AsyncNode> node,
                        action->GetInput(port));
  ABSL_ASSIGN_OR_RETURN(std::optional<T> value, NextValue<T>(node));
  if (!value.has_value()) {
    return fallback;
  }
  return *std::move(value);
}

// Writes a JSON-serializable value to a stream.
template <typename T>
absl::Status PutJson(AsyncNode& node, const T& value, bool final) {
  ABSL_ASSIGN_OR_RETURN(data::Chunk chunk, EncodeJsonChunk<T>(value));
  return node.PutChunk(std::move(chunk), std::nullopt, final).Await().status();
}

// Encodes a string the way every other language's registry expects one: a JSON
// scalar, not a bare `text/plain` payload.
data::Chunk TextChunk(const std::string& text) {
  data::Chunk chunk;
  chunk.metadata =
      data::ChunkMetadata{.mimetype = std::string(a11::data::kJsonMimetype)};
  chunk.data = nlohmann::json(text).dump();
  return chunk;
}

// Adapts a chunk write to the Task the recognizer callbacks return.
a11::Task WriteTask(const a11::Future<std::uint32_t>& write) {
  a11::Promise<a11::Unit> promise;
  a11::Task task = promise.future();
  promise.SetCancellationCallback([write]() { write.Cancel().IgnoreError(); })
      .IgnoreError();
  write.OnReady([promise = std::move(promise)](
                    const absl::StatusOr<std::uint32_t>& result) mutable {
    promise
        .SetResult(result.ok() ? absl::StatusOr<a11::Unit>(a11::Unit{})
                               : absl::StatusOr<a11::Unit>(result.status()))
        .IgnoreError();
  });
  return task;
}

// -------------------------- list_audio_inputs ------------------------------

ActionHandler MakeListAudioInputsHandler() {
  return [](std::shared_ptr<Action> action) {
    return a11::SubmitTask([action = std::move(action)]() -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(const absl::Time deadline,
                            DeadlineFromAction(action));
      if (deadline <= absl::Now()) {
        return absl::DeadlineExceededError("x-a11-deadline has already passed");
      }
      ABSL_ASSIGN_OR_RETURN(const std::shared_ptr<AsyncNode> out,
                            action->GetOutput("inputs"));
      ABSL_ASSIGN_OR_RETURN(std::vector<DeviceInfo> devices, ListDevices());
      std::vector<DeviceInfo> inputs;
      for (DeviceInfo& device : devices) {
        if (device.max_input_channels > 0) {
          inputs.push_back(std::move(device));
        }
      }
      for (std::size_t i = 0; i < inputs.size(); ++i) {
        const bool last = (i + 1 == inputs.size());
        ABSL_RETURN_IF_ERROR(PutJson<DeviceInfo>(*out, inputs[i], last));
      }
      if (inputs.empty()) {
        // The framework closes the writer; this only marks the logical end.
        ABSL_RETURN_IF_ERROR(
            out->Finalize({.wait = true, .close = false}).Await().status());
      }
      return absl::OkStatus();
    });
  };
}

// ----------------------------- capture_audio -------------------------------

ActionHandler MakeCaptureAudioHandler() {
  return [](std::shared_ptr<Action> action) {
    return a11::SubmitTask([action = std::move(action)]() -> absl::Status {
      auto stop = std::make_shared<CaptureStop>();

      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> options_node,
                            action->GetInput("options"));
      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> control_node,
                            action->GetInput("control_events"));
      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> audio_out,
                            action->GetOutput("audio"));
      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> events_out,
                            action->GetOutput("events"));
      stop->control = control_node;

      ABSL_ASSIGN_OR_RETURN(const absl::Time deadline,
                            DeadlineFromAction(action));
      if (deadline <= absl::Now()) {
        return absl::DeadlineExceededError("x-a11-deadline has already passed");
      }

      ABSL_ASSIGN_OR_RETURN(std::optional<AudioInputOptions> options,
                            NextValue<AudioInputOptions>(options_node));
      if (!options.has_value()) {
        return absl::InvalidArgumentError("capture_audio requires options");
      }

      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AudioInput> input,
                            AudioInput::Open(*options));
      ABSL_ASSIGN_OR_RETURN(stop->sub,
                            input->Subscribe(options->ResolvedBufferFrames()));

      // Runs on a foreign thread before the handler fiber is cancelled, so it
      // must not block: RequestStop is atomic + idempotent Close, CancelReader
      // only unblocks the watcher.
      ABSL_RETURN_IF_ERROR(action->SetOnCancelled(
          [stop](const std::shared_ptr<Action>&) -> absl::Status {
            RequestStop(stop);
            stop->control->CancelReader();
            return absl::OkStatus();
          }));

      a11::Task control_task = WatchControlForStop(control_node, stop);
      a11::Task deadline_task = WatchDeadline(deadline, stop);

      ABSL_RETURN_IF_ERROR(PutJson<AudioCaptureEvent>(
          *events_out, AudioCaptureEvent::Started(), /*final=*/false));

      absl::Status loop_status = absl::OkStatus();
      bool cancelled = false;
      std::uint64_t last_dropped = 0;
      while (true) {
        absl::StatusOr<AudioBuffer> buffer = stop->sub->Read().Await();
        if (!buffer.ok()) {
          if (buffer.status().code() == absl::StatusCode::kCancelled) {
            cancelled = true;
          } else if (buffer.status().code() != absl::StatusCode::kOutOfRange) {
            loop_status = buffer.status();  // a real device error
          }
          break;  // OutOfRange is the graceful end after Close()
        }
        // Awaiting the write is the backpressure point; while it blocks, the
        // device drops buffers into this subscription (counted below).
        ABSL_ASSIGN_OR_RETURN(data::Chunk audio_chunk,
                              EncodeAudioBufferChunk(*buffer));
        ABSL_RETURN_IF_ERROR(
            audio_out->PutChunk(std::move(audio_chunk)).Await().status());
        const std::uint64_t dropped = stop->sub->dropped();
        if (dropped != last_dropped) {
          ABSL_RETURN_IF_ERROR(PutJson<AudioCaptureEvent>(
              *events_out,
              AudioCaptureEvent::BuffersDropped(dropped - last_dropped),
              /*final=*/false));
          last_dropped = dropped;
        }
      }

      // Tear down and join the watchers before touching outputs / returning.
      RequestStop(stop);
      control_node->CancelReader();
      control_task.Await().IgnoreError();
      deadline_task.Await().IgnoreError();

      if (cancelled) {
        return absl::CancelledError("capture_audio cancelled");
      }
      if (!loop_status.ok()) {
        return loop_status;  // framework aborts unfinished outputs
      }

      // Graceful finish: mark logical end, then release the writers.
      ABSL_RETURN_IF_ERROR(PutJson<AudioCaptureEvent>(
          *events_out, AudioCaptureEvent::Stopped(), /*final=*/true));
      ABSL_RETURN_IF_ERROR(events_out->Close().Await().status());
      ABSL_RETURN_IF_ERROR(
          audio_out->Finalize({.wait = true}).Await().status());

      return absl::OkStatus();
    });
  };
}

// ------------------------- capture_transcription ---------------------------

ActionHandler MakeCaptureTranscriptionHandler() {
  return [](std::shared_ptr<Action> action) {
    return a11::SubmitTask([action = std::move(action)]() -> absl::Status {
      auto stop = std::make_shared<CaptureStop>();

      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> control_node,
                            action->GetInput("control_events"));
      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> pieces_out,
                            action->GetOutput("transcription_pieces"));
      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> events_out,
                            action->GetOutput("events"));
      stop->control = control_node;

      ABSL_ASSIGN_OR_RETURN(const absl::Time deadline,
                            DeadlineFromAction(action));
      if (deadline <= absl::Now()) {
        return absl::DeadlineExceededError("x-a11-deadline has already passed");
      }

      ABSL_ASSIGN_OR_RETURN(
          AudioInputOptions capture_options,
          ReadOptionalOptions<AudioInputOptions>(action, "capture_options",
                                                 AudioInputOptions{}));
      ABSL_ASSIGN_OR_RETURN(
          SpeechRecognizerOptions asr_options,
          ReadOptionalOptions<SpeechRecognizerOptions>(
              action, "asr_options", SpeechRecognizerOptions{}));
      // `model` and `vad_model` accept a shorthand as well as a path, and an
      // absent model means the default one. The blocking forms:
      ABSL_ASSIGN_OR_RETURN(
          asr_options.model,
          internal::ResolveAsrModelBlocking(asr_options.model,
                                            LogModelProgress("transcription")));
      ABSL_ASSIGN_OR_RETURN(
          asr_options.vad_model,
          internal::ResolveVadModelBlocking(asr_options.vad_model,
                                            LogModelProgress("VAD")));

      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AudioInput> input,
                            AudioInput::Open(capture_options));
      std::string model = asr_options.model;
      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<SpeechRecognizer> recognizer,
                            SpeechRecognizer::Create(std::move(model), input,
                                                     std::move(asr_options)));

      // Bridge recognizer callbacks onto the output ports.
      OnTranscription on_piece =
          [pieces_out](std::optional<std::string> piece) -> a11::Task {
        if (piece.has_value()) {
          return WriteTask(pieces_out->PutChunk(TextChunk(*piece)));
        }
        return pieces_out->Finalize({.wait = true});
      };
      OnRecognitionDone on_done = [events_out]() -> a11::Task {
        return a11::SubmitTask([events_out]() -> absl::Status {
          ABSL_RETURN_IF_ERROR(PutJson<TranscriptionEvent>(
              *events_out, TranscriptionEvent::InferenceStopped(),
              /*final=*/true));
          return events_out->Close().Await().status();
        });
      };

      ABSL_RETURN_IF_ERROR(action->SetOnCancelled(
          [stop, recognizer](const std::shared_ptr<Action>&) -> absl::Status {
            RequestStop(stop);
            stop->control->CancelReader();
            (void)recognizer->Stop();  // non-blocking; do not Await here
            return absl::OkStatus();
          }));

      ABSL_RETURN_IF_ERROR(PutJson<TranscriptionEvent>(
          *events_out, TranscriptionEvent::CaptureStarted(), /*final=*/false));
      ABSL_RETURN_IF_ERROR(
          recognizer->Start(std::move(on_piece), std::move(on_done))
              .Await()
              .status());
      ABSL_RETURN_IF_ERROR(PutJson<TranscriptionEvent>(
          *events_out, TranscriptionEvent::InferenceStarted(),
          /*final=*/false));

      a11::Task control_task = WatchControlForStop(control_node, stop);
      a11::Task deadline_task = WatchDeadline(deadline, stop);

      // Park until a stop command, deadline, or cancellation, then stop the
      // recognizer (all three funnel through stop->event / OnCancel).
      thread::Select({stop->event.OnEvent(), thread::OnCancel()});
      absl::Status run = recognizer->Stop().Await().status();

      RequestStop(stop);  // ensure the deadline watcher wakes and exits
      control_node->CancelReader();
      (void)control_task.Await();
      (void)deadline_task.Await();
      return run;  // outputs already finalized by the recognizer epilogue
    });
  };
}

// ------------------------- transcribe_audio --------------------------------

ActionHandler MakeTranscribeAudioHandler() {
  return [](std::shared_ptr<Action> action) {
    return a11::SubmitTask([action = std::move(action)]() -> absl::Status {
      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> audio_node,
                            action->GetInput("audio"));
      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> pieces_out,
                            action->GetOutput("transcription_pieces"));
      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<AsyncNode> events_out,
                            action->GetOutput("events"));

      ABSL_ASSIGN_OR_RETURN(const absl::Time deadline,
                            DeadlineFromAction(action));
      if (deadline <= absl::Now()) {
        return absl::DeadlineExceededError("x-a11-deadline has already passed");
      }

      ABSL_ASSIGN_OR_RETURN(
          SpeechRecognizerOptions asr_options,
          ReadOptionalOptions<SpeechRecognizerOptions>(
              action, "asr_options", SpeechRecognizerOptions{}));
      // `model` and `vad_model` accept a shorthand as well as a path, and an
      // absent model means the default one. The blocking forms:
      ABSL_ASSIGN_OR_RETURN(
          asr_options.model,
          internal::ResolveAsrModelBlocking(asr_options.model,
                                            LogModelProgress("transcription")));
      ABSL_ASSIGN_OR_RETURN(
          asr_options.vad_model,
          internal::ResolveVadModelBlocking(asr_options.vad_model,
                                            LogModelProgress("VAD")));
      std::string model = asr_options.model;
      // When delivery stalls, endpoint the pending utterance a little after the
      // silence bound so genuine in-content pauses still endpoint via the VAD.
      const absl::Duration pause_after =
          absl::Milliseconds(asr_options.min_silence_millis + 500);
      ABSL_ASSIGN_OR_RETURN(std::shared_ptr<SpeechRecognizer> recognizer,
                            SpeechRecognizer::CreateForStream(
                                std::move(model), std::move(asr_options)));

      OnTranscription on_piece =
          [pieces_out](std::optional<std::string> piece) -> a11::Task {
        if (piece.has_value()) {
          return WriteTask(pieces_out->PutChunk(TextChunk(*piece)));
        }
        return pieces_out->Finalize({.wait = true});
      };
      OnRecognitionDone on_done = [events_out]() -> a11::Task {
        return a11::SubmitTask([events_out]() -> absl::Status {
          ABSL_RETURN_IF_ERROR(PutJson<TranscriptionEvent>(
              *events_out, TranscriptionEvent::InferenceStopped(),
              /*final=*/true));
          return events_out->Close().Await().status();
        });
      };

      // The input node, read one AudioBuffer at a time, is the recognizer's
      // stream source; a closed stream reports OutOfRange to end the run.
      AudioBufferReader reader = [audio_node]() -> a11::Future<AudioBuffer> {
        return a11::Submit<AudioBuffer>(
            [audio_node]() -> absl::StatusOr<AudioBuffer> {
              ABSL_ASSIGN_OR_RETURN(std::optional<data::Chunk> chunk,
                                    audio_node->NextChunk().Await());
              if (!chunk.has_value() || chunk->IsNull()) {
                return absl::OutOfRangeError("audio input stream ended");
              }
              return DecodeAudioBufferChunk(*chunk);
            });
      };

      // No control events: cancellation stops the run and closes the reader.
      ABSL_RETURN_IF_ERROR(action->SetOnCancelled(
          [recognizer,
           audio_node](const std::shared_ptr<Action>&) -> absl::Status {
            audio_node->CancelReader();
            (void)recognizer->Stop();  // non-blocking
            return absl::OkStatus();
          }));

      ABSL_RETURN_IF_ERROR(PutJson<TranscriptionEvent>(
          *events_out, TranscriptionEvent::CaptureStarted(), /*final=*/false));
      ABSL_RETURN_IF_ERROR(
          recognizer
              ->StartStream(std::move(reader), std::move(on_piece),
                            std::move(on_done), pause_after,
                            [audio_node]() { audio_node->CancelReader(); })
              .Await()
              .status());
      ABSL_RETURN_IF_ERROR(PutJson<TranscriptionEvent>(
          *events_out, TranscriptionEvent::InferenceStarted(),
          /*final=*/false));

      // A finite deadline stops the recognizer when it is reached. The guard
      // event (shared with the watcher) lets the watcher exit as soon as the
      // run ends on its own, and keeps its storage alive past this frame.
      auto deadline_done = std::make_shared<thread::PermanentEvent>();
      a11::Task deadline_task = a11::ReadyTask();
      if (deadline < absl::InfiniteFuture()) {
        deadline_task = a11::SubmitTask([deadline, deadline_done,
                                         recognizer]() -> absl::Status {
          if (thread::SelectUntil(deadline, {thread::OnCancel(),
                                             deadline_done->OnEvent()}) < 0) {
            (void)recognizer->Stop();  // non-blocking
          }
          return absl::OkStatus();
        });
      }

      // Await natural completion (input stream closed), the deadline, or
      // cancellation; the recognizer's epilogue finalizes both outputs once.
      absl::Status run = recognizer->Wait().Await().status();
      deadline_done->Notify();
      (void)deadline_task.Await();
      return run;
    });
  };
}

}  // namespace

ActionSchema ListAudioInputsSchema() {
  ActionSchema schema;
  schema.name = std::string(kListAudioInputsAction);
  schema.description =
      "List the host's available audio input devices, one AudioDeviceInfo per "
      "input, as a stream.";
  schema.outputs.emplace(
      "inputs",
      Port("inputs",
           TaggedMimetype(a11::data::kJsonMimetype, kAudioDeviceInfoTypeTag),
           "One AudioDeviceInfo per available audio input device.",
           /*required=*/false, /*unary=*/false));
  AddDeadlineHeader(schema);
  return schema;
}

ActionSchema CaptureAudioSchema() {
  ActionSchema schema;
  schema.name = std::string(kCaptureAudioAction);
  schema.description =
      "Capture audio from an input device and stream fixed-size AudioBuffers "
      "until a stop control event arrives or the action is cancelled.";
  schema.inputs.emplace(
      "options",
      Port("options",
           TaggedMimetype(a11::data::kJsonMimetype, kAudioInputOptionsTypeTag),
           "Capture parameters: device selection, sample rate, channels and "
           "the delivered buffer size.",
           /*required=*/true, /*unary=*/true));
  schema.inputs.emplace(
      "control_events",
      Port("control_events",
           TaggedMimetype(a11::data::kJsonMimetype, kAudioControlEventTypeTag),
           "Control commands; a stop command finishes capture gracefully.",
           /*required=*/true, /*unary=*/false));
  schema.outputs.emplace(
      "audio",
      Port("audio",
           TaggedMimetype(a11::data::kMsgpackMimetype, kAudioBufferTypeTag),
           "Stream of captured AudioBuffers.",
           /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "events",
      Port("events",
           TaggedMimetype(a11::data::kJsonMimetype, kAudioCaptureEventTypeTag),
           "Stream of capture lifecycle and dropped-buffer events.",
           /*required=*/false, /*unary=*/false));
  AddDeadlineHeader(schema);
  return schema;
}

ActionSchema CaptureTranscriptionSchema() {
  ActionSchema schema;
  schema.name = std::string(kCaptureTranscriptionAction);
  schema.description =
      "Capture audio and stream recognized text pieces until a stop control "
      "event arrives or the action is cancelled.";
  schema.inputs.emplace(
      "capture_options",
      Port("capture_options",
           TaggedMimetype(a11::data::kJsonMimetype, kAudioInputOptionsTypeTag),
           "Optional capture parameters; the default input is used if omitted.",
           /*required=*/false, /*unary=*/true));
  schema.inputs.emplace(
      "asr_options", Port("asr_options",
                          TaggedMimetype(a11::data::kJsonMimetype,
                                         kSpeechRecognizerOptionsTypeTag),
                          "Speech recognition parameters; model is required.",
                          /*required=*/false, /*unary=*/true));
  schema.inputs.emplace(
      "control_events",
      Port(
          "control_events",
          TaggedMimetype(a11::data::kJsonMimetype, kAudioControlEventTypeTag),
          "Control commands; a stop command finishes transcription gracefully.",
          /*required=*/true, /*unary=*/false));
  schema.outputs.emplace(
      "transcription_pieces",
      Port("transcription_pieces", "text/plain",
           "Recognized text pieces as utterances are decoded.",
           /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "events",
      Port("events",
           TaggedMimetype(a11::data::kJsonMimetype, kTranscriptionEventTypeTag),
           "Stream of capture and inference lifecycle events.",
           /*required=*/false, /*unary=*/false));
  AddDeadlineHeader(schema);
  return schema;
}

ActionSchema TranscribeAudioSchema() {
  ActionSchema schema;
  schema.name = std::string(kTranscribeAudioAction);
  schema.description =
      "Transcribe a caller-supplied stream of AudioBuffers, emitting the same "
      "text pieces and lifecycle events as capture_transcription. Stopping is "
      "driven by closing the audio input or cancelling the action; there are "
      "no control events. Transcription is endpointed if the input stalls.";
  schema.inputs.emplace(
      "audio",
      Port("audio",
           TaggedMimetype(a11::data::kMsgpackMimetype, kAudioBufferTypeTag),
           "Stream of AudioBuffers to transcribe; closing it ends the run.",
           /*required=*/true, /*unary=*/false));
  schema.inputs.emplace(
      "asr_options", Port("asr_options",
                          TaggedMimetype(a11::data::kJsonMimetype,
                                         kSpeechRecognizerOptionsTypeTag),
                          "Speech recognition parameters; model is required.",
                          /*required=*/false, /*unary=*/true));
  schema.outputs.emplace(
      "transcription_pieces",
      Port("transcription_pieces", "text/plain",
           "Recognized text pieces as utterances are decoded.",
           /*required=*/false, /*unary=*/false));
  schema.outputs.emplace(
      "events",
      Port("events",
           TaggedMimetype(a11::data::kJsonMimetype, kTranscriptionEventTypeTag),
           "Stream of inference lifecycle events.",
           /*required=*/false, /*unary=*/false));
  AddDeadlineHeader(schema);
  return schema;
}

ActionHandler ListAudioInputsHandler() {
  return MakeListAudioInputsHandler();
}

ActionHandler CaptureAudioHandler() {
  return MakeCaptureAudioHandler();
}

ActionHandler CaptureTranscriptionHandler() {
  return MakeCaptureTranscriptionHandler();
}

ActionHandler TranscribeAudioHandler() {
  return MakeTranscribeAudioHandler();
}

absl::StatusOr<absl::Time> ParseDeadlineHeader(std::string_view value) {
  if (value.empty()) {
    return absl::InfiniteFuture();
  }
  const bool nanos = absl::EndsWith(value, "ns");
  if (nanos) {
    value.remove_suffix(2);
  }
  std::int64_t magnitude = 0;
  if (!absl::SimpleAtoi(value, &magnitude) || magnitude < 0) {
    return absl::InvalidArgumentError(
        "x-a11-deadline must be a non-negative base-10 integer of "
        "milliseconds since the epoch, or nanoseconds with an 'ns' suffix");
  }
  return nanos ? absl::FromUnixNanos(magnitude)
               : absl::FromUnixMillis(magnitude);
}

absl::Status EnsureAudioTypesRegistered() {
  static const absl::Status status = [] {
    absl::Status result =
        RegisterAudioTypes(a11::data::GlobalSerializationRegistry());
    if (absl::IsAlreadyExists(result)) {
      return absl::OkStatus();
    }
    return result;
  }();
  return status;
}

absl::Status RegisterAudioActions(a11::actions::ActionRegistry& registry) {
  ABSL_RETURN_IF_ERROR(EnsureAudioTypesRegistered());
  ABSL_RETURN_IF_ERROR(registry.Register(std::string(kListAudioInputsAction),
                                         ListAudioInputsSchema(),
                                         ListAudioInputsHandler()));
  ABSL_RETURN_IF_ERROR(registry.Register(std::string(kCaptureAudioAction),
                                         CaptureAudioSchema(),
                                         CaptureAudioHandler()));
  ABSL_RETURN_IF_ERROR(registry.Register(
      std::string(kCaptureTranscriptionAction), CaptureTranscriptionSchema(),
      CaptureTranscriptionHandler()));
  ABSL_RETURN_IF_ERROR(registry.Register(std::string(kTranscribeAudioAction),
                                         TranscribeAudioSchema(),
                                         TranscribeAudioHandler()));
  return absl::OkStatus();
}

}  // namespace a11::sdk::audio
