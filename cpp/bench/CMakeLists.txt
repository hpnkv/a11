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

# The native benchmark binary. Its own executable rather than a mode of a test:
# a benchmark links the whole runtime and runs for minutes, and neither belongs
# in a target CI runs on every commit.
add_executable(a11_bench harness.cc bench_main.cc)
add_executable(a11::bench ALIAS a11_bench)
target_link_libraries(
  a11_bench PRIVATE a11::concurrency a11::data a11::net a11::nodes a11::stores
                    a11::actions a11::service a11::flow_lang a11::flow_runtime
                    nlohmann_json::nlohmann_json a11::allocator a11::warnings)
target_include_directories(a11_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
# The uv round-trip row includes a11/net/internal/http_transport.h, which pulls
# in uvw; uvw's headers only compile against its own target's definitions.
target_link_libraries(a11_bench PRIVATE uvw::uvw)
