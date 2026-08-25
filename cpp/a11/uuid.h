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

/**
 * @brief
 *   Generate a short random identifier as lowercase hexadecimal.
 *
 * Where `NewUuid()` names something a human may have to correlate across
 * systems, this names something that only has to be unique inside one -- an
 * action, and through it every node id derived from it. Twelve digits is 48
 * random bits: a one-in-a-million chance of a collision needs some 24 000 live
 * ids in one keyspace, far above what a session holds. Eight digits is 32 bits
 * and reaches the same odds at ninety-two, which is why the minimum here is not
 * the minimum you should ask for.
 *
 * Every character is a hex digit, so the result is always a valid
 * ::a11::data::ValidateName name and contains neither `-` nor `#` -- which
 * matters because a node id is `<action id>#<port>` and every parser of one
 * splits at the single `#`.
 *
 * Not cryptographically secure, for the same reason `NewUuid()` is not.
 *
 * @param hex_digits
 *   Length of the result, clamped to [8, 16].
 * @return
 *   A freshly generated identifier of @p hex_digits characters.
 */
std::string NewShortId(int hex_digits = 12);

/**
 * @brief Generate a stream identifier: @p prefix and 128 random bits as hex.
 *
 * What every transport names a stream, a session or a data channel with. The
 * prefix says which transport minted it, which is the difference between two
 * ids in one log being confusing and being informative.
 *
 * Not cryptographically secure, for the same reason `NewUuid()` is not.
 *
 * @param prefix Written in front of the digits, e.g. `"ws-"`. May be empty.
 * @return A freshly generated identifier.
 */
std::string NewStreamId(std::string_view prefix = "");

}  // namespace a11

#endif  // A11_UUID_H_
