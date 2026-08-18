// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief MessagePack helpers behind A11's native wire value serializers.
 */

#ifndef A11_DATA_MSGPACK_H_
#define A11_DATA_MSGPACK_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <absl/base/nullability.h>
#include <absl/functional/function_ref.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <nlohmann/json.hpp>

namespace a11::data {

/**
 * @brief Incrementally encode the fields of one A11 wire record.
 *
 * A11 records are sequences of MessagePack objects, not one enclosing array;
 * the matching MsgpackReader therefore also rejects trailing fields.
 */
class MsgpackWriter {
 public:
  /// Encode into a buffer of this writer's own.
  MsgpackWriter() : bytes_(&owned_) {}

  /**
   * @brief Encode into a caller-supplied buffer, appending to what is there.
   *
   * @p target must outlive the writer. This is what lets a nested record be
   * encoded into a reused scratch buffer, or straight onto the end of a parent
   * record, rather than into a fresh string per level; see PackRecord().
   */
  explicit MsgpackWriter(std::string* absl_nonnull target)
      : bytes_(target) {}

  // Holds a pointer into its own member in the owning case, so copying or
  // moving one would leave the pointer behind. Nothing needs to.
  MsgpackWriter(const MsgpackWriter&) = delete;
  MsgpackWriter& operator=(const MsgpackWriter&) = delete;

  /// Append a JSON-compatible field to the encoded record.
  absl::Status Pack(const nlohmann::json& value);

  // The typed field writers below encode directly into the buffer, producing
  // byte-for-byte what Pack() produces for the equivalent JSON value -- the
  // same MessagePack size classes, chosen the same way -- without building a
  // `nlohmann::json` or the intermediate `std::string` that `Pack` gets back
  // from `PackMsgpack`. On the wire path that mattered: every field cost an
  // allocation and two copies of its own encoding, and A11's records nest three
  // deep, so a payload was allocated and copied about nine times on its way out.
  // `MsgpackWriterEncodingTest` pins the equivalence.

  /// Append a MessagePack nil.
  void PackNil();
  /// Append a MessagePack boolean.
  void PackBool(bool value);
  /// Append an unsigned integer in the narrowest MessagePack form that fits.
  void PackUint(std::uint64_t value);
  /// Append a signed integer in the narrowest MessagePack form that fits.
  void PackInt(std::int64_t value);
  /// Append a string field, copying @p value once.
  void PackString(std::string_view value);
  /// Append a binary field, copying @p value once.
  void PackBinary(std::string_view value);
  /// Append an array header; @p length element fields must follow.
  void PackArrayHeader(size_t length);
  /// Append a map header; @p length key/value field pairs must follow.
  void PackMapHeader(size_t length);

  /**
   * @brief
   *   Append a nested record as one binary field, encoded by @p encode.
   *
   * @p encode is handed a writer over a pooled scratch buffer, so a nested
   * record costs no allocation of its own after the pool has warmed. The result
   * is embedded exactly as building the child with its own writer and packing
   * `Binary(child)` would: one binary field, the same size class, the same
   * bytes.
   */
  absl::Status PackRecord(
      absl::FunctionRef<absl::Status(MsgpackWriter* absl_nonnull)> encode);

  /// View the encoded bytes without transferring ownership.
  [[nodiscard]] const std::string& bytes() const { return *bytes_; }

  /**
   * @brief Transfer the encoded bytes out of this writer.
   *
   * Only meaningful for a writer that owns its buffer; moving out of a borrowed
   * one would empty the caller's buffer.
   */
  [[nodiscard]] std::string TakeBytes() { return std::move(*bytes_); }

 private:
  /// Reserve @p extra bytes beyond what is already written.
  void Reserve(size_t extra);

  std::string owned_;
  std::string* absl_nonnull bytes_;
};

/// Sequentially decode the fields of one A11 MessagePack wire record.
class MsgpackReader {
 public:
  /// Read from @p bytes, which must remain alive while this reader is used.
  explicit MsgpackReader(std::string_view bytes) : bytes_(bytes) {}

  /// Decode the next field and advance the byte cursor.
  absl::StatusOr<nlohmann::json> Read();

  /**
   * @brief
   *   Read the next field as MessagePack binary without copying its payload.
   *
   * Read() materialises a @c nlohmann::json, which for a binary field means
   * allocating a vector and copying the bytes into it; callers then copy a
   * second time to get a @c Bytes. A11's records nest -- a WireMessage holds
   * binary fragments, a fragment holds a binary chunk, a chunk holds a binary
   * payload -- so a single 4 KiB fragment was copied several times on the way
   * in, and decode ran at a flat ~130 MiB/s against encode's 2+ GiB/s.
   *
   * The returned view points into the buffer this reader was constructed over,
   * which must outlive it.
   */
  absl::StatusOr<std::string_view> ReadBinaryView();

  /**
   * @brief
   *   Read the next field as a MessagePack array header, returning its length.
   *
   * Lets a caller iterate a list of binary records with ReadBinaryView()
   * instead of materialising the whole array as JSON first.
   */
  absl::StatusOr<size_t> ReadArrayLength();
  /// Return an error if unread trailing bytes remain in the record.
  absl::Status EnsureFullyConsumed() const;

  /// Current byte offset, useful when diagnosing malformed peer data.
  [[nodiscard]] size_t position() const { return position_; }

 private:
  std::string_view bytes_;
  size_t position_ = 0;
};

/// Encode a structured Abseil status for action and transport status fields.
absl::StatusOr<std::string> PackStatus(const absl::Status& status);
/// Decode a structured status without losing A11 status details.
absl::StatusOr<absl::Status> UnpackStatus(std::string_view bytes);

/// Wrap arbitrary bytes as a MessagePack binary JSON value.
nlohmann::json Binary(std::string_view bytes);
/// Read a binary JSON value, naming @p field_name in any validation error.
absl::StatusOr<std::string> GetBinary(const nlohmann::json& value,
                                      std::string_view field_name);

}  // namespace a11::data

#endif  // A11_DATA_MSGPACK_H_
