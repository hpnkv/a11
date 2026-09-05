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

# HTTP as Actions. A thin layer over a11::net -- the schemas, the two handlers,
# and the decoding web-fetch does -- kept as its own target so a11_net does not
# gain a dependency on the actions and nodes layers above it.
set(A11_HTTP_ACTIONS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/http_actions.cc
    PARENT_SCOPE)
