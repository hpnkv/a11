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
    PARENT_SCOPE)
