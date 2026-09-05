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

cmake_minimum_required(VERSION 3.28)
project(a11_install_smoke LANGUAGES CXX)

find_package(a11 CONFIG REQUIRED)

add_executable(a11_install_smoke smoke.cc)
target_link_libraries(a11_install_smoke PRIVATE a11::service)
if(TARGET a11::audio)
  target_link_libraries(a11_install_smoke PRIVATE a11::audio)
  target_compile_definitions(a11_install_smoke PRIVATE A11_INSTALL_SMOKE_AUDIO=1)
endif()
target_compile_features(a11_install_smoke PRIVATE cxx_std_20)
