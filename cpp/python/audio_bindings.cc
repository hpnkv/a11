// Copyright 2026 The A11 Authors.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "a11/actions/action.h"
#include "a11/actions/registry.h"
#include "a11/actions/schema.h"
#include "python/bindings.h"
#include "python/interop.h"
#include "python/native_types.h"
#include "sdk/audio/actions/audio_actions.h"
#include "sdk/audio/actions/audio_events.h"
#include "sdk/audio/actions/audio_serialization.h"
#include "sdk/audio/audio_buffer.h"
#include "sdk/audio/audio_input.h"
#include "sdk/audio/device.h"
#include "sdk/audio/speech_recognizer.h"

namespace a11::python {
namespace {

namespace audio = a11::sdk::audio;

// A read-only 2-D (channels x frames) float view over an AudioBuffer, used to
// back the buffer protocol so `memoryview(buffer)` is zero-copy and keeps the
// buffer alive for the view's lifetime.
py::buffer_info AudioBufferView(audio::AudioBuffer& buffer) {
  const auto frames = static_cast<py::ssize_t>(buffer.num_frames);
  const auto channels = static_cast<py::ssize_t>(buffer.num_channels);
  return py::buffer_info(buffer.samples.data(),
                         static_cast<py::ssize_t>(sizeof(float)),
                         /*format=*/"f", /*ndim=*/2, {channels, frames},
                         {static_cast<py::ssize_t>(sizeof(float)) * frames,
                          static_cast<py::ssize_t>(sizeof(float))},
                         /*readonly=*/true);
}

// Builds an AudioBuffer from any Python buffer-protocol object (bytes, a NumPy
// array, a CPU PyTorch tensor, ...). Samples are taken as 32-bit floats in
// channel-major order; raw byte buffers are reinterpreted as float32. A 2-D
// buffer's leading dimension is treated as the channel count.
bool IsCContiguous(const py::buffer_info& info) {
  py::ssize_t expected = info.itemsize;
  for (py::ssize_t axis = info.ndim - 1; axis >= 0; --axis) {
    if (info.shape[axis] == 0) {
      return true;
    }
    if (info.strides[axis] != expected) {
      return false;
    }
    expected *= info.shape[axis];
  }
  return true;
}

audio::AudioBuffer MakeAudioBufferFromBuffer(const py::buffer& data,
                                             double sample_rate,
                                             int num_channels) {
  const py::buffer_info info = data.request();
  if (!IsCContiguous(info)) {
    ThrowStatus(absl::InvalidArgumentError(
        "audio samples must be a C-contiguous buffer"));
  }
  std::vector<float> samples;
  if (info.format == py::format_descriptor<float>::format()) {
    const auto* first = static_cast<const float*>(info.ptr);
    samples.assign(first, first + info.size);
  } else if (info.itemsize == 1) {
    const auto byte_count = static_cast<size_t>(info.size);
    if (byte_count % sizeof(float) != 0) {
      ThrowStatus(absl::InvalidArgumentError(
          "raw audio bytes must be a whole number of float32 samples"));
    }
    samples.resize(byte_count / sizeof(float));
    std::memcpy(samples.data(), info.ptr, byte_count);
  } else {
    ThrowStatus(absl::InvalidArgumentError(
        "audio samples must be float32 or a raw byte buffer"));
  }

  size_t channels = num_channels > 0 ? static_cast<size_t>(num_channels) : 1;
  if (info.ndim == 2) {
    // A 2-D (channels, frames) buffer names its own channel count.
    channels = static_cast<size_t>(info.shape[0]);
  }
  if (channels == 0 || samples.size() % channels != 0) {
    ThrowStatus(absl::InvalidArgumentError(
        "sample count is not a multiple of the channel count"));
  }
  if (!(sample_rate > 0.0)) {
    ThrowStatus(
        absl::InvalidArgumentError("sample_rate must be a positive number"));
  }
  audio::AudioBuffer buffer;
  buffer.num_channels = channels;
  buffer.num_frames = samples.size() / channels;
  buffer.sample_rate = sample_rate;
  buffer.samples = std::move(samples);
  return buffer;
}

std::shared_ptr<audio::AudioInput> OpenAudioInput(
    audio::AudioInputOptions options) {
  absl::StatusOr<std::shared_ptr<audio::AudioInput>> input;
  {
    py::gil_scoped_release release;
    input = audio::AudioInput::Open(std::move(options));
  }
  return ValueOrThrow(std::move(input));
}

std::shared_ptr<audio::SpeechRecognizer> CreateSpeechRecognizer(
    std::string model_path, const py::object& source,
    audio::SpeechRecognizerOptions options) {
  enum class SourceKind { kDefault, kInput, kSubscription };
  SourceKind source_kind = SourceKind::kDefault;
  std::shared_ptr<audio::AudioInput> input;
  std::shared_ptr<audio::AudioSubscription> subscription;
  if (!source.is_none()) {
    if (py::isinstance<audio::AudioInput>(source)) {
      source_kind = SourceKind::kInput;
      input = source.cast<std::shared_ptr<audio::AudioInput>>();
    } else if (py::isinstance<audio::AudioSubscription>(source)) {
      source_kind = SourceKind::kSubscription;
      subscription = source.cast<std::shared_ptr<audio::AudioSubscription>>();
    } else {
      ThrowStatus(absl::InvalidArgumentError(
          "source must be an AudioInput, AudioSubscription, or None"));
    }
  }

  absl::StatusOr<std::shared_ptr<audio::SpeechRecognizer>> recognizer;
  {
    py::gil_scoped_release release;
    switch (source_kind) {
      case SourceKind::kDefault:
        recognizer = audio::SpeechRecognizer::Create(std::move(model_path),
                                                     std::move(options));
        break;
      case SourceKind::kInput:
        recognizer = audio::SpeechRecognizer::Create(
            std::move(model_path), std::move(input), std::move(options));
        break;
      case SourceKind::kSubscription:
        recognizer = audio::SpeechRecognizer::Create(
            std::move(model_path), std::move(subscription), std::move(options));
        break;
    }
  }
  return ValueOrThrow(std::move(recognizer));
}

absl::StatusOr<std::pair<audio::OnTranscription, audio::OnRecognitionDone>>
MakeRecognitionCallbacks(const py::object& on_transcription,
                         const py::object& on_done) {
  absl::StatusOr<std::shared_ptr<AsyncPythonCallback>> transcription =
      AsyncPythonCallback::Create(on_transcription);
  if (!transcription.ok()) {
    return transcription.status();
  }
  absl::StatusOr<std::shared_ptr<AsyncPythonCallback>> done =
      AsyncPythonCallback::Create(on_done);
  if (!done.ok()) {
    return done.status();
  }
  audio::OnTranscription native_transcription =
      [callback = std::move(*transcription)](std::optional<std::string> piece) {
        return callback->Call(std::move(piece));
      };
  audio::OnRecognitionDone native_done = [callback = std::move(*done)]() {
    return callback->Call();
  };
  return std::pair(std::move(native_transcription), std::move(native_done));
}

// GIL-reacquiring release for a Python type held as an ActionPortSchema
// typeinfo handle. Mirrors the actions binding's own deleter so the referent
// stays alive for exactly as long as any copy of the schema.
void ReleaseAudioTypeInfo(void* object) {
  if (object == nullptr || Py_IsInitialized() == 0) {
    return;
  }
  const PyGILState_STATE gil = PyGILState_Ensure();
  Py_DECREF(static_cast<PyObject*>(object));
  PyGILState_Release(gil);
}

std::shared_ptr<void> TypeInfoFromClass(const py::object& cls) {
  Py_INCREF(cls.ptr());
  return std::shared_ptr<void>(cls.ptr(), &ReleaseAudioTypeInfo);
}

// Resolves the Python class a port's typeinfo should point at from the type tag
// embedded in the port's declared media type; str for text/plain; else empty.
std::shared_ptr<void> TypeInfoForPort(const py::module_& native,
                                      const std::string& port_type) {
  const std::pair<std::string_view, const char*> kByTag[] = {
      {audio::kAudioBufferTypeTag, "AudioBuffer"},
      {audio::kAudioInputOptionsTypeTag, "AudioInputOptions"},
      {audio::kSpeechRecognizerOptionsTypeTag, "SpeechRecognizerOptions"},
      {audio::kAudioDeviceInfoTypeTag, "AudioDeviceInfo"},
      {audio::kAudioControlEventTypeTag, "AudioControlEvent"},
      {audio::kAudioCaptureEventTypeTag, "AudioCaptureEvent"},
      {audio::kTranscriptionEventTypeTag, "TranscriptionEvent"},
  };
  for (const auto& [tag, attr] : kByTag) {
    if (absl::StrContains(port_type, absl::StrCat("type=", tag))) {
      return TypeInfoFromClass(native.attr(attr));
    }
  }
  if (port_type == "text/plain") {
    return TypeInfoFromClass(py::reinterpret_borrow<py::object>(
        reinterpret_cast<PyObject*>(&PyUnicode_Type)));
  }
  return nullptr;
}

void AttachTypeInfo(a11::actions::ActionSchema& schema,
                    const py::module_& native) {
  for (auto& [name, port] : schema.inputs) {
    port.typeinfo = TypeInfoForPort(native, port.type);
  }
  for (auto& [name, port] : schema.outputs) {
    port.typeinfo = TypeInfoForPort(native, port.type);
  }
}

// One audio Action, ready to register: its name, its schema with the bound
// Python audio classes attached as port typeinfo (C++ cannot fabricate those),
// and its native handler.
struct AudioActionEntry {
  std::string_view name;
  a11::actions::ActionSchema schema;
  a11::actions::ActionHandler handler;
};

// The four audio Actions in protocol order. Both the Python export and
// RegisterAudioActionsPy build from this, so they cannot drift apart.
std::vector<AudioActionEntry> AudioActionEntries() {
  if (const absl::Status status = audio::EnsureAudioTypesRegistered();
      !status.ok()) {
    ThrowStatus(status);
  }
  const py::module_ native = py::module_::import("a11._native");
  std::vector<AudioActionEntry> entries;
  const auto add = [&](std::string_view name,
                       a11::actions::ActionSchema schema,
                       a11::actions::ActionHandler handler) {
    AttachTypeInfo(schema, native);
    entries.push_back(AudioActionEntry{.name = name,
                                       .schema = std::move(schema),
                                       .handler = std::move(handler)});
  };
  add(audio::kListAudioInputsAction, audio::ListAudioInputsSchema(),
      audio::ListAudioInputsHandler());
  add(audio::kCaptureAudioAction, audio::CaptureAudioSchema(),
      audio::CaptureAudioHandler());
  add(audio::kCaptureTranscriptionAction, audio::CaptureTranscriptionSchema(),
      audio::CaptureTranscriptionHandler());
  add(audio::kTranscribeAudioAction, audio::TranscribeAudioSchema(),
      audio::TranscribeAudioHandler());
  return entries;
}

// Returns the audio Actions as (name, schema, handler) triples so Python can
// hold them, register a subset, or inspect a schema before registering.
// a11.sdk.audio.actions is the typed surface over this; the stub generator
// renders bound classes inside a nested generic as `...`, so there is nothing
// to gain from spelling the element types out here.
py::list AudioActionsPy() {
  py::list result;
  for (AudioActionEntry& entry : AudioActionEntries()) {
    result.append(py::make_tuple(
        py::str(std::string(entry.name)), py::cast(std::move(entry.schema)),
        py::cast(NativeActionHandler(std::move(entry.handler)))));
  }
  return result;
}

// Registers every C++ audio Action on `registry`.
void RegisterAudioActionsPy(
    const std::shared_ptr<a11::actions::ActionRegistry>& registry) {
  if (registry == nullptr) {
    ThrowStatus(absl::InvalidArgumentError("registry must not be None"));
  }
  for (AudioActionEntry& entry : AudioActionEntries()) {
    if (const absl::Status status =
            registry->Register(std::string(entry.name),
                               std::move(entry.schema),
                               std::move(entry.handler));
        !status.ok()) {
      ThrowStatus(status);
    }
  }
}

void BindAudioEvents(py::module_& module) {
  py::class_<audio::AudioControlEvent>(
      module, "AudioControlEvent",
      "A command on an Action's control_events input; 'stop' finishes capture "
      "gracefully.")
      .def(py::init([](const std::string& command) {
             absl::StatusOr<audio::AudioControlEvent::Command> parsed =
                 audio::ParseControlCommand(command);
             if (!parsed.ok()) {
               ThrowStatus(parsed.status());
             }
             return audio::AudioControlEvent{*parsed};
           }),
           "Construct a control event.", py::arg("command") = "stop")
      .def_static("stop", &audio::AudioControlEvent::Stop,
                  "A stop command that finishes capture gracefully.")
      .def_property(
          "command",
          [](const audio::AudioControlEvent& self) {
            return std::string(audio::ToString(self.command));
          },
          [](audio::AudioControlEvent& self, const std::string& value) {
            absl::StatusOr<audio::AudioControlEvent::Command> parsed =
                audio::ParseControlCommand(value);
            if (!parsed.ok()) {
              ThrowStatus(parsed.status());
            }
            self.command = *parsed;
          },
          "The command name, e.g. 'stop'.")
      .def(py::self == py::self)
      .def("__repr__", [](const audio::AudioControlEvent& self) {
        return absl::StrCat("AudioControlEvent(command='",
                            audio::ToString(self.command), "')");
      });

  py::class_<audio::AudioCaptureEvent>(
      module, "AudioCaptureEvent",
      "A capture lifecycle or dropped-buffer notification from capture_audio.")
      .def(py::init([](const std::string& kind, std::uint64_t dropped) {
             absl::StatusOr<audio::AudioCaptureEvent::Kind> parsed =
                 audio::ParseCaptureKind(kind);
             if (!parsed.ok()) {
               ThrowStatus(parsed.status());
             }
             audio::AudioCaptureEvent event{*parsed, dropped};
             if (const absl::Status valid = event.Validate(); !valid.ok()) {
               ThrowStatus(valid);
             }
             return event;
           }),
           "Construct a capture event.", py::arg("kind") = "started",
           py::arg("dropped") = 0)
      .def_static("started", &audio::AudioCaptureEvent::Started)
      .def_static("stopped", &audio::AudioCaptureEvent::Stopped)
      .def_static("buffers_dropped", &audio::AudioCaptureEvent::BuffersDropped,
                  py::arg("count"))
      .def_property(
          "kind",
          [](const audio::AudioCaptureEvent& self) {
            return std::string(audio::ToString(self.kind));
          },
          [](audio::AudioCaptureEvent& self, const std::string& value) {
            absl::StatusOr<audio::AudioCaptureEvent::Kind> parsed =
                audio::ParseCaptureKind(value);
            if (!parsed.ok()) {
              ThrowStatus(parsed.status());
            }
            self.kind = *parsed;
          },
          "The event kind: 'started', 'buffers_dropped' or 'stopped'.")
      .def_readwrite("dropped", &audio::AudioCaptureEvent::dropped,
                     "Buffers dropped since the previous event.")
      .def(py::self == py::self)
      .def("__repr__", [](const audio::AudioCaptureEvent& self) {
        return absl::StrCat("AudioCaptureEvent(kind='",
                            audio::ToString(self.kind),
                            "', dropped=", self.dropped, ")");
      });

  py::class_<audio::TranscriptionEvent>(
      module, "TranscriptionEvent",
      "A capture/inference lifecycle notification from capture_transcription.")
      .def(py::init([](const std::string& kind) {
             absl::StatusOr<audio::TranscriptionEvent::Kind> parsed =
                 audio::ParseTranscriptionKind(kind);
             if (!parsed.ok()) {
               ThrowStatus(parsed.status());
             }
             return audio::TranscriptionEvent{*parsed};
           }),
           "Construct a transcription event.",
           py::arg("kind") = "capture_started")
      .def_static("capture_started", &audio::TranscriptionEvent::CaptureStarted)
      .def_static("inference_started",
                  &audio::TranscriptionEvent::InferenceStarted)
      .def_static("inference_stopped",
                  &audio::TranscriptionEvent::InferenceStopped)
      .def_static("capture_stopped", &audio::TranscriptionEvent::CaptureStopped)
      .def_property(
          "kind",
          [](const audio::TranscriptionEvent& self) {
            return std::string(audio::ToString(self.kind));
          },
          [](audio::TranscriptionEvent& self, const std::string& value) {
            absl::StatusOr<audio::TranscriptionEvent::Kind> parsed =
                audio::ParseTranscriptionKind(value);
            if (!parsed.ok()) {
              ThrowStatus(parsed.status());
            }
            self.kind = *parsed;
          },
          "The event kind, e.g. 'inference_started'.")
      .def(py::self == py::self)
      .def("__repr__", [](const audio::TranscriptionEvent& self) {
        return absl::StrCat("TranscriptionEvent(kind='",
                            audio::ToString(self.kind), "')");
      });
}

}  // namespace

void BindAudio(py::module_& module) {
  py::class_<audio::DeviceInfo>(
      module, "AudioDeviceInfo",
      "Static metadata describing one host audio device.")
      .def_readonly("index", &audio::DeviceInfo::index,
                    "PortAudio device index, stable within a process run.")
      .def_readonly("name", &audio::DeviceInfo::name, "Human-readable name.")
      .def_readonly("host_api", &audio::DeviceInfo::host_api,
                    "Host API backing the device (e.g. Core Audio, ALSA).")
      .def_readonly("max_input_channels",
                    &audio::DeviceInfo::max_input_channels,
                    "Maximum capture channels the device offers.")
      .def_readonly("max_output_channels",
                    &audio::DeviceInfo::max_output_channels,
                    "Maximum playback channels the device offers.")
      .def_readonly("default_sample_rate",
                    &audio::DeviceInfo::default_sample_rate,
                    "Default sample rate in hertz.")
      .def_property_readonly(
          "default_low_input_latency",
          [](const audio::DeviceInfo& self) {
            return DurationToPython(self.default_low_input_latency);
          },
          "Suggested latency for interactive input use.")
      .def_property_readonly(
          "default_high_input_latency",
          [](const audio::DeviceInfo& self) {
            return DurationToPython(self.default_high_input_latency);
          },
          "Suggested latency for robust, buffered input use.")
      .def_readonly("is_default_input", &audio::DeviceInfo::is_default_input,
                    "Whether this is the host default input device.")
      .def_readonly("is_default_output", &audio::DeviceInfo::is_default_output,
                    "Whether this is the host default output device.")
      .def("__repr__", [](const audio::DeviceInfo& self) {
        return "<AudioDeviceInfo index=" + std::to_string(self.index) +
               " name='" + self.name + "'>";
      });

  py::class_<audio::AudioBuffer>(
      module, "AudioBuffer",
      "A captured block of samples stored channel-major (planar). Use "
      "`memoryview(buffer)` for a zero-copy (channels x frames) float view, or "
      "`channel(i)` for one channel.",
      py::buffer_protocol())
      .def_buffer(&AudioBufferView)
      .def(py::init(&MakeAudioBufferFromBuffer),
           "Build an AudioBuffer from a buffer-protocol object (bytes, a NumPy "
           "array, a CPU PyTorch tensor, ...). Samples are read as "
           "channel-major "
           "float32 (raw bytes are reinterpreted as float32); a 2-D buffer's "
           "first dimension is the channel count.",
           py::arg("data"), py::arg("sample_rate"), py::arg("num_channels") = 1)
      .def_property_readonly(
          "num_channels",
          [](const audio::AudioBuffer& self) { return self.num_channels; },
          "Number of channels in this buffer.")
      .def_property_readonly(
          "num_frames",
          [](const audio::AudioBuffer& self) { return self.num_frames; },
          "Number of samples per channel in this buffer.")
      .def_property_readonly(
          "sample_rate",
          [](const audio::AudioBuffer& self) { return self.sample_rate; },
          "Sample rate, in hertz, the samples were captured at.")
      .def_property_readonly(
          "end_time",
          [](const audio::AudioBuffer& self) {
            return TimeToPython(self.end_time);
          },
          "Best-effort instant the final sample in this buffer was taken.");

  py::class_<audio::AudioInputOptions>(
      module, "AudioInputOptions",
      "How an AudioInput opens its capture stream.")
      .def(py::init([](int device_index, std::string device_name,
                       double sample_rate, int channels, size_t block_frames,
                       size_t ring_blocks, size_t buffer_frames) {
             audio::AudioInputOptions options{
                 .device_index = device_index,
                 .device_name = std::move(device_name),
                 .sample_rate = sample_rate,
                 .channels = channels,
                 .block_frames = block_frames,
                 .ring_blocks = ring_blocks,
                 .buffer_frames = buffer_frames,
             };
             if (const absl::Status valid = options.Validate(); !valid.ok()) {
               ThrowStatus(valid);
             }
             return options;
           }),
           "Construct validated audio input options.",
           py::arg("device_index") = -1, py::arg("device_name") = "",
           py::arg("sample_rate") = 0.0, py::arg("channels") = 0,
           py::arg("block_frames") = 256, py::arg("ring_blocks") = 32,
           py::arg("buffer_frames") = 0)
      .def_readwrite("device_index", &audio::AudioInputOptions::device_index,
                     "Device index to capture from, or negative for default.")
      .def_readwrite("device_name", &audio::AudioInputOptions::device_name,
                     "Input device name to capture from; empty selects by "
                     "index or the default input.")
      .def_readwrite("sample_rate", &audio::AudioInputOptions::sample_rate,
                     "Requested sample rate in hertz, or 0 for the default.")
      .def_readwrite("channels", &audio::AudioInputOptions::channels,
                     "Requested channel count, or 0 for the device's count.")
      .def_readwrite("block_frames", &audio::AudioInputOptions::block_frames,
                     "Frames per PortAudio callback block.")
      .def_readwrite("ring_blocks", &audio::AudioInputOptions::ring_blocks,
                     "Depth of the internal callback-to-fiber ring, in blocks.")
      .def_readwrite("buffer_frames", &audio::AudioInputOptions::buffer_frames,
                     "Frames per delivered subscription buffer, or 0 for the "
                     "block size.")
      .def(
          "__eq__",
          [](const audio::AudioInputOptions& self,
             const audio::AudioInputOptions& other) {
            return self.device_index == other.device_index &&
                   self.device_name == other.device_name &&
                   self.sample_rate == other.sample_rate &&
                   self.channels == other.channels &&
                   self.block_frames == other.block_frames &&
                   self.ring_blocks == other.ring_blocks &&
                   self.buffer_frames == other.buffer_frames;
          },
          py::is_operator());

  py::class_<audio::SpeechRecognizerOptions>(
      module, "SpeechRecognizerOptions",
      "Configuration for whisper.cpp transcription, a cheap energy VAD gate, "
      "and optional whisper.cpp Silero neural VAD.")
      .def(py::init([](std::string model_path, std::string language,
                       bool translate, int inference_threads, bool use_gpu,
                       bool flash_attention, bool use_context,
                       std::string initial_prompt,
                       size_t subscription_buffer_millis, float vad_threshold,
                       float vad_noise_ratio, size_t vad_window_millis,
                       size_t min_speech_millis, size_t min_silence_millis,
                       size_t speech_pad_millis, size_t max_speech_seconds,
                       std::string vad_model_path, float silero_threshold) {
             audio::SpeechRecognizerOptions options{
                 .model_path = std::move(model_path),
                 .language = std::move(language),
                 .translate = translate,
                 .inference_threads = inference_threads,
                 .use_gpu = use_gpu,
                 .flash_attention = flash_attention,
                 .use_context = use_context,
                 .initial_prompt = std::move(initial_prompt),
                 .subscription_buffer_millis = subscription_buffer_millis,
                 .vad_threshold = vad_threshold,
                 .vad_noise_ratio = vad_noise_ratio,
                 .vad_window_millis = vad_window_millis,
                 .min_speech_millis = min_speech_millis,
                 .min_silence_millis = min_silence_millis,
                 .speech_pad_millis = speech_pad_millis,
                 .max_speech_seconds = max_speech_seconds,
                 .vad_model_path = std::move(vad_model_path),
                 .silero_threshold = silero_threshold,
             };
             const absl::Status valid = options.Validate();
             if (!valid.ok()) {
               ThrowStatus(valid);
             }
             return options;
           }),
           "Construct validated speech recognition options.",
           py::arg("model_path") = "", py::arg("language") = "auto",
           py::arg("translate") = false, py::arg("inference_threads") = 0,
           py::arg("use_gpu") = true, py::arg("flash_attention") = true,
           py::arg("use_context") = false, py::arg("initial_prompt") = "",
           py::arg("subscription_buffer_millis") = 100,
           py::arg("vad_threshold") = 0.01f, py::arg("vad_noise_ratio") = 2.5f,
           py::arg("vad_window_millis") = 20,
           py::arg("min_speech_millis") = 250,
           py::arg("min_silence_millis") = 600,
           py::arg("speech_pad_millis") = 160,
           py::arg("max_speech_seconds") = 30, py::arg("vad_model_path") = "",
           py::arg("silero_threshold") = 0.5f)
      .def_readwrite("model_path", &audio::SpeechRecognizerOptions::model_path,
                     "Path to the whisper.cpp model; used by the transcription "
                     "action (empty is rejected there).")
      .def_readwrite("language", &audio::SpeechRecognizerOptions::language,
                     "Whisper language code, or 'auto'.")
      .def_readwrite("translate", &audio::SpeechRecognizerOptions::translate,
                     "Translate speech to English.")
      .def_readwrite("inference_threads",
                     &audio::SpeechRecognizerOptions::inference_threads,
                     "Decoder threads, or zero for the bounded default.")
      .def_readwrite("use_gpu", &audio::SpeechRecognizerOptions::use_gpu,
                     "Use a compiled GPU backend when available.")
      .def_readwrite("flash_attention",
                     &audio::SpeechRecognizerOptions::flash_attention,
                     "Use flash attention when supported.")
      .def_readwrite("use_context",
                     &audio::SpeechRecognizerOptions::use_context,
                     "Carry decoder context between utterances.")
      .def_readwrite("initial_prompt",
                     &audio::SpeechRecognizerOptions::initial_prompt,
                     "Optional initial decoder prompt.")
      .def_readwrite(
          "subscription_buffer_millis",
          &audio::SpeechRecognizerOptions::subscription_buffer_millis,
          "Duration of internally-created capture buffers.")
      .def_readwrite("vad_threshold",
                     &audio::SpeechRecognizerOptions::vad_threshold,
                     "Absolute RMS speech threshold.")
      .def_readwrite("vad_noise_ratio",
                     &audio::SpeechRecognizerOptions::vad_noise_ratio,
                     "Speech threshold relative to learned noise.")
      .def_readwrite("vad_window_millis",
                     &audio::SpeechRecognizerOptions::vad_window_millis,
                     "RMS analysis window duration.")
      .def_readwrite("min_speech_millis",
                     &audio::SpeechRecognizerOptions::min_speech_millis,
                     "Minimum voiced duration accepted.")
      .def_readwrite("min_silence_millis",
                     &audio::SpeechRecognizerOptions::min_silence_millis,
                     "Silence needed to endpoint speech.")
      .def_readwrite("speech_pad_millis",
                     &audio::SpeechRecognizerOptions::speech_pad_millis,
                     "Audio retained around an utterance.")
      .def_readwrite("max_speech_seconds",
                     &audio::SpeechRecognizerOptions::max_speech_seconds,
                     "Maximum utterance duration before splitting.")
      .def_readwrite("vad_model_path",
                     &audio::SpeechRecognizerOptions::vad_model_path,
                     "Path to a Silero VAD model; empty disables Silero VAD.")
      .def_readwrite("silero_threshold",
                     &audio::SpeechRecognizerOptions::silero_threshold,
                     "Silero speech-probability threshold in (0, 1].");

  py::classh<audio::AudioSubscription>(
      module, "AudioSubscription",
      "A live subscription delivering fixed-size buffers from an AudioInput.")
      .def_property_readonly("buffer_size",
                             &audio::AudioSubscription::buffer_size,
                             "Frames per channel in every delivered buffer.")
      .def_property_readonly("channels", &audio::AudioSubscription::channels,
                             "Number of channels in every delivered buffer.")
      .def_property_readonly(
          "sample_rate", &audio::AudioSubscription::sample_rate,
          "Sample rate, in hertz, of every delivered buffer.")
      .def_property_readonly(
          "dropped", &audio::AudioSubscription::dropped,
          "Buffers dropped because this subscription fell behind.")
      .def(
          "read",
          [](const std::shared_ptr<audio::AudioSubscription>& self) {
            return FutureToPython(self->Read());
          },
          "Await the next captured buffer for this subscription.")
      .def(
          "close",
          [](audio::AudioSubscription& self) {
            py::gil_scoped_release release;
            self.Close();
          },
          "Stop delivering; stops capture if this was the last subscription.");

  py::classh<audio::AudioInput>(
      module, "AudioInput",
      "A capturable input device that samples continuously while at least one "
      "subscription is alive.")
      .def(py::init(&OpenAudioInput),
           "Resolve the device and validate options without starting capture.",
           py::arg("options") = audio::AudioInputOptions{})
      .def_static(
          "open", &OpenAudioInput,
          "Resolve the device and validate options without starting capture.",
          py::arg("options") = audio::AudioInputOptions{})
      .def_property_readonly(
          "device", [](const audio::AudioInput& self) { return self.device(); },
          "The captured device's metadata.")
      .def_property_readonly(
          "name", [](const audio::AudioInput& self) { return self.name(); },
          "The captured device's name.")
      .def_property_readonly(
          "device_index",
          [](const audio::AudioInput& self) { return self.device_index(); },
          "The captured device's index.")
      .def_property_readonly(
          "sample_rate",
          [](const audio::AudioInput& self) { return self.sample_rate(); },
          "Sample rate, in hertz, capture runs at.")
      .def_property_readonly(
          "channels",
          [](const audio::AudioInput& self) { return self.channels(); },
          "Number of channels every subscription receives.")
      .def_property_readonly(
          "capturing",
          [](const audio::AudioInput& self) { return self.capturing(); },
          "Whether a capture stream is currently open.")
      .def(
          "subscribe",
          [](const std::shared_ptr<audio::AudioInput>& self,
             size_t buffer_size) {
            absl::StatusOr<std::shared_ptr<audio::AudioSubscription>>
                subscription;
            {
              py::gil_scoped_release release;
              subscription = self->Subscribe(buffer_size);
            }
            return ValueOrThrow(std::move(subscription));
          },
          "Begin receiving buffers of `buffer_size` frames per channel.",
          py::arg("buffer_size"));

  py::classh<audio::SpeechRecognizer>(
      module, "SpeechRecognizer",
      "Restartable local automatic speech recognizer backed by whisper.cpp. A "
      "cheap energy gate endpoints utterances so silence never reaches the "
      "decoder; a Silero VAD model, when configured, then filters each "
      "utterance to genuine speech before inference.")
      .def(py::init(&CreateSpeechRecognizer),
           "Load a whisper.cpp GGML/GGUF model. `source` may be an AudioInput, "
           "an AudioSubscription, or None for the default input.",
           py::arg("model_path"), py::arg("source") = py::none(),
           py::arg("options") = audio::SpeechRecognizerOptions{})
      .def_static("create", &CreateSpeechRecognizer,
                  "Load a model and construct a speech recognizer.",
                  py::arg("model_path"), py::arg("source") = py::none(),
                  py::arg("options") = audio::SpeechRecognizerOptions{})
      .def(
          "start",
          [](const std::shared_ptr<audio::SpeechRecognizer>& self,
             const py::object& on_transcription, const py::object& on_done) {
            absl::StatusOr<
                std::pair<audio::OnTranscription, audio::OnRecognitionDone>>
                callbacks = MakeRecognitionCallbacks(on_transcription, on_done);
            if (!callbacks.ok()) {
              return FutureToPython(a11::FailedTask(callbacks.status()));
            }
            a11::Task started;
            {
              py::gil_scoped_release release;
              started = self->Start(std::move(callbacks->first),
                                    std::move(callbacks->second));
            }
            return FutureToPython(std::move(started));
          },
          "Start capture and deliver awaited text/None and done callbacks.",
          py::arg("on_transcription"), py::arg("on_done"))
      .def(
          "stop",
          [](audio::SpeechRecognizer& self) {
            a11::Task stopped;
            {
              py::gil_scoped_release release;
              stopped = self.Stop();
            }
            return FutureToPython(std::move(stopped));
          },
          "Request an orderly stop and await terminal callbacks.")
      .def_property_readonly("running", &audio::SpeechRecognizer::running,
                             "Whether a recognition run is active.")
      .def_property_readonly("model_path", &audio::SpeechRecognizer::model_path,
                             "Path of the loaded whisper model.")
      .def_property_readonly(
          "options", &audio::SpeechRecognizer::options,
          "The validated native options used by this recognizer.")
      .def(
          "get_status",
          [](const audio::SpeechRecognizer& self) {
            return StatusToPython(self.GetStatus());
          },
          "Return the current or final recognition status.");

  module.def(
      "list_audio_devices",
      [] {
        absl::StatusOr<std::vector<audio::DeviceInfo>> devices;
        {
          py::gil_scoped_release release;
          devices = audio::ListDevices();
        }
        return ValueOrThrow(std::move(devices));
      },
      "Return metadata for every audio device, in index order.");
  module.def(
      "default_audio_input_device",
      [] {
        absl::StatusOr<audio::DeviceInfo> device;
        {
          py::gil_scoped_release release;
          device = audio::DefaultInputDevice();
        }
        return ValueOrThrow(std::move(device));
      },
      "Return metadata for the host's default input device.");
  module.def(
      "audio_device_info",
      [](int index) {
        absl::StatusOr<audio::DeviceInfo> device;
        {
          py::gil_scoped_release release;
          device = audio::DeviceInfoAt(index);
        }
        return ValueOrThrow(std::move(device));
      },
      "Return metadata for the audio device at `index`.", py::arg("index"));

  module.def(
      "audio_buffer_to_msgpack",
      [](const audio::AudioBuffer& buffer) {
        absl::StatusOr<std::string> bytes = audio::A11ToMsgpackBytes(buffer);
        return py::bytes(ValueOrThrow(std::move(bytes)));
      },
      "Encode an AudioBuffer to A11's MessagePack representation.",
      py::arg("buffer"));
  module.def(
      "audio_buffer_from_msgpack",
      [](const py::bytes& data) {
        absl::StatusOr<audio::AudioBuffer> buffer =
            audio::A11FromMsgpackBytes(a11::data::TypeTag<audio::AudioBuffer>{},
                                       static_cast<std::string>(data));
        return ValueOrThrow(std::move(buffer));
      },
      "Decode an AudioBuffer from A11's MessagePack representation.",
      py::arg("data"));

  BindAudioEvents(module);
  module.def("audio_actions", &AudioActionsPy,
             "Return the audio Actions as (name, schema, handler) triples in "
             "protocol order, each schema's ports already wired to the "
             "matching audio type and their serializers installed. Use these "
             "to register a subset, inspect a schema before registering, or "
             "hand a handler to Action.bind_handler().");
  module.def("register_audio_actions", &RegisterAudioActionsPy,
             py::arg("registry"),
             "Register every audio Action on `registry`, wiring each port's "
             "typeinfo to the matching audio type and ensuring their "
             "serializers are installed.");
}

}  // namespace a11::python
