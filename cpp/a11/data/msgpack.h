// Copyright 2026 The A11 Authors.

#ifndef A11_DATA_MSGPACK_H_
#define A11_DATA_MSGPACK_H_

#include <cstddef>
#include <string>
#include <string_view>

#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <nlohmann/json.hpp>

namespace a11::data {

// A11's native wire models are encoded as a sequence of MessagePack objects,
// not as one enclosing array. These helpers retain that format and reject
// trailing data. The value codec itself is nlohmann JSON's MessagePack codec.
class MsgpackWriter {
 public:
  absl::Status Pack(const nlohmann::json& value);

  [[nodiscard]] const std::string& bytes() const { return bytes_; }

  [[nodiscard]] std::string TakeBytes() { return std::move(bytes_); }

 private:
  std::string bytes_;
};

class MsgpackReader {
 public:
  explicit MsgpackReader(std::string_view bytes) : bytes_(bytes) {}

  absl::StatusOr<nlohmann::json> Read();
  absl::Status EnsureFullyConsumed() const;

  [[nodiscard]] size_t position() const { return position_; }

 private:
  std::string_view bytes_;
  size_t position_ = 0;
};

absl::StatusOr<std::string> PackStatus(const absl::Status& status);
absl::StatusOr<absl::Status> UnpackStatus(std::string_view bytes);

nlohmann::json Binary(std::string_view bytes);
absl::StatusOr<std::string> GetBinary(const nlohmann::json& value,
                                      std::string_view field_name);

}  // namespace a11::data

#endif  // A11_DATA_MSGPACK_H_
