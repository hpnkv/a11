# Copyright 2026 The A11 Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set(A11_AUDIO_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_buffer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_input.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/device.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/speech_recognizer.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/internal/audio_processing.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/internal/portaudio_session.cc
    PARENT_SCOPE)

# The Action-based interface layered on top of the raw capture/recognition
# primitives. Kept as a separate target so the base audio library does not gain
# a dependency on the higher actions/nodes layer.
set(A11_AUDIO_ACTIONS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/audio_events.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/audio_serialization.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/audio_actions.cc
    # The model catalogue sits here rather than with the base audio sources so
    # that a11_audio does not gain a dependency on a11::net to download one.
    ${CMAKE_CURRENT_SOURCE_DIR}/model_registry.cc
    PARENT_SCOPE)
