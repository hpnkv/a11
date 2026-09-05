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

# The Flow standard library: the actions a composition needs to be a program
# rather than only a description of other people's deployed work. Its own target
# so that a11_actions gains no dependency on the filesystem, on processes or on
# the network, and so a host can leave the whole thing out by not linking it.
set(A11_FLOW_ACTIONS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/ports.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/options.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/policy.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/stop.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/time_actions.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/fs_actions.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/sandbox.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/process_actions.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/system_actions.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/flow_actions.cc
    PARENT_SCOPE)
