/*
 * Copyright 2026 The A11 Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// The sibling header of `schemas.cc`, and the reason the scanner reads one.
//
// Nearly every C++ action names itself with a `constexpr std::string_view`
// declared here rather than in the implementation, so a scan that read only the
// `.cc` would drop every one of them for having no name.

#ifndef A11_TESTDATA_FLOW_DISCOVER_SCHEMAS_H_
#define A11_TESTDATA_FLOW_DISCOVER_SCHEMAS_H_

#include <string_view>

namespace a11::testdata {

inline constexpr std::string_view kAssembledAction = "cpp-assembled";
inline constexpr std::string_view kOctetStream = "application/octet-stream";

}  // namespace a11::testdata

#endif  // A11_TESTDATA_FLOW_DISCOVER_SCHEMAS_H_
