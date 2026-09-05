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

set(A11_PYTHON_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/actions_bindings.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core_bindings.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/data_bindings.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/debug_bindings.cc
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
