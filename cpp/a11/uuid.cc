// Copyright 2026 The A11 Authors.

#include "a11/uuid.h"

#include <cstdint>
#include <string>

#include <absl/random/random.h>
#include <absl/strings/str_format.h>

namespace a11 {

std::string NewUuid() {
  // Thread-local so a hot path does not reseed a generator per call.
  thread_local absl::BitGen generator;
  std::uint64_t high = absl::Uniform<std::uint64_t>(generator);
  std::uint64_t low = absl::Uniform<std::uint64_t>(generator);
  // Version 4 in the high nibble of octet 6, and the RFC 4122 variant in the
  // top two bits of octet 8.
  high = (high & ~UINT64_C(0xf000)) | UINT64_C(0x4000);
  low = (low & ~(UINT64_C(0xc) << 60U)) | (UINT64_C(0x8) << 60U);
  return absl::StrFormat(
      "%08x-%04x-%04x-%04x-%012x", static_cast<std::uint32_t>(high >> 32U),
      static_cast<std::uint16_t>(high >> 16U), static_cast<std::uint16_t>(high),
      static_cast<std::uint16_t>(low >> 48U),
      low & UINT64_C(0x0000ffffffffffff));
}

}  // namespace a11
