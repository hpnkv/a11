// Copyright 2026 The A11 Authors.

/**
 * @file
 * @brief Owned, binary-safe values returned by the asynchronous Redis client.
 */

#ifndef A11_REDIS_REPLY_H_
#define A11_REDIS_REPLY_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/status/statusor.h>

namespace a11::redis {

/** The RESP value kind held by a Reply. */
enum class ReplyType {
  kNull,
  kString,
  kInteger,
  kDouble,
  kBoolean,
  kArray,
  kMap,
  kSet,
};

/**
 * An owning C++ representation of a RESP2/RESP3 value.
 *
 * Hiredis replies are valid only for the duration of its callback. Reply
 * recursively owns every byte and child value, so it is safe to carry through
 * an A11 Future or retain after the libuv callback returns. Redis error frames
 * are translated to `absl::Status` and therefore never appear as Reply values.
 * Map elements are stored as alternating key/value entries.
 */
class Reply {
 public:
  Reply() = default;

  static Reply Null();
  static Reply String(std::string value);
  static Reply Integer(std::int64_t value);
  static Reply Double(double value);
  static Reply Boolean(bool value);
  static Reply Array(std::vector<Reply> values);
  static Reply Map(std::vector<Reply> alternating_keys_and_values);
  static Reply Set(std::vector<Reply> values);

  [[nodiscard]] ReplyType type() const { return type_; }

  [[nodiscard]] bool is_null() const { return type_ == ReplyType::kNull; }

  /** Borrow the binary-safe string bytes without making another payload copy. */
  absl::StatusOr<std::string_view> AsStringView() const;
  absl::StatusOr<std::string> AsString() const;
  absl::StatusOr<std::int64_t> AsInteger() const;
  absl::StatusOr<double> AsDouble() const;
  absl::StatusOr<bool> AsBoolean() const;
  absl::StatusOr<const std::vector<Reply>* absl_nonnull> AsElements() const;

  [[nodiscard]] std::string DebugString() const;

  friend bool operator==(const Reply&, const Reply&) = default;

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const Reply& value) {
    sink.Append(value.DebugString());
  }

 private:
  explicit Reply(ReplyType type) : type_(type) {}

  ReplyType type_ = ReplyType::kNull;
  std::string string_value_;
  std::int64_t integer_value_ = 0;
  double double_value_ = 0.0;
  bool boolean_value_ = false;
  std::vector<Reply> elements_;
};

}  // namespace a11::redis

#endif  // A11_REDIS_REPLY_H_
