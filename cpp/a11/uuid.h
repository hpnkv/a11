// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Random identifier generation shared across A11 subsystems.
 */

#ifndef A11_UUID_H_
#define A11_UUID_H_

#include <string>

namespace a11 {

/**
 * @brief
 *   Generate a random RFC 4122 version 4 UUID in canonical text form.
 *
 * The result is 36 characters, lowercase, `8-4-4-4-12` hyphenated, with the
 * version and variant bits set. Randomness comes from a thread-local
 * `absl::BitGen`, so this is cheap to call in a loop but is **not**
 * cryptographically secure: use it for identity, never for secrets.
 *
 * @return
 *   A freshly generated UUID string.
 */
std::string NewUuid();

}  // namespace a11

#endif  // A11_UUID_H_
