// Copyright 2026 The A11 Authors.

#include "redis/reply.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/escaping.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_join.h>

namespace a11::redis {
namespace {

std::string TypeName(ReplyType type) {
  switch (type) {
    case ReplyType::kNull:
      return "null";
    case ReplyType::kString:
      return "string";
    case ReplyType::kInteger:
      return "integer";
    case ReplyType::kDouble:
      return "double";
    case ReplyType::kBoolean:
      return "boolean";
    case ReplyType::kArray:
      return "array";
    case ReplyType::kMap:
      return "map";
    case ReplyType::kSet:
      return "set";
  }
  return "unknown";
}

absl::Status WrongType(ReplyType actual, const std::string& expected) {
  return absl::FailedPreconditionError(
      absl::StrCat("Redis reply is ", TypeName(actual), ", not ", expected));
}

}  // namespace

Reply Reply::Null() {
  return Reply(ReplyType::kNull);
}

Reply Reply::String(std::string value) {
  Reply reply(ReplyType::kString);
  reply.string_value_ = std::move(value);
  return reply;
}

Reply Reply::Integer(std::int64_t value) {
  Reply reply(ReplyType::kInteger);
  reply.integer_value_ = value;
  return reply;
}

Reply Reply::Double(double value) {
  Reply reply(ReplyType::kDouble);
  reply.double_value_ = value;
  return reply;
}

Reply Reply::Boolean(bool value) {
  Reply reply(ReplyType::kBoolean);
  reply.boolean_value_ = value;
  return reply;
}

Reply Reply::Array(std::vector<Reply> values) {
  Reply reply(ReplyType::kArray);
  reply.elements_ = std::move(values);
  return reply;
}

Reply Reply::Map(std::vector<Reply> alternating_keys_and_values) {
  Reply reply(ReplyType::kMap);
  reply.elements_ = std::move(alternating_keys_and_values);
  return reply;
}

Reply Reply::Set(std::vector<Reply> values) {
  Reply reply(ReplyType::kSet);
  reply.elements_ = std::move(values);
  return reply;
}

absl::StatusOr<std::string> Reply::AsString() const {
  ABSL_ASSIGN_OR_RETURN(std::string_view value, AsStringView());
  return std::string(value);
}

absl::StatusOr<std::string_view> Reply::AsStringView() const {
  if (type_ != ReplyType::kString) {
    return WrongType(type_, "a string");
  }
  return std::string_view(string_value_);
}

absl::StatusOr<std::int64_t> Reply::AsInteger() const {
  if (type_ != ReplyType::kInteger) {
    return WrongType(type_, "an integer");
  }
  return integer_value_;
}

absl::StatusOr<double> Reply::AsDouble() const {
  if (type_ != ReplyType::kDouble) {
    return WrongType(type_, "a double");
  }
  return double_value_;
}

absl::StatusOr<bool> Reply::AsBoolean() const {
  if (type_ != ReplyType::kBoolean) {
    return WrongType(type_, "a boolean");
  }
  return boolean_value_;
}

absl::StatusOr<const std::vector<Reply>* absl_nonnull> Reply::AsElements()
    const {
  if (type_ != ReplyType::kArray && type_ != ReplyType::kMap &&
      type_ != ReplyType::kSet) {
    return WrongType(type_, "an aggregate");
  }
  return &elements_;
}

std::string Reply::DebugString() const {
  switch (type_) {
    case ReplyType::kNull:
      return "null";
    case ReplyType::kString:
      return absl::StrCat("b'", absl::CEscape(string_value_), "'");
    case ReplyType::kInteger:
      return absl::StrCat(integer_value_);
    case ReplyType::kDouble:
      return absl::StrCat(double_value_);
    case ReplyType::kBoolean:
      return boolean_value_ ? "true" : "false";
    case ReplyType::kArray:
    case ReplyType::kMap:
    case ReplyType::kSet:
      return absl::StrCat(
          TypeName(type_), "(",
          absl::StrJoin(elements_, ", ",
                        [](std::string* output, const Reply& child) {
                          absl::StrAppend(output, child.DebugString());
                        }),
          ")");
  }
  return "unknown";
}

}  // namespace a11::redis
