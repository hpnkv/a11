// Copyright 2026 The A11 Authors.
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
