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

#ifndef A11_JVM_INTEROP_H_
#define A11_JVM_INTEROP_H_

#include <string>
#include <string_view>

#include <absl/log/check.h>
#include <jni.h>

#include "a11/json_codec.h"
#include "a11/status.h"
#include "a11/utf8.h"
#include "jvm/values.h"

namespace a11::jvm {
inline jclass Class(JNIEnv* env, const char* name) {
  static thread::Mutex mu;
  static absl::flat_hash_map<std::string, jclass> classes ABSL_GUARDED_BY(mu);
  thread::MutexLock lock(&mu);
  auto found = classes.find(name);
  if (found != classes.end()) {
    return found->second;
  }
  jclass local = env->FindClass(name);
  CHECK(local != nullptr) << name;
  // JNI preserves the Java reference type through NewGlobalRef.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto result = static_cast<jclass>(env->NewGlobalRef(local));
  env->DeleteLocalRef(local);
  CHECK(result != nullptr);
  classes.emplace(name, result);
  return result;
}

inline std::int64_t Id(JNIEnv* env, jobject object) {
  CHECK(object != nullptr);
  static jmethodID method =
      env->GetMethodID(Class(env, "a11/NativeObject"), "getNativeId", "()J");
  return env->CallLongMethod(object, method);
}

inline std::string Text(JNIEnv* env, jstring value) {
  CHECK(value != nullptr);
  const jsize size = env->GetStringLength(value);
  const jchar* chars = env->GetStringChars(value, nullptr);
  CHECK(chars != nullptr);
  std::string result;
  result.reserve(static_cast<size_t>(size));
  for (jsize index = 0; index < size; ++index) {
    std::uint32_t code = chars[index];
    if (code >= 0xd800 && code <= 0xdbff && index + 1 < size &&
        chars[index + 1] >= 0xdc00 && chars[index + 1] <= 0xdfff) {
      code = 0x10000 + ((code - 0xd800) << 10) + (chars[++index] - 0xdc00);
    } else if (code >= 0xdc80 && code <= 0xdcff) {
      result.push_back(static_cast<char>(code & 0xff));
      continue;
    }
    if (code < 0x80) {
      result.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      result.push_back(static_cast<char>(0xc0 | (code >> 6)));
      result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else if (code < 0x10000) {
      result.push_back(static_cast<char>(0xe0 | (code >> 12)));
      result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
      result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
      result.push_back(static_cast<char>(0xf0 | (code >> 18)));
      result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
      result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
      result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
  }
  env->ReleaseStringChars(value, chars);
  return result;
}

inline jstring Text(JNIEnv* env, std::string_view value) {
  std::u16string result;
  for (size_t index = 0; index < value.size();) {
    const auto lead = static_cast<unsigned char>(value[index]);
    const size_t width = utf8::SequenceWidth(static_cast<char>(lead));
    // Surrogate escapes preserve native byte strings that are not UTF-8.
    // Unpaired JVM surrogates retain their WTF-8 bytes in native storage.
    const auto sequence = value.substr(index, width);
    const bool surrogate = width == 3 && sequence.size() == 3 && lead == 0xed &&
                           static_cast<unsigned char>(sequence[1]) >= 0xa0 &&
                           utf8::IsContinuation(sequence[1]) &&
                           utf8::IsContinuation(sequence[2]);
    if (!utf8::IsValid(sequence) && !surrogate) {
      result.push_back(static_cast<char16_t>(0xdc00 | lead));
      ++index;
      continue;
    }
    std::uint32_t code = lead & (width == 1 ? 0x7f : (0x7f >> width));
    for (size_t part = 1; part < width; ++part) {
      code = (code << 6) |
             (static_cast<unsigned char>(value[index + part]) & 0x3f);
    }
    index += width;
    if (code < 0x10000) {
      result.push_back(static_cast<char16_t>(code));
    } else {
      code -= 0x10000;
      result.push_back(static_cast<char16_t>(0xd800 | (code >> 10)));
      result.push_back(static_cast<char16_t>(0xdc00 | (code & 0x3ff)));
    }
  }
  return env->NewString(reinterpret_cast<const jchar*>(result.data()),
                        static_cast<jsize>(result.size()));
}

inline std::string Bytes(JNIEnv* env, jbyteArray value) {
  CHECK(value != nullptr);
  std::string result(static_cast<size_t>(env->GetArrayLength(value)), '\0');
  env->GetByteArrayRegion(value, 0, static_cast<jsize>(result.size()),
                          reinterpret_cast<jbyte*>(result.data()));
  return result;
}

inline jbyteArray Bytes(JNIEnv* env, std::string_view value) {
  CHECK_LE(value.size(),
           static_cast<size_t>(std::numeric_limits<jsize>::max()));
  auto result = env->NewByteArray(static_cast<jsize>(value.size()));
  if (result != nullptr) {
    env->SetByteArrayRegion(result, 0, static_cast<jsize>(value.size()),
                            reinterpret_cast<const jbyte*>(value.data()));
  }
  return result;
}

inline jobject BoxLong(JNIEnv* env, jlong value) {
  static jmethodID method = env->GetStaticMethodID(
      Class(env, "java/lang/Long"), "valueOf", "(J)Ljava/lang/Long;");
  return env->CallStaticObjectMethod(Class(env, "java/lang/Long"), method,
                                     value);
}

inline jobject JsonNode(JNIEnv* env, const nlohmann::json& value) {
  if (value.is_null()) {
    return nullptr;
  }
  if (value.is_string()) {
    return Text(env, value.get_ref<const std::string&>());
  }
  if (value.is_boolean()) {
    auto cls = Class(env, "java/lang/Boolean");
    return env->CallStaticObjectMethod(
        cls, env->GetStaticMethodID(cls, "valueOf", "(Z)Ljava/lang/Boolean;"),
        static_cast<jboolean>(value.get<bool>()));
  }
  if (value.is_number_unsigned() &&
      value.get<std::uint64_t>() >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max())) {
    auto cls = Class(env, "java/math/BigInteger");
    auto text = Text(env, std::to_string(value.get<std::uint64_t>()));
    auto result = env->NewObject(
        cls, env->GetMethodID(cls, "<init>", "(Ljava/lang/String;)V"), text);
    env->DeleteLocalRef(text);
    return result;
  }
  if (value.is_number_integer()) {
    return BoxLong(env, value.get<std::int64_t>());
  }
  if (value.is_binary()) {
    const auto& bytes = value.get_binary();
    return Bytes(env,
                 std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                  bytes.size()));
  }
  if (value.is_number()) {
    auto cls = Class(env, "java/lang/Double");
    return env->CallStaticObjectMethod(
        cls, env->GetStaticMethodID(cls, "valueOf", "(D)Ljava/lang/Double;"),
        value.get<double>());
  }
  if (value.is_array()) {
    auto cls = Class(env, "java/util/ArrayList");
    static auto constructor = env->GetMethodID(cls, "<init>", "(I)V");
    return env->NewObject(cls, constructor, static_cast<jint>(value.size()));
  }
  CHECK(value.is_object());
  auto cls = Class(env, "java/util/LinkedHashMap");
  static auto constructor = env->GetMethodID(cls, "<init>", "()V");
  return env->NewObject(cls, constructor);
}

inline jobject JsonValue(JNIEnv* env, const nlohmann::json& value) {
  jobject result = JsonNode(env, value);
  if (!value.is_structured() || env->ExceptionCheck()) {
    return result;
  }
  static auto add = env->GetMethodID(Class(env, "java/util/ArrayList"), "add",
                                     "(Ljava/lang/Object;)Z");
  static auto put = env->GetMethodID(
      Class(env, "java/util/LinkedHashMap"), "put",
      "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

  struct Frame {
    const nlohmann::json* value;
    jobject container;
    nlohmann::json::const_iterator next;
  };

  std::vector<Frame> stack;
  stack.push_back(
      Frame{.value = &value, .container = result, .next = value.cbegin()});
  while (!stack.empty()) {
    auto& frame = stack.back();
    if (frame.next == frame.value->cend()) {
      if (frame.container != result) {
        env->DeleteLocalRef(frame.container);
      }
      stack.pop_back();
      continue;
    }
    auto next = frame.next++;
    const auto& child = *next;
    auto converted = JsonNode(env, child);
    if (env->ExceptionCheck()) {
      return nullptr;
    }
    if (frame.value->is_array()) {
      env->CallBooleanMethod(frame.container, add, converted);
    } else {
      auto key = Text(env, next.key());
      env->CallObjectMethod(frame.container, put, key, converted);
      env->DeleteLocalRef(key);
    }
    if (env->ExceptionCheck()) {
      return nullptr;
    }
    if (child.is_structured()) {
      stack.push_back(Frame{
          .value = &child, .container = converted, .next = child.cbegin()});
    } else if (converted != nullptr) {
      env->DeleteLocalRef(converted);
    }
  }
  return result;
}

inline jobject Status(JNIEnv* env, const absl::Status& status) {
  jobject details = JsonValue(env, StatusDetails(status));
  if (env->ExceptionCheck()) {
    return nullptr;
  }
  jstring message = Text(env, status.message());
  jclass cls = Class(env, "a11/internal/NativeResults");
  auto method = env->GetStaticMethodID(
      cls, "status", "(ILjava/lang/String;Ljava/util/List;)La11/Status;");
  jobject result = env->CallStaticObjectMethod(
      cls, method, static_cast<jint>(status.code()), message, details);
  env->DeleteLocalRef(message);
  env->DeleteLocalRef(details);
  return result;
}

inline jobject Ok(JNIEnv* env, jobject value) {
  auto cls = Class(env, "a11/Ok");
  return env->NewObject(
      cls, env->GetMethodID(cls, "<init>", "(Ljava/lang/Object;)V"), value);
}

inline jobject Wrapped(JNIEnv* env, const char* name, std::int64_t id) {
  auto cls = Class(env, name);
  return env->NewObject(cls, env->GetMethodID(cls, "<init>", "(J)V"),
                        static_cast<jlong>(id));
}
}  // namespace a11::jvm
#endif  // A11_JVM_INTEROP_H_
