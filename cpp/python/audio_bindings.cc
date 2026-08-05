// Copyright 2026 The A11 Authors.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/statusor.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "python/bindings.h"
#include "python/interop.h"
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
      .def(py::init([](int device_index, double sample_rate, int channels,
                       size_t block_frames, size_t ring_blocks) {
             audio::AudioInputOptions options{
                 .device_index = device_index,
                 .sample_rate = sample_rate,
                 .channels = channels,
                 .block_frames = block_frames,
                 .ring_blocks = ring_blocks,
             };
             if (const absl::Status valid = options.Validate(); !valid.ok()) {
               ThrowStatus(valid);
             }
             return options;
           }),
           "Construct validated audio input options.",
           py::arg("device_index") = -1, py::arg("sample_rate") = 0.0,
           py::arg("channels") = 0, py::arg("block_frames") = 256,
           py::arg("ring_blocks") = 32)
      .def_readwrite("device_index", &audio::AudioInputOptions::device_index,
                     "Device index to capture from, or negative for default.")
      .def_readwrite("sample_rate", &audio::AudioInputOptions::sample_rate,
                     "Requested sample rate in hertz, or 0 for the default.")
      .def_readwrite("channels", &audio::AudioInputOptions::channels,
                     "Requested channel count, or 0 for the device's count.")
      .def_readwrite("block_frames", &audio::AudioInputOptions::block_frames,
                     "Frames per PortAudio callback block.")
      .def_readwrite("ring_blocks", &audio::AudioInputOptions::ring_blocks,
                     "Depth of the internal callback-to-fiber ring, in blocks.")
      .def(
          "__eq__",
          [](const audio::AudioInputOptions& self,
             const audio::AudioInputOptions& other) {
            return self.device_index == other.device_index &&
                   self.sample_rate == other.sample_rate &&
                   self.channels == other.channels &&
                   self.block_frames == other.block_frames &&
                   self.ring_blocks == other.ring_blocks;
          },
          py::is_operator());

  py::class_<audio::SpeechRecognizerOptions>(
      module, "SpeechRecognizerOptions",
      "Configuration for whisper.cpp transcription, a cheap energy VAD gate, "
      "and optional whisper.cpp Silero neural VAD.")
      .def(
          py::init([](std::string language, bool translate,
                      int inference_threads, bool use_gpu, bool flash_attention,
                      bool use_context, std::string initial_prompt,
                      size_t subscription_buffer_millis, float vad_threshold,
                      float vad_noise_ratio, size_t vad_window_millis,
                      size_t min_speech_millis, size_t min_silence_millis,
                      size_t speech_pad_millis, size_t max_speech_seconds,
                      std::string vad_model_path, float silero_threshold) {
            audio::SpeechRecognizerOptions options{
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
          py::arg("language") = "auto", py::arg("translate") = false,
          py::arg("inference_threads") = 0, py::arg("use_gpu") = true,
          py::arg("flash_attention") = true, py::arg("use_context") = false,
          py::arg("initial_prompt") = "",
          py::arg("subscription_buffer_millis") = 100,
          py::arg("vad_threshold") = 0.01f, py::arg("vad_noise_ratio") = 2.5f,
          py::arg("vad_window_millis") = 20, py::arg("min_speech_millis") = 250,
          py::arg("min_silence_millis") = 600,
          py::arg("speech_pad_millis") = 160,
          py::arg("max_speech_seconds") = 30, py::arg("vad_model_path") = "",
          py::arg("silero_threshold") = 0.5f)
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

  py::class_<audio::AudioSubscription,
             std::shared_ptr<audio::AudioSubscription>>(
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

  py::class_<audio::AudioInput, std::shared_ptr<audio::AudioInput>>(
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

  py::class_<audio::SpeechRecognizer, std::shared_ptr<audio::SpeechRecognizer>>(
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
}

}  // namespace a11::python
