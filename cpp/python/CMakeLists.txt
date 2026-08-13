set(A11_PYTHON_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/actions_bindings.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core_bindings.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/data_bindings.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/flow_bindings.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/http_bindings.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/interop.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/logging_bindings.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/module.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/net_bindings.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/nodes_bindings.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/obs_bindings.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/service_bindings.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/stores_bindings.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/webrtc_bindings.cc)
if (A11_BUILD_REDIS)
    list(APPEND A11_PYTHON_SOURCES
         ${CMAKE_CURRENT_SOURCE_DIR}/redis_bindings.cc)
endif ()
if (A11_BUILD_AUDIO)
    list(APPEND A11_PYTHON_SOURCES
         ${CMAKE_CURRENT_SOURCE_DIR}/audio_bindings.cc)
endif ()
set(A11_PYTHON_SOURCES ${A11_PYTHON_SOURCES} PARENT_SCOPE)
