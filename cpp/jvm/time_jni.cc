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

#include <absl/hash/hash.h>
#include <absl/time/clock.h>

#include "a11/time.h"
#include "jvm/interop.h"

using namespace a11::jvm;

namespace {
template <typename T>
T Native(JNIEnv* env, jobject value) {
  auto result = Object<T>(Id(env, value));
  CHECK_OK(result.status());
  return **result;
}

template <typename T>
jlong Keep(T value) {
  return static_cast<jlong>(Retain(std::make_shared<T>(value)));
}

jobject Components(JNIEnv* env, std::int64_t seconds, int nanos,
                   int infinity = 0) {
  auto cls = Class(env, "a11/internal/TimeComponents");
  return env->NewObject(cls, env->GetMethodID(cls, "<init>", "(JII)V"),
                        static_cast<jlong>(seconds), static_cast<jint>(nanos),
                        static_cast<jint>(infinity));
}
}  // namespace

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeTime_durationParse(JNIEnv* env, jobject, jstring text) {
  absl::Duration result;
  std::string error;
  if (!absl::ParseDuration(Text(env, text), &result)) {
    return Status(env, absl::InvalidArgumentError("Invalid duration"));
  }
  return Ok(env, Wrapped(env, "a11/Duration", Keep(result)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_a11_internal_NativeTime_durationString(JNIEnv* env, jobject,
                                            jobject self) {
  return Text(env, absl::FormatDuration(Native<absl::Duration>(env, self)));
}

extern "C" JNIEXPORT jint JNICALL
Java_a11_internal_NativeTime_durationHash(JNIEnv* env, jobject, jobject self) {
  return static_cast<jint>(
      absl::Hash<absl::Duration>{}(Native<absl::Duration>(env, self)));
}

extern "C" JNIEXPORT jint JNICALL Java_a11_internal_NativeTime_durationCompare(
    JNIEnv* env, jobject, jobject self, jobject other) {
  auto left = Native<absl::Duration>(env, self);
  auto right = Native<absl::Duration>(env, other);
  return left < right ? -1 : (left > right ? 1 : 0);
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeTime_durationNanosecondCount(JNIEnv* env, jobject,
                                                     jobject self) {
  auto result = a11::DurationNanoseconds(Native<absl::Duration>(env, self));
  if (!result.ok()) {
    return Status(env, result.status());
  }
  return Ok(env, BoxLong(env, *result));
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeTime_durationParts(JNIEnv* env, jobject, jobject self) {
  const auto value = Native<absl::Duration>(env, self);
  if (value == absl::InfiniteDuration()) {
    return Ok(env, Components(env, 0, 0, 1));
  }
  if (value == -absl::InfiniteDuration()) {
    return Ok(env, Components(env, 0, 0, -1));
  }
  absl::Duration remainder;
  const auto seconds = absl::IDivDuration(value, absl::Seconds(1), &remainder);
  auto nanos = a11::DurationNanoseconds(remainder);
  if (!nanos.ok()) {
    return Status(env, nanos.status());
  }
  return Ok(env, Components(env, seconds, static_cast<int>(*nanos)));
}

extern "C" JNIEXPORT jlong JNICALL Java_a11_internal_NativeTime_durationAdd(
    JNIEnv* env, jobject, jobject self, jobject other) {
  return Keep(Native<absl::Duration>(env, self) +
              Native<absl::Duration>(env, other));
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeTime_durationSubtract(JNIEnv* env, jobject,
                                              jobject self, jobject other) {
  return Keep(Native<absl::Duration>(env, self) -
              Native<absl::Duration>(env, other));
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeTime_timeParse(JNIEnv* env, jobject, jstring text) {
  absl::Time result;
  std::string error;
  if (!absl::ParseTime(absl::RFC3339_full, Text(env, text), &result, &error)) {
    return Status(env, absl::InvalidArgumentError(error));
  }
  return Ok(env, Wrapped(env, "a11/Time", Keep(result)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_a11_internal_NativeTime_timeString(JNIEnv* env, jobject, jobject self) {
  return Text(
      env, absl::FormatTime(absl::RFC3339_full, Native<absl::Time>(env, self),
                            absl::UTCTimeZone()));
}

extern "C" JNIEXPORT jint JNICALL
Java_a11_internal_NativeTime_timeHash(JNIEnv* env, jobject, jobject self) {
  return static_cast<jint>(
      absl::Hash<absl::Time>{}(Native<absl::Time>(env, self)));
}

extern "C" JNIEXPORT jint JNICALL Java_a11_internal_NativeTime_timeCompare(
    JNIEnv* env, jobject, jobject self, jobject other) {
  auto left = Native<absl::Time>(env, self);
  auto right = Native<absl::Time>(env, other);
  return left < right ? -1 : (left > right ? 1 : 0);
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeTime_timeNanoseconds(JNIEnv* env, jobject,
                                             jobject self) {
  auto result = a11::TimeNanosecondsSinceEpoch(Native<absl::Time>(env, self));
  if (!result.ok()) {
    return Status(env, result.status());
  }
  return Ok(env, BoxLong(env, *result));
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeTime_timeParts(JNIEnv* env, jobject, jobject self) {
  const auto value = Native<absl::Time>(env, self);
  if (value == absl::InfiniteFuture() || value == absl::InfinitePast()) {
    return Status(
        env, absl::OutOfRangeError("An infinite instant cannot be converted"));
  }
  const auto seconds = absl::ToUnixSeconds(value);
  auto nanos = a11::DurationNanoseconds(value - absl::FromUnixSeconds(seconds));
  if (!nanos.ok()) {
    return Status(env, nanos.status());
  }
  return Ok(env, Components(env, seconds, static_cast<int>(*nanos)));
}

extern "C" JNIEXPORT jlong JNICALL Java_a11_internal_NativeTime_timeAdd(
    JNIEnv* env, jobject, jobject self, jobject other) {
  return Keep(Native<absl::Time>(env, self) +
              Native<absl::Duration>(env, other));
}

extern "C" JNIEXPORT jlong JNICALL Java_a11_internal_NativeTime_timeSubtract(
    JNIEnv* env, jobject, jobject self, jobject other) {
  return Keep(Native<absl::Time>(env, self) -
              Native<absl::Duration>(env, other));
}

extern "C" JNIEXPORT jobject JNICALL Java_a11_internal_NativeTime_durationValue(
    JNIEnv* env, jobject, jlong seconds, jint nanoseconds, jint infinity) {
  if (infinity < -1 || infinity > 1) {
    return Status(env, absl::InvalidArgumentError("Invalid time infinity"));
  }
  if (nanoseconds < -999999999 || nanoseconds > 999999999) {
    return Status(env,
                  absl::InvalidArgumentError("Nanoseconds exceed a second"));
  }
  auto value = infinity == 1
                   ? absl::InfiniteDuration()
                   : (infinity == -1 ? -absl::InfiniteDuration()
                                     : absl::Seconds(seconds) +
                                           absl::Nanoseconds(nanoseconds));
  return Ok(env, Wrapped(env, "a11/Duration", Keep(value)));
}

extern "C" JNIEXPORT jobject JNICALL Java_a11_internal_NativeTime_timeValue(
    JNIEnv* env, jobject, jlong seconds, jint nanoseconds, jint infinity) {
  if (infinity < -1 || infinity > 1) {
    return Status(env, absl::InvalidArgumentError("Invalid time infinity"));
  }
  if (nanoseconds < 0 || nanoseconds > 999999999) {
    return Status(env,
                  absl::InvalidArgumentError("Nanoseconds exceed a second"));
  }
  auto value = infinity == 1
                   ? absl::InfiniteFuture()
                   : (infinity == -1 ? absl::InfinitePast()
                                     : absl::FromUnixSeconds(seconds) +
                                           absl::Nanoseconds(nanoseconds));
  return Ok(env, Wrapped(env, "a11/Time", Keep(value)));
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeTime_durationNanoseconds(JNIEnv*, jobject,
                                                 jlong count) {
  return Keep(absl::Nanoseconds(count));
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeTime_durationMicroseconds(JNIEnv*, jobject,
                                                  jlong count) {
  return Keep(absl::Microseconds(count));
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeTime_durationMilliseconds(JNIEnv*, jobject,
                                                  jlong count) {
  return Keep(absl::Milliseconds(count));
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeTime_durationSeconds(JNIEnv*, jobject, jlong count) {
  return Keep(absl::Seconds(count));
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeTime_durationMinutes(JNIEnv*, jobject, jlong count) {
  return Keep(absl::Minutes(count));
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeTime_durationHours(JNIEnv*, jobject, jlong count) {
  return Keep(absl::Hours(count));
}

extern "C" JNIEXPORT jlong JNICALL Java_a11_internal_NativeTime_durationNegate(
    JNIEnv* env, jobject, jobject self) {
  return Keep(-Native<absl::Duration>(env, self));
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeTime_durationMultiply(JNIEnv* env, jobject,
                                              jobject self, jlong factor) {
  return Keep(Native<absl::Duration>(env, self) * factor);
}

extern "C" JNIEXPORT jlong JNICALL Java_a11_internal_NativeTime_durationDivide(
    JNIEnv* env, jobject, jobject self, jlong factor) {
  return Keep(Native<absl::Duration>(env, self) / factor);
}

extern "C" JNIEXPORT jlong JNICALL Java_a11_internal_NativeTime_timeDifference(
    JNIEnv* env, jobject, jobject self, jobject other) {
  return Keep(Native<absl::Time>(env, self) - Native<absl::Time>(env, other));
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeTime_timeNow(JNIEnv*, jobject) {
  return Keep(absl::Now());
}
