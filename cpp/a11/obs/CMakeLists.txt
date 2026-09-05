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

set(A11_OBS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/provider.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/trace_context.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/tracer.cc)
if (A11_WITH_OTLP_HTTP)
    list(APPEND A11_OBS_SOURCES
         ${CMAKE_CURRENT_SOURCE_DIR}/internal/otlp_http_exporter.cc)
endif ()
set(A11_OBS_SOURCES ${A11_OBS_SOURCES} PARENT_SCOPE)
