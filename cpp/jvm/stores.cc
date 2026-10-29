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

#include "a11/nodes/async_node.h"
#include "a11/nodes/node_map.h"
#include "a11/stores/chunk_store_reader.h"
#include "a11/stores/chunk_store_writer.h"
#include "a11/stores/local_chunk_store.h"
#include "jvm/bridge.h"
#ifdef A11_BUILD_SQLITE
#include "a11/stores/sqlite_chunk_store.h"
#endif

namespace a11::jvm {
namespace {

template <typename T>
absl::StatusOr<nlohmann::json> Saved(
    absl::StatusOr<std::shared_ptr<T>> result) {
  if (!result.ok()) {
    return result.status();
  }
  return Save(std::move(*result));
}

absl::StatusOr<stores::ChunkStoreReaderOptions> ReaderOptions(
    const nlohmann::json& args) {
  stores::ChunkStoreReaderOptions options;
  ABSL_ASSIGN_OR_RETURN(options.ordered,
                        DefaultArg<bool>(args, "ordered", options.ordered));
  ABSL_ASSIGN_OR_RETURN(
      options.pop_chunks,
      DefaultArg<bool>(args, "pop_chunks", options.pop_chunks));
  ABSL_ASSIGN_OR_RETURN(
      options.num_chunks_to_buffer,
      DefaultArg<std::uint64_t>(args, "num_chunks_to_buffer",
                                options.num_chunks_to_buffer));
  ABSL_ASSIGN_OR_RETURN(options.offset, DefaultArg<std::uint32_t>(
                                            args, "offset", options.offset));
  ABSL_ASSIGN_OR_RETURN(
      options.sticky_mimetype,
      DefaultArg<bool>(args, "sticky_mimetype", options.sticky_mimetype));
  if (args.find("max_chunks_to_read") != args.end() &&
      !args.at("max_chunks_to_read").is_null()) {
    ABSL_ASSIGN_OR_RETURN(options.max_chunks_to_read,
                          Arg<std::uint64_t>(args, "max_chunks_to_read"));
  }
  return options;
}

absl::StatusOr<stores::ChunkStoreWriterOptions> WriterOptions(
    const nlohmann::json& args) {
  stores::ChunkStoreWriterOptions options;
  ABSL_ASSIGN_OR_RETURN(options.offset, DefaultArg<std::uint32_t>(
                                            args, "offset", options.offset));
  ABSL_ASSIGN_OR_RETURN(
      options.max_chunks_to_write_at_once,
      DefaultArg<std::uint64_t>(args, "max_chunks_to_write_at_once",
                                options.max_chunks_to_write_at_once));
  ABSL_ASSIGN_OR_RETURN(
      options.sticky_mimetype,
      DefaultArg<bool>(args, "sticky_mimetype", options.sticky_mimetype));
  if (args.find("num_chunks_to_buffer") != args.end() &&
      !args.at("num_chunks_to_buffer").is_null()) {
    ABSL_ASSIGN_OR_RETURN(options.num_chunks_to_buffer,
                          Arg<std::uint64_t>(args, "num_chunks_to_buffer"));
  }
  return options;
}
}  // namespace

void BindStores(absl::flat_hash_map<std::string, Operation>* operations) {
  (*operations)["reader.defaults"] =
      [](std::int64_t,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    stores::ChunkStoreReaderOptions options;
    return nlohmann::json{
        {"ordered", options.ordered},
        {"pop_chunks", options.pop_chunks},
        {"num_chunks_to_buffer", options.num_chunks_to_buffer},
        {"offset", options.offset},
        {"sticky_mimetype", options.sticky_mimetype}};
  };
  (*operations)["reader.validateOptions"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto options, ReaderOptions(args));
    return Result(options.Validate());
  };
  (*operations)["writer.defaults"] =
      [](std::int64_t,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    stores::ChunkStoreWriterOptions options;
    return nlohmann::json{
        {"offset", options.offset},
        {"max_chunks_to_write_at_once", options.max_chunks_to_write_at_once},
        {"sticky_mimetype", options.sticky_mimetype}};
  };
  (*operations)["writer.validateOptions"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto options, WriterOptions(args));
    return Result(options.Validate());
  };
  (*operations)["store.local"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(std::string id, Arg<std::string>(args, "id"));
    ABSL_ASSIGN_OR_RETURN(auto store,
                          stores::LocalChunkStore::Create(std::move(id)));
    return Save(std::static_pointer_cast<stores::ChunkStore>(std::move(store)));
  };
#ifdef A11_BUILD_SQLITE
  (*operations)["store.sqlite"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(std::string id, Arg<std::string>(args, "id"));
    ABSL_ASSIGN_OR_RETURN(std::string root, Arg<std::string>(args, "root"));
    ABSL_ASSIGN_OR_RETURN(auto store, stores::SQLiteChunkStore::Create(
                                          std::move(id), std::move(root)));
    return Save(std::static_pointer_cast<stores::ChunkStore>(std::move(store)));
  };
#endif
  (*operations)["store.id"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto store, Object<stores::ChunkStore>(id));
    ABSL_ASSIGN_OR_RETURN(std::string name, store->GetId());
    return nlohmann::json(name);
  };
  (*operations)["store.get"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto store, Object<stores::ChunkStore>(id));
    ABSL_ASSIGN_OR_RETURN(auto seq, Arg<std::uint32_t>(args, "seq"));
    ABSL_ASSIGN_OR_RETURN(absl::Time deadline, Deadline(args));
    return Async(store->Get(seq, deadline));
  };
  (*operations)["store.getByArrivalOrder"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto store, Object<stores::ChunkStore>(id));
    ABSL_ASSIGN_OR_RETURN(auto offset, Arg<std::uint64_t>(args, "offset"));
    ABSL_ASSIGN_OR_RETURN(absl::Time deadline, Deadline(args));
    return Async(store->GetByArrivalOrder(offset, deadline));
  };
  (*operations)["store.next"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto store, Object<stores::ChunkStore>(id));
    ABSL_ASSIGN_OR_RETURN(auto limit, DefaultArg<size_t>(args, "limit", 1));
    ABSL_ASSIGN_OR_RETURN(absl::Time deadline, Deadline(args));
    return Async(store->Next(deadline, limit));
  };
  (*operations)["store.putMany"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto store, Object<stores::ChunkStore>(id));
    ABSL_ASSIGN_OR_RETURN(nlohmann::json fields,
                          Arg<nlohmann::json>(args, "fragments"));
    if (!fields.is_array()) {
      return absl::InvalidArgumentError("Expected a fragment array");
    }
    std::vector<data::NodeFragment> fragments;
    for (const nlohmann::json& item : fields) {
      ABSL_ASSIGN_OR_RETURN(data::NodeFragment fragment,
                            data::NodeFragmentFromJsonValue(item));
      fragments.push_back(std::move(fragment));
    }
    return Async(store->PutMany(std::move(fragments)));
  };
  (*operations)["store.clearData"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto store, Object<stores::ChunkStore>(id));
    ABSL_ASSIGN_OR_RETURN(auto seq, Arg<std::uint32_t>(args, "seq"));
    return Async(store->ClearData(seq));
  };
  (*operations)["store.seqForArrivalOrder"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto store, Object<stores::ChunkStore>(id));
    ABSL_ASSIGN_OR_RETURN(auto offset, Arg<std::uint64_t>(args, "offset"));
    return Async(store->GetSeqForArrivalOrder(offset));
  };
  (*operations)["store.closeWritesWithStatus"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto store, Object<stores::ChunkStore>(id));
    ABSL_ASSIGN_OR_RETURN(absl::Status status, StatusArg(args, "status"));
    ABSL_ASSIGN_OR_RETURN(
        bool return_status,
        DefaultArg<bool>(args, "returnStatusIfAlreadyClosed", false));
    return Async(
        store->CloseWritesWithStatus(std::move(status), return_status));
  };
  (*operations)["store.size"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto store, Object<stores::ChunkStore>(id));
    return Async(store->Size());
  };
  (*operations)["store.finalSeq"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto store, Object<stores::ChunkStore>(id));
    return Async(store->GetFinalSeq());
  };
  (*operations)["store.holdsObjects"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto store, Object<stores::ChunkStore>(id));
    return nlohmann::json(store->HoldsObjects());
  };
  (*operations)["reader.create"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto store,
                          ObjectArg<stores::ChunkStore>(args, "store"));
    ABSL_ASSIGN_OR_RETURN(
        nlohmann::json fields,
        DefaultArg<nlohmann::json>(args, "options", nlohmann::json::object()));
    ABSL_ASSIGN_OR_RETURN(auto options, ReaderOptions(fields));
    return Saved(stores::ChunkStoreReader::Create(std::move(store), options));
  };
  (*operations)["writer.create"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto store,
                          ObjectArg<stores::ChunkStore>(args, "store"));
    ABSL_ASSIGN_OR_RETURN(
        nlohmann::json fields,
        DefaultArg<nlohmann::json>(args, "options", nlohmann::json::object()));
    ABSL_ASSIGN_OR_RETURN(auto options, WriterOptions(fields));
    return Saved(stores::ChunkStoreWriter::Create(std::move(store), options));
  };
  (*operations)["reader.next"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto reader, Object<stores::ChunkStoreReader>(id));
    ABSL_ASSIGN_OR_RETURN(absl::Duration timeout, Timeout(args));
    return Async(reader->Next(timeout));
  };
  (*operations)["reader.nextMany"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto reader, Object<stores::ChunkStoreReader>(id));
    ABSL_ASSIGN_OR_RETURN(auto limit, Arg<size_t>(args, "limit"));
    ABSL_ASSIGN_OR_RETURN(absl::Duration timeout, Timeout(args));
    return Async(reader->NextMany(limit, timeout));
  };
  (*operations)["reader.status"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto object, Object<stores::ChunkStoreReader>(id));
    return Encode(object->GetStatus());
  };
  (*operations)["reader.store"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto object, Object<stores::ChunkStoreReader>(id));
    return Encode(object->store());
  };
  (*operations)["reader.done"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto object, Object<stores::ChunkStoreReader>(id));
    return Async(object->Done());
  };
  (*operations)["reader.ensureStarted"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto object, Object<stores::ChunkStoreReader>(id));
    object->EnsureStarted();
    return Null();
  };
  (*operations)["reader.cancel"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto object, Object<stores::ChunkStoreReader>(id));
    object->Cancel();
    return Null();
  };
  (*operations)["writer.status"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto object, Object<stores::ChunkStoreWriter>(id));
    return Encode(object->GetStatus());
  };
  (*operations)["writer.store"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto object, Object<stores::ChunkStoreWriter>(id));
    return Encode(object->store());
  };
  (*operations)["writer.ensureStarted"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto object, Object<stores::ChunkStoreWriter>(id));
    object->EnsureStarted();
    return Null();
  };
  (*operations)["writer.flush"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto object, Object<stores::ChunkStoreWriter>(id));
    object->Flush();
    return Null();
  };
  (*operations)["writer.putChunk"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto writer, Object<stores::ChunkStoreWriter>(id));
    ABSL_ASSIGN_OR_RETURN(nlohmann::json fields,
                          Arg<nlohmann::json>(args, "chunk"));
    ABSL_ASSIGN_OR_RETURN(data::Chunk chunk, data::ChunkFromJsonValue(fields));
    ABSL_ASSIGN_OR_RETURN(auto seq, Sequence(args));
    ABSL_ASSIGN_OR_RETURN(bool final, DefaultArg<bool>(args, "final", false));
    return Async(writer->PutChunk(std::move(chunk), seq, final));
  };
  (*operations)["writer.enqueueChunk"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto writer, Object<stores::ChunkStoreWriter>(id));
    ABSL_ASSIGN_OR_RETURN(nlohmann::json fields,
                          Arg<nlohmann::json>(args, "chunk"));
    ABSL_ASSIGN_OR_RETURN(data::Chunk chunk, data::ChunkFromJsonValue(fields));
    ABSL_ASSIGN_OR_RETURN(auto seq, Sequence(args));
    ABSL_ASSIGN_OR_RETURN(bool final, DefaultArg<bool>(args, "final", false));
    auto write = writer->EnqueueChunk(std::move(chunk), seq, final);
    return nlohmann::json{{"admitted", Async(write.admitted)},
                          {"confirmation", Async(write.confirmation)}};
  };
  (*operations)["writer.abort"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto writer, Object<stores::ChunkStoreWriter>(id));
    ABSL_ASSIGN_OR_RETURN(absl::Status status, StatusArg(args, "status"));
    return Async(writer->AbortWithStatus(std::move(status)));
  };
  (*operations)["writer.cancel"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto writer, Object<stores::ChunkStoreWriter>(id));
    return Async(writer->Cancel());
  };
  (*operations)["writer.drainAndClose"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto writer, Object<stores::ChunkStoreWriter>(id));
    return Async(writer->DrainAndClose());
  };
  (*operations)["writer.waitForBufferToDrain"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto writer, Object<stores::ChunkStoreWriter>(id));
    return Async(writer->WaitForBufferToDrain());
  };
  (*operations)["node.create"] =
      [](std::int64_t,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto store,
                          ObjectArg<stores::ChunkStore>(args, "store"));
    return Saved(nodes::AsyncNode::Create(std::move(store)));
  };
  (*operations)["node.putChunk"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    ABSL_ASSIGN_OR_RETURN(nlohmann::json fields,
                          Arg<nlohmann::json>(args, "chunk"));
    ABSL_ASSIGN_OR_RETURN(data::Chunk chunk, data::ChunkFromJsonValue(fields));
    ABSL_ASSIGN_OR_RETURN(auto seq, Sequence(args));
    ABSL_ASSIGN_OR_RETURN(bool final, DefaultArg<bool>(args, "final", false));
    return Async(node->PutChunk(std::move(chunk), seq, final));
  };
  (*operations)["node.nextFragment"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    ABSL_ASSIGN_OR_RETURN(absl::Duration timeout, Timeout(args));
    return Async(node->NextFragment(timeout));
  };
  (*operations)["node.abort"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    ABSL_ASSIGN_OR_RETURN(absl::Status status, StatusArg(args, "status"));
    return Async(node->AbortWithStatus(std::move(status)));
  };
  (*operations)["node.id"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    ABSL_ASSIGN_OR_RETURN(std::string name, node->GetId());
    return Encode(name);
  };
  (*operations)["node.reader"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    return Saved(node->reader());
  };
  (*operations)["node.writer"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    return Saved(node->writer());
  };
  (*operations)["node.store"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    return Encode(node->GetChunkStore());
  };
  (*operations)["node.isWritable"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    return Async(node->IsWritable());
  };
  (*operations)["node.size"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    return Async(node->GetChunkStore()->Size());
  };
  (*operations)["node.close"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    return Async(node->Close());
  };
  (*operations)["node.waitForBufferToDrain"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    return Async(node->WaitForBufferToDrain());
  };
  (*operations)["node.readerStatus"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    return Encode(node->GetReaderStatus());
  };
  (*operations)["node.writerStatus"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    return Encode(node->GetWriterStatus());
  };
  (*operations)["node.writerAbortStatus"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    return Encode(node->GetWriterAbortStatus());
  };
  (*operations)["node.cancel"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    node->Cancel();
    return Null();
  };
  (*operations)["node.cancelReader"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    node->CancelReader();
    return Null();
  };
  (*operations)["node.cancelWriter"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto node, Object<nodes::AsyncNode>(id));
    node->CancelWriter();
    return Null();
  };
  (*operations)["nodeMap.create"] =
      [](std::int64_t,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    return Saved(nodes::NodeMap::Create());
  };
  (*operations)["nodeMap.get"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto map, Object<nodes::NodeMap>(id));
    ABSL_ASSIGN_OR_RETURN(std::string name, Arg<std::string>(args, "id"));
    return Saved(map->Get(name));
  };
  (*operations)["nodeMap.getIfExists"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto map, Object<nodes::NodeMap>(id));
    ABSL_ASSIGN_OR_RETURN(std::string name, Arg<std::string>(args, "id"));
    return Saved(map->GetIfExists(name));
  };
  (*operations)["nodeMap.discard"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto map, Object<nodes::NodeMap>(id));
    ABSL_ASSIGN_OR_RETURN(std::string name, Arg<std::string>(args, "id"));
    ABSL_ASSIGN_OR_RETURN(auto expected,
                          ObjectArg<nodes::AsyncNode>(args, "expected", true));
    return Saved(map->Discard(name, expected));
  };
  (*operations)["nodeMap.contains"] =
      [](std::int64_t id,
         const nlohmann::json& args) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto map, Object<nodes::NodeMap>(id));
    ABSL_ASSIGN_OR_RETURN(std::string name, Arg<std::string>(args, "id"));
    return nlohmann::json(map->Contains(name));
  };
  (*operations)["nodeMap.size"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto map, Object<nodes::NodeMap>(id));
    return nlohmann::json(map->Size());
  };
  (*operations)["nodeMap.ids"] =
      [](std::int64_t id,
         const nlohmann::json&) -> absl::StatusOr<nlohmann::json> {
    ABSL_ASSIGN_OR_RETURN(auto map, Object<nodes::NodeMap>(id));
    return nlohmann::json(map->Ids());
  };
}
}  // namespace a11::jvm
