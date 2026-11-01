// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef A11_TESTS_INSTALL_SMOKE_REGEX_HOLDER_H_
#define A11_TESTS_INSTALL_SMOKE_REGEX_HOLDER_H_

#include "a11/internal/regex.h"

struct RegexHolder {
  a11::internal::Regex expression;
};

#endif  // A11_TESTS_INSTALL_SMOKE_REGEX_HOLDER_H_
