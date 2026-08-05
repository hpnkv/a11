#include <memory>

#include "a11/nodes/node_map.h"
#ifdef A11_INSTALL_SMOKE_AUDIO
#include "sdk/audio/speech_recognizer.h"
#endif

int main() {
  const auto node_map = a11::nodes::NodeMap::Create();
  if (!node_map.ok() || *node_map == nullptr) {
    return 1;
  }
#ifdef A11_INSTALL_SMOKE_AUDIO
  // This reaches SpeechRecognizer's translation unit (and therefore validates
  // all static whisper.cpp link dependencies) without opening an audio device.
  const auto recognizer = a11::sdk::audio::SpeechRecognizer::Create(
      "a11-install-smoke-model-does-not-exist.ggml");
  if (recognizer.ok()) {
    return 1;
  }
#endif
  return 0;
}
