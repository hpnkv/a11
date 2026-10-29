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

#include <limits>

#include <jni.h>

#include "a11/json_codec.h"
#include "jvm/bridge.h"
#include "jvm/interop.h"
#include "thread/introspect.h"

namespace {
jbyteArray Encode(JNIEnv* env, const nlohmann::json& value) {
  const auto bytes = a11::PackMsgpack(value, "JVM response");
  if (!bytes.ok()) {
    return Encode(env, a11::jvm::Envelope(bytes.status()));
  }
  if (bytes->size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    return Encode(env, a11::jvm::Envelope(absl::ResourceExhaustedError(
                           "JVM response too large")));
  }
  jbyteArray output = env->NewByteArray(static_cast<jsize>(bytes->size()));
  if (output != nullptr) {
    env->SetByteArrayRegion(output, 0, static_cast<jsize>(bytes->size()),
                            reinterpret_cast<const jbyte*>(bytes->data()));
  }
  return output;
}
}  // namespace

extern "C" JNIEXPORT jbyteArray JNICALL Java_a11_internal_NativeBridge_request(
    JNIEnv* env, jobject, jbyteArray encoded) {
  if (encoded == nullptr) {
    return Encode(env, a11::jvm::Envelope(
                           absl::InvalidArgumentError("Null JVM request")));
  }
  const jsize size = env->GetArrayLength(encoded);
  std::string bytes(static_cast<size_t>(size), '\0');
  env->GetByteArrayRegion(encoded, 0, size,
                          reinterpret_cast<jbyte*>(bytes.data()));
  if (env->ExceptionCheck()) {
    return nullptr;
  }
  const auto parsed = a11::UnpackMsgpack(bytes, "JVM request");
  if (!parsed.ok()) {
    return Encode(env, a11::jvm::Envelope(parsed.status()));
  }
  return Encode(env, a11::jvm::Envelope(a11::jvm::Dispatch(*parsed)));
}

namespace {
jlongArray Owned(JNIEnv* env, const nlohmann::json& event) {
  const auto& owned = event.at("owned");
  std::vector<jlong> handles;
  handles.reserve(owned.size());
  for (const auto& handle : owned) {
    handles.push_back(handle.get<std::int64_t>());
  }
  auto result = env->NewLongArray(static_cast<jsize>(handles.size()));
  if (result != nullptr) {
    env->SetLongArrayRegion(result, 0, static_cast<jsize>(handles.size()),
                            handles.data());
  }
  return result;
}
}  // namespace

extern "C" JNIEXPORT jobjectArray JNICALL
Java_a11_internal_NativeBridge_poll(JNIEnv* env, jobject) {
  using namespace a11::jvm;
  const auto events = TakeEvents();
  auto result =
      env->NewObjectArray(static_cast<jsize>(events.size()),
                          Class(env, "a11/internal/NativeEvent"), nullptr);
  for (size_t index = 0; index < events.size(); ++index) {
    const auto& event = events[index];
    auto kind = event.find("kind");
    jobject converted = nullptr;
    if (kind == event.end()) {
      auto cls = Class(env, "a11/internal/NativeEvent$Completion");
      auto status = a11::StatusFromJson(event.at("result").at("status"));
      CHECK_OK(status.status());
      jobject value =
          status->ok() ? Ok(env, JsonValue(env, event.at("result").at("value")))
                       : Status(env, *status);
      auto owned = Owned(env, event);
      converted = env->NewObject(
          cls, env->GetMethodID(cls, "<init>", "(JLa11/StatusOr;[J)V"),
          static_cast<jlong>(event.at("token").get<std::int64_t>()), value,
          owned);
      env->DeleteLocalRef(value);
      env->DeleteLocalRef(owned);
    } else if (*kind == "callback") {
      auto cls = Class(env, "a11/internal/NativeEvent$Invocation");
      auto arguments = JsonValue(env, event.at("args"));
      auto owned = Owned(env, event);
      converted = env->NewObject(
          cls, env->GetMethodID(cls, "<init>", "(JJLjava/lang/Object;[J)V"),
          static_cast<jlong>(event.at("token").get<std::int64_t>()),
          static_cast<jlong>(event.at("promise").get<std::int64_t>()),
          arguments, owned);
      if (arguments != nullptr) {
        env->DeleteLocalRef(arguments);
      }
      env->DeleteLocalRef(owned);
    } else if (*kind == "cancelCallback") {
      auto cls = Class(env, "a11/internal/NativeEvent$Cancellation");
      converted = env->NewObject(
          cls, env->GetMethodID(cls, "<init>", "(J)V"),
          static_cast<jlong>(event.at("promise").get<std::int64_t>()));
    } else {
      CHECK_EQ(*kind, "releaseCallback");
      auto cls = Class(env, "a11/internal/NativeEvent$Release");
      converted = env->NewObject(
          cls, env->GetMethodID(cls, "<init>", "(J)V"),
          static_cast<jlong>(event.at("token").get<std::int64_t>()));
    }
    if (env->ExceptionCheck()) {
      return nullptr;
    }
    env->SetObjectArrayElement(result, static_cast<jsize>(index), converted);
    env->DeleteLocalRef(converted);
  }
  return result;
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeBridge_completeCallbackNative(JNIEnv* env, jobject,
                                                      jlong promise, jint code,
                                                      jstring message,
                                                      jstring details) {
  const auto encoded = a11::jvm::Text(env, details);
  absl::Status result;
  if (code < 0 || code > 16) {
    result = absl::InvalidArgumentError("Invalid callback status code");
  } else if (encoded.empty()) {
    result = absl::Status(static_cast<absl::StatusCode>(code),
                          a11::jvm::Text(env, message));
  } else {
    auto parsed = a11::ParseJson(encoded, "callback status details");
    result = parsed.ok()
                 ? a11::MakeStatus(static_cast<absl::StatusCode>(code),
                                   a11::jvm::Text(env, message), *parsed)
                 : parsed.status();
  }
  return a11::jvm::Status(
      env, a11::jvm::CompleteCallback(promise, std::move(result)));
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeBridge_shutdown(JNIEnv*, jobject) {
  a11::jvm::Shutdown();
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeBridge_currentFiberId(JNIEnv*, jobject) {
  return static_cast<jlong>(thread::CurrentFiberId());
}
