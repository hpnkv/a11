// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief MessagePack helpers behind A11's native wire value serializers.
 */

#ifndef A11_DATA_MSGPACK_H_
#define A11_DATA_MSGPACK_H_

#include <cstddef>
#include <string>
#include <string_view>

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
  /// Append a JSON-compatible field to the encoded record.
  absl::Status Pack(const nlohmann::json& value);

  /// View the encoded bytes without transferring ownership.
  [[nodiscard]] const std::string& bytes() const { return bytes_; }

  /// Transfer the encoded bytes out of this writer.
  [[nodiscard]] std::string TakeBytes() { return std::move(bytes_); }

 private:
  std::string bytes_;
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
