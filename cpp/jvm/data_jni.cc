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

#include "a11/data/types.h"
#include "a11/service/session.h"
#include "a11/stores/chunk_store.h"
#include "a11/time.h"
#include "jvm/interop.h"
#include "jvm/net.h"

using namespace a11::jvm;

namespace {
template <typename T>
auto Ref(JNIEnv* env, jobject object) {
  return Value<T>(Id(env, object));
}

template <typename T>
T& Get(JNIEnv* env, jobject object) {
  return Ref<T>(env, object)->get();
}

std::uint32_t Uint32(jlong value) {
  CHECK_GE(value, 0);
  CHECK_LE(value, std::numeric_limits<std::uint32_t>::max());
  return static_cast<std::uint32_t>(value);
}

jlong UnboxLong(JNIEnv* env, jobject value) {
  static auto method =
      env->GetMethodID(Class(env, "java/lang/Long"), "longValue", "()J");
  return env->CallLongMethod(value, method);
}
}  // namespace

extern "C" JNIEXPORT jbyteArray JNICALL
Java_a11_internal_NativeData_chunkData(JNIEnv* env, jobject, jobject self) {
  return Bytes(env, Get<a11::data::Chunk>(env, self).data);
}

extern "C" JNIEXPORT void JNICALL Java_a11_internal_NativeData_chunkDataSet(
    JNIEnv* env, jobject, jobject self, jbyteArray value) {
  Get<a11::data::Chunk>(env, self).data = Bytes(env, value);
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeData_nodeRefOffset(JNIEnv* env, jobject, jobject self) {
  return Get<a11::data::NodeRef>(env, self).offset;
}

extern "C" JNIEXPORT void JNICALL Java_a11_internal_NativeData_nodeRefOffsetSet(
    JNIEnv* env, jobject, jobject self, jlong value) {
  Get<a11::data::NodeRef>(env, self).offset = Uint32(value);
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeData_nodeRefLength(JNIEnv* env, jobject, jobject self) {
  return Get<a11::data::NodeRef>(env, self).length.has_value()
             ? BoxLong(env, static_cast<jlong>(
                                *Get<a11::data::NodeRef>(env, self).length))
             : nullptr;
}

extern "C" JNIEXPORT void JNICALL Java_a11_internal_NativeData_nodeRefLengthSet(
    JNIEnv* env, jobject, jobject self, jobject value) {
  if (value == nullptr) {
    Get<a11::data::NodeRef>(env, self).length.reset();
  } else {
    Get<a11::data::NodeRef>(env, self).length =
        static_cast<std::uint64_t>(UnboxLong(env, value));
  }
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeData_nodeFragmentSeq(JNIEnv* env, jobject,
                                             jobject self) {
  return Get<a11::data::NodeFragment>(env, self).seq.has_value()
             ? BoxLong(env, static_cast<jlong>(
                                *Get<a11::data::NodeFragment>(env, self).seq))
             : nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeData_nodeFragmentSeqSet(JNIEnv* env, jobject,
                                                jobject self, jobject value) {
  if (value == nullptr) {
    Get<a11::data::NodeFragment>(env, self).seq.reset();
  } else {
    CHECK_GE(UnboxLong(env, value), 0);
    Get<a11::data::NodeFragment>(env, self).seq = Uint32(UnboxLong(env, value));
  }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_a11_internal_NativeData_nodeFragmentContinued(JNIEnv* env, jobject,
                                                   jobject self) {
  return Get<a11::data::NodeFragment>(env, self).continued;
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeData_nodeFragmentContinuedSet(JNIEnv* env, jobject,
                                                      jobject self,
                                                      jboolean value) {
  Get<a11::data::NodeFragment>(env, self).continued = value;
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeData_metadataTimestamp(JNIEnv* env, jobject,
                                               jobject self) {
  auto& value = Get<a11::data::ChunkMetadata>(env, self).timestamp;
  return value.has_value()
             ? Wrapped(env, "a11/Time",
                       Retain(std::make_shared<absl::Time>(*value)))
             : nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeData_metadataTimestampSet(JNIEnv* env, jobject,
                                                  jobject self, jobject value) {
  auto& target = Get<a11::data::ChunkMetadata>(env, self).timestamp;
  if (value == nullptr) {
    target.reset();
  } else {
    auto result = Object<absl::Time>(Id(env, value));
    CHECK_OK(result.status());
    target = **result;
  }
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeData_metadataMicros(JNIEnv* env, jobject,
                                            jobject self) {
  auto& value = Get<a11::data::ChunkMetadata>(env, self).timestamp;
  return value.has_value() ? BoxLong(env, absl::ToUnixMicros(*value)) : nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeData_metadataMicrosSet(JNIEnv* env, jobject,
                                               jobject self, jobject value) {
  auto& target = Get<a11::data::ChunkMetadata>(env, self).timestamp;
  if (value == nullptr) {
    target.reset();
  } else {
    target = absl::FromUnixMicros(UnboxLong(env, value));
  }
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeData_chunkMetadata(JNIEnv* env, jobject, jobject self) {
  auto parent = Ref<a11::data::Chunk>(env, self);
  if (!parent->get().metadata.has_value()) {
    return nullptr;
  }
  return Wrapped(env, "a11/ChunkMetadata",
                 KeepView<a11::data::ChunkMetadata>(
                     parent, ValueMember::kMetadata, [](auto& value) -> auto& {
                       CHECK(value.metadata.has_value())
                           << "Chunk metadata was removed";
                       return *value.metadata;
                     }));
}

extern "C" JNIEXPORT void JNICALL Java_a11_internal_NativeData_chunkMetadataSet(
    JNIEnv* env, jobject, jobject self, jobject value) {
  auto& target = Get<a11::data::Chunk>(env, self).metadata;
  if (value == nullptr) {
    Ref<a11::data::Chunk>(env, self)->Invalidate(ValueMember::kMetadata);
    target.reset();
  } else {
    auto copied = Get<a11::data::ChunkMetadata>(env, value);
    Ref<a11::data::Chunk>(env, self)->Invalidate(ValueMember::kMetadata);
    target = std::move(copied);
  }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_a11_internal_NativeData_chunkIsEmpty(JNIEnv* env, jobject, jobject self) {
  return Get<a11::data::Chunk>(env, self).IsEmpty();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_a11_internal_NativeData_chunkIsNull(JNIEnv* env, jobject, jobject self) {
  return Get<a11::data::Chunk>(env, self).IsNull();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_a11_internal_NativeData_fragmentIsChunk(JNIEnv* env, jobject,
                                             jobject self) {
  return std::holds_alternative<a11::data::Chunk>(
      Get<a11::data::NodeFragment>(env, self).data);
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeData_fragmentChunk(JNIEnv* env, jobject, jobject self) {
  auto parent = Ref<a11::data::NodeFragment>(env, self);
  auto result = parent->get().GetChunk();
  if (!result.ok()) {
    return Status(env, result.status());
  }
  return Ok(
      env, Wrapped(env, "a11/Chunk",
                   KeepView<a11::data::Chunk>(
                       parent, ValueMember::kPayload, [](auto& value) -> auto& {
                         auto field = value.GetChunk();
                         CHECK_OK(field.status())
                             << "Fragment payload variant changed";
                         return **field;
                       })));
}

extern "C" JNIEXPORT void JNICALL Java_a11_internal_NativeData_fragmentChunkSet(
    JNIEnv* env, jobject, jobject self, jobject value) {
  auto copied = Get<a11::data::Chunk>(env, value);
  auto parent = Ref<a11::data::NodeFragment>(env, self);
  parent->Invalidate(ValueMember::kPayload);
  parent->get().data = std::move(copied);
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeData_fragmentNodeRef(JNIEnv* env, jobject,
                                             jobject self) {
  auto parent = Ref<a11::data::NodeFragment>(env, self);
  auto result = parent->get().GetNodeRef();
  if (!result.ok()) {
    return Status(env, result.status());
  }
  return Ok(
      env, Wrapped(env, "a11/NodeRef",
                   KeepView<a11::data::NodeRef>(
                       parent, ValueMember::kPayload, [](auto& value) -> auto& {
                         auto field = value.GetNodeRef();
                         CHECK_OK(field.status())
                             << "Fragment payload variant changed";
                         return **field;
                       })));
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeData_fragmentNodeRefSet(JNIEnv* env, jobject,
                                                jobject self, jobject value) {
  auto copied = Get<a11::data::NodeRef>(env, value);
  auto parent = Ref<a11::data::NodeFragment>(env, self);
  parent->Invalidate(ValueMember::kPayload);
  parent->get().data = std::move(copied);
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeData_chunkMetadataMap(JNIEnv* env, jobject,
                                              jobject self) {
  return KeepView<a11::data::ByteMap>(
      Ref<a11::data::ChunkMetadata>(env, self), ValueMember::kAttributes,
      [](auto& value) -> auto& { return value.attributes; });
}

extern "C" JNIEXPORT jlong JNICALL
Java_a11_internal_NativeData_actionMessageMap(JNIEnv* env, jobject,
                                              jobject self) {
  return KeepView<a11::data::ByteMap>(
      Ref<a11::data::ActionMessage>(env, self), ValueMember::kHeaders,
      [](auto& value) -> auto& { return value.headers; });
}

extern "C" JNIEXPORT jlong JNICALL Java_a11_internal_NativeData_wireMessageMap(
    JNIEnv* env, jobject, jobject self) {
  return KeepView<a11::data::ByteMap>(
      Ref<a11::data::WireMessage>(env, self), ValueMember::kHeaders,
      [](auto& value) -> auto& { return value.headers; });
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_a11_internal_NativeData_mapKeys(JNIEnv* env, jobject, jobject id) {
  auto& value = Value<a11::data::ByteMap>(Id(env, id))->get();
  auto result = env->NewObjectArray(static_cast<jsize>(value.size()),
                                    Class(env, "java/lang/String"), nullptr);
  jsize index = 0;
  for (const auto& [key, unused] : value) {
    auto name = Text(env, key);
    env->SetObjectArrayElement(result, index++, name);
    env->DeleteLocalRef(name);
  }
  return result;
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_a11_internal_NativeData_mapGet(
    JNIEnv* env, jobject, jobject id, jstring key) {
  auto& value = Value<a11::data::ByteMap>(Id(env, id))->get();
  auto found = value.find(Text(env, key));
  return found == value.end() ? nullptr : Bytes(env, found->second);
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_a11_internal_NativeData_mapPut(
    JNIEnv* env, jobject, jobject id, jstring key, jbyteArray bytes) {
  auto& value = Value<a11::data::ByteMap>(Id(env, id))->get();
  auto name = Text(env, key);
  auto found = value.find(name);
  auto previous = found == value.end() ? nullptr : Bytes(env, found->second);
  if (bytes == nullptr) {
    value.erase(name);
  } else {
    value[std::move(name)] = Bytes(env, bytes);
  }
  return previous;
}

extern "C" JNIEXPORT jobject JNICALL Java_a11_internal_NativeData_streamSend(
    JNIEnv* env, jobject, jobject self, jobject message) {
  auto stream = Object<a11::jvm::BoundStream>(Id(env, self));
  if (!stream.ok()) {
    return Status(env, stream.status());
  }
  return Status(
      env, (*stream)->stream->Send(Get<a11::data::WireMessage>(env, message)));
}

extern "C" JNIEXPORT jobject JNICALL Java_a11_internal_NativeData_sessionSend(
    JNIEnv* env, jobject, jobject self, jobject message) {
  auto session = Object<a11::service::SessionWithRecv>(Id(env, self));
  if (!session.ok()) {
    return Status(env, session.status());
  }
  return Status(env,
                (*session)->Send(Get<a11::data::WireMessage>(env, message)));
}

extern "C" JNIEXPORT jobject JNICALL Java_a11_internal_NativeData_storePut(
    JNIEnv* env, jobject, jobject self, jobject fragment) {
  auto store = Object<a11::stores::ChunkStore>(Id(env, self));
  if (!store.ok()) {
    return Status(env, store.status());
  }
  auto pending = std::shared_ptr<a11::jvm::PendingOperation>(
      std::make_shared<a11::jvm::PendingFuture<std::uint32_t>>(
          (*store)->Put(Get<a11::data::NodeFragment>(env, fragment))));
  return Ok(env, BoxLong(env, Retain(std::move(pending))));
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeData_metadataSetAttribute(JNIEnv* env, jobject,
                                                  jobject self, jstring key,
                                                  jbyteArray value) {
  return Status(env, Get<a11::data::ChunkMetadata>(env, self).SetAttribute(
                         Text(env, key), Bytes(env, value)));
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeData_metadataGetAttribute(JNIEnv* env, jobject,
                                                  jobject self, jstring key) {
  auto result =
      Get<a11::data::ChunkMetadata>(env, self).GetAttribute(Text(env, key));
  if (!result.ok()) {
    return Status(env, result.status());
  }
  return Ok(env, Bytes(env, *result));
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeData_chunkMaterialize(JNIEnv* env, jobject,
                                              jobject self) {
  return Status(env, Get<a11::data::Chunk>(env, self).Materialize());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_a11_internal_NativeData_chunkHasObject(JNIEnv* env, jobject,
                                            jobject self) {
  return Get<a11::data::Chunk>(env, self).HasObject();
}

namespace {
template <typename T>
void InvalidateChildren(const std::shared_ptr<ValueReference<T>>& value) {
  if constexpr (std::is_same_v<T, a11::data::Chunk>) {
    value->Invalidate(ValueMember::kMetadata);
  } else if constexpr (std::is_same_v<T, a11::data::NodeFragment>) {
    value->Invalidate(ValueMember::kPayload);
  } else if constexpr (std::is_same_v<T, a11::data::ActionMessage>) {
    value->Invalidate(ValueMember::kInputs);
    value->Invalidate(ValueMember::kOutputs);
  } else if constexpr (std::is_same_v<T, a11::data::WireMessage>) {
    value->Invalidate(ValueMember::kFragments);
    value->Invalidate(ValueMember::kActions);
  }
}

template <typename T>
void Assign(JNIEnv* env, jobject self, jobject other, bool move) {
  auto target = Ref<T>(env, self);
  auto source = Ref<T>(env, other);
  if (&target->get() == &source->get()) {
    return;
  }
  if (move) {
    target->get() = std::move(source->get());
  } else {
    target->get() = source->get();
  }
  InvalidateChildren(target);
  if (move) {
    InvalidateChildren(source);
  }
}

template <typename T>
void Swap(JNIEnv* env, jobject self, jobject other) {
  auto target = Ref<T>(env, self);
  auto source = Ref<T>(env, other);
  if (&target->get() == &source->get()) {
    return;
  }
  std::swap(target->get(), source->get());
  InvalidateChildren(target);
  InvalidateChildren(source);
}
}  // namespace

extern "C" JNIEXPORT jint JNICALL
Java_a11_internal_NativeData_mapSize(JNIEnv* env, jobject, jobject self) {
  return static_cast<jint>(
      Value<a11::data::ByteMap>(Id(env, self))->get().size());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_a11_internal_NativeData_mapContainsKey(JNIEnv* env, jobject, jobject self,
                                            jstring key) {
  auto& map = Value<a11::data::ByteMap>(Id(env, self))->get();
  return map.find(Text(env, key)) != map.end();
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeData_mapClear(JNIEnv* env, jobject, jobject self) {
  Value<a11::data::ByteMap>(Id(env, self))->get().clear();
}

extern "C" JNIEXPORT void JNICALL Java_a11_internal_NativeData_mapErase(
    JNIEnv* env, jobject, jobject self, jstring key) {
  Value<a11::data::ByteMap>(Id(env, self))->get().erase(Text(env, key));
}

namespace {
size_t Hash(const a11::data::ChunkMetadata& value) {
  return absl::HashOf(value.mimetype, value.timestamp, value.attributes);
}

size_t Hash(const a11::data::Chunk& value) {
  return absl::HashOf(value.metadata.has_value(),
                      value.metadata ? Hash(*value.metadata) : 0, value.ref,
                      value.data, value.object.get());
}

size_t Hash(const a11::data::NodeRef& value) {
  return absl::HashOf(value.id, value.offset, value.length);
}

size_t Hash(const a11::data::NodeFragment& value) {
  return absl::HashOf(
      value.id, value.seq, value.continued, value.data.index(),
      std::visit([](const auto& payload) { return Hash(payload); },
                 value.data));
}

size_t Hash(const a11::data::Port& value) {
  return absl::HashOf(value.name, value.id);
}

size_t Hash(const a11::data::ActionMessage& value) {
  auto hash = absl::HashOf(value.id, value.name, value.headers);
  for (const auto& port : value.inputs) {
    hash = absl::HashOf(hash, Hash(port));
  }
  hash = absl::HashOf(hash, value.inputs.size());
  for (const auto& port : value.outputs) {
    hash = absl::HashOf(hash, Hash(port));
  }
  return hash;
}

size_t Hash(const a11::data::WireMessage& value) {
  auto hash = absl::HashOf(value.headers);
  for (const auto& fragment : value.node_fragments) {
    hash = absl::HashOf(hash, Hash(fragment));
  }
  hash = absl::HashOf(hash, value.node_fragments.size());
  for (const auto& action : value.actions) {
    hash = absl::HashOf(hash, Hash(action));
  }
  return hash;
}
}  // namespace

namespace {
template <typename Parent, typename Element>
void ReplaceVector(JNIEnv* env, jobject self, jobjectArray values,
                   std::vector<Element> Parent::* field, ValueMember member) {
  const auto size = env->GetArrayLength(values);
  std::vector<Element> replacement;
  replacement.reserve(static_cast<size_t>(size));
  for (jsize index = 0; index < size; ++index) {
    auto value = env->GetObjectArrayElement(values, index);
    replacement.push_back(Get<Element>(env, value));
    env->DeleteLocalRef(value);
  }
  auto parent = Ref<Parent>(env, self);
  parent->Invalidate(member);
  parent->get().*field = std::move(replacement);
}

template <typename Parent, typename Element>
void ClearVector(JNIEnv* env, jobject self,
                 std::vector<Element> Parent::* field, ValueMember member) {
  auto parent = Ref<Parent>(env, self);
  parent->Invalidate(member);
  (parent->get().*field).clear();
}
}  // namespace

namespace {
a11::data::ByteMap ReadByteMap(JNIEnv* env, jobjectArray keys,
                               jobjectArray values) {
  const auto size = env->GetArrayLength(keys);
  CHECK_EQ(size, env->GetArrayLength(values));
  a11::data::ByteMap result;
  result.reserve(static_cast<size_t>(size));
  for (jsize index = 0; index < size; ++index) {
    // JNI erases the component type of these typed JVM arrays.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast)
    auto key = static_cast<jstring>(env->GetObjectArrayElement(keys, index));
    auto value =
        static_cast<jbyteArray>(env->GetObjectArrayElement(values, index));
    // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
    result.emplace(Text(env, key), Bytes(env, value));
    env->DeleteLocalRef(key);
    env->DeleteLocalRef(value);
  }
  return result;
}

void MergeByteMap(a11::data::ByteMap& target,
                  const a11::data::ByteMap& source) {
  if (&target == &source) {
    return;
  }
  for (const auto& [key, bytes] : source) {
    target.insert_or_assign(key, bytes);
  }
}
}  // namespace

extern "C" JNIEXPORT void JNICALL Java_a11_internal_NativeData_mapAssignNative(
    JNIEnv* env, jobject, jobject self, jobject other) {
  auto& target = Value<a11::data::ByteMap>(Id(env, self))->get();
  auto& source = Value<a11::data::ByteMap>(Id(env, other))->get();
  if (&target != &source) {
    target = source;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeData_mapAssign(JNIEnv* env, jobject, jobject self,
                                       jobjectArray keys, jobjectArray values) {
  auto replacement = ReadByteMap(env, keys, values);
  Value<a11::data::ByteMap>(Id(env, self))->get() = std::move(replacement);
}

extern "C" JNIEXPORT void JNICALL Java_a11_internal_NativeData_mapPutAllNative(
    JNIEnv* env, jobject, jobject self, jobject other) {
  MergeByteMap(Value<a11::data::ByteMap>(Id(env, self))->get(),
               Value<a11::data::ByteMap>(Id(env, other))->get());
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeData_mapPutAll(JNIEnv* env, jobject, jobject self,
                                       jobjectArray keys, jobjectArray values) {
  auto values_to_add = ReadByteMap(env, keys, values);
  auto& target = Value<a11::data::ByteMap>(Id(env, self))->get();
  for (auto& [key, bytes] : values_to_add) {
    target.insert_or_assign(key, std::move(bytes));
  }
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeData_metadataMillis(JNIEnv* env, jobject,
                                            jobject self) {
  auto& value = Get<a11::data::ChunkMetadata>(env, self).timestamp;
  return value.has_value() ? BoxLong(env, absl::ToUnixMillis(*value)) : nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_a11_internal_NativeData_metadataMillisSet(JNIEnv* env, jobject,
                                               jobject self, jobject value) {
  auto& target = Get<a11::data::ChunkMetadata>(env, self).timestamp;
  if (value == nullptr) {
    target.reset();
  } else {
    target = absl::FromUnixMillis(UnboxLong(env, value));
  }
}

namespace {
template <typename T>
jobject EncodeRecord(JNIEnv* env, jobject self) {
  auto result = Get<T>(env, self).ToMsgpack();
  if (!result.ok()) {
    return Status(env, result.status());
  }
  return Ok(env, Bytes(env, *result));
}

template <typename T>
jobject DecodeRecord(JNIEnv* env, jbyteArray bytes, const char* class_name) {
  auto result = T::FromMsgpack(Bytes(env, bytes));
  if (!result.ok()) {
    return Status(env, result.status());
  }
  return Ok(env, Wrapped(env, class_name, KeepValue(std::move(*result))));
}

template <typename Parent, typename Element>
jobject VectorGet(JNIEnv* env, jobject self, jint index,
                  std::vector<Element> Parent::* field, ValueMember member,
                  const char* class_name) {
  auto parent = Ref<Parent>(env, self);
  CHECK_GE(index, 0);
  CHECK_LT(static_cast<size_t>(index), (parent->get().*field).size());
  return Wrapped(env, class_name,
                 KeepView<Element>(
                     parent, member,
                     [field, index](Parent& value) -> Element& {
                       CHECK_LT(static_cast<size_t>(index),
                                (value.*field).size());
                       return (value.*field)[static_cast<size_t>(index)];
                     },
                     static_cast<size_t>(index)));
}

template <typename Parent, typename Element>
jobject VectorSet(JNIEnv* env, jobject self, jint index, jobject value,
                  std::vector<Element> Parent::* field, ValueMember member,
                  const char* class_name) {
  auto parent = Ref<Parent>(env, self);
  auto& target = parent->get().*field;
  CHECK_GE(index, 0);
  CHECK_LT(static_cast<size_t>(index), target.size());
  auto copied = Get<Element>(env, value);
  auto previous = std::move(target[static_cast<size_t>(index)]);
  parent->Invalidate(member);
  target[static_cast<size_t>(index)] = std::move(copied);
  return Wrapped(env, class_name, KeepValue(std::move(previous)));
}

template <typename Parent, typename Element>
void VectorAdd(JNIEnv* env, jobject self, jint index, jobject value,
               std::vector<Element> Parent::* field, ValueMember member) {
  auto parent = Ref<Parent>(env, self);
  auto& target = parent->get().*field;
  CHECK_GE(index, 0);
  CHECK_LE(static_cast<size_t>(index), target.size());
  auto copied = Get<Element>(env, value);
  parent->Invalidate(member);
  target.insert(target.begin() + index, std::move(copied));
}

template <typename Parent, typename Element>
jobject VectorRemove(JNIEnv* env, jobject self, jint index,
                     std::vector<Element> Parent::* field, ValueMember member,
                     const char* class_name) {
  auto parent = Ref<Parent>(env, self);
  auto& target = parent->get().*field;
  CHECK_GE(index, 0);
  CHECK_LT(static_cast<size_t>(index), target.size());
  auto previous = std::move(target[static_cast<size_t>(index)]);
  parent->Invalidate(member);
  target.erase(target.begin() + index);
  return Wrapped(env, class_name, KeepValue(std::move(previous)));
}
}  // namespace

#define A11_JVM_VALUE(Type, name)                                              \
  extern "C" JNIEXPORT jlong JNICALL Java_a11_internal_NativeData_##name##New( \
      JNIEnv*, jobject) {                                                      \
    return KeepValue(a11::data::Type{});                                       \
  }                                                                            \
  extern "C" JNIEXPORT jlong JNICALL                                           \
  Java_a11_internal_NativeData_##name##Copy(JNIEnv* env, jobject,              \
                                            jobject self) {                    \
    return KeepValue(Get<a11::data::Type>(env, self));                         \
  }                                                                            \
  extern "C" JNIEXPORT jobject JNICALL                                         \
  Java_a11_internal_NativeData_##name##Validate(JNIEnv* env, jobject,          \
                                                jobject self) {                \
    return Status(env, Get<a11::data::Type>(env, self).Validate());            \
  }                                                                            \
  extern "C" JNIEXPORT jobject JNICALL                                         \
  Java_a11_internal_NativeData_##name##Encode(JNIEnv* env, jobject,            \
                                              jobject self) {                  \
    return EncodeRecord<a11::data::Type>(env, self);                           \
  }                                                                            \
  extern "C" JNIEXPORT jobject JNICALL                                         \
  Java_a11_internal_NativeData_##name##Decode(JNIEnv* env, jobject,            \
                                              jbyteArray bytes) {              \
    return DecodeRecord<a11::data::Type>(env, bytes, "a11/" #Type);            \
  }                                                                            \
  extern "C" JNIEXPORT void JNICALL                                            \
  Java_a11_internal_NativeData_##name##CopyFrom(JNIEnv* env, jobject,          \
                                                jobject self, jobject other) { \
    Assign<a11::data::Type>(env, self, other, false);                          \
  }                                                                            \
  extern "C" JNIEXPORT void JNICALL                                            \
  Java_a11_internal_NativeData_##name##MoveFrom(JNIEnv* env, jobject,          \
                                                jobject self, jobject other) { \
    Assign<a11::data::Type>(env, self, other, true);                           \
  }                                                                            \
  extern "C" JNIEXPORT void JNICALL Java_a11_internal_NativeData_##name##Swap( \
      JNIEnv* env, jobject, jobject self, jobject other) {                     \
    Swap<a11::data::Type>(env, self, other);                                   \
  }                                                                            \
  extern "C" JNIEXPORT jboolean JNICALL                                        \
  Java_a11_internal_NativeData_##name##Equals(JNIEnv* env, jobject,            \
                                              jobject self, jobject other) {   \
    return Get<a11::data::Type>(env, self) ==                                  \
           Get<a11::data::Type>(env, other);                                   \
  }                                                                            \
  extern "C" JNIEXPORT jint JNICALL Java_a11_internal_NativeData_##name##Hash( \
      JNIEnv* env, jobject, jobject self) {                                    \
    return static_cast<jint>(Hash(Get<a11::data::Type>(env, self)));           \
  }                                                                            \
  extern "C" JNIEXPORT jstring JNICALL                                         \
  Java_a11_internal_NativeData_##name##DebugString(JNIEnv* env, jobject,       \
                                                   jobject self) {             \
    return Text(env, Get<a11::data::Type>(env, self).DebugString());           \
  }                                                                            \
  extern "C" JNIEXPORT jlong JNICALL                                           \
  Java_a11_internal_NativeData_##name##ApproxBytes(JNIEnv* env, jobject,       \
                                                   jobject self) {             \
    return static_cast<jlong>(Get<a11::data::Type>(env, self).ApproxBytes());  \
  }

A11_JVM_VALUE(ChunkMetadata, chunkMetadata)
A11_JVM_VALUE(Chunk, chunk)
A11_JVM_VALUE(NodeRef, nodeRef)
A11_JVM_VALUE(NodeFragment, nodeFragment)
A11_JVM_VALUE(Port, port)
A11_JVM_VALUE(ActionMessage, actionMessage)
A11_JVM_VALUE(WireMessage, wireMessage)
#undef A11_JVM_VALUE

#define A11_JVM_TEXT(Type, name, field)                                       \
  extern "C" JNIEXPORT jstring JNICALL Java_a11_internal_NativeData_##name(   \
      JNIEnv* env, jobject, jobject self) {                                   \
    return Text(env, Get<a11::data::Type>(env, self).field);                  \
  }                                                                           \
  extern "C" JNIEXPORT void JNICALL Java_a11_internal_NativeData_##name##Set( \
      JNIEnv* env, jobject, jobject self, jstring value) {                    \
    Get<a11::data::Type>(env, self).field = Text(env, value);                 \
  }
A11_JVM_TEXT(ChunkMetadata, chunkMetadataMimetype, mimetype)
A11_JVM_TEXT(Chunk, chunkRef, ref)
A11_JVM_TEXT(NodeRef, nodeRefId, id)
A11_JVM_TEXT(NodeFragment, nodeFragmentId, id)
A11_JVM_TEXT(Port, portName, name)
A11_JVM_TEXT(Port, portId, id)
A11_JVM_TEXT(ActionMessage, actionMessageId, id)
A11_JVM_TEXT(ActionMessage, actionMessageName, name)
#undef A11_JVM_TEXT

#define A11_JVM_VECTOR(Parent, Element, name, field, member)                   \
  extern "C" JNIEXPORT jint JNICALL Java_a11_internal_NativeData_##name##Size( \
      JNIEnv* env, jobject, jobject self) {                                    \
    return static_cast<jint>(Get<a11::data::Parent>(env, self).field.size());  \
  }                                                                            \
  extern "C" JNIEXPORT jobject JNICALL                                         \
  Java_a11_internal_NativeData_##name##Get(JNIEnv* env, jobject, jobject self, \
                                           jint index) {                       \
    return VectorGet(env, self, index, &a11::data::Parent::field,              \
                     ValueMember::member, "a11/" #Element);                    \
  }                                                                            \
  extern "C" JNIEXPORT jobject JNICALL                                         \
  Java_a11_internal_NativeData_##name##Set(JNIEnv* env, jobject, jobject self, \
                                           jint index, jobject value) {        \
    return VectorSet(env, self, index, value, &a11::data::Parent::field,       \
                     ValueMember::member, "a11/" #Element);                    \
  }                                                                            \
  extern "C" JNIEXPORT void JNICALL Java_a11_internal_NativeData_##name##Add(  \
      JNIEnv* env, jobject, jobject self, jint index, jobject value) {         \
    VectorAdd(env, self, index, value, &a11::data::Parent::field,              \
              ValueMember::member);                                            \
  }                                                                            \
  extern "C" JNIEXPORT jobject JNICALL                                         \
  Java_a11_internal_NativeData_##name##Remove(JNIEnv* env, jobject,            \
                                              jobject self, jint index) {      \
    return VectorRemove(env, self, index, &a11::data::Parent::field,           \
                        ValueMember::member, "a11/" #Element);                 \
  }                                                                            \
  extern "C" JNIEXPORT void JNICALL                                            \
  Java_a11_internal_NativeData_##name##Replace(                                \
      JNIEnv* env, jobject, jobject self, jobjectArray values) {               \
    ReplaceVector(env, self, values, &a11::data::Parent::field,                \
                  ValueMember::member);                                        \
  }                                                                            \
  extern "C" JNIEXPORT void JNICALL                                            \
  Java_a11_internal_NativeData_##name##Clear(JNIEnv* env, jobject,             \
                                             jobject self) {                   \
    ClearVector(env, self, &a11::data::Parent::field, ValueMember::member);    \
  }
A11_JVM_VECTOR(ActionMessage, Port, actionMessageInputs, inputs, kInputs)
A11_JVM_VECTOR(ActionMessage, Port, actionMessageOutputs, outputs, kOutputs)
A11_JVM_VECTOR(WireMessage, NodeFragment, wireMessageNodeFragments,
               node_fragments, kFragments)
A11_JVM_VECTOR(WireMessage, ActionMessage, wireMessageActions, actions,
               kActions)
#undef A11_JVM_VECTOR

namespace {
template <typename T>
T MoveValue(JNIEnv* env, jobject self) {
  auto source = Ref<T>(env, self);
  auto moved = std::move(source->get());
  InvalidateChildren(source);
  return moved;
}
}  // namespace

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeData_streamSendMoved(JNIEnv* env, jobject, jobject self,
                                             jobject message) {
  auto stream = Object<a11::jvm::BoundStream>(Id(env, self));
  if (!stream.ok()) {
    return Status(env, stream.status());
  }
  return Status(env, (*stream)->stream->Send(
                         MoveValue<a11::data::WireMessage>(env, message)));
}

extern "C" JNIEXPORT jobject JNICALL
Java_a11_internal_NativeData_sessionSendMoved(JNIEnv* env, jobject,
                                              jobject self, jobject message) {
  auto session = Object<a11::service::SessionWithRecv>(Id(env, self));
  if (!session.ok()) {
    return Status(env, session.status());
  }
  return Status(
      env, (*session)->Send(MoveValue<a11::data::WireMessage>(env, message)));
}

extern "C" JNIEXPORT jobject JNICALL Java_a11_internal_NativeData_storePutMoved(
    JNIEnv* env, jobject, jobject self, jobject fragment) {
  auto store = Object<a11::stores::ChunkStore>(Id(env, self));
  if (!store.ok()) {
    return Status(env, store.status());
  }
  auto pending = std::shared_ptr<PendingOperation>(
      std::make_shared<PendingFuture<std::uint32_t>>(
          (*store)->Put(MoveValue<a11::data::NodeFragment>(env, fragment))));
  return Ok(env, BoxLong(env, Retain(std::move(pending))));
}
