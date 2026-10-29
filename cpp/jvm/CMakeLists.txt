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

set(A11_JVM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/bridge.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/time.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/core.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/stores.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/net.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/flow.cc
    PARENT_SCOPE)

set(A11_JVM_JNI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/jni.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/data_jni.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/control_jni.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/time_jni.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/framing_jni.cc
    PARENT_SCOPE)
