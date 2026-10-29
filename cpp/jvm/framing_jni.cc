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

#include "a11/net/byte_chunking.h"
#include "jvm/interop.h"

using namespace a11::jvm;

namespace {
absl::StatusOr<a11::net::ByteChunkingOptions> Options(JNIEnv* env,
                                                      jobject object) {
  auto cls = Class(env, "a11/net/ByteChunkingOptions");
  a11::net::ByteChunkingOptions options;
#define A11_FRAMING_SIZE(field, name)                                       \
  {                                                                         \
    static auto id = env->GetFieldID(cls, name, "J");                       \
    auto value = static_cast<std::uint64_t>(env->GetLongField(object, id)); \
    if (value > std::numeric_limits<size_t>::max())                         \
      return absl::OutOfRangeError(                                         \
          "Byte chunking limit exceeds native size range");                 \
    options.field = static_cast<size_t>(value);                             \
  }
  A11_FRAMING_SIZE(packet_size, "packetSize")
  A11_FRAMING_SIZE(max_message_size, "maxMessageSize")
  A11_FRAMING_SIZE(max_pending_messages, "maxPendingMessages")
  A11_FRAMING_SIZE(max_pending_bytes, "maxPendingBytes")
#undef A11_FRAMING_SIZE
  ABSL_RETURN_IF_ERROR(options.Validate());
  return options;
}

a11::net::BytePacket& Packet(JNIEnv* env, jobject self) {
  return Value<a11::net::BytePacket>(Id(env, self))->get();
}

std::shared_ptr<a11::net::ByteReassembler> Assembler(JNIEnv* env,
                                                     jobject self) {
  auto value = Object<a11::net::ByteReassembler>(Id(env, self));
  CHECK_OK(value.status());
  return *value;
}

std::uint32_t Uint32(jlong value) {
  CHECK_GE(value, 0);
  CHECK_LE(value, std::numeric_limits<std::uint32_t>::max());
  return static_cast<std::uint32_t>(value);
}
}  // namespace

extern "C" JNIEXPORT jlongArray JNICALL
Java_a11_internal_NativeFraming_defaults(JNIEnv* env, jobject) {
  const a11::net::ByteChunkingOptions options;
  const jlong values[] = {static_cast<jlong>(options.packet_size),
                          static_cast<jlong>(options.max_message_size),
                          static_cast<jlong>(options.max_pending_messages),
                          static_cast<jlong>(options.max_pending_bytes)};
  auto result = env->NewLongArray(4);
  if (result != nullptr) {
    env->SetLongArrayRegion(result, 0, 4, values);
  }
  return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_a11_internal_NativeFraming_minimumPacketSize(JNIEnv*, jobject) {
  return static_cast<jint>(a11::net::kMinimumBytePacketSize);
}

extern "C" JNIEXPORT jobject JNICALL Java_a11_internal_NativeFraming_validate(
    JNIEnv* env, jobject, jobject options) {
  return Status(env, Options(env, options).status());
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeFraming_create(JNIEnv* env, jobject, jobject raw) {
  auto options = Options(env, raw);
  if (!options.ok()) {
    return Status(env, options.status());
  }
  return Ok(
      env,
      BoxLong(env,
              Retain(std::make_shared<a11::net::ByteReassembler>(*options))));
}

extern "C" JNIEXPORT jobject JNICALL Java_a11_internal_NativeFraming_split(
    JNIEnv* env, jobject, jbyteArray bytes, jlong transient, jint size) {
  if (size < 0) {
    return Status(
        env, absl::InvalidArgumentError("Packet size must not be negative"));
  }
  auto packets = a11::net::SplitOwnedBytesIntoPackets(
      Bytes(env, bytes), static_cast<std::uint64_t>(transient),
      static_cast<size_t>(size));
  if (!packets.ok()) {
    return Status(env, packets.status());
  }
  if (packets->size() >
      static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    return Status(env, absl::ResourceExhaustedError(
                           "Packet count exceeds JVM collection range"));
  }
  auto cls = Class(env, "java/util/ArrayList");
  static auto constructor = env->GetMethodID(cls, "<init>", "(I)V");
  static auto add = env->GetMethodID(cls, "add", "(Ljava/lang/Object;)Z");
  auto result =
      env->NewObject(cls, constructor, static_cast<jint>(packets->size()));
  for (const auto& packet : *packets) {
    auto converted = Bytes(env, packet);
    env->CallBooleanMethod(result, add, converted);
    env->DeleteLocalRef(converted);
  }
  return Ok(env, result);
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeFraming_parse(JNIEnv* env, jobject, jbyteArray bytes) {
  auto packet = a11::net::ParseOwnedBytePacket(Bytes(env, bytes));
  if (!packet.ok()) {
    return Status(env, packet.status());
  }
  return Ok(env,
            Wrapped(env, "a11/net/BytePacket", KeepValue(std::move(*packet))));
}

extern "C" JNIEXPORT jobject JNICALL Java_a11_internal_NativeFraming_feed(
    JNIEnv* env, jobject, jobject self, jbyteArray bytes) {
  auto result = Assembler(env, self)->Feed(Bytes(env, bytes));
  if (!result.ok()) {
    return Status(env, result.status());
  }
  if (!result->has_value()) {
    return Ok(env, nullptr);
  }
  if ((*result)->size() >
      static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    return Status(env, absl::ResourceExhaustedError(
                           "Reassembled message exceeds JVM array range"));
  }
  return Ok(env, Bytes(env, **result));
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeFraming_clear(JNIEnv* env, jobject, jobject self) {
  Assembler(env, self)->Clear();
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeFraming_pendingMessages(JNIEnv* env, jobject,
                                                jobject self) {
  return static_cast<jlong>(Assembler(env, self)->pending_message_count());
}

extern "C" JNIEXPORT jlong JNICALL Java_a11_internal_NativeFraming_pendingBytes(
    JNIEnv* env, jobject, jobject self) {
  return static_cast<jlong>(Assembler(env, self)->pending_byte_count());
}

extern "C" JNIEXPORT jlong JNICALL Java_a11_internal_NativeFraming_packetCreate(
    JNIEnv* env, jobject, jint type, jbyteArray payload, jlong transient,
    jlong sequence, jlong count) {
  CHECK_GE(type, 0);
  CHECK_LE(type, 2);
  return KeepValue(a11::net::BytePacket{
      .type = static_cast<a11::net::BytePacketType>(type),
      .payload = Bytes(env, payload),
      .transient_id = static_cast<std::uint64_t>(transient),
      .sequence = Uint32(sequence),
      .packet_count = Uint32(count)});
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeFraming_packetCopy(JNIEnv* env, jobject, jobject self) {
  return KeepValue(Packet(env, self));
}

extern "C" JNIEXPORT jint JNICALL
Java_a11_internal_NativeFraming_packetType(JNIEnv* env, jobject, jobject self) {
  return static_cast<jint>(Packet(env, self).type);
}

extern "C" JNIEXPORT void JNICALL Java_a11_internal_NativeFraming_packetTypeSet(
    JNIEnv* env, jobject, jobject self, jint value) {
  CHECK_GE(value, 0);
  CHECK_LE(value, 2);
  Packet(env, self).type = static_cast<a11::net::BytePacketType>(value);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_a11_internal_NativeFraming_packetPayload(JNIEnv* env, jobject,
                                              jobject self) {
  return Bytes(env, Packet(env, self).payload);
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeFraming_packetPayloadSet(JNIEnv* env, jobject,
                                                 jobject self,
                                                 jbyteArray value) {
  Packet(env, self).payload = Bytes(env, value);
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeFraming_packetTransientId(JNIEnv* env, jobject,
                                                  jobject self) {
  return static_cast<jlong>(Packet(env, self).transient_id);
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeFraming_packetTransientIdSet(JNIEnv* env, jobject,
                                                     jobject self,
                                                     jlong value) {
  Packet(env, self).transient_id = static_cast<std::uint64_t>(value);
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeFraming_packetSequence(JNIEnv* env, jobject,
                                               jobject self) {
  return Packet(env, self).sequence;
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeFraming_packetSequenceSet(JNIEnv* env, jobject,
                                                  jobject self, jlong value) {
  Packet(env, self).sequence = Uint32(value);
}

extern "C" JNIEXPORT jlong JNICALL Java_a11_internal_NativeFraming_packetCount(
    JNIEnv* env, jobject, jobject self) {
  return Packet(env, self).packet_count;
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeFraming_packetCountSet(JNIEnv* env, jobject,
                                               jobject self, jlong value) {
  Packet(env, self).packet_count = Uint32(value);
}
