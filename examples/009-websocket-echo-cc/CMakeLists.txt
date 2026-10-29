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
project(a11_websocket_echo LANGUAGES CXX)

find_package(a11 CONFIG REQUIRED)
add_executable(websocket-echo main.cc)
target_compile_features(websocket-echo PRIVATE cxx_std_20)
target_compile_options(websocket-echo PRIVATE -fno-exceptions)
target_link_libraries(websocket-echo PRIVATE a11::service)
