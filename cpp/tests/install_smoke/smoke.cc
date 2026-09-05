// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

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
