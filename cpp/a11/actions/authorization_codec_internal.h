// Copyright 2026 The A11 Authors

#ifndef A11_ACTIONS_AUTHORIZATION_CODEC_INTERNAL_H_
#define A11_ACTIONS_AUTHORIZATION_CODEC_INTERNAL_H_

#include <string>
#include <string_view>

namespace a11::actions::internal {
std::string UnpaddedWebSafeBase64(std::string_view value);
}  // namespace a11::actions::internal

#endif  // A11_ACTIONS_AUTHORIZATION_CODEC_INTERNAL_H_
