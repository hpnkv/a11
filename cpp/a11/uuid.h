// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Random identifier generation shared across A11 subsystems.
 */

#ifndef A11_UUID_H_
#define A11_UUID_H_

#include <cstdint>
#include <string>

namespace a11 {

/**
 * @brief
 *   64 random bits from a thread-local generator.
 *
 * For building identifiers. Every id in A11 -- session, stream, data channel,
 * WebSocket masking key -- should come from here rather than from a locally
 * constructed `absl::BitGen`: constructing one seeds Randen from the operating
 * system's entropy source, which costs microseconds and, at a site called per
 * connection or per call, costs them repeatedly for no benefit. One generator
 * per thread needs no lock and no seeding after the first call.
 *
 * Not cryptographically secure, for the same reason `NewUuid()` is not: use it
 * for identity, never for secrets.
 *
 * @return
 *   64 uniformly distributed random bits.
 */
std::uint64_t RandomUint64();

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
