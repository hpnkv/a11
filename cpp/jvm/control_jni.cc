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

#include "a11/data/msgpack.h"
#include "a11/stores/chunk_store.h"
#include "a11/stores/chunk_store_reader.h"
#include "jvm/interop.h"

using namespace a11::jvm;

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeControl_validateName(JNIEnv* env, jobject,
                                             jstring value) {
  return Status(env, a11::data::ValidateName(Text(env, value)));
}

extern "C" JNIEXPORT jint JNICALL
Java_a11_internal_NativeControl_statusCodeFromHttp(JNIEnv*, jobject,
                                                   jint value) {
  return static_cast<jint>(a11::StatusCodeFromHttp(value));
}

extern "C" JNIEXPORT jint JNICALL
Java_a11_internal_NativeControl_statusCodeToHttp(JNIEnv*, jobject, jint value) {
  return a11::StatusCodeToHttp(static_cast<absl::StatusCode>(value));
}

extern "C" JNIEXPORT jint JNICALL
Java_a11_internal_NativeControl_statusCodeFromWebSocket(JNIEnv*, jobject,
                                                        jint value) {
  if (value < 0 || value > 65535) {
    return static_cast<jint>(absl::StatusCode::kUnknown);
  }
  return static_cast<jint>(
      a11::StatusCodeFromWebSocket(static_cast<std::uint16_t>(value)));
}

extern "C" JNIEXPORT jint JNICALL
Java_a11_internal_NativeControl_statusCodeToWebSocket(JNIEnv*, jobject,
                                                      jint value) {
  return a11::StatusCodeToWebSocket(static_cast<absl::StatusCode>(value));
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeControl_release(JNIEnv*, jobject, jlong id) {
  Release(id);
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeControl_futureIsReady(JNIEnv* env, jobject,
                                              jobject value) {
  auto future = Object<PendingOperation>(Id(env, value));
  if (!future.ok()) {
    return Status(env, future.status());
  }
  auto cls = Class(env, "java/lang/Boolean");
  auto result = env->CallStaticObjectMethod(
      cls, env->GetStaticMethodID(cls, "valueOf", "(Z)Ljava/lang/Boolean;"),
      static_cast<jboolean>((*future)->IsReady()));
  return Ok(env, result);
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeControl_futureCancel(JNIEnv* env, jobject, jlong id) {
  auto future = Object<PendingOperation>(id);
  return Status(env, future.ok() ? (*future)->Cancel() : future.status());
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeControl_storeHandleCount(JNIEnv*, jobject) {
  return static_cast<jlong>(
      HandleCount(typeid(std::shared_ptr<a11::stores::ChunkStore>)));
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeControl_readerHandleCount(JNIEnv*, jobject) {
  return static_cast<jlong>(
      HandleCount(typeid(std::shared_ptr<a11::stores::ChunkStoreReader>)));
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeControl_decodeStatus(JNIEnv* env, jobject,
                                             jbyteArray bytes) {
  auto decoded = a11::data::UnpackStatus(Bytes(env, bytes));
  if (!decoded.ok()) {
    return Status(env, decoded.status());
  }
  auto status = Status(env, *decoded);
  if (env->ExceptionCheck()) {
    return nullptr;
  }
  auto cls = Class(env, "a11/DecodedStatus");
  auto boxed = env->NewObject(
      cls, env->GetMethodID(cls, "<init>", "(La11/Status;)V"), status);
  return Ok(env, boxed);
}
